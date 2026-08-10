#!/usr/bin/env python3
"""Inject an FM TOWNS "IPL4" boot sector into a finished ISO9660 image.

    tools/inject_ipl.py <image.iso> <IPL.BIN> <PAYLOAD.NAME>

This is the modern equivalent of Fujitsu's DOS-era IPL.COM: build the
filesystem first, then write the IPL into the image's System Area and
patch into it the on-disc location of the file the IPL has to load.

Why patch rather than compile the location in
-------------------------------------------
The FM TOWNS boot ROM only ever preloads the first four sectors of the
disc, so an IPL that wants more than 8KB of code has to read the rest
itself, by LBA. That LBA is a property of the *finished* image, and the
image cannot be built until the IPL exists - the classic chicken and egg.
Building a throwaway probe ISO first to discover the LBA (what this repo
did before) resolves it only as long as the probe and the real image
agree down to the sector, which is a silent, easily broken invariant:
anything that changes a file placed before the payload shifts it, and the
IPL then loads whatever moved into its place.

Patching afterwards has no such invariant. The System Area (LBA 0-15) is
reserved by ISO9660 and is not addressed by anything in the filesystem,
so writing it after the fact changes no offsets and invalidates nothing.

Header layout (see src/boot/bootsect.S)
---------------------------------------
    0x00  "IPL4"    magic the boot ROM looks for
    0x04  jmp       entry point (boot ROM does call far <seg>:4)
    0x20  u32       IO.SYS start sector, for the UX/Marty ROM disk
    0x24  u32       IO.SYS sector count, likewise (must be >= 1)
    0x28  u32       payload start LBA      <- patched here
    0x2C  u32       payload sector count   <- patched here

0x20/0x24 are left as the IPL built them (0 / 1: "IO.SYS is sector 0",
which the ROM reads, finds is not an IO.SYS, and gives up on - see
docs/HOWFMTOWNS_BOOTS_FROM_CD.txt).
"""

import os
import struct
import sys

SECTOR = 2048
SYSTEM_AREA_SECTORS = 16            # ISO9660: LBA 0-15, then the PVD
SYSTEM_AREA = SYSTEM_AREA_SECTORS * SECTOR
IPL_PRELOAD_BYTES = 8192            # what the boot ROM preloads and runs
PVD_LBA = 16

OFF_PAYLOAD_LBA = 0x28
OFF_PAYLOAD_SECTORS = 0x2C

# The boot ROM's disk read (call far 0xFFFB:0x14) takes the sector number
# in DX with CX = 0, so bootsect.S can only address a 16-bit LBA.
MAX_LBA = 0xFFFF


def fail(msg):
    sys.stderr.write("inject_ipl.py: %s\n" % msg)
    sys.exit(1)


def find_file(iso, name):
    """Return (lba, size_in_bytes) of `name` in the image's root directory.

    Only the root directory is searched: the payload lives there, and a
    full path walk would be dead weight.
    """
    iso.seek(PVD_LBA * SECTOR)
    pvd = iso.read(SECTOR)
    if len(pvd) < SECTOR or pvd[0] != 1 or pvd[1:6] != b"CD001":
        fail("no ISO9660 Primary Volume Descriptor at LBA %d" % PVD_LBA)

    # The PVD's root directory record sits at offset 156; within a
    # directory record, +2 is the extent LBA and +10 its length, both
    # stored as little-endian then big-endian ("both-byte-order").
    root = pvd[156:156 + 34]
    root_lba = struct.unpack_from("<I", root, 2)[0]
    root_len = struct.unpack_from("<I", root, 10)[0]

    iso.seek(root_lba * SECTOR)
    data = iso.read(root_len)

    want = name.encode("ascii")
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            # Zero pads out the tail of a directory sector; the next
            # record, if any, starts at the following sector boundary.
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + rec_len]
        id_len = rec[32]
        ident = rec[33:33 + id_len]
        # ISO9660 level 1 appends a ";1" version to every file name.
        if ident == want or ident == want + b";1":
            return struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        pos += rec_len

    fail("%s is not in the image's root directory" % name)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__.split("\n\n")[1].strip() + "\n")
        return 2

    iso_path, ipl_path, payload_name = argv[1], argv[2], argv[3]

    try:
        with open(ipl_path, "rb") as f:
            ipl = f.read()
    except OSError as e:
        fail("cannot read %s: %s (run make first?)" % (ipl_path, e.strerror))

    if ipl[:4] != b"IPL4":
        fail("%s does not start with the \"IPL4\" magic" % ipl_path)
    if len(ipl) > IPL_PRELOAD_BYTES:
        # Anything past this is never preloaded, so it would never run.
        fail("%s is %d bytes; the boot ROM only preloads and runs the "
             "first %d" % (ipl_path, len(ipl), IPL_PRELOAD_BYTES))

    if os.path.getsize(iso_path) % SECTOR != 0:
        fail("%s is not a whole number of %d-byte sectors" % (iso_path, SECTOR))

    with open(iso_path, "r+b") as iso:
        lba, size = find_file(iso, payload_name)
        sectors = (size + SECTOR - 1) // SECTOR

        if lba > MAX_LBA:
            fail("%s lands at LBA %d, past the %d the boot ROM's disk read "
                 "can address; give it more weight in the mkisofs sort file "
                 "so it is placed earlier on the disc"
                 % (payload_name, lba, MAX_LBA))
        if lba < SYSTEM_AREA_SECTORS:
            fail("%s claims LBA %d, inside the reserved System Area"
                 % (payload_name, lba))
        if sectors == 0:
            fail("%s is empty" % payload_name)

        image = bytearray(ipl)
        image += bytes(SYSTEM_AREA - len(image))
        struct.pack_into("<I", image, OFF_PAYLOAD_LBA, lba)
        struct.pack_into("<I", image, OFF_PAYLOAD_SECTORS, sectors)

        iso.seek(0)
        iso.write(image)

    print("Injected %s into %s: %s at LBA %d, %d sectors (%d bytes)"
          % (ipl_path, iso_path, payload_name, lba, sectors, size))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
