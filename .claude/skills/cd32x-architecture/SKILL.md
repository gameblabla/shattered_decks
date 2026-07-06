# CD32X source architecture

Use this skill before changing any code that the Mega CD32X build compiles. It maps the source layout, the platform seams, the hardware split, and every build define — so changes land in the right translation unit with no guessing.

## Layer map

```
src/main.c                     — game core: story scenes, deck editor, battle flow, UI (see AGENTS.md line anchors)
src/game/                      — gameplay data: ai.c, assets.c (asset streaming/caches), deck.c, palette.c, sounds.c
src/engine/                    — platform-agnostic engine:
  platform.h                   — the seam: storage/assets/background/text-overlay/input hooks each platform implements
  cfx_screen_config.h          — 320x240 only when WAIFU_FM_CD32X; all other builds 256x240
  renderer3d.c (+_internal.h, _spans.inc)  — shared 3D renderer
  renderer3d_generic.c / _pcfx.c / _cd32x.c — per-platform WHOLE implementations (policy: split TUs, never ifdef'd headers)
src/platform/
  sdl12_main.c, host_*.c       — SDL 1.2 / headless host glue (NOT compiled for CD32X)
  pcfx/                        — PC-FX port (out of scope for CD32X work)
  cd32x/                       — everything CD32X-specific (below)
src/generated/                 — build-generated asset headers. NEVER edit by hand.
```

## CD32X hardware split (src/platform/cd32x/)

Two CPU programs cooperate. Put logic on the correct side; the SH-2 must NEVER call Sega-CD BIOS services directly.

**M68K resident supervisor** (Mega-CD side — owns CD & audio BIOS):
- `cd32x_md_crt0.s`, `cd32x_boot_main.c` — startup + service loop: uploads SH-2 program to 32X SDRAM, then serves controller/tick, CD-ROM, CD-DA, PCM commands forever.
- `cd32x_cdda.s` — Sega-CD BIOS CD-DA calls. Supervisor tracks active track/loop and re-asserts CD-DA after every data read (data reads silence CD-DA otherwise).
- `cd32x_cdfh.c/.h` — supervisor-side CDFS (from RaycastDemo); serves `load_file` reads into Word RAM.
- `cd32x_pcm.c/.h` — RF5C164 PCM: wave-RAM upload, 8 channels (4 used for SFX one-shots). In-game PCM music pump is OPEN-LOOP GET_TICKS driven from Sub-CPU INT2 — the closed-loop play-pointer variant was a measured regression; do not reintroduce it.
- `cd32x_md_iface.s/.h`, `cd32x_md_runtime.c`, `cd32x_md_font.s` — MD-side runtime + the 32X↔MD command mailbox (`MARS_SYS_COMM*` registers).

**SH-2 game program** (32X side):
- `cd32x_crt0.s`, `mars-cd.ld`, `cd32x_sh2_main.c` — entry point driving the common `game_api.h` loop.
- `waifu_cd32x_video.c/.h` — 320x240 8bpp 32X VDP presentation (rules below).
- `waifu_cd32x_audio.c/.h` — maps the generic music API to CD-DA tracks (track 2 title, 3 overworld/deck, 4 battle, 5 boss, 6 final boss, 7 victory, 8 fail — `docs/cd32x/CDDA.md`) + the PCM SFX bridge.
- `waifu_cd32x_cdrom.c/.h` — SH-2 side of the CD seam: request → supervisor reads to Word RAM → `CPY_TO_32X`.
- `waifu_cd32x_storage.c` — story saves via Sub-CPU `_BURAM` BIOS to internal Backup RAM.
- `waifu_cd32x_memory.h` — SDRAM budget map (see numbers below). Deliberately NO CPU shadow framebuffer (would cost ~75 KiB).
- `waifu_cd32x_input.c/.h`, `waifu_cd32x_runtime.c`, `cd32x_files.s/.h`, `cd32x_32x.h`.

## Memory budgets (memorize before adding ANY buffer)

| Region | Size | Used for |
|---|---|---|
| 32X SDRAM | 256 KiB | SH-2 code (~150 KiB) + asset arena (`WAIFU_ASSET_RAM_BUDGET=196608` budget cap) |
| Staged SH-2 image | < 131072 B hard limit | text+data only; BSS is free (not staged) |
| Word RAM (Sub-CPU) | 256 KiB | CD read staging; big blobs must be read offset-aware — `CARD_BIG_ART_CD.BIN` is ~903 KiB and can NEVER be loaded whole |
| RF5C164 wave RAM | 64 KiB | PCM samples — off both SDRAM and Word RAM budgets |
| Framebuffer DRAM | 2 pages | shared bandwidth between BOTH SH-2s; frame cost is store-bound |

## Video hardware rules (violating these caused real shipped bugs)

1. Two physical framebuffer pages. The CPU can write ONLY the back page (`0x24000000`); `0x24020000` is the overwrite window of that SAME page, not a second buffer. The front page is never CPU-writable.
2. `cd32x_wait_fb_flip()` flips every frame (it is also the vblank sync). Consequence: **any screen not fully redrawn every frame exists in only one page and flashes**. Never "compose once" into the framebuffer. Never lock the FS bit (→ permanent back page → black screen, verified).
3. Palette entry 0: the MD-priority bit `0x8000` MUST be part of entry 0's single write. Writing entry 0 bare then OR-ing the bit later gives a 1-frame near-white flash (entry 0 raw color is 246,241,239). Never touch entry 0 bare.
4. MD CRAM data-port writes during active display render as bright dot artifacts — all MD CRAM writes must be vblank-held (`set_palette` waits for the VDP vblank flag); SH-2 quantizes forwarded MD fades to 32 steps.
5. MD-vs-32X layer visibility is per-32X-CRAM-entry via bit 15 combined with `MARS_VDP_PRIO_32X`. The MD boot text planes must be CLEAR_A/CLEAR_B'd at handoff or they show through the 32X image.

## Defines reference

**Fixed by `Makefile.cd32x` (`SH2_CFLAGS_BASE`) — do not add/remove these in source; do not define them for other targets:**

| Define | Meaning |
|---|---|
| `__32X__`, `PLATFORM=7`, `BY16=1`, `HARDWARE_DIV=1` | toolchain/platform basics |
| `WAIFU_FM_CD32X` | master CD32X switch (gates 320x240, arena, seams) |
| `WAIFU_FM_NO_HEADLESS_MAIN` | exclude the host headless runner main |
| `WAIFU_ASSET_NO_STDIO` | asset loaders must not use stdio |
| `WAIFU_ASSET_BIG_CACHE_SLOTS=2` | 2-slot big-art cache (battle needs attacker+defender resident) |
| `WAIFU_ASSET_RAM_BUDGET=196608` | asset staging arena budget in bytes |
| `WAIFU_BG_CACHE_DISABLE` | no board-background full-frame cache |
| `WAIFU_BATTLE_BASE_CACHE_DISABLE` | no battle-base full-frame cache |
| `WAIFU_FLOOR_SAMPLE_CACHE_DISABLE` | no 512 KiB floor sample cache (direct per-pixel sampling instead) |
| `WAIFU_ASSET_USE_CDROM` / `WAIFU_ASSET_USE_CART_ROM` | asset source; chosen by `CD32X_ASSET_MODE` (default `cdrom`) — exactly one is defined |

**Asset-externalization switches (assets streamed from CD instead of compiled in):** `WAIFU_ASSET_EXTERNAL_TITLE_IMAGE`, `WAIFU_ASSET_EXTERNAL_ENDING_IMAGE`, `WAIFU_ASSET_EXTERNAL_STORY_PORTRAITS`, `WAIFU_ASSET_EXTERNAL_CARD_IMAGES`. On CD32X the title image is external — `waifu_assets_title_screen_img()` returns NULL; only the palette is compiled in (`src/generated/cd32x_title_asset.h`). Code must handle the NULL path. `WAIFU_STORY_PORTRAIT_CD_STRIDE` is the portrait record stride shared by `assets.c`, `waifu_cd32x_cdrom.c`, and the supervisor — keep all three in sync.

**Optional feature/visual toggles (in-tree, off unless stated):** `WAIFU_CD32X_BOARD_FLAT_TOP` (flat-shaded field top; default is fully textured — see comment at src/main.c ~line 94), `WAIFU_CD32X_FIELD_SIDE_WALLS`, `WAIFU_BOARD_FAST_AFFINE_ENABLE`, `WAIFU_SOUND_USE_STREAMED_MUSIC`, `WAIFU_PCFX_*` (PC-FX only — must never appear in a CD32X build).

**Debug (EXTRA_CFLAGS, throwaway):** `CD32X_DEBUG_AUTOBATTLE`, `CD32X_DEBUG_ENDING`, `CD32X_DEBUG_VOID`, `CD32X_DEBUG_BOOT_SCENE=N` (re-add ad hoc), `WAIFU_CD32X_DEBUG_FPS`, `WAIFU_PROFILE_RENDER`. See the cd32x-build-verify skill.

**Other-target defines you must NOT set for CD32X:** `WAIFU_FM_PCFX`, `WAIFU_FM_HEADLESS_TESTS`.

## What NOT to do

- Do NOT put `#ifdef WAIFU_FM_CD32X` blocks into shared code when a `platform.h` hook or a file in `src/platform/cd32x/` can carry the difference. Narrow switches are the last resort.
- Do NOT define feature switches in headers or source files — they belong in `Makefile.cd32x` only, so PC-FX/host builds stay byte-identical.
- Do NOT edit `src/generated/*` (asset pipeline output) or `src/platform/pcfx/*` / `host_*` for a CD32X fix.
- Do NOT hardcode 320 or 256 in UI code — use `WAIFU_UI_EXTRA_W` / `WAIFU_UI_CENTER_DX` (both 0 at 256-wide, so PC-FX is unaffected).
- Do NOT call Sega-CD BIOS from the SH-2; route through the supervisor mailbox.
- Do NOT load files larger than Word RAM in one read; use offset-aware/per-record reads (per-card face LRU is the existing pattern).
- Do NOT remove the CD-DA re-assert after data reads.
- Do NOT add renderer platform policy to `renderer3d.c` — SH-2-specific span/policy choices go in `renderer3d_cd32x.c`.
