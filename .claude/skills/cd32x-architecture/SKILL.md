# CD32X source architecture

Use this skill before changing any code that the Mega CD32X build compiles. It maps the source layout, the platform seams, and the hardware split so changes land in the right translation unit.

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
  sdl12_main.c, host_*.c       — SDL 1.2 / headless host glue
  pcfx/                        — PC-FX port (not in scope for CD32X work)
  cd32x/                       — everything CD32X-specific (below)
src/generated/                 — build-generated asset headers (cd32x_title_asset.h, cd32x_music_pcm.h, cd32x_sfx_pcm.h, deck_pools.h, …)
```

## CD32X hardware split (src/platform/cd32x/)

Two CPUs cooperate; keep responsibilities on the right side:

**M68K resident supervisor** (Mega-CD side — owns CD & audio BIOS):
- `cd32x_md_crt0.s`, `cd32x_boot_main.c` — startup + service loop: uploads SH-2 program to 32X SDRAM, then serves controller/tick, CD-ROM, CD-DA, PCM commands forever.
- `cd32x_cdda.s` — Sega-CD BIOS CD-DA calls (SH-2 never calls the BIOS directly). Supervisor re-asserts CD-DA after each data read (data reads stop CD-DA otherwise).
- `cd32x_cdfh.c/.h` — supervisor-side CDFS (from RaycastDemo), serves `load_file` reads into Word RAM.
- `cd32x_pcm.c/.h` — RF5C164 PCM: wave-RAM upload, 8 channels (4 used for SFX one-shots); in-game PCM music is an open-loop GET_TICKS pump driven from Sub-CPU INT2 (closed-loop play-pointer was a regression).
- `cd32x_md_iface.s/.h`, `cd32x_md_runtime.c`, `cd32x_md_font.s` — MD-side runtime + the 32X↔MD command mailbox (MARS_SYS_COMM*).

**SH-2 game program** (32X side):
- `cd32x_crt0.s`, `mars-cd.ld`, `cd32x_sh2_main.c` — entry point driving the common `game_api.h` loop.
- `waifu_cd32x_video.c/.h` — 320x240 8bpp 32X VDP. Two physical framebuffer pages; CPU writes only the back page (0x24000000); `cd32x_wait_fb_flip()` flips every frame → **anything not redrawn every frame flashes** (the historical white-flash class of bugs). Palette rule: entry 0's MD-priority bit (0x8000) must be part of its single write — never write entry 0 bare; MD CRAM writes must be vblank-held.
- `waifu_cd32x_audio.c/.h` — maps generic music API to CD-DA tracks (track 2 title … 8 fail; see docs/cd32x/CDDA.md) + PCM SFX bridge.
- `waifu_cd32x_cdrom.c/.h` — SH-2 side of the CD seam: request → supervisor reads to Word RAM → `CPY_TO_32X`.
- `waifu_cd32x_storage.c` — story saves via Sub-CPU `_BURAM` BIOS to internal BRAM.
- `waifu_cd32x_memory.h` — SDRAM budget map: 256 KiB total ≈ 149 KiB code + **224 KiB-budget uncached asset staging arena** (selected in `src/game/assets.c` under `WAIFU_FM_CD32X`); deliberately NO CPU shadow framebuffer (would cost 75 KiB).
- `waifu_cd32x_input.c/.h`, `waifu_cd32x_runtime.c`, `cd32x_files.s/.h`, `cd32x_32x.h`.

## Feature switches (Makefile.cd32x only — never leak into other builds)

`WAIFU_FM_CD32X`, `WAIFU_FLOOR_SAMPLE_CACHE_DISABLE` (no 512 KiB floor cache in SH-2 .bss), `WAIFU_BG_CACHE_DISABLE`, `WAIFU_BATTLE_BASE_CACHE_DISABLE`, `WAIFU_ASSET_USE_CDROM` / `WAIFU_ASSET_USE_CART_ROM`, `WAIFU_ASSET_RAM_BUDGET`, `WAIFU_ASSET_NO_STDIO`. Debug: `CD32X_DEBUG_AUTOBATTLE`, `CD32X_DEBUG_BOOT_SCENE`, `WAIFU_CD32X_DEBUG_FPS`.

## Rules of thumb

- Common code (`src/main.c`, `src/game`, `src/engine`) stays platform-agnostic; CD32X specifics go behind `src/engine/platform.h` hooks or into `src/platform/cd32x/` — narrow `WAIFU_FM_CD32X` switches only when unavoidable.
- Renderer changes: shared logic in `renderer3d.c`, SH-2 span/policy choices in `renderer3d_cd32x.c`.
- UI must stay resolution-responsive: use `WAIFU_UI_EXTRA_W` / `WAIFU_UI_CENTER_DX` (both 0 at 256 wide) rather than hardcoding 320 or 256.
- Anything touching CD reads must preserve the CD-DA re-assert behavior, and big files (e.g. `CARD_BIG_ART_CD.BIN`, ~903 KiB) can NOT be loaded whole — Word RAM is 256 KiB; use offset-aware/per-record reads (per-card face LRU is the existing pattern).
