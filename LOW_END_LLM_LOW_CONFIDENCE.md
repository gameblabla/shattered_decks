# Low-Confidence Areas — LLM Gap Analysis

This file documents what I, as an LLM reading this codebase without executing it,
am not confident about. Use as a checklist when reviewing my edits.

**Each area below now maps to a skill in `.claude/skills/` that mitigates it — load
that skill BEFORE working in the area (see the "mitigating skill" column in the
summary table).** The skills turn "I can't reason about this" into a concrete
build/verify loop plus the hardware rules whose violation shipped real bugs. A
skill does not make the risk disappear: it tells you what to check and how to
confirm it empirically instead of guessing.

FM TOWNS work is covered by `fmtowns-build-verify` and `fmtowns-architecture`.
As of 2026-08-27 the owner has confirmed that the current game and its updated
CD-ROM access work on real FM TOWNS hardware; older blanket “never run on real
hardware” statements are stale. Keep uncertainty scoped to the exact subsystem
or scene that lacks evidence.

---

## 1. CD32X SH-2 image size limit (131072 bytes)

I cannot predict whether a given change will push the image past the BlastEm
staging limit. The AGENTS.md lists several regressions where debug overlays,
new features, or even changed CFLAGS silently crossed the line and produced
a black/hung build. I have no mental model of what each `#include`, `static`
array, or `__divdi3`-triggering division costs in compiled SH-2 bytes.

**Mitigation:** I always run `stat -c %s build/cd32x/sh2/waifucd32x.sh2.bin`
as a gate, but if the toolchain isn't set up, I can't verify.

---

## 2. PC-FX KRAM / page-flip / RAINBOW details

`waifu_pcfx_video.c` (2691 lines) is the largest platform file and I have not
read it in full. Specific unknowns:

- The interaction between `pcfx_king_set_bg0_page_inline`, `front_page`/`back_page`
  swap, and the RAINBOW `0x1000` page bit.
- How `WAIFU_PCFX_DIRTY_PRESENT` diff tracking actually works with `page_shadow`
  mirrors and KRAM byte-swap.
- Which exact `out.h` sequences produce the "CRAM dot" artifacts, and what
  vblank-hold patterns are already in place vs. missing.
- The RAINBOW transfer start line / block count constraints for avoiding
  corrupted macroblocks at quality 100.
- Why the third build/link pass in `Makefile.pcfx` is needed for LBA consistency.

I can write PC-FX code by analogy with the CD32X patterns and the doc notes,
but I cannot reason about whether a given 8bpp present change will flash,
corrupt, or desync on real hardware.

---

## 3. Inline assembly fragments

The project uses SH-2 assembly (`cd32x_md_iface.s`, `cd32x_files.s`,
`cd32x_crt0.s`, `cd32x_cdda.s`, `cd32x_md_font.s`, `cd32x_pcm.c`) and
inline asm in C files. I have very weak reasoning about:

- Whether `extu.b` is present on every loaded palette byte in the SH-2 asm
  span fill (the docs say `mov.b` sign-extends and missing `extu.b` causes
  crashes when indices >= 128).
- The exact register assignments in the mailbox protocol between M68K and SH-2
  (`MARS_SYS_COMM0..14`) — I can look them up but cannot mentally verify
  they match between `cd32x_md_iface.s`, `waifu_cd32x_cdrom.c`, and
  `cd32x_boot_main.c`.
- The Sega-CD BIOS trap vectors (`_BURAM`, `BRMSERCH`, `BRMWRITE`, `BRMREAD`,
  CDC entry points) and their exact argument/preserved-register conventions.
- Whether `bsr` vs `bsr.b` range matters for a given branch target.

---

## 4. Asset pipeline output (`src/generated/`)

The project has a Python-based asset pipeline (`tools/gen_*.py`) that produces
headers in `src/generated/` (card data, deck pools, sound assets, rainbow
backgrounds, CD32X title art, PC-FX YUV422 ending, etc.). I am told never to
edit these files by hand, but I also cannot regenerate them without running
`make assets` or the individual Python scripts. If a file is stale or if I need
to understand the exact struct layout (e.g. `WaifuCardData` fields, palette
entry ordering), I must read the generated file and cross-reference with the
generator script — I cannot trust that the on-disk file matches the source data.

Known specific risk: `tools/gen_sound_assets.py` must be re-run when the
`WaifuSoundEffect` enum or `sounds/*.wav` files change. If I add a new SFX
enum entry without regenerating, the build still compiles but the new slot
gets the wrong PCM data (array index mismatch). I need to remember to check
whether the generator was run.

---

## 5. The 40+ regression scripts

There are ~55 `.txt` scripts under `scripts/`. I haven't read any of them.
I don't know:

- Which frame ranges they use (so I don't know if my timing change would
  desync them).
- Which scenarios they actually cover vs. duplicate.
- How `story_save_duels_regression.sh` orchestrates the story-save flow.
- Which `--regression-*` flags exist and what each asserts.

When the docs say "covered by --regression-fusion-equip" or similar, I trust
the claim but cannot verify the coverage.

---

## 6. PC-FX vs. CD32X vs. host code paths under `#ifdef`

The shared `src/main.c` (14786 lines) has many `#ifdef WAIFU_FM_PCFX`,
`#ifdef WAIFU_FM_CD32X`, and `#ifdef WAIFU_FM_HEADLESS_TESTS` blocks.
I have not read most of main.c. When I edit a shared function, there is a
risk that:

- I add code under one `#ifdef` that changes a shared local variable's
  semantics.
- I remove what looks like dead code but is actually a console-specific
  workaround that isn't documented.
- I change a `#ifdef WAIFU_FM_PCFX` guard thinking it's PC-FX-only but
  miss a `#else` branch that CD32X falls through to.

The AGENTS.md caught one such case: `music_track_for_current_state` had a
`#ifdef WAIFU_FM_CD32X` that pulled the title/menu case group into the
`NONE` return by fall-through, making CD32X menus silent. I could easily
reintroduce a similar fall-through bug.

---

## 7. Palette management

The game uses multiple palettes (COMMON, DIALOGUE, story-specific skies,
PC-FX VDC overlay at base 256, CD32X CRAM with priority bit on entry 0).
Specific unknowns:

- Which palette is active at any given state transition — the code appears to
  call `waifu_fm_use_common_palette()` at various points but I don't have a
  complete state machine of when the palette is reloaded.
- The PC-FX VDC overlay palette offset (base 256) and how it interacts with
  KING's low 8bpp entries — I know the doc says title/menu fade colors can
  "degrade" story-mode deck editor colors if low entries change, but I don't
  know exactly which entries or why.
- The `tex_reserved_indices` mechanism in `tools/gen_assets.py` that reserves
  texture atlas colors in the dialogue palette — I understand the concept but
  cannot verify the generated output.

---

## 8. Music/SFX platform dispatch

The audio has three completely different backends:
- **Host**: software mixer `waifu_sound_mix_s16()` with streamed WAV music
  (44100 Hz resampling with fixed-point nearest-neighbour).
- **PC-FX**: CD-DA music + KING ADPCM SFX (SoundBox), with PSG fallback notes.
- **CD32X**: RF5C164 PCM SFX via COMM14 + streamed PCM music double-buffered
  in Sub-CPU PRG RAM.

I have not read any of the platform audio files thoroughly:
`src/platform/pcfx/waifu_pcfx_audio.c` (846 lines),
`src/platform/cd32x/cd32x_pcm.c` (593 lines),
`src/platform/cd32x/waifu_cd32x_audio.c` (207 lines),
`src/game/sounds.c` (617 lines). The doc describes many subtle constraints
(COMM0 busy-wait vs. COMM14 fire-and-forget, ADPCM KRAM base address,
CD-DA re-assert after data reads), but I can't trace the actual instruction
flow to verify a change is safe.

---

## 9. Framebuffer dirty tracking (PC-FX `WAIFU_PCFX_DIRTY_PRESENT`)

The PC-FX present path uses a clever diff-based partial upload: it compares
the current framebuffer against a `page_shadow` and uploads only changed
16px bands. The doc says the former "direct big-art optimization" was removed
because it desynced shadow from KRAM and caused comb garbage / card flicker.
I don't understand the shadow-mirror algorithm well enough to modify it
confidently. If I needed to add a new blit path (e.g. hardware-accelerated
tile blit), I might inadvertently reintroduce the same desync.

---

## 10. The `#ifdef WAIFU_FM_NO_HEADLESS_MAIN` / test hooks split

`src/main.c` is compiled into FOUR very different contexts:
1. Headless runner (`-DWAIFU_FM_HEADLESS_TESTS`, no `NO_HEADLESS_MAIN`)
2. SDL 1.2 frontend (no `NO_HEADLESS_MAIN`, no `HEADLESS_TESTS`)
3. CD32X SH-2 (`-DWAIFU_FM_NO_HEADLESS_MAIN`, no `HEADLESS_TESTS`)
4. PC-FX V810 (`-DWAIFU_FM_NO_HEADLESS_MAIN`, no `HEADLESS_TESTS`)

Functions used only by the test harness live under `#ifdef WAIFU_FM_HEADLESS_TESTS`.
The `main()` entry point and `load_command_file()` live under the opposite guard.
I need to be careful which guard I put new code behind. Adding new globals
for gameplay logic is fine (all builds need them), but adding a debug printf
behind the wrong guard could silently bloat the CD32X SH-2 image or fail to
link on PC-FX.

---

## Summary table

| Area | Confidence | Why | Mitigating skill |
|---|---|---|---|
| CD32X SH-2 image size budget | Low | Can't estimate compiled size | cd32x-build-verify (gate S), cd32x-improvements |
| PC-FX KRAM/page-flip internals | Very low | 2691 lines, not read, subtle hardware | pcfx-architecture, pcfx-build-verify |
| Inline assembly (SH-2/M68K) | Very low | Can't mentally execute asm | cd32x-architecture (asm invariants) |
| Asset pipeline correctness | Low | Can't regenerate, can't verify alignment | headless-core (regenerate-or-desync) |
| Regression script coverage | Low | Haven't read the 55 scripts | headless-core (regression-flag map), pcfx/cd32x-build-verify (script lists) |
| Shared main.c #ifdef maze | Medium | Doc helps but I haven't read all 15K lines | headless-core (portability rules), pcfx/cd32x-architecture |
| Palette state machine | Low | Many palettes, transitions, platform nuances | pcfx-architecture (palettes), cd32x-architecture (video rules) |
| Audio backend details | Very low | Three completely different stacks, not read | pcfx-architecture (audio), cd32x-architecture (audio) |
| PC-FX dirty present diff tracking | Very low | Desync bugs were subtle and shipped | pcfx-architecture (video present rules) |
| Four-build-context main.c split | Medium | Clear guards but easy to miss one | headless-core (portability rules) |

Skills added/extended for this gap analysis: **pcfx-architecture** and
**pcfx-build-verify** (new — PC-FX had zero skill coverage); **headless-core**
gained an asset-pipeline "regenerate-or-desync" section and a per-flag regression
map; **cd32x-architecture** gained an assembly-invariants section.
