#!/usr/bin/env python3
"""Build the input-enabled variant of the supplied headless OpenMSX binary.

The capture archive contains the executable but not the original ``headless``
source tree.  The input hook is therefore kept as readable assembly and
applied reproducibly to the known 21.0 ELF layout instead of distributing an
unexplained, hand-edited binary.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


INPORT_VADDR = 0x12C5A3
HOOK_VADDR = 0x12CE00
ORIGINAL_INPORT = bytes.fromhex("40 0f b6 c6 3c 98")


class BackendPatchError(RuntimeError):
    pass


def elf_file_offset(data: bytes, virtual_address: int, size: int = 1) -> int:
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise BackendPatchError("input backend is not a little-endian ELF64 executable")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    phoff, phentsize, phnum = header[5], header[9], header[10]
    for index in range(phnum):
        offset = phoff + index * phentsize
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", data, offset
        )
        if p_type != 1 or not (p_vaddr <= virtual_address < p_vaddr + p_memsz):
            continue
        file_offset = p_offset + virtual_address - p_vaddr
        if file_offset < 0 or file_offset + size > p_offset + p_filesz:
            raise BackendPatchError(f"backend address 0x{virtual_address:X} is not file-backed")
        return file_offset
    raise BackendPatchError(f"backend address 0x{virtual_address:X} is not in a load segment")


def assemble_hook() -> bytes:
    source = Path(__file__).with_name("input_hook.S")
    if not source.is_file():
        raise BackendPatchError(f"input hook source is missing: {source}")
    with tempfile.TemporaryDirectory(prefix="openmsx-hook-") as temp:
        temp_path = Path(temp)
        object_file = temp_path / "input_hook.o"
        linked_file = temp_path / "input_hook.elf"
        binary_file = temp_path / "input_hook.bin"
        commands = [
            ["as", "--64", "-o", str(object_file), str(source)],
            ["ld", "-nostdlib", "-e", "openmsx_input_hook", f"-Ttext={HOOK_VADDR:#x}", "-o", str(linked_file), str(object_file)],
            ["objcopy", "-O", "binary", "--only-section=.text", str(linked_file), str(binary_file)],
        ]
        try:
            for command in commands:
                subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except (OSError, subprocess.CalledProcessError) as exc:
            detail = getattr(exc, "stderr", b"").decode(errors="replace").strip()
            raise BackendPatchError(f"cannot assemble input hook{': ' + detail if detail else ''}") from exc
        hook = binary_file.read_bytes()
    if not hook or len(hook) > 0x1200:
        raise BackendPatchError(f"input hook has invalid size: {len(hook)} bytes")
    return hook


def patch_backend(source: Path, destination: Path) -> Path:
    try:
        original = source.read_bytes()
    except OSError as exc:
        raise BackendPatchError(f"cannot read backend {source}: {exc}") from exc

    entry_offset = elf_file_offset(original, INPORT_VADDR, len(ORIGINAL_INPORT))
    hook = assemble_hook()
    hook_offset = elf_file_offset(original, HOOK_VADDR, len(hook))
    current = original[entry_offset : entry_offset + len(ORIGINAL_INPORT)]
    relative = HOOK_VADDR - (INPORT_VADDR + 5)
    patch_jump = b"\xE9" + struct.pack("<i", relative)
    if current[:5] == patch_jump and original[hook_offset : hook_offset + len(hook)] == hook:
        # The source itself was already patched.  Keep the operation idempotent.
        if source.resolve() != destination.resolve():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            destination.chmod(source.stat().st_mode & 0o777)
        return destination
    if current[:5] not in (ORIGINAL_INPORT[:5], patch_jump):
        raise BackendPatchError(
            "unsupported headless backend: the expected openMSX 21.0 inPort layout was not found"
        )
    if current[:5] == ORIGINAL_INPORT[:5] and any(original[hook_offset : hook_offset + len(hook)]):
        raise BackendPatchError(f"backend hook area at 0x{HOOK_VADDR:X} is not unused padding")

    patched = bytearray(original)
    patched[entry_offset : entry_offset + 5] = patch_jump
    patched[hook_offset : hook_offset + len(hook)] = hook
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
    try:
        temporary.write_bytes(patched)
        temporary.chmod(source.stat().st_mode & 0o777)
        os.replace(temporary, destination)
    except OSError as exc:
        raise BackendPatchError(f"cannot write patched backend {destination}: {exc}") from exc
    finally:
        try:
            temporary.unlink()
        except OSError:
            pass
    return destination


def ensure_input_backend(source: Path, destination: Path) -> Path:
    """Return an input-capable backend, creating the cache copy if required."""

    if source.resolve() == destination.resolve():
        return source
    if destination.is_file() and os.access(destination, os.X_OK):
        try:
            hook = assemble_hook()
            data = destination.read_bytes()
            entry_offset = elf_file_offset(data, INPORT_VADDR, 2)
            hook_offset = elf_file_offset(data, HOOK_VADDR, len(hook))
            relative = HOOK_VADDR - (INPORT_VADDR + 5)
            patch_jump = b"\xE9" + struct.pack("<i", relative)
            if data[entry_offset : entry_offset + 5] == patch_jump and data[hook_offset : hook_offset + len(hook)] == hook:
                return destination
        except (OSError, BackendPatchError):
            pass
    return patch_backend(source, destination)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args(argv)
    try:
        print(ensure_input_backend(args.source, args.destination))
    except BackendPatchError as exc:
        print(f"openmsx backend patch: {exc}", file=os.sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(os.sys.argv[1:]))
