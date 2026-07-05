#!/usr/bin/env python3
"""Small ISO9660 image writer for the CD32X build.

This intentionally implements only the mkisofs/genisoimage subset used by
Makefile.cd32x:

    -sysid <id> -volid <id> -generic-boot <file> -o <image> <root>

It writes a single-session MODE1/2048 ISO9660 image with a 16-sector system
area.  The generic boot file is copied into that system area, which matches the
Sega CD boot-block use case.  It is a fallback for hosts that do not have
cdrkit/genisoimage, not a general-purpose mkisofs replacement.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import os
from pathlib import Path
import struct
import sys
from dataclasses import dataclass, field
from typing import Iterable

SECTOR_SIZE = 2048
SYSTEM_AREA_SECTORS = 16
PVD_SECTOR = 16
VDT_SECTOR = 17
L_PATH_TABLE_SECTOR = 18
M_PATH_TABLE_SECTOR = 19
FIRST_DATA_SECTOR = 20


def _pad_sector(data: bytes) -> bytes:
    rem = len(data) % SECTOR_SIZE
    if rem:
        return data + (b"\0" * (SECTOR_SIZE - rem))
    return data


def _both_endian_16(value: int) -> bytes:
    return struct.pack("<H", value) + struct.pack(">H", value)


def _both_endian_32(value: int) -> bytes:
    return struct.pack("<I", value) + struct.pack(">I", value)


def _a_chars(text: str, length: int) -> bytes:
    # ISO9660 A-characters. Spaces are valid padding. Truncate conservatively.
    encoded = text.upper().encode("ascii", "replace")[:length]
    return encoded.ljust(length, b" ")


def _iso_name(name: str, is_dir: bool) -> str:
    out = []
    for ch in name.upper():
        if "A" <= ch <= "Z" or "0" <= ch <= "9" or ch in "_.":
            out.append(ch)
        else:
            out.append("_")
    cleaned = "".join(out).strip(".") or "_"
    if is_dir:
        return cleaned.replace(".", "_")
    if ";" not in cleaned:
        cleaned += ";1"
    return cleaned


def _dir_datetime() -> bytes:
    now = _dt.datetime.now(_dt.UTC)
    # ISO9660 7-byte directory timestamp. GMT offset is signed 15-minute units.
    return bytes([now.year - 1900, now.month, now.day, now.hour, now.minute, now.second, 0])


def _volume_datetime() -> bytes:
    now = _dt.datetime.now(_dt.UTC)
    return now.strftime("%Y%m%d%H%M%S00").encode("ascii") + b"\0"


@dataclass
class Node:
    source: Path
    iso_name: str
    is_dir: bool
    parent: "Node | None" = None
    children: list["Node"] = field(default_factory=list)
    size: int = 0
    extent: int = 0
    dir_number: int = 0
    dir_data: bytes = b""

    @property
    def display_path(self) -> str:
        if self.parent is None:
            return "/"
        parts = []
        n: Node | None = self
        while n is not None and n.parent is not None:
            parts.append(n.iso_name)
            n = n.parent
        return "/" + "/".join(reversed(parts))


def _scan_tree(root_path: Path) -> Node:
    root = Node(source=root_path, iso_name="", is_dir=True, parent=None)

    def walk(node: Node) -> None:
        entries = sorted(node.source.iterdir(), key=lambda p: p.name.upper())
        seen: set[str] = set()
        for path in entries:
            if path.name in {".", ".."}:
                continue
            is_dir = path.is_dir()
            if not is_dir and not path.is_file():
                continue
            iso_name = _iso_name(path.name, is_dir)
            if iso_name in seen:
                raise SystemExit(f"ISO9660 name collision in {node.display_path}: {path.name} -> {iso_name}")
            seen.add(iso_name)
            child = Node(
                source=path,
                iso_name=iso_name,
                is_dir=is_dir,
                parent=node,
                size=0 if is_dir else path.stat().st_size,
            )
            node.children.append(child)
            if is_dir:
                walk(child)

    walk(root)
    return root


def _directories(root: Node) -> list[Node]:
    dirs: list[Node] = []

    def rec(node: Node) -> None:
        if node.is_dir:
            dirs.append(node)
            for child in node.children:
                rec(child)

    rec(root)
    for i, d in enumerate(dirs, start=1):
        d.dir_number = i
    return dirs


def _dir_record(node: Node, name: bytes | str) -> bytes:
    if isinstance(name, str):
        name_bytes = name.encode("ascii")
    else:
        name_bytes = name
    flags = 0x02 if node.is_dir else 0x00
    size = len(node.dir_data) if node.is_dir else node.size
    rec = bytearray()
    rec.append(0)  # filled below
    rec.append(0)  # extended attribute length
    rec += _both_endian_32(node.extent)
    rec += _both_endian_32(size)
    rec += _dir_datetime()
    rec.append(flags)
    rec.append(0)  # file unit size
    rec.append(0)  # interleave gap size
    rec += _both_endian_16(1)  # volume sequence number
    rec.append(len(name_bytes))
    rec += name_bytes
    if len(rec) & 1:
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def _build_directory_data(dirs: list[Node]) -> None:
    for d in dirs:
        parent = d.parent if d.parent is not None else d
        entries = [_dir_record(d, b"\0"), _dir_record(parent, b"\1")]
        for child in d.children:
            entries.append(_dir_record(child, child.iso_name))
        d.dir_data = _pad_sector(b"".join(entries))


def _assign_extents(root: Node) -> list[Node]:
    dirs = _directories(root)

    files: list[Node] = []

    def rec(n: Node) -> None:
        for c in n.children:
            if c.is_dir:
                rec(c)
            else:
                files.append(c)

    rec(root)

    # Directory record lengths do not depend on sector numbers, but directory
    # records do contain both child-directory and file extents.  Build once with
    # placeholder values to learn each directory size, assign final directory and
    # file extents, then rebuild the directory sectors with those final file
    # extents.  The previous fallback ISO writer assigned file extents after the
    # last directory build, leaving files at extent 0 and making the Sega CD boot
    # loader read the system area instead of APP.BIN.
    sector = FIRST_DATA_SECTOR
    for d in dirs:
        d.extent = sector
        d.dir_data = b"\0" * SECTOR_SIZE
        sector += 1
    _build_directory_data(dirs)

    sector = FIRST_DATA_SECTOR
    for d in dirs:
        d.extent = sector
        sector += len(d.dir_data) // SECTOR_SIZE

    for f in files:
        f.extent = sector
        sector += (f.size + SECTOR_SIZE - 1) // SECTOR_SIZE

    _build_directory_data(dirs)
    return dirs


def _path_table(dirs: Iterable[Node], endian: str) -> bytes:
    data = bytearray()
    for d in dirs:
        ident = b"\0" if d.parent is None else d.iso_name.encode("ascii")
        parent_number = 1 if d.parent is None else d.parent.dir_number
        data.append(len(ident))
        data.append(0)
        data += struct.pack("<I" if endian == "little" else ">I", d.extent)
        data += struct.pack("<H" if endian == "little" else ">H", parent_number)
        data += ident
        if len(ident) & 1:
            data.append(0)
    return bytes(data)


def _pvd(system_id: str, volume_id: str, volume_sectors: int, path_table_size: int, root: Node) -> bytes:
    pvd = bytearray(SECTOR_SIZE)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = _a_chars(system_id, 32)
    pvd[40:72] = _a_chars(volume_id, 32)
    pvd[80:88] = _both_endian_32(volume_sectors)
    pvd[120:124] = _both_endian_16(1)  # volume set size
    pvd[124:128] = _both_endian_16(1)  # volume sequence number
    pvd[128:132] = _both_endian_16(SECTOR_SIZE)
    pvd[132:140] = _both_endian_32(path_table_size)
    pvd[140:144] = struct.pack("<I", L_PATH_TABLE_SECTOR)
    pvd[144:148] = struct.pack("<I", 0)
    pvd[148:152] = struct.pack(">I", M_PATH_TABLE_SECTOR)
    pvd[152:156] = struct.pack(">I", 0)
    pvd[156:190] = _dir_record(root, b"\0")
    pvd[190:318] = b" " * 128  # volume set id
    pvd[318:446] = b" " * 128  # publisher
    pvd[446:574] = b" " * 128  # data preparer
    pvd[574:702] = b" " * 128  # application
    pvd[702:739] = b" " * 37
    pvd[739:776] = b" " * 37
    pvd[776:813] = b" " * 37
    pvd[813:850] = b" " * 37
    stamp = _volume_datetime()
    pvd[813:830] = stamp  # creation date/time
    pvd[830:847] = stamp  # modification date/time
    pvd[847:864] = b"0" * 16 + b"\0"  # expiration not specified
    pvd[864:881] = b"0" * 16 + b"\0"  # effective not specified
    pvd[881] = 1  # file structure version
    return bytes(pvd)


def _vdt() -> bytes:
    vdt = bytearray(SECTOR_SIZE)
    vdt[0] = 255
    vdt[1:6] = b"CD001"
    vdt[6] = 1
    return bytes(vdt)


def _write_file_padded(out, path: Path) -> None:
    with path.open("rb") as fh:
        while True:
            block = fh.read(1024 * 1024)
            if not block:
                break
            out.write(block)
    rem = path.stat().st_size % SECTOR_SIZE
    if rem:
        out.write(b"\0" * (SECTOR_SIZE - rem))


def build_iso(root_dir: Path, output: Path, system_id: str, volume_id: str, boot: Path | None) -> None:
    root_dir = root_dir.resolve()
    if not root_dir.is_dir():
        raise SystemExit(f"not a directory: {root_dir}")
    root = _scan_tree(root_dir)
    dirs = _assign_extents(root)
    l_path = _pad_sector(_path_table(dirs, "little"))
    m_path = _pad_sector(_path_table(dirs, "big"))
    path_table_size = len(_path_table(dirs, "little"))

    file_sectors = 0
    files: list[Node] = []

    def rec(n: Node) -> None:
        for c in n.children:
            if c.is_dir:
                rec(c)
            else:
                files.append(c)

    rec(root)
    for f in files:
        file_sectors += (f.size + SECTOR_SIZE - 1) // SECTOR_SIZE
    dir_sectors = sum(len(d.dir_data) // SECTOR_SIZE for d in dirs)
    volume_sectors = FIRST_DATA_SECTOR + dir_sectors + file_sectors

    system_area = bytearray(SECTOR_SIZE * SYSTEM_AREA_SECTORS)
    if boot is not None:
        boot_data = boot.read_bytes()
        if len(boot_data) > len(system_area):
            raise SystemExit(f"generic boot block is larger than 16 sectors: {boot}")
        system_area[: len(boot_data)] = boot_data

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as out:
        out.write(system_area)
        out.write(_pvd(system_id, volume_id, volume_sectors, path_table_size, root))
        out.write(_vdt())
        out.write(l_path)
        out.write(m_path)
        for d in dirs:
            out.write(d.dir_data)
        for f in files:
            _write_file_padded(out, f.source)

    actual = output.stat().st_size // SECTOR_SIZE
    if actual != volume_sectors:
        raise RuntimeError(f"internal sector count mismatch: wrote {actual}, expected {volume_sectors}")
    print(f"{volume_sectors} extents written ({(volume_sectors * SECTOR_SIZE + 1048575) // 1048576} MB)")


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="CD32X ISO9660 fallback writer")
    p.add_argument("-sysid", default="SEGA SEGACD")
    p.add_argument("-volid", default="CD32X")
    p.add_argument("-generic-boot", dest="generic_boot")
    p.add_argument("-full-iso9660-filenames", action="store_true")
    p.add_argument("-o", dest="output", required=True)
    p.add_argument("root")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    ns = parse_args(argv)
    build_iso(
        root_dir=Path(ns.root),
        output=Path(ns.output),
        system_id=ns.sysid,
        volume_id=ns.volid,
        boot=Path(ns.generic_boot) if ns.generic_boot else None,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
