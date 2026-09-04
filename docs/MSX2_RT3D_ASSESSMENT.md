# MSX2 / MSX2+ real-time polygon and card rendering assessment

Date: 2026-09-04

## Conclusion

The RT3D material supports a useful, fast MSX2 renderer, but not a general
replacement for the current full-screen scene compositor.

The best fit for this game is a hybrid:

* keep the authored full-screen story, map, and duel views as streamed SCREEN 8
  images;
* use flat polygons for small transient geometry, such as a board highlight,
  an optional low-poly cut-in, or a moving card back;
* keep settled card faces in a VRAM cache and redraw only the card that is
  moving or has changed;
* use a software horizontal-span texture mapper for that one moving card, with
  precomputed poses whenever the motion can be enumerated.

This gives real-time-looking motion at a sensible frame rate while keeping the
large, carefully authored parts of the game out of the Z80 rasterizer. A full
live polygon scene or fifteen live textured cards every frame would not be a
fast decent-speed MSX2 mode.

## What the RT3D files contribute

`MSX_docs/RT3D/readme.txt` describes a real-time, highly optimized Z80 engine
for ordinary MSX2-or-higher hardware. The model files (`bird.txt`, `jet.txt`,
`spider.txt`, `wasp.txt`, and `ballpoin.txt`) show the useful data shape: a
polygon count, per-polygon detail/colour information, vertex references, and
optional texture-map points.

The most important optimization in `MSX_docs/RT3D/notes.txt` is to store a
vertex as indices into separate X, Y, and Z coordinate arrays. The nine products
of a rotation matrix are calculated once for each distinct coordinate value and
cached; a vertex then becomes table fetches and additions rather than a new set
of multiplications. The second rule is even more valuable: reject invisible
triangles first, and calculate no point or value that is not needed.

`MSX2_PORT_PLAN.md` already applies the other relevant ideas to the port:
back-face culling, painter ordering, Bresenham edge walks, VDP LINE for solid
geometry, and horizontal CPU spans for textures. Its timing values are design
bands, not measurements; they must not be presented as physical hardware data
until the timing ROM is run on hardware.

## Flat filled polygons

### Feasible use

Flat polygons are feasible in real time when the scene is small and the number
of visible edges is bounded. The V9938 does not provide a triangle or polygon
fill primitive, so the renderer must either:

1. walk two edges and emit horizontal spans, or
2. use the VDP LINE command for outlines and a span/fill pass for the interior.

The RT3D approach is appropriate: choose the dominant span direction, use
incremental edge error terms instead of a division per scanline, cull by the
normal before projecting all corners, and draw back-to-front. For an authored
object, projected vertices, sort order, and even flat-shading variants should
be baked. For a genuinely moving camera, the indexed-coordinate caches from
`notes.txt` are the first optimization to add.

### ROM and speed tradeoff

A polygon model can use dramatically less ROM than many full-screen or
pre-warped bitmap poses. It does not make the pixels free: the Z80 must still
touch each filled pixel, and a large polygon can cost more CPU than streaming a
pre-rendered 54,272-byte SCREEN 8 page through the existing compositor.

The practical MSX2 target is a small overlay or one low-poly object, updated at
roughly 12--20 Hz while the cursor and HUD remain responsive. A whole duel
arena made from live polygons should be treated as an experiment behind a
timing gate, not as the shipping baseline. The current retained compositor is
also valuable because it can stream a complete authored view while the VDP
command engine and the CPU work independently.

## Texture-mapped cards

Texture mapping is possible, but it is software. The V9938 can move and fill
VRAM; it cannot fetch a texel for each destination pixel. A textured triangle
therefore uses horizontal spans and direct SCREEN 8 `OUT` writes. Perspective
correction should be omitted: linear U/V stepping is substantially cheaper and
is visually adequate for a small card.

There are two useful modes:

| Mode | Runtime work | Recommendation |
| --- | --- | --- |
| Baked span program | No geometry arithmetic; run precomputed COPY/DUP/SKIP rows when a card first appears | Shipping path for resting cards and enumerable flight poses |
| One live affine quad | Two edge DDAs, table-based U/V steps, direct texel output | Optional path for the single card currently in motion |

The port plan's working estimate is about 0.44 frame to rasterize one 40x28
card into a cache in the baked path, paid once per card arrival, and about 1.9
frames for one 56x40 live pose. These are engineering estimates, not MSX2
measurements. Fifteen live cards would be about 6.6 estimated frames even in
the baked path and roughly 28.5 in the live path, so it is not a per-frame
rendering strategy.

The important optimization is where the pixels go. Rasterize a changed card
into an off-screen VRAM cache, then restore it with a transparent VDP move. Do
not re-rasterize every settled card after every page flip. This is why a
texture-mapped card path can coexist with the current retained board.

(note, the game already does texture mapping with the cards but its a bit on the slow side versus RT3D binary)

## ROM budget comparison

The current generated MSX2 card set uses 79 textures at a 2,048-byte stride
(`src/generated/msx2_scenes.h`), about 158 KB for the generated card blob shown
by the build inputs. The port plan's future 48x64 master-texture budget is
3,072 bytes per card, about 240 KB for 78 cards, or about 720 KB if three
pre-baked brightness variants are retained. That is much smaller than storing
many warped copies for every card, view, and flight pose.

For full-screen art, polygonizing a scene only saves ROM if the scene can be
described by a genuinely small model and a small set of textures. A painted
story background usually cannot: its bitmap is already the most direct
representation. The right ROM-saving target is therefore card pose data and
small 3D effects, not the story paintings themselves.

## Recommended implementation gates

1. Build a timing ROM for OUTI/OUT writes, VDP LINE/fill commands, the baked
   span interpreter, the live affine inner loop, and the PSG ISR.
2. Add one flat-polygon highlight object to a hidden-page test screen. Compare
   its output with an offline reference and measure worst-case edge count.
3. Add one textured card, first with a baked span program and then with one live
   affine pose. Verify VRAM-cache restore after page flips and background repair.
4. Only expand the live path if measured timings leave headroom. Keep the
   current bitmap compositor as the fallback for every scene and all settled
   cards.

The RT3D material justifies this scoped approach: cache shared calculations,
reject invisible work early, and spend runtime only on the object the player is
currently watching.
