---
name: pcfx-architecture
description: NEC PC-FX (V810) source architecture — KING/VDC/KRAM/RAINBOW video model, page-flip + dirty present, 16M title/ending, the 8bpp fade-to-black glitch class, palettes, CD-DA + ADPCM audio, and the BackupRAM save format. Use BEFORE changing any code the PC-FX build compiles, so changes land in the right file and respect the hardware rules that caused real shipped glitches. Pair with pcfx-build-verify to actually build and confirm.
---

# PC-FX source architecture

Use this before editing anything the `Makefile.pcfx` build compiles. It maps the
files, the KING/VDC hardware model, and the rules whose violation shipped real
glitches (sheared title, pink boot frame, one-lit-frame fade flash, silent
CD-DA, unloadable saves). It is a distilled index — AGENTS.md's "PC-FX" bullets
are the authority; when a detail here is thinner than you need, grep AGENTS.md
and the named source file.

## Layer map

```
src/main.c                       — game core (shared with all targets; PC-FX bits under #ifdef WAIFU_FM_PCFX)
src/game/, src/engine/           — platform-agnostic core (see headless-core skill)
src/engine/renderer3d.c
src/engine/renderer3d_pcfx.c     — PC-FX WHOLE 3D impl (policy: split TU, never ifdef'd headers)
src/platform/pcfx/
  pcfx_main.c                    — frontend: drives game_api.h loop, owns music/asset-ready gates
  waifu_pcfx_video.c  (~2700 ln) — KING/VDC/KRAM/RAINBOW presentation. The big one.
  waifu_pcfx_audio.c             — CD-DA music + KING ADPCM (SoundBox) SFX
  waifu_pcfx_cdrom.c             — CD reads: blob → CPU RAM or direct → KRAM (eris_cd_read_kram)
  pcfx_biosfs.c / _asm.s         — BIOS filesystem dispatcher for BackupRAM (save files)
  waifu_pcfx_input.c             — pad → WaifuFmInput
  fastking.s                     — inline KRAM writer (no jal/rts) used by the dirty-band uploader
src/generated/
  title_asset.h                  — title/ending palettes + 8bpp + PC-FX YUV422 blobs (gen_title_asset.py)
  pcfx_palette_assets.h          — VDC-overlay fade LUTs (gen_pcfx_palette_assets.py; reads title_asset.h)
  waifu_assets.h                 — cards/portraits/common palette (NEVER hand-edit any generated file)
```

Toolchain: V810 GCC at `/opt/v810-gcc` (`v810-gcc`), liberis (`eris_*`) for KING/VCE/CD.

## KING / VDC / KRAM / RAINBOW model (the mental model you must have)

- **KING** is the graphics engine; **KRAM** is its RAM. `BG0` is the game plane.
  - **8bpp mode** (`KING_BGMODE_8`): 256-color, colors come from KING's low VCE
    entries. This is the whole GAME (board, menus drawn to the CPU framebuffer).
  - **16M mode** (`KING_BGMODE_16M`): direct YUV422 (no palette). This is the
    TITLE and ENDING full-screen art, uploaded once from CD straight into KRAM.
- **VCE** palette is **Y8U4V4**, NOT RGB888. The title/ending 16M path and the
  8bpp palette both go through YUV; that is why `gen_title_asset.py` optimizes
  against a YUV→RGB565 round-trip, not raw RGB.
- **VDC** (dual, `VDC_CHIP_0/1`) is a tile layer composited IN FRONT of KING BG0
  (`eris_tetsu_set_priorities(7,0,6,…)`). It draws: menu/prompt text, and the
  **17-level ordered-dither black fade mask** over the 16M title (the title
  "fade" is a VDC mask, NOT a palette fade — it composites over the 16M layer).
- **RAINBOW** is a separate backdrop layer (page bit `0x1000`) used for story-map
  skies behind the transparent 3D scene. Requested via
  `waifu_pcfx_video_request_rainbow_backdrop`.
- **KRAM page aliasing (memorize):** the 8bpp page 0 and the 16M title page 0
  BOTH live at KRAM word offset 0. No byte value is "black" in both
  interpretations, so showing offset-0 across a mid-scan 16M↔8bpp switch flashes
  a stripe band / white edge / pink wash. Mitigations already in place: raise the
  VDC black mask BEFORE `eris_king_set_bg_mode(16M)`; `begin_8bpp` displays the
  SECOND 8bpp page (offset `PAGE_STRIDE_WORDS`, which aliases no 16M page). Do
  not "simplify" back to displaying page 0 across a mode switch.

## Video present rules (each fixed a shipped glitch — do not regress)

1. **Dirty present** (`WAIFU_PCFX_DIRTY_PRESENT`, default 1): each 8bpp present
   diffs the new framebuffer against `page_shadow[2]` and uploads only changed
   16px bands via `fastking.s`. The `page_shadow` mirrors must stay in lockstep
   with KRAM. A separate "direct" uploader that wrote KRAM without updating the
   shadow desynced them and caused comb garbage / card flicker — that path was
   removed. If you add a new blit, it MUST update the matching page_shadow band
   (or invalidate it) or it will desync on the next diff.
2. **Two pages, flipped for tear-free double buffering.** `front_page`/`back_page`
   track which is shown; the presenter uploads to the back page then flips.
   Content not re-uploaded to BOTH pages exists in one page only.
3. **8bpp fade-to-black "one lit frame before black" glitch (two parts, BOTH
   needed):** (a) `apply_black_dither_fade()` at fully-black (`visible<=0`) must
   also `clear_screen(IDX_BLACK)` — a palette-only black leaves the faded scene
   in the framebuffer for the next state to flash. (b) `draw_transition_black_hold_frame()`
   must KEEP the palette black (`apply_black_dither_fade(0)`) through the hold —
   the VCE palette write lands immediately but the cleared framebuffer only
   reaches screen on the next KRAM page flip; resetting the fade to full flashes
   the dimmed scene fully-lit for one frame on real hardware (pcfxemu presents
   atomically so it can't be seen there — you MUST reason about it, not just
   look). This covers deck-editor exits, story fire/plaza→deck, and menu fades.
4. **Boot/loading white flash:** `begin_8bpp` fills KRAM with the `IDX_BLACK`
   *index*, but the 256 VCE colour entries are not uploaded until the first
   `present_8bpp` (during asset load). `set_king_8bpp_video` only sets the palette
   BANK. At cold boot the VCE palette RAM powers up bright on hardware, so the
   black-index framebuffer flashes full-screen WHITE until that first present —
   right as the loading screen comes up. `begin_8bpp` therefore force-blackens all
   256 entries (`WAIFU_PCFX_NEUTRAL_BLACK`) after `set_king_8bpp_video`; keep it.
   pcfxemu clears VCE to black so it never shows this class — reason about the
   init ordering, do not expect a capture to reveal it.
5. **Sanctum/pyramid/save screens** render through the normal CPU framebuffer on
   top of `draw_story_sanctum_background()` (3D scene + RAINBOW stay visible
   behind the blue panels). Do NOT route them through the old VDC-only
   `waifu_pcfx_video_request_sanctum` overlay bypass — it made the scene vanish.
6. **RAINBOW ordering:** after a map RAINBOW request, do not let the VDC `NONE`
   background clear run in the same present, or the picture-mode `0x4000` bit is
   lost and the scene falls to black.
7. **The `Makefile.pcfx cd` target links THREE times** to make the baked LBA/CDDA
   table match the final image (enabling generated LBAs can shift the boot
   binary by a sector). Do not "optimize" it to a single pass.

## Palettes

- 8bpp gameplay uses the **common** palette (`waifu_fm_use_common_palette()`);
  title/menu use the **title** palette (`waifu_fm_use_title_palette()`); dialogue
  has its own. The active palette is chosen per state — grep `use_*_palette` at a
  state transition rather than assuming.
- The **VDC overlay palette is at base 256**, separate from KING's low 8bpp
  entries. Title/menu fade colors live there so they don't disturb the low 8bpp
  entries the in-game deck editor reuses.
- `apply_black_dither_fade()` on PC-FX records a palette fade LEVEL only (plus the
  clear-at-black rule above); the visible dither LUTs are in
  `pcfx_palette_assets.h` (regenerated from `title_asset.h`'s title palette).

## Audio (`waifu_pcfx_audio.c`)

- **Music = CD-DA.** Track numbers come from `cdda_tracks.h` (generated by
  pcfx-cdlink from the alphabetical `Music/*.wav` order). Base volume 56/63,
  SFX duck 38.
- **SFX = KING ADPCM (SoundBox)**, samples from `pcfx_sfx_adpcm.h`.
- Gates that prevent SCSI stalls / silence (keep them):
  - Do NOT start title CD-DA until `waifu_assets_title_ready()` — `pcfx_main.c`
    gates it and stops any stray BIOS/drive CD-DA before raising mixer volume.
  - Do NOT stop the active track during a VISIBLE fade — issuing the SCSI stop
    mid-fade stalls Battle Mode selection. Only suppress STARTING music while
    assets/fade aren't ready.
  - Data reads interact with CD-DA; the pump watches `waifu_pcfx_cd_read_seq()`.

## BackupRAM saves (`pcfx_biosfs.c` + save code in `src/main.c`)

- Two volumes: **internal** BackupRAM (`/SRAM`, `PCFX_BIOSFS_PATH_INTERNAL`) and
  **external** FX-BMP (`PCFX_BIOSFS_PATH_EXTERNAL`); file `.../WAIFCARD/SAVE.DAT`.
- Format is a **123-byte binary blob** (`save_build_blob`/`save_parse_blob`):
  magic bytes `'W' 'A' 'I' 'F'`, version `0x01`, packed story state, u16 checksum
  of bytes 0..120. Keep the magic as the plain char literal `'W'` — see the
  AGENTS.md note; a `W`→`WAIFU_FM_WIDTH` rename once turned it into a multi-char
  constant and every load silently failed (parser compared a u8 against a full
  int → always-true `!=`). Watch for `-Wcharacter-constant-too-long` /
  "comparison is always true" warnings on these lines.
- Load flow: `begin_story_load()` → if exactly one device has a save, direct
  `WAIFU_I_STORY_LOAD_TO_MAP`; if both, the `WAIFU_I_STORY_LOAD_DEVICE` picker; if
  neither, stay on the picker. `story_save_exists_device()` is cached per device.

## 3D (`renderer3d_pcfx.c`)

- PC-FX pyramid/volcano faces must NOT be culled by screen-space winding inside
  `draw_tri3d_pyramid_face`; normalize negative projected winding and leave
  occlusion to painter order, or the pyramid disappears.
- Renderer variation is capability flags in `renderer3d_port.h`, not inline
  target `#ifdef`s (shared policy with headless/CD32X).

## What NOT to do

- Do NOT hand-edit `src/generated/*` — regenerate via the pipeline (see
  headless-core skill; the title/ending/palette headers come from
  `gen_title_asset.py` + `gen_pcfx_palette_assets.py`).
- Do NOT display KRAM offset 0 across a 16M↔8bpp mode switch (aliasing flash).
- Do NOT add an 8bpp blit that skips the `page_shadow` update (dirty-present desync).
- Do NOT reset the palette fade to full during a black-hold frame (one-lit-frame flash).
- Do NOT stop title/active CD-DA during a visible fade (SCSI stall).
- Do NOT route sanctum/save screens through the VDC-only overlay bypass.
- Do NOT "fix" a PC-FX issue by editing CD32X (`src/platform/cd32x/`) or host code.
- Do NOT trust pcfxemu's atomic present to reveal one-frame flashes — reason about
  the KRAM-flip-vs-palette-write timing (see pcfx-build-verify's accurate-backend note).
