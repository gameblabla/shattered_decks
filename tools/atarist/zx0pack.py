"""ZX0 packing for the Atari ST floppy.

The floppy is 720 KB and the port fills it: 619 KB of art and music against a
disk that also has to carry the program.  ZX0 buys back about 190 KB of that,
which is what makes room for anything else the port grows.

**THE DEPACKER WANTS `zx0`, NOT `zx0 -c`.**  `AtariST/unzx0_68000/unzx0_68000.S`
is Marty and Hodges' 68000 depacker and it reads the CURRENT (v2) stream, not
the classic v1 one -- verified by transliterating that assembly into
`reference_decompress` below and round-tripping both compressor modes through
it: v2 comes back byte for byte, v1 walks off the end of the buffer.  Every
blob this module writes is round-tripped through the same transliteration, so a
file that would not depack on the hardware fails the build instead.

**Decompression is IN PLACE.**  A 512 KB machine has no room for a scratch
buffer beside a 79 KB card sheet, so the packed bytes are loaded into the TAIL
of the destination buffer and the depacker overtakes them: `lead` is the
largest number of bytes the writer is ever ahead of the reader, so placing the
source at `dst + lead` means the reader is never overtaken.  It is measured,
not estimated -- the same pass that verifies the round trip records it.

`zx0` runs in QUICK mode.  Its optimal parser is superlinear and takes minutes
on the 80 KB card sheet; quick costs about 1.5% of ratio and runs in a moment,
and the build re-runs it whenever any source art changes.
"""

import os
import struct
import subprocess
import tempfile

MAGIC = b"ZX0!"
HDR = 16                # magic, raw size, lead, packed size -- all big-endian

# How much bigger than its contents a buffer that takes a packed load must be.
# Mirrored by ATARIST_ZX0_SLACK in src/atarist/atarist_disk.h; `pack` asserts
# no blob needs more.
SLACK = 256


def reference_decompress(src):
    """A transliteration of unzx0_68000.S, branch for branch.

    Returns the decompressed bytes and `lead`, the largest excess of bytes
    written over bytes read -- which is exactly where an in-place source has to
    start.  The bit reader mirrors the assembly's two kinds of read: a CONTROL
    bit refills the queue when it is empty, a data or flag bit does not, and
    the encoder's groups are even so the second never finds it empty."""
    a0 = 0
    out = bytearray()
    d1 = 0x80                   # bit queue, sentinel in the top bit
    d2 = -1                     # rep-offset, held negative
    lead = 0

    def note():
        nonlocal lead
        if len(out) - a0 > lead:
            lead = len(out) - a0

    def bit():
        nonlocal d1, a0
        d1 <<= 1
        carry = (d1 >> 8) & 1
        d1 &= 0xff
        if d1 == 0:
            note()
            d1 = (src[a0] << 1) | carry
            a0 += 1
            carry = (d1 >> 8) & 1
            d1 &= 0xff
        return carry

    def elias(d0):              # interlaced Elias gamma, seeded in d0.w
        while True:
            if bit():
                return d0
            d0 = ((d0 << 1) | bit()) & 0xffff

    state = "literals"
    while True:
        if state == "literals":
            for _ in range(elias(1)):
                note()
                out.append(src[a0])
                a0 += 1
            state = "offset" if bit() else "rep"
        elif state == "rep":
            for _ in range(elias(1)):
                out.append(out[len(out) + d2])
            state = "offset" if bit() else "literals"
        else:
            # d0.w starts at $fffe (moveq #-2) and only its low byte is read
            # back, so the bits that shift out of the top are discarded.
            d0 = (elias(0xfffe) + 1) & 0xff         # addq.b #1: the high byte
            if d0 == 0:
                return bytes(out), lead             # the end-of-data marker
            note()
            d2 = ((d0 - 256) << 8) | src[a0]        # negative, sign kept
            a0 += 1
            lenbit = d2 & 1
            d2 >>= 1                                # asr.l #1
            if lenbit:
                n = 2
            else:
                # The length's first control bit rode in the offset byte, so
                # the loop is entered one data bit in -- and it reaches the
                # copy WITHOUT the subq, so the count is one more.
                n = elias(((1 << 1) | bit()) & 0xffff) + 1
            for _ in range(n):
                out.append(out[len(out) + d2])
            state = "offset" if bit() else "literals"


def compress(raw):
    """`raw` through the system zx0, returned as (packed, lead)."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in")
        dst = os.path.join(tmp, "out")
        with open(src, "wb") as f:
            f.write(raw)
        subprocess.run(["zx0", "-q", "-f", src, dst],
                       check=True, capture_output=True)
        with open(dst, "rb") as f:
            packed = f.read()
    back, lead = reference_decompress(packed)
    if back != raw:
        raise RuntimeError("zx0 round trip failed: the 68000 depacker would "
                           "not reproduce this blob")
    if lead + len(packed) - len(raw) > SLACK:
        raise RuntimeError("zx0 needs %d bytes of slack, ATARIST_ZX0_SLACK is "
                           "%d" % (lead + len(packed) - len(raw), SLACK))
    return packed, lead


def pack(raw):
    """`raw` as a DAT/MUS file the ST's Atarist_DiskLoadPacked understands.

    A blob that does not actually get smaller is returned UNPACKED -- the
    loader takes either, and paying a decompression for nothing is worse than
    the four bytes of magic saved."""
    packed, lead = compress(raw)
    if len(packed) + HDR >= len(raw):
        return raw
    return (struct.pack(">4sIII", MAGIC, len(raw), lead, len(packed)) + packed)
