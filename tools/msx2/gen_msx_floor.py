#!/usr/bin/env python3
"""Compile visible floor spans, never destination bitmaps, from the board mesh.

A pose is 115 row offsets (the extra one closes the last row, so the reader
copies exactly the bytes a row holds), a backdrop rectangle table, then
variable records:

    end-x (0=256), material, and for a textured span its sampler payload.

    material 0        black         no payload (the backdrop already owns it)
    material 1/2      the two walls no payload
    material 3/4      textured, v is constant along the row:
                      [page offset][acc Q8.8][dU Q8.8]          5 bytes
    material 5/6      textured, v moves along the row:
                      [page offset][2dV][accU][2dU][accV]       9 bytes

Both payloads are in the order the sampler pops them off a borrowed stack --
`ld sp, record` and four pops is a quarter of the cost of copying the record
into fixed variables, and the moving-v loop has to borrow SP for its dV step
anyway.  The two paths exist because the Z80 sampler for a constant-v span is a five
instruction loop (`ld c,h / ld a,(bc) / out / add hl,de`) that keeps the whole
texel address in one register pair: `acc` is already the texture's low address
byte in H and the U fraction in L, and `page offset` is what the texture base's
high byte is raised by.  That only works while nothing else moves inside the
byte, so a rotating pose -- where V walks too -- carries the second accumulator
and samples a 16x16 mip instead, whose row is a nibble and can be OR-ed in.

THE BACKDROP IS RECTANGLES, NOT SPANS.  A V9938 command costs about a
thousand T-states to set up and then paints GRAPHIC 7 at ~2.75 us a pixel, so
one command is worth roughly 360 filled pixels: the two hundred per-row black
spans of a pose were nearly fifty milliseconds of pure command set-up.  The
table below covers every black pixel with a dozen-odd rectangles instead,
chosen by a DP that trades painted area against that per-command price, and is
free to overlap the arena because the spans are drawn after it.  A full-band
draw then skips its black records entirely; a clipped repair still fills them,
having painted no backdrop.

Poses are therefore whole-pose fine (32x32, every span constant-v) or whole-pose
mip (16x16): mixing the two inside one picture would show as tiles of different
detail side by side.  UV is perspective-correct at scanline ends and affine
between them.  Painter visibility and shared Bresenham boundaries are resolved
here.  No runtime division.
"""
from pathlib import Path
import math
import struct
import numpy as np
from PIL import Image
import msx2_grb332 as grb

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'src/msx2/assets'
HEADER = ROOT / 'src/msx2/msx2_floor_data.h'

# The resident texture block, in the order the runtime keeps it: the two 16x16
# mips first, so a 256-byte-aligned base makes every mip row byte-addressable,
# then the two fine 32x32 materials.
MIP_BYTES = 16 * 16
FINE_BYTES = 32 * 32
MIP_BASE = 0
FINE_BASE = 2 * MIP_BYTES

# What one command costs, in pixels the same command could have painted.
COMMAND_PIXELS = 360
MAX_RECTS = 24
STRIDE_PIXELS = 6000
# Band rows 0..SPLIT_ROW-1 keep the finer stride; the rest take three.  Both
# halves have to divide by their own stride, and 36 + 78 does.
SPLIT_ROW = 36


def edge_x(a, b, y):
    if a[1] > b[1]:
        a, b = b, a
    dy = b[1] - a[1]
    if not dy or not a[1] <= y < b[1]:
        return None
    dx = b[0] - a[0]
    return a[0] + (1 if dx >= 0 else -1) * ((dy // 2 + (y-a[1])*abs(dx)) // dy)


def homography(q):
    matrix, rhs = [], []
    for (x,y), (u,v) in zip(q, ((0,0),(1,0),(1,1),(0,1))):
        matrix += [[x,y,1,0,0,0,-u*x,-u*y], [0,0,0,x,y,1,-v*x,-v*y]]
        rhs += [u,v]
    try:
        return np.linalg.solve(matrix, rhs)
    except np.linalg.LinAlgError:
        return None


def uv(h, x, y):
    z = h[6]*x+h[7]*y+1
    return ((h[0]*x+h[1]*y+h[2])/z, (h[3]*x+h[4]*y+h[5])/z)


def faces_of(record):
    xs = struct.unpack_from('<52h', record)
    pts = list(zip(xs, record[104:156]))
    flags = record[156]
    zr, zb = (4,46) if flags & 2 else (0,40)
    xc, xb = (5,35) if flags & 1 else (0,30)
    faces = []
    for c in range(5):
        faces.append(([zr*6+c,zr*6+c+1,zb+c+1,zb+c],1))
    for r in range(4):
        faces.append(([r*6+xc,(r+1)*6+xc,xb+r+1,xb+r],2))
    for r in range(4):
        for c in range(5):
            faces.append(([r*6+c,r*6+c+1,(r+1)*6+c+1,(r+1)*6+c],3+((r+c)&1)))
    return [(list(map(pts.__getitem__, ids)), mat) for ids,mat in faces]


def coverage(shapes):
    """owners[y-14][x]: the face index painting that pixel, -1 for nothing."""
    rows = []
    for y in range(14,128):
        owners = [-1]*256
        for i,(q,_) in enumerate(shapes):
            hits = [x for a,b in zip(q,q[1:]+q[:1]) if (x := edge_x(a,b,y)) is not None]
            if len(hits) >= 2:
                l,r = max(0,min(hits)), min(256,max(hits))
                if l < r:
                    owners[l:r] = [i]*(r-l)
        rows.append(owners)
    return rows


def clampq(value, scale):
    """Q8.8 texel coordinate, held inside [0, scale) so the packed index of a
    sample can never carry out of the byte the sampler walks."""
    return max(0, min(scale*256 - 1, int(round(value))))


def span_payload(h, x, end, y, scale, material):
    """(dv_is_zero, payload bytes) for one textured span at row y."""
    n = max(1, end-x-1)
    u0,v0 = uv(h, x+.5, y+.5)
    u1,v1 = uv(h, end-.5, y+.5)
    for value in (u0,v0,u1,v1):
        if not math.isfinite(value):
            raise ValueError('nonfinite UV')
    u0 = clampq(u0*scale*256, scale); u1 = clampq(u1*scale*256, scale)
    v0 = clampq(v0*scale*256, scale); v1 = clampq(v1*scale*256, scale)
    du = (u1-u0)//n
    dv = (v1-v0)//n
    if abs(du) > 32767 or abs(dv) > 32767:
        raise ValueError('UV step overflow')
    # The last sample must stay inside the material as well: an affine walk
    # that leaves it wraps into the next texture row and prints a seam.
    while u0 + du*n >= scale*256 or u0 + du*n < 0:
        du -= 1 if du > 0 else -1
    while v0 + dv*n >= scale*256 or v0 + dv*n < 0:
        dv -= 1 if dv > 0 else -1
    tex = material - 3
    if dv == 0:
        # v is fixed: fold its whole row offset into the address byte the
        # sampler carries in H, and the page it needs into the base offset.
        row = (v0 >> 8) * scale
        if scale == 32:
            offset = FINE_BASE + tex*FINE_BYTES + row
        else:
            offset = MIP_BASE + tex*MIP_BYTES + row
        assert offset & 255 == (offset & 255)
        acc = ((offset & 255) << 8) + u0
        return True, bytes((3 + tex, offset >> 8)) + struct.pack('<2H', acc & 65535, du & 65535)
    offset = MIP_BASE + tex*MIP_BYTES
    # The moving-v sampler walks two pixels a step, so the steps are baked
    # doubled: nothing on the Z80 side then has to shift them.
    du2, dv2 = du*2, (dv << 5)
    if not (-32768 <= du2 < 32768 and -32768 <= dv2 < 32768):
        raise ValueError('doubled UV step overflow')
    return False, bytes((5 + tex, offset >> 8)) + struct.pack(
        '<4H', dv2 & 65535, u0 & 65535, du2 & 65535, (v0 << 4) & 65535)


def backdrop(owners):
    """Rectangles covering every pixel no face paints, overlapping the arena
    where that is cheaper than another command."""
    left, right = [], []
    for own in owners:
        painted = [x for x,who in enumerate(own) if who >= 0]
        left.append(painted[0] if painted else 256)
        right.append(painted[-1]+1 if painted else 256)
    n = len(owners)

    def group_cost(i, j):
        l = max(left[i:j]); r = min(right[i:j])
        rects = [(0, i, l, j-i)] if l else []
        if r < 256:
            rects.append((r, i, 256-r, j-i))
        return sum(w*h for _,_,w,h in rects) + COMMAND_PIXELS*len(rects), rects

    best = [(0, [])] + [None]*n
    for j in range(1, n+1):
        for i in range(j):
            if best[i] is None:
                continue
            cost, rects = group_cost(i, j)
            total = best[i][0] + cost
            if best[j] is None or total < best[j][0]:
                best[j] = (total, best[i][1] + rects)
    rects = best[n][1]
    if len(rects) > MAX_RECTS:
        raise ValueError(f'backdrop needs {len(rects)} rectangles')
    return rects


def pose_blob(record, scale):
    shapes = faces_of(record)
    transforms = [homography(q) if mat >= 3 else None for q,mat in shapes]
    owners = coverage(shapes)
    rects = backdrop(owners)
    data = bytearray(115*2 + 1 + MAX_RECTS*4 + 4)
    data[115*2] = len(rects)
    textured = 0
    for i,(x,y,w,h) in enumerate(rects):
        struct.pack_into('<4B', data, 115*2+1+i*4, x, 14+y, w & 255, h)
    largest = 0
    fine = True
    for row,y in enumerate(range(14,128)):
        own = owners[row]
        struct.pack_into('<H',data,row*2,len(data))
        start = len(data)
        x = 0
        while x < 256:
            who = own[x]
            mat = shapes[who][1] if who >= 0 else 0
            end = x+1
            # Merge adjoining wall pieces, retain tile boundaries for UV.
            while end < 256 and (own[end] == who or
                  (mat < 3 and (shapes[own[end]][1] if own[end]>=0 else 0) == mat)):
                end += 1
            if mat >= 3:
                h = transforms[who]
                if h is None:
                    raise ValueError('visible degenerate textured face')
                flat,payload = span_payload(h, x, end, y, scale, mat)
                fine = fine and flat
                textured += end - x
                data += bytes((end & 255,)) + payload
            else:
                data += bytes((end & 255, mat))
            x = end
        largest = max(largest,len(data)-start)
    # How many screen rows one drawn row stands for while the camera moves.
    # A near pose is where the frame time is, and where a doubled row is a
    # third of a tile; a distant one is cheap already and loses its slab wall
    # to the aliasing, so it keeps its lines.
    # A doubled row is a third of a distant tile and most of its slab wall, so
    # a pose whose board is small keeps its lines where the board is far --
    # the top of the band -- and only trebles them lower down, where a tile is
    # tall enough not to notice and where most of the frame is spent anyway.
    near = textured > STRIDE_PIXELS
    data[115*2 + 1 + MAX_RECTS*4] = 3 if near else 2
    data[115*2 + 4 + MAX_RECTS*4] = 255 if near else SPLIT_ROW
    # The rows the arena actually reaches.  Outside them a row has nothing to
    # walk and nothing to fetch: it only has to go back to black where the
    # page's last pose left something, which the sliver pass already does.
    body = [r for r,own in enumerate(owners) if any(o >= 0 for o in own)]
    data[115*2 + 2 + MAX_RECTS*4] = body[0] if body else 0
    data[115*2 + 3 + MAX_RECTS*4] = (body[-1] + 1) if body else 0
    struct.pack_into('<H',data,114*2,len(data))
    if len(data)>16384 or largest>128:
        raise ValueError(f'floor bounds exceeded: pose={len(data)}, row={largest}')
    return data,largest,fine


def main():
    # Allocate after the scene manifest, independently of its asset generator.
    last = 5
    for raw in (ASSETS/'manifest.txt').read_text().splitlines():
        fields = raw.split('#')[0].split()
        if fields:
            _,seg,name = fields
            last = max(last,int(seg)+( (ASSETS/name).stat().st_size+16383)//16384)
    textures = bytearray()
    for size in (16,32):
        for i in (1,2):
            with Image.open(ROOT/f'assets/source/textures/sandstone_{i}.png') as im:
                textures += grb.quantize(im.convert('RGB'),(size,size))
    assert len(textures) == FINE_BASE + 2*FINE_BYTES
    (ASSETS/'floor_textures.bin').write_bytes(textures)
    mesh = (ASSETS/'board_mesh.bin').read_bytes()
    assert len(mesh)%512 == 0
    blob = bytearray()
    sizes=[]
    directory=[]
    maxrow=0
    mips=0
    for pos in range(0,len(mesh),512):
        data,row,fine = pose_blob(mesh[pos:pos+512], 32)
        if not fine:
            # A rotating pose samples the mip on both of its paths, so that no
            # two tiles of one picture carry different detail.
            data,row,_ = pose_blob(mesh[pos:pos+512], 16)
            mips += 1
        sizes.append(len(data)); maxrow=max(maxrow,row)
        # Whole poses remain in one segment, with padding for bounded row reads.
        if len(blob)%16384 + len(data) + 128 > 16384:
            blob += bytes(16384-len(blob)%16384)
        base = len(blob)%16384
        directory.append((len(blob)//16384,base))
        for r in range(115):
            offset = struct.unpack_from('<H',data,r*2)[0]
            struct.pack_into('<H',data,r*2,base+offset)
        blob += data + bytes(128)
    (ASSETS/'floor_spans.bin').write_bytes(blob)
    (ASSETS/'floor_manifest.txt').write_text(f'FLOOR_TEXTURES {last} floor_textures.bin\nFLOOR_SPANS {last+1} floor_spans.bin\n')
    HEADER.write_text('// Generated by tools/msx2/gen_msx_floor.py; do not edit.\n#pragma once\n'
        f'#define MSX2_FLOOR_TEXTURE_SEG {last}\n#define MSX2_FLOOR_SPAN_SEG {last+1}\n'
        f'#define MSX2_FLOOR_POSES {len(sizes)}\n#define MSX2_FLOOR_ROW_BYTES {maxrow}\n'
        f'#define MSX2_FLOOR_TEX_BYTES {len(textures)}\n'
        f'#define MSX2_FLOOR_MIP_BASE {MIP_BASE}\n#define MSX2_FLOOR_FINE_BASE {FINE_BASE}\n'
        f'#define MSX2_FLOOR_MAX_RECTS {MAX_RECTS}\n'
        f'#define MSX2_FLOOR_STRIDE_AT {115*2 + 1 + MAX_RECTS*4}\n'
        f'#define MSX2_FLOOR_ROW0_AT {115*2 + 2 + MAX_RECTS*4}\n'
        f'#define MSX2_FLOOR_ROW1_AT {115*2 + 3 + MAX_RECTS*4}\n'
        f'#define MSX2_FLOOR_SPLIT_AT {115*2 + 4 + MAX_RECTS*4}\n'
        + 'static const unsigned char g_floor_segments[] = {' + ','.join(str(d[0]) for d in directory) + '};\n'
        + 'static const unsigned short g_floor_offsets[] = {' + ','.join(str(d[1]) for d in directory) + '};\n')
    print(f'Floor: 16x16 mips + 32x32 fine GRB332 ({len(textures)} bytes), {len(sizes)} span poses '
          f'({mips} mip), {sum(sizes)} useful bytes, max row {maxrow}; segments {last}..{last+(len(blob)+16383)//16384}')

if __name__ == '__main__':
    main()
