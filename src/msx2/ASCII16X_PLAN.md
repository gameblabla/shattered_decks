# Optional ASCII16-X cartridges — implementation plan

Date: 2026-09-06. Status: implemented and verified in the MSX2 fork. The four
mapper/presentation build combinations now have separate output trees and ROM
names; the NEO-16 shipping binary remains byte-identical to the clean baseline.

The implementation uses ASCII16-X's fixed 0x4000–0x7FFF resident ROM area and
its mirrored 0x8000–0xBFFF page-2 window. This leaves page-0 RAM available for
the machine and avoids a startup copy of resident code. Scene, rules and boot
code are linked into page-2 segments, and all page-2 borrowing goes through the
fixed mapper wrappers. The proposed RAM-copy layout below remains the fallback
design if a future feature exhausts the fixed resident area.

## Deliverable and constraints

Add a build-time mapper choice, keeping NEO-16 as the default. Ship four
independent artifacts:

| Presentation | Existing mapper | Additional mapper | Default capacity |
|---|---|---|---|
| MSX2, SCREEN 8 | NEO-16 | ASCII16-X | 8 MiB each |
| MSX2+/turbo R, SCREEN 10 pictures and SCREEN 8 duel | NEO-16 | ASCII16-X | 8 MiB each |

Keep every asset, soundtrack, scene, gameplay feature and existing R800
enhancement. Do not downsize either cartridge to ordinary ASCII16's range.
Keep explicit `ROM_SIZE_KB=16384` builds possible as well; cartridge hardware
must have the requested capacity. Mapper choice and presentation choice must
remain independent. No runtime mapper auto-detection inside the game is needed.

This work stays in the MSX fork and its build/tool wrappers. It does not require
changes to shared gameplay, source artwork, image encoders or sound encoding.
Flash save support is a separate feature; existing password/disk behavior stays.

## Findings that determine the design

### Current repository

- `Makefile.msx2` and `project_config.js` default to 8192 KiB and select NEO-16
  unless `MAPPER=ascii16x` is supplied. The four shipping names are
  `waifu_msx2.rom`, `waifu_msx2p.rom`, `waifu_msx2_ascii16x.rom` and
  `waifu_msx2p_ascii16x.rom`.
- ASCII16-X keeps resident `_CODE`, `_HOME` and initializer bytes below 0x8000;
  its switched scene banks occupy segments 3, 4, 5, 6 and 7, with assets still
  starting at segment 8. The packer checks every linked area and bank-call
  contract before writing assets.
- The final ASCII16-X plus build uses 16,372 bytes in segment 0, 13,977 in the
  duel bank, 4,987 in the modal bank, 11,134 in the story bank and 591 in the
  boot bank. The 12-byte resident margin is intentional and build-enforced.
- The IM 2 vectors and handler remain in page-3 RAM. ASCII16-X uses an 11-byte
  crt0 data prefix; NEO-16 retains its 15-byte prefix. Both are checked against
  the link map before packaging.
- `msx2_bank.c` and `msx2_stream.c` now share logical mapper operations. ASCII16-X
  uses address-encoded single-byte writes and restores the caller's segment
  after nested scene, rules, asset and music-window operations.
- BIOS/disk/audio probes temporarily expose BIOS in page 0. `Msx2_CpuFast()`
  already enables R800/ROM mode and reinstalls IM 2 after CHGCPU. Older notes
  saying turbo R acceleration is out of scope are obsolete.
- The vendored SDK already implements `ROM_ASCII16X`: target configuration,
  signature, mapper access and mapped-ROM crt0. This saves mapper/toolchain
  bring-up work, but does not make the game's current memory layout compatible.

### Mapper contract

ASCII16-X has two independent 16 KiB windows: 0x4000 and 0x8000. Page 0
mirrors the latter; it is not an independent third bank. Capacity is up to
64 MiB in the specification; the published XL hardware design is 8 MiB.
Bank selection combines low eight data bits with four address bits:
`write8(0x6000 | (segment & 0x0F00), segment & 0xFF)` for page 1,
and `0x7000` in that expression for page 2. The recommended detection string
is `ASCII16X` at ROM offset 0x10. See the
[author's specification](https://www.grauw.nl/projects/ascii-x/ascii16-x/) and
[hardware project](https://github.com/grauw/ascii-x).

Consequently, replacing NEO's `ld (0x7000),hl` with the same instruction under
a different target is incorrect. Likewise, executing the old page-0 duel bank
while streaming an unrelated asset through page 2 cannot work with cartridge
mirroring. Both the register operation and the code layout must change.

## Recommended ASCII16-X memory layout

Use ordinary machine RAM in page 0 for a permanently resident code image,
copied once at startup. Relink the existing scene banks for page 2. This is an
implementation proposal; the first milestone must prove its placement budgets.

| CPU range | ASCII16-X use | ROM backing |
|---|---|---|
| 0x0000–0x3FFF | Fixed RAM code, constants and runtime helpers | Segment 1, copied at boot |
| 0x4000–0x7FFF | Fixed ROM: startup, critical resident routines | Segment 0 |
| 0x8000–0xBFFF | Switched duel/modal/story/boot code or asset/music data | Segments 2/3/4/5 and asset pool |
| 0xC000–0xFFFF | Existing data, stack, IM 2 vectors/ISR, BIOS work area | Machine RAM |

This preserves approximately 32 KiB of resident capacity and a 16 KiB code
window without repeated code copying. It requires writable RAM in page 0 in
addition to page 3; target a 64 KiB RAM MSX2 with 128 KiB VRAM, without requiring
a RAM mapper expansion. Startup must discover/select the actual RAM slot and
subslot and establish a distinct physical page when the RAM is mapper-backed.
Never infer that a RAM page is usable from the machine-generation byte alone.

The permanent RAM image is compiled for its execution addresses. It is **not**
the old 0x8000 resident image copied unchanged to 0x0000: absolute calls, data
references and library placement must be relinked. Reserve any needed low
vectors explicitly; do not lose SDCC/runtime RST entry points or silently reserve
space over the nearly full duel bank.

Keep the NEO layout behind its existing build choice. For ASCII16-X, reuse the
same module bodies with different placement wrappers/linker areas. The SDK's
ASCII bank numbering is different: its bank 0 is 0x4000 and bank 1 is 0x8000;
the current `_b0`/`_b2` filenames must not be reused blindly.

### Alternatives considered

- Copying duel/modal/story banks into page-0 RAM preserves more old addresses,
  but nested calls require copying the displaced bank back. Current battle
  effects cross the duel/modal boundary repeatedly. Do not make a full-bank
  copy the default cost of those calls.
- Caching those banks in extra mapper RAM can avoid copies, but adds a RAM
  expansion requirement. Keep it as a later optimization, not a dependency.
- Reducing permanent code to 16 KiB and moving everything else to page 2
  avoids page-0 RAM, but requires a much larger repartition of the existing
  almost-full 32 KiB resident set. Use only if the recommended boot/layout
  proof reveals a concrete compatibility problem.

## Implementation sequence

### 1. Freeze baselines and prove the layout

The four shipping links are now isolated by mapper, presentation, ROM size and
test mode. The ASCII16-X build was checked through real openMSX while it booted,
streamed high assets, entered the duel bank and delivered V-blank audio ticks.
The resident placement, mapper shadow restoration and asset ranges are checked
by `tools/msx2/pack_msx_rom.py`; no startup RAM copy was needed after the fixed
resident image was reduced to fit 0x4000–0x7FFF.

The clean pre-task NEO-16 MSX2 ROM and the final NEO-16 MSX2 ROM compare equal.
NEO-16 remains on its original layout and MSX2+ SDK configuration; ASCII16-X
uses the additional scene banks and its address-encoded page-2 mapper path.

### 2. Add independent, isolated build selections

The delivered interface is:

```sh
make -f Makefile.msx2                         # existing NEO MSX2 default
make -f Makefile.msx2 plus                    # existing NEO plus default
make -f Makefile.msx2 MAPPER=ascii16x          # ASCII16-X MSX2
make -f Makefile.msx2 plus MAPPER=ascii16x     # ASCII16-X plus/turbo R
make -f Makefile.msx2 MAPPER=ascii16x ROM_SIZE_KB=16384
```

Validate mapper values; select `ROM_NEO16` or `ROM_ASCII16X` in
`project_config.js` and pass an explicit game-side mapper define. Both use
16-bit segment identifiers. Preserve the configured ROM capacity in `plus`
instead of forcing 8192 in its recursive make invocation.

Give each combination of mapper, presentation and shipping/test mode its own
objects, libraries, map and ROM. Include ROM size, seed and fixture in rebuild
identity as well. The build directories now carry the mapper, presentation,
mode, ROM size, seed and fixture in their names, so combinations cannot erase
or reuse another's products.
Keep existing public NEO filenames; use clearly suffixed ASCII16-X filenames.

### 3. Implement startup and linker placement

Use the SDK's ASCII16-X target support with project-local mapper, bank wrappers
and packer checks under `src/msx2/` and `tools/msx2/`. The implementation keeps
the ISR in page-3 RAM and leaves the fixed resident image in segment 0, so it
does not depend on the SDK's `RAMISR_SEGMENT0` convenience mode or on a page-0
RAM code loader.

Initialize mapper registers and both bytes of every shadow before use; do not
assume reset mappings or zeroed RAM. Check initializer sources, `_HOME`, SDCC
helpers, constants and library sections, not just `_CODE`. The target-specific
11-byte ASCII16-X and 15-byte NEO-16 crt0 data prefixes are checked in
`msx2_main.c` and `pack_msx_rom.py`, while crt0 state remains intact during the
static RAM clear.

### 4. Unify mapper operations and make window borrowing safe

Add a small mapper backend (`msx2_mapper.h` plus C/assembly as appropriate)
with separate NEO and ASCII16-X implementations. Expose the logical operations
the game needs rather than exporting SDK bank-number meanings to callers.
Use single-byte address-encoded ASCII writes; reject out-of-range segment
numbers instead of truncating them. Keep efficient inline assembly for the
streaming paths and document register clobbers.

For ASCII16-X, the old `Bank0Enter/Leave` scene trampolines select page-2 code.
Every nested call saves the previous segment and restores it before returning.
Put the full switch/call/restore path in fixed code. Both endpoints and their
constants must remain available until the displaced caller is restored.

Convert **all** direct writes in `msx2_stream.c`, including chunk copies,
rectangle rows and `Msx2_RomRead`. Replace hard-coded restoration of segment 1
with the caller's actual mapped code segment. The recursive seam-splitting
read must retain correct state at every return. Do not use one shared scratch
save slot for mainline and ISR borrowing.

Update hardware and shadows atomically under DI; honor/document interrupt
state at each entry point. Retain the short interrupt opportunities between
256-byte bulk-copy pages. Audio must save and restore whichever scene or
asset segment was interrupted. Its entire instruction path, including SDCC
helpers and tables, must be fixed while the music segment is mapped.

Audit pointers handed from scene banks to resident routines: page-2 constants,
strings and tables disappear during asset/music mapping. Fetch required fields
before switching or stage the small record in RAM. Similarly, do not retain a
function pointer to a banked routine without its segment identity.

### 5. Preserve BIOS, disk and turbo R behavior

Audit `msx2_disk.c`, `msx2_audio_probe.c`, entropy probing and
`Msx2_CpuFast()` together. The current primary-slot-only BIOS restoration
depends on never changing the BIOS subslot state; mapping RAM into page 0 can
invalidate that assumption. Save/restore the actual slot and subslot mappings,
using safe slot-switch routines without temporarily removing the page-3 stack.

Keep BIOS gates and everything they execute outside displaced page-0 RAM.
Account for CALSLT temporarily replacing page 1 as well: execute the interslot
transition through BIOS and restore the cartridge before returning to it.
Preserve the page-2 scene identity across disk and probe operations.

CHGCPU can re-enable interrupts and reset interrupt mode. Its transition must
have valid interrupt handling while BIOS occupies page 0, then restore RAM
mapping, I/IM 2 and the appropriate window before normal gameplay resumes.
Retain `g_msx2_r800` and the existing finer camera rendering/nine-pose turns;
verify actual R800 execution, not merely a turbo R model name.

### 6. Packaging, checks and emulator commands

`pack_msx_rom.py` and `msx2.sh ram` validate each mapper's actual
areas: fixed ROM, permanent RAM image, every switched code bank, data/stack
headroom and image capacity. Check runtime addresses **and** ROM load offsets,
including image-copy length and overlap with vectors. Make missing required
map symbols or placement inputs fatal rather than silently skipping checks.

Retain existing boot-bank/ISR helper checks and adapt them to the new placement.
Validate the complete call/data dependencies of the switch-critical routines;
an address below 0x8000 alone no longer proves residency during a BIOS call.
Check segment reservations and explicit asset interval overlaps, including
zero-filled assets, plus correct header/signature and exact padded ROM length.
Compare manifest entries and asset payload hashes against the matching NEO
presentation build: mapper adaptation must not delete or degrade content.

Every run/trace/shot/verify path in `msx2.sh` selects the matching ROM,
map, machine and explicit `ASCII16-X` romtype. Update `bench_floor.py` and
any other hard-coded NEO launchers. Keep `neo-test` and add a separate
ASCII16-X regression instead of claiming that the NEO test covers both.
Check the installed emulator's advertised mapper support before running.
[openMSX lists ASCII16-X support](https://openmsx.org/features.html).

## Verification and acceptance

The implementation evidence currently recorded is:

- Four independently named 8 MiB shipping ROM outputs build and pass mapper,
  placement, asset and exact-size checks.
- The NEO-16 MSX2 ROM compares byte-for-byte with the clean pre-task build.
- ASCII16-X MSX2 reaches the title and duel in real openMSX. ASCII16-X plus
  reaches the title, streams SCREEN 10 artwork and enters the duel on
  `C-BIOS_MSX2+` with `-romtype ASCII16-X`.
- The ASCII16-X plus fixed segment ends at 0x7FF3, and all switched code banks
  remain below 0xC000. The packer rejects any future boundary regression.

The longer hardware and longevity matrix below remains the release checklist.

1. **Mapper probe:** distinguish banks 0, 1, 255, 256 and 511 in an 8 MiB
   test image, including 255→256 crossings and the last byte of the image.
   Exercise banks 512 and 1023 in a 16 MiB test image. Check restoration after
   nested code/data/music switches and page-0 RAM independence from page 2.
   Include cold boot, reset, primary and expanded cartridge slots. Test the
   detection signature as well as an explicitly selected mapper.
2. **Build matrix:** both mappers × both presentations × shipping, soak,
   story-soak and regression. The status file records an existing regression
   variant size failure; reproduce it first and resolve placement if still
   present rather than dropping this verification mode. Also check explicit
   larger-size builds and variant switching without cleaning other products.
3. **Behavior matrix:** MSX2 ROM on MSX2; plus ROM on MSX2+ and FS-A1GT
   turbo R; baseline ROM on turbo R as an additional compatibility check.
   Cover PSG, OPLL and MSX-AUDIO, high-bank music boundaries/loops, input during
   streaming, and interrupted/nested bank returns.
4. **Observable scenes:** capture title/menu, narration, both portrait speaker
   states, road, editor, card check, populated duel, camera turns, attack/fusion
   cut-ins, rewards and ending. Compare settled VRAM/palette output with the
   matching NEO build; compare state at logical milestones with fixed seeds,
   not at identical elapsed times. Check YJK/YAE output and SCREEN 8 returns.
5. **Persistence and longevity:** password round-trip, disk absence/failure and
   real emulated DSKIO read/write on a scratch disk; completed 300-second duel
   soak and full story-soak for each mapper/presentation. Record probe checksums,
   bank state, stack margin, audio errors and completion results.
6. **Performance:** compare scene entry, portrait redraw, battle effects,
   camera cadence and audio tick delivery on the same machine profiles.
   Any persistent per-frame code copying or music stalls fail the proposed
   architecture. Establish tolerances from the recorded NEO baseline rather
   than inventing a speed claim before measuring.
7. **Hardware:** verify full-size cartridges on MSX2 and MSX2+/turbo R,
   including high-bank assets/audio and R800 switching. Record cartridge
   capacity and mapper implementation. Emulator results remain emulator
   evidence until those physical checks are done. Use real openMSX for all
   compiled-C tests; never the SDK's broken bundled headless Z80.

## Completion criteria and main risks

The first reviewable implementation milestone is a working layout probe and
four successful shipping links with unchanged asset content and 8 MiB output.
The release milestone is four separately named, verified ROMs with documented
commands and evidence, plus larger-image boundary coverage.

The main risks are the tight fixed-code budget, page-2 caller pointers during
temporary mapping, and BIOS/R800 slot transitions after introducing page-0 RAM.
Resolve those in the layout probe and banking work before long gameplay runs.
ROM capacity is not the limiting factor. No content cuts are part of this plan.
