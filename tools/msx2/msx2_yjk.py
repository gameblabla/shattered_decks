#!/usr/bin/env python3
"""YJK / YJK+YAE conversion for the MSX2+ (V9958) SCREEN 10-12 targets.

WHAT THE HARDWARE STORES
------------------------
SCREEN 10, 11 and 12 keep one byte per pixel in exactly the layout GRAPHIC 7
(SCREEN 8) uses -- 256 bytes a line, a line per VRAM row, two pages -- so a
picture converted here streams through the same code the MSX2 build already
has.  Only the *meaning* of a byte changes.

Colour is carried by two planes at once:

  * Y, five bits, PER PIXEL, in bits 7..3.
  * J and K, six bits each, SHARED BY EVERY GROUP OF FOUR pixels whose x is
    the same multiple of four.  Their bits are spread across the low three
    bits of the group's four bytes: K = b0[2:0] | b1[2:0]<<3, and
    J = b2[2:0] | b3[2:0]<<3, both two's complement in -32..31.

  R = Y + J,  G = Y + K,  B = (5Y - 2J - K) / 4,  each clamped to 0..31.

So a group of four pixels may hold four brightnesses but only one hue: this is
chroma subsampling, and the failure mode it produces -- a coloured edge
smearing three pixels to its right -- is the thing an encoder has to spend its
effort on.

In SCREEN 10 and 11 bit 3 does double duty (YAE): when it is SET the pixel is
not YJK at all, it is palette entry bits 7..4 out of the ordinary 16-colour
MSX palette, at full RGB accuracy and with no group sharing.  A YJK pixel
therefore has an EVEN Y (bit 3 clear, so Y's own low bit is 0), which is the
one thing that separates SCREEN 10 from SCREEN 12: 12 spends bit 3 on Y and
has no palette pixels at all.

That is the whole design of this port's MSX2+ build: pictures are YJK, and
every piece of interface -- text, its box, panel rules -- is YAE, so the words
stay exactly the colour they were asked for and never take a hue from the
painting three pixels away.

Note that a YAE pixel's low three bits are still read as part of its group's
K or J.  They are preserved here rather than zeroed, so text written over a
picture cannot pull the colour out of the pixels beside it.

USAGE
-----
    msx2_yjk.py encode in.png out.bin [--dither 0.9] [--preview shot.png]
                       [--palette pal.json] [--yae] [--gla]
    msx2_yjk.py decode in.bin out.png [--palette pal.json]
    msx2_yjk.py compare a.png b.png            # PSNR, for tuning

As a module: encode(img, ...) -> (bytes, palette), decode(data, palette, size).
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

# ── The colour space ─────────────────────────────────────────────────────────
#
# Everything below works in the VDP's own 5-bit RGB, because that is the only
# resolution the decode equations have: a converter that reasons in 8-bit and
# rounds at the end spends its error budget twice.

MAX5 = 31


def to5(a):
    """8-bit RGB float -> 5-bit float, no rounding (dithering wants the tail)."""
    return a * (MAX5 / 255.0)


def from5(a):
    return np.clip(a, 0, MAX5) * (255.0 / MAX5)


def rgb_to_y(rgb5):
    """Y = (4B + 2R + G) / 8, the inverse of the decode equations."""
    return (4.0 * rgb5[..., 2] + 2.0 * rgb5[..., 0] + rgb5[..., 1]) / 8.0


def yjk_to_rgb(y, j, k):
    r = np.clip(y + j, 0, MAX5)
    g = np.clip(y + k, 0, MAX5)
    b = np.clip((5.0 * y - 2.0 * j - k) / 4.0, 0, MAX5)
    return np.stack([r, g, b], axis=-1)


# ── The palette ──────────────────────────────────────────────────────────────

def palette_bytes(palette):
    """16 (r,g,b) triples in 0..7 -> the 32 bytes the VDP wants."""
    out = bytearray()
    for r, g, b in palette[:16]:
        out.append(((r & 7) << 4) | (b & 7))
        out.append(g & 7)
    return bytes(out)


def palette_from_bytes(data):
    return [((data[i * 2] >> 4) & 7, data[i * 2 + 1] & 7, data[i * 2] & 7)
            for i in range(16)]


def palette_rgb5(palette):
    """The palette in the same 0..31 space the YJK maths uses (3 bits -> 5)."""
    return np.array([[r * MAX5 / 7.0, g * MAX5 / 7.0, b * MAX5 / 7.0]
                     for r, g, b in palette], dtype=np.float64)


# The interface palette this port's MSX2+ build programs into SCREEN 10.  It is
# the GRB332 UI colours of the MSX2 build, said again in the 3-bit RGB the
# palette registers hold -- so a panel, a rule and a line of text come out the
# same colour in both builds, and in the plus build they come out EXACTLY that
# colour instead of the nearest thing a shared chroma allows.
UI_PALETTE = [
    (0, 0, 0),        # 0  transparent/black
    (1, 1, 2),        # 1  panel navy
    (7, 6, 3),        # 2  gold
    (6, 5, 3),        # 3  sand
    (4, 3, 2),        # 4  dark sand
    (2, 6, 6),        # 5  teal
    (7, 2, 1),        # 6  red
    (7, 7, 7),        # 7  white
    (5, 4, 2),        # 8  bronze
    (3, 2, 1),        # 9  shadow
    (2, 4, 7),        # 10 sky blue
    (7, 5, 1),        # 11 amber
    (1, 4, 2),        # 12 deep green
    (5, 1, 1),        # 13 dark red
    (3, 3, 4),        # 14 slate
    (6, 6, 5),        # 15 bone
]


# The eight entries the interface owns.  A scene may fit the other eight to
# itself (see fit_palette): YJK's weak spot is the dark end -- Y moves in steps
# of two, which is sixteen levels of an eight-bit channel -- and eight palette
# colours dropped into exactly the shadows a picture actually has are worth
# more than three decibels, which is the whole difference between this encoder
# and a good one.
UI_RESERVED = 8


def fit_palette(img, reserved=UI_RESERVED, base=None, rounds=24, seed=1):
    """The 16 entries for one picture: the interface's, then a fit.

    The fit is a k-means over the pixels YJK reproduces WORST -- not over the
    picture, which would spend all eight entries on the sky that YJK already
    draws perfectly well.
    """
    base = list(base or UI_PALETTE)[:reserved]
    if reserved >= 16:
        return base[:16]
    src = to5(np.asarray(img.convert("RGB"), dtype=np.float64))
    _data, _pal, err = _encode_plain(src, screen12=False)
    flat = src.reshape(-1, 3)
    e = err.reshape(-1)
    sel = flat[e > np.percentile(e, 60)]
    n = 16 - reserved
    if len(sel) < n:
        sel = flat
    rng = np.random.default_rng(seed)
    centres = sel[rng.choice(len(sel), n, replace=False)].copy()
    for _ in range(rounds):
        d = ((sel[:, None, :] - centres[None, :, :]) ** 2).sum(axis=2)
        owner = d.argmin(axis=1)
        for i in range(n):
            m = owner == i
            if m.any():
                centres[i] = sel[m].mean(axis=0)
    fitted = [tuple(int(v) for v in np.round(np.clip(c, 0, MAX5) / MAX5 * 7))
              for c in centres]
    return base + fitted


def _encode_plain(src, screen12):
    """One undithered YJK pass over a 5-bit image: bytes, decoded, error."""
    h, w, _ = src.shape
    y = np.clip(rgb_to_y(src), 0, MAX5)
    j, k = _group_chroma(src, y)
    jj = np.repeat(j, 4, axis=1)
    kk = np.repeat(k, 4, axis=1)
    y = _best_y(src, jj, kk, not screen12)
    got = yjk_to_rgb(y, jj, kk)
    return y, (j, k), ((got - src) ** 2).sum(axis=2)


# ── Encoding ─────────────────────────────────────────────────────────────────

def _group_chroma(rgb5, y, mask=None):
    """J and K for every group of four pixels, as the least-squares fit.

    With Y already chosen per pixel, R = Y + J and G = Y + K are linear in J
    and K, so the fit that minimises squared error over the group is simply the
    mean of the per-pixel residuals.  (B depends on both and is left to the Y
    pass to absorb; weighting it in moves every hue towards blue for no visible
    gain -- the eye is reading R and G here.)

    A `mask` (True where the pixel is really drawn) restricts the mean to the
    pixels that exist.  A cut-out bust is the case: three quarters of a boundary
    group can be the transparent ground, and the ground in this project's
    portrait art is BLACK, so an unmasked mean hands the whole group the hue of
    nothing at all -- which is the black fringe four pixels wide that YJK puts
    round a silhouette a paletted mode only ever put one pixel of.
    """
    h, w, _ = rgb5.shape
    groups = w // 4
    dj = (rgb5[..., 0] - y).reshape(h, groups, 4)
    dk = (rgb5[..., 1] - y).reshape(h, groups, 4)
    if mask is None:
        j = dj.mean(axis=2)
        k = dk.mean(axis=2)
    else:
        m = mask.reshape(h, groups, 4).astype(np.float64)
        n = m.sum(axis=2)
        safe = np.maximum(n, 1.0)
        j = np.where(n > 0, (dj * m).sum(axis=2) / safe, dj.mean(axis=2))
        k = np.where(n > 0, (dk * m).sum(axis=2) / safe, dk.mean(axis=2))
    return np.clip(np.round(j), -32, 31), np.clip(np.round(k), -32, 31)


def spread(rgb8, mask):
    """The opaque colours pushed outward over everything the mask excludes.

    Y is per pixel and a transparent one is never blitted, so its brightness
    does not matter -- but its COLOUR still reaches the picture through the
    group chroma and through the dithering carry, and in the source art it is
    black.  Growing the figure outward a few pixels before encoding is what
    makes a boundary group fit the figure rather than the hole beside it."""
    out = rgb8.copy()
    known = mask.copy()
    for _ in range(6):
        if known.all():
            break
        for axis, shift in ((1, 1), (1, -1), (0, 1), (0, -1)):
            src = np.roll(out, shift, axis=axis)
            have = np.roll(known, shift, axis=axis)
            if axis == 1:
                if shift > 0:
                    have[:, 0] = False
                else:
                    have[:, -1] = False
            else:
                if shift > 0:
                    have[0, :] = False
                else:
                    have[-1, :] = False
            take = have & ~known
            out[take] = src[take]
            known = known | take
    return out


def _best_y(rgb5, j, k, even):
    """The Y each pixel wants, given the chroma its group settled on.

    Minimising (Y+J-R)^2 + (Y+K-G)^2 + ((5Y-2J-K)/4 - B)^2 over Y is a
    quadratic with one root; the 5/4 on B is what makes it worth writing out
    rather than averaging the three.
    """
    r, g, b = rgb5[..., 0], rgb5[..., 1], rgb5[..., 2]
    num = (r - j) + (g - k) + 1.25 * (b + (2.0 * j + k) / 4.0)
    y = num / (2.0 + 1.5625)
    y = np.clip(y, 0, MAX5)
    if even:
        return np.clip(np.round(y / 2.0) * 2.0, 0, 30)
    return np.clip(np.round(y), 0, MAX5)


def encode(img, dither=0.0, palette=None, allow_yae=False, yae_gain=1.0,
           screen12=False, fit_yae=0, mask=None, solid=None):
    """One picture -> (bytes, palette).

    `dither` is the fraction of each pixel's residual error carried into its
    neighbours (Floyd-Steinberg).  Chroma is shared by four pixels, so the
    residual an encoder cannot spend is large and structured -- diffusing it is
    worth more here than it is in a paletted mode, and 0.9 is what this port
    bakes with.

    `mask` is a cut-out's own opacity, True where a pixel's colour is worth
    fitting to: it keeps the transparent ground out of the group chroma and out
    of the dither carry (see _group_chroma).  `solid` is the wider mask of
    pixels whose colour is real at all, and it is what spread() grows outward
    from -- a bust is BLITTED past `mask`, out to whole chroma groups, so the
    pixels between the two have to carry something, and the nearest real pixel
    is the right something.  `solid` defaults to `mask`.

    `allow_yae` lets a pixel escape into the 16-colour palette when that is
    closer than anything its group's chroma can reach.  It is OFF for this
    port's pictures on purpose: the palette is the interface's, and a picture
    that borrows entries from it cannot be drawn over.
    """
    if fit_yae and not screen12:
        palette = fit_palette(img, reserved=16 - fit_yae, base=palette)
        allow_yae = True
    rgb8 = np.asarray(img.convert("RGB"), dtype=np.float64)
    h, w, _ = rgb8.shape
    if w % 4:
        raise ValueError("width must be a multiple of four (chroma groups)")
    if mask is not None:
        mask = np.asarray(mask, dtype=bool)
        rgb8 = spread(rgb8, np.asarray(solid, dtype=bool)
                            if solid is not None else mask)
    src = to5(rgb8)
    pal5 = palette_rgb5(palette or UI_PALETTE)
    even = not screen12

    out = np.zeros((h, w), dtype=np.uint8)
    # Floyd-Steinberg carries error to the right and to the next row, so the
    # rows are walked in order and one row of carry is kept ahead.
    carry = np.zeros((h + 1, w + 2, 3), dtype=np.float64)

    for y0 in range(h):
        row = src[y0] + carry[y0, 1:w + 1]
        row = np.clip(row, -8.0, MAX5 + 8.0)     # a runaway carry helps nobody
        rowv = row.reshape(1, w, 3)

        # Two passes: a provisional Y to fit the chroma to, then the Y that
        # chroma actually wants.  One pass is visibly worse on skin.
        y_prov = np.clip(rgb_to_y(rowv), 0, MAX5)
        j, k = _group_chroma(rowv, y_prov,
                             None if mask is None else mask[y0:y0 + 1])
        jj = np.repeat(j, 4, axis=1)
        kk = np.repeat(k, 4, axis=1)
        yv = _best_y(rowv, jj, kk, even)

        got = yjk_to_rgb(yv, jj, kk)[0]
        bytes_row = (((yv[0].astype(np.int32) & 31) << 3)).astype(np.uint8)
        # The group's chroma bits live in the low three bits of the four bytes.
        ji = (j[0].astype(np.int32) & 63)
        ki = (k[0].astype(np.int32) & 63)
        bytes_row[0::4] |= (ki & 7).astype(np.uint8)
        bytes_row[1::4] |= ((ki >> 3) & 7).astype(np.uint8)
        bytes_row[2::4] |= (ji & 7).astype(np.uint8)
        bytes_row[3::4] |= ((ji >> 3) & 7).astype(np.uint8)

        if allow_yae and not screen12:
            # A pixel may leave YJK for the palette when the palette is closer.
            # Its low three bits are KEPT: they still belong to the group.
            err_yjk = ((got - row) ** 2).sum(axis=1)
            d = ((pal5[None, :, :] - row[:, None, :]) ** 2).sum(axis=2)
            idx = d.argmin(axis=1)
            err_pal = d[np.arange(w), idx]
            take = err_pal * yae_gain < err_yjk
            if take.any():
                keep = bytes_row & 7
                bytes_row = np.where(take,
                                     ((idx << 4) | 8 | keep).astype(np.uint8),
                                     bytes_row)
                got = np.where(take[:, None], pal5[idx], got)

        out[y0] = bytes_row

        if dither > 0.0:
            err = (row - got) * dither
            if mask is not None:
                # Error is neither taken from nor pushed into a pixel nothing
                # will draw, exactly as the GRB332 bust path does it.
                err = err * mask[y0][:, None]
            # 7/16 right, 3/16 down-left, 5/16 down, 1/16 down-right.
            nxt = carry[y0 + 1]
            here = carry[y0]
            here[2:w + 2] += err * (7.0 / 16.0)
            nxt[0:w] += err * (3.0 / 16.0)
            nxt[1:w + 1] += err * (5.0 / 16.0)
            nxt[2:w + 2] += err * (1.0 / 16.0)

    return out.tobytes(), (palette or UI_PALETTE)


# ── Decoding, for previews and for checking someone else's converter ─────────

def decode(data, palette=None, size=(256, 212), screen12=False):
    w, h = size
    pal = palette_rgb5(palette or UI_PALETTE)
    a = np.frombuffer(data[:w * h], dtype=np.uint8).reshape(h, w).astype(np.int32)
    y = (a >> 3).astype(np.float64)
    low = (a & 7)
    groups = w // 4
    lo = low.reshape(h, groups, 4)
    k = (lo[:, :, 0] | (lo[:, :, 1] << 3)).astype(np.int32)
    j = (lo[:, :, 2] | (lo[:, :, 3] << 3)).astype(np.int32)
    k = np.where(k > 31, k - 64, k).astype(np.float64)
    j = np.where(j > 31, j - 64, j).astype(np.float64)
    jj = np.repeat(j, 4, axis=1)
    kk = np.repeat(k, 4, axis=1)
    rgb = yjk_to_rgb(y, jj, kk)
    if not screen12:
        yae = (a & 8) != 0
        idx = (a >> 4) & 15
        rgb = np.where(yae[..., None], pal[idx], rgb)
    return Image.fromarray(from5(rgb).round().astype(np.uint8), "RGB")


def psnr(a, b):
    x = np.asarray(a.convert("RGB"), dtype=np.float64)
    y = np.asarray(b.convert("RGB"), dtype=np.float64)
    mse = ((x - y) ** 2).mean()
    return float("inf") if mse == 0 else 10.0 * np.log10(255.0 * 255.0 / mse)


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    enc = sub.add_parser("encode")
    enc.add_argument("src")
    enc.add_argument("dst")
    enc.add_argument("--dither", type=float, default=0.0,
                     help="Floyd-Steinberg strength, 0..1 (0.9 is this port's)")
    enc.add_argument("--yae", action="store_true",
                     help="let pixels escape into the 16-colour palette")
    enc.add_argument("--fit-yae", type=int, default=0, metavar="N",
                     help="fit the last N palette entries to this picture and "
                          "use them (8 is what this port bakes with)")
    enc.add_argument("--yae-gain", type=float, default=1.0)
    enc.add_argument("--screen12", action="store_true",
                     help="SCREEN 12: no palette pixels, odd Y allowed")
    enc.add_argument("--palette", help="16-entry JSON palette, 0..7 per channel")
    enc.add_argument("--palette-out", help="write the palette as 32 VDP bytes")
    enc.add_argument("--preview", help="decode what was written, as a PNG")
    enc.add_argument("--gla", action="store_true",
                     help="prepend the 4-byte width/height header Graph Saurus uses")

    dec = sub.add_parser("decode")
    dec.add_argument("src")
    dec.add_argument("dst")
    dec.add_argument("--palette")
    dec.add_argument("--size", default="256x212")
    dec.add_argument("--screen12", action="store_true")

    cmp_ = sub.add_parser("compare")
    cmp_.add_argument("a")
    cmp_.add_argument("b")

    args = ap.parse_args(argv)

    if args.cmd == "compare":
        print("%.2f dB" % psnr(Image.open(args.a), Image.open(args.b)))
        return 0

    palette = None
    if getattr(args, "palette", None):
        with open(args.palette) as f:
            palette = [tuple(e) for e in json.load(f)]

    if args.cmd == "encode":
        img = Image.open(args.src).convert("RGB")
        data, palette = encode(img, dither=args.dither, palette=palette,
                               allow_yae=args.yae, yae_gain=args.yae_gain,
                               screen12=args.screen12, fit_yae=args.fit_yae)
        blob = data
        if args.gla:
            blob = bytes([img.width & 255, img.width >> 8,
                          img.height & 255, img.height >> 8]) + blob
        with open(args.dst, "wb") as f:
            f.write(blob)
        if args.palette_out:
            with open(args.palette_out, "wb") as f:
                f.write(palette_bytes(palette))
        if args.preview:
            decode(data, palette, img.size, args.screen12).save(args.preview)
            print("%s: %d bytes, %.2f dB" %
                  (args.dst, len(blob),
                   psnr(img, decode(data, palette, img.size, args.screen12))))
        return 0

    if args.cmd == "decode":
        w, h = (int(v) for v in args.size.split("x"))
        raw = open(args.src, "rb").read()
        if len(raw) == w * h + 4:
            raw = raw[4:]
        decode(raw, palette, (w, h), args.screen12).save(args.dst)
        return 0

    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
