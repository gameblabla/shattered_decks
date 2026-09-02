# openMSX project command layer

This directory turns the supplied openMSX 21.0 headless/capture executable
into a small ROM analysis and deterministic-input toolkit.  The original
binary remains the backend for the existing commands, so NEO-1.2 mapper,
V9938 planar capture, snapshots, and coverage generation are unchanged.  On
first use, the wrapper creates a cached input-enabled copy by applying
`input_hook.S` to the supplied executable's MSX PPI read path.

The accompanying source tarball is the upstream OpenMSX 21.0 tree plus the
NEO mapper patch; the capture ZIP contains the separate headless binary and
capture patch.  The source archive does not include that headless shell's
original build directory, so this command layer is kept in `openmsx/` and
uses the supplied executable as its backend.

Build the executable wrapper with:

```sh
make -C openmsx
```

Run the backend self-test and the NEO-1.2 large-ROM regression together with:

```sh
make -C openmsx selftest
# or only the mapper regression:
openmsx/openmsx neo-test
# or only the keyboard/joystick injection regression:
make -C openmsx input-test
```

The mapper test creates temporary 80 MiB NEO-8 and NEO-16 images, selects
segments `0x2001` and `0x1001`, and checks the marker read back by the selected
backend.  This catches the old 12-bit high-byte mask that incorrectly aliased
the bank to segment 1.

The wrapper locates the backend in this order:

1. `OPENMSX_BASE=/path/to/openmsx`
2. the MSXgl copy under `MSXgl-main/`
3. the executable in the supplied capture ZIP, unpacked into `openmsx/.cache/`

## Commands

Assembly is an ergonomic alias for the backend's disassembler/reassembler
round-trip format. Generate the annotated source first, then reassemble it:

```sh
openmsx/openmsx cov demo.rom demo.cov 1 --direct-cart
openmsx/openmsx disasm demo.rom demo.asm demo.cov
openmsx/openmsx asm demo.asm /tmp/demo.roundtrip.rom
```

`asm` accepts the source syntax emitted by this bundled backend; SDCC listing
files such as `src/msx2/out/*.asm` are compiler output, not this reassembler's
input language.

Coverage generation still uses the deterministic backend.  `summary` adds a
human-readable or JSON report without changing the `.cov` file:

```sh
openmsx/openmsx cov game.rom game.cov 600 --bios MSX2.rom --direct-cart
openmsx/openmsx cov summary game.cov --top 20
openmsx/openmsx cov summary game.cov --json
```

Input scripts accept the frame syntax used by BlastEm as well as a readable
form. Bare tokens are one-frame taps; `+TOKEN` and `-TOKEN` press and release
keys explicitly:

```text
120f:+RIGHT
180f:-RIGHT
240f:A
300f:+P
360f:-P
```

Check or convert a script:

```sh
openmsx/openmsx input check game.input --frames 600
openmsx/openmsx input compile game.input game.normalized --format normalized
openmsx/openmsx input compile game.input game.tcl --format openmsx --frames 600
openmsx/openmsx input compile game.input --format json
```

`input run` executes the schedule through the bundled deterministic backend.
The wrapper appends a private schedule trailer to a temporary ROM; the
backend's patched PPI path exposes the corresponding active-low MSX keyboard
row whenever the emulated program reads keyboard port `0xA9` (after selecting
the row on `0xAA`):

```sh
openmsx/openmsx input run game.rom game.input 600 \
  --bios MSX2.rom --direct-cart --screenshot final.png
```

The MSX2 port's action vocabulary maps `P`, `START`, and `A` to the space
key, `B` to escape, and direction tokens to the MSX cursor matrix.  Raw
matrix events are also supported with `MATRIX:<row>:<mask>`.  The same
direction and `A`/`B` tokens are exposed through both joystick ports.
The backend follows the MSX PSG connector selector (PSG register 15 bit 6),
so programs reading either joystick connector receive the scheduled state.

The patch is deliberately limited to the supplied OpenMSX 21.0 headless ELF
layout.  If `OPENMSX_BASE` points to a different binary, the wrapper rejects
it instead of claiming that input was injected.  The source archive does not
contain the original headless shell sources, so the small assembly hook and
its ELF patcher are the reproducible build seam for this bundled executable.
