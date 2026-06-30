# CD32X outstanding-issues fix plan

Status snapshot (as of this writing):
- The CD32X port **boots → title → menu → Battle Mode → name entry → pre-duel
  dialogue → deck editor (card faces render) → 3D duel board (field + cards
  render)**. The original "stuck at CARD BACK / off-white" battle-load blocker is
  fixed and in the tree (per-card face LRU + supervisor `load_file` serving).
- The tree is at a **clean, building baseline** — all white-flash experiments
  have been reverted. `make -f Makefile.cd32x all` succeeds.

**Progress (2026-06-30):**
- [x] Debug auto-enter-battle shortcut: `CD32X_DEBUG_AUTOBATTLE` (build with
  `make -f Makefile.cd32x EXTRA_CFLAGS=-DCD32X_DEBUG_AUTOBATTLE all`). Throwaway.
- [x] **Issue 1 (white flash)** — title resident in CPU asset arena +
  `is_hardware()`→0 routes title/menu through software full-redraw; verified
  adjacent title frames byte-identical and adjacent in-battle frames equally
  populated (no stale page).
- [x] **Issue 2 (CD-DA stops on read)** — supervisor tracks active track/loop and
  re-asserts CD-DA after each card/blob read. (Code-complete; audio not
  headless-verifiable.)
- [x] **Issue 5 (responsive HUD)** — status bar full width, LP panel right-edge
  anchored, hand re-centered via `WAIFU_UI_EXTRA_W`/`WAIFU_UI_CENTER_DX` (both 0
  at 256, so PC-FX/256x240 unchanged).
- [ ] **Issue 3 / 4 (big-art + portraits)** — still need an offset-aware
  supervisor read (load-whole-file trick can't be used: CARD_BIG_ART is ~903 KiB
  > Word RAM). Not yet implemented.

Open issues reported by the user (priority order roughly as listed):
1. **White-screen flash every ~1 frame**, title through entire game. *(this doc's
   main focus)*
2. **CD-DA music stops abruptly** when a duel starts (card data is read).
3. **Card-check screen shows no cards.**
4. **Story-mode 2D portraits don't load properly.**
5. **HUD/status bar/hand hardcoded for 256x240** — must become responsive for
   320x240 (full-width status bar, centered hand + text) without regressing
   256x240 or costing PC-FX any performance.

A throwaway **debug auto-enter-battle** shortcut (compile-time) should be added
first to cut headless iteration from ~8 min to ~3 min, and removed before
finalizing. (Each code change shifts boot timing, so fixed-frame headless
captures are not directly comparable; prefer driving to a known UI state, or use
the shortcut, and capture adjacent frames N / N+1 to detect flashing.)

---

## Issue 1 — White-screen flash (root cause)

Confirmed against `CD32X/blastem-src/32x_video.c`:

- The 32X has two physical framebuffer pages. The **CPU can only access the
  *back* (non-displayed) page** at `0x24000000` (`MARS_FRAMEBUFFER`);
  `0x24020000` (`MARS_OVERWRITE_IMG`) is the *overwrite window of that same back
  page*, **not** a second independently addressable buffer. The displayed
  (front) page is never CPU-writable.
- `FBCR` (`MARS_VDP_FBCTL`, `0x2000410A`) bit `FS` selects which physical page is
  displayed; writing it requests a swap that takes effect at vblank.
  `cd32x_wait_fb_flip()` (in `waifu_cd32x_video.c`) **flips every frame** and
  also serves as the vblank sync. Flipping is mandatory — locking `FS` (no flip)
  shows the永远-back page → black screen (verified).
- The common game + the CD32X video backend assume a **single persistent
  framebuffer**: `waifu_fm_framebuffer()` returns the fixed `0x24000000` back
  window, `present_8bpp()` is effectively a no-op (src == dst), and the renderer
  draws straight into the back page. The code deliberately avoids a CPU shadow
  framebuffer to save 75 KiB of SH-2 SDRAM (see `waifu_cd32x_memory.h` notes).
- Because the back page is a *different physical page each frame* (the flip
  swaps it), content that is **not fully redrawn every frame** ends up in only
  one physical page; the other page shows stale/black → the per-frame flash.

Worst offenders:
- **Title screen**: its 320x240 pixels are streamed from CD (`TITLE_SCREEN_IMG.BIN`)
  directly into the framebuffer. Only the palette is compiled in
  (`src/generated/cd32x_title_asset.h` → `cd32x_title_screen_palette_rgb`);
  `waifu_assets_title_screen_img()` returns **NULL** on CD32X. The title is
  composed **once** on entry through the "hardware text overlay" optimization
  (`waifu_platform_text_overlay_is_hardware()` returns 1 on CD32X, so
  `draw_title_full_event()` runs only on `g_i_frame == 0`). Worse,
  `draw_title_full_event()` calls `clear_screen()`, which would wipe the
  direct-uploaded art if re-run. So the art can only ever be in one page.
- **Menu / other "compose-once on the hardware overlay" screens** have the same
  shape (composed on entry / on change → lands in one page).
- **In-battle** is fully redrawn every frame (3D caches are disabled on CD32X via
  `WAIFU_BG_CACHE_DISABLE` / `WAIFU_BATTLE_BASE_CACHE_DISABLE`), so it likely
  double-buffers cleanly already — **verify** (capture adjacent in-battle frames).

Fixes already ruled out (do not retry):
- Single-buffer / no flip → black (flip mandatory).
- Mirroring `present` to `0x24020000` → all black (overwrite window of same page).
- Committing the title to both pages via a flip-trick → still flashes (game
  re-clears the back page each frame).
- Composing the title on two consecutive frames → killed the art
  (`clear_screen` wipes the direct-uploaded art; no CPU source to redraw).
- A full CPU shadow framebuffer (the clean general fix) → **does not fit** SH-2
  SDRAM (256 KiB: ~149 KiB code + asset staging + story portraits leave < 75 KiB).

### Recommended fix (Issue 1)

Make CD32X **full-redraw each frame** for the overlay-composed screens (so both
flipped pages always receive a complete frame), backed by keeping their
backgrounds **resident in CPU RAM** (the asset arena is idle on the title/menu
screens). Concretely:

1. **Keep the CD32X title resident in CPU RAM.**
   - `src/game/assets.c`: on CD32X, stage the title into a resident CPU buffer
     instead of (or in addition to) streaming it straight to the framebuffer.
     Options:
     - Reuse the asset arena (`waifu_cd32x_asset_arena()`), which is unused on
       the title/menu screens, OR add a dedicated `CD32X_TITLE_SCREEN_BYTES`
       (76 800) resident region that is *released/overlapped* before battle card
       staging (the title is not needed during a duel).
     - Make `waifu_assets_title_screen_img()` return that resident buffer on
       CD32X (currently `return NULL;` at `assets.c:~823`).
     - `stage_title_ptr()` (CD32X branch, `assets.c:192`) currently returns the
       video title upload buffer; point it at the resident CPU buffer if the
       title is loaded there.
   - Keep the title load path (`waifu_assets_load_step`, `WAIFU_ASSET_REQUEST_TITLE`
     case) reading `TITLE_SCREEN_IMG.BIN` into the resident CPU buffer for CD32X.

2. **Disable the compose-once optimization on CD32X** so the title/menu redraw
   every frame:
   - `src/platform/cd32x/waifu_cd32x_video.c:408` —
     `waifu_platform_text_overlay_is_hardware()` should return **0** for CD32X
     (only). This routes `main.c` title/menu states into their `else`
     (software/full-redraw) branches: `clear_screen` + `draw_title_background` +
     `draw_title_logo` + `draw_title_prompt` / `draw_menu_screen` every frame.
   - Verify every state guarded by `waifu_platform_text_overlay_is_hardware()`
     in `src/main.c` has a working `else` (full-redraw) branch on CD32X. Grep:
     `grep -n waifu_platform_text_overlay_is_hardware src/main.c`
     (title `~10550`, menu `~10581`, plus transition helpers
     `transition_draw_menu_source` `~3955`, and any story-map/plaza composes).
   - Once `is_hardware()==0`, `waifu_platform_text_overlay()` /
     `_clear()` (video.c `~387`) become no-ops for CD32X (text is drawn by the
     software path); make sure they don't double-draw or fight the framebuffer.

3. **Make `draw_title_background()` work on CD32X.**
   - `src/main.c:3887` `draw_title_background()` blits
     `waifu_assets_title_screen_img()` via `draw_card_raw(..., TITLE_SCREEN_W,
     TITLE_SCREEN_H, 0,0, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT)`. On CD32X the title is
     `CD32X_TITLE_SCREEN_W/H` (320x240) — make the source dimensions use the
     CD32X title size (e.g. `WAIFU_TITLE_ASSET_W/H` already defined in assets.c,
     or a `waifu_assets_title_screen_dims()` accessor) so a 320-wide source is
     not misread as 256-wide. (320==`WAIFU_FM_WIDTH`, so no scaling needed; just
     a straight copy.)
   - The title palette path already exists (`palette.c` uses
     `cd32x_title_screen_palette_rgb` under `WAIFU_FM_CD32X`).

4. **Keep `present_8bpp` as the back-page packer and keep the per-frame flip.**
   No change needed there once the renderer fully populates the back page each
   frame. Optionally delete the now-pointless `commit_title_upload` page-1 copy.

5. **Verify in-battle.** Capture adjacent in-battle frames (N, N+1). If they
   flash, the battle renderer is not fully clearing+redrawing the whole 320x240
   each frame — ensure `clear_screen()` + full draw runs each frame on CD32X
   (caches are already disabled), or extend the same full-redraw guarantee.

SDRAM note: keeping a 76 800-byte resident title in CPU RAM is only affordable if
it overlaps memory not used simultaneously (the title is not shown during a
duel). Confirm with the compile-time `WaifuCd32xAssetStageFits` check and a
runtime smoke test (the full card atlas previously overran SDRAM into the stack
and crashed — watch for the same).

### Files to modify (Issue 1)
- `src/platform/cd32x/waifu_cd32x_video.c` — `waifu_platform_text_overlay_is_hardware()`
  → 0; review `waifu_platform_text_overlay()/_clear()`; optionally simplify
  `commit_title_upload`.
- `src/game/assets.c` — title resident CPU buffer; `waifu_assets_title_screen_img()`
  returns it on CD32X; `stage_title_ptr()` / title load step; possibly a
  `waifu_assets_title_screen_dims()` accessor.
- `src/main.c` — `draw_title_background()` source dims for CD32X; confirm
  title/menu/transition `else` (full-redraw) branches are correct on CD32X.
- (no change expected) `src/platform/cd32x/waifu_cd32x_video.c`
  `cd32x_wait_fb_flip` / `present_8bpp` keep flipping + packing.

---

## Issue 2 — CD-DA stops when card data is read

Likely the same class as the documented PC-FX "data read stops CD-DA" bug. On
Sega CD, issuing a data read (the supervisor `load_file`/`read_cd` for card
faces/art) stops CD-DA playback.

Investigate / fix:
- `src/platform/cd32x/cd32x_boot_main.c` — the supervisor `cd32x_service_cd_request`
  / `cd32x_service_card_face_request` issue CD reads via the BIOS. After a data
  read completes, **resume CD-DA** (re-issue play/resume for the current track)
  if music was playing, or pause/resume around the read.
- `src/platform/cd32x/cd32x_cdda.s` (`cd32x_bios_cdda_play/stop/init`) and
  `waifu_cd32x_cdrom.c` (`waifu_cd32x_cdda_play/stop`) — add a "resume current
  track from current/last position" path, or keep a `g_current_cdda_track` and
  restart it after data reads.
- Coordinate with `cd32x_sh2_main.c` music gate (it already avoids re-issuing
  title CD-DA). The duel start changes music track anyway (battle theme) — ensure
  that switch actually starts and is not killed by the immediately-following card
  data reads. Sequence the first battle data load *before* starting battle CD-DA,
  or re-assert CD-DA after the initial card working-set load.
- Reference: memory note `pcfx-cdda-music-playback` (analogous PC-FX bug).

### Files: `cd32x_boot_main.c`, `cd32x_cdda.s`, `waifu_cd32x_cdrom.c`,
`cd32x_sh2_main.c`, possibly `src/game/sounds.c` (CD32X music hooks).

---

## Issue 3 — Card-check shows no cards

The card-check / preview path draws large card art (112x112). On CD32X big-art
prewarm is disabled and `CARD_BIG_ART` offset reads are unsupported by the
supervisor seam, so `waifu_assets_card_big_art(card_id)` returns NULL for most
cards → blank preview.

Investigate / fix:
- `src/game/assets.c` — `load_big_card_art_cached()` /
  `waifu_assets_card_big_art()` on CD32X: `cd_read_blob_slice(CARD_BIG_ART,
  off=card_id*CARD_BIG_ONE_BYTES, ...)` fails because `waifu_cd32x_cdrom.c`
  `read_blob_slice` rejects non-zero offsets for non-CARD_FACES blobs.
- `src/platform/cd32x/waifu_cd32x_cdrom.c` + `cd32x_boot_main.c` — add an
  offset-aware big-art read for CD32X, mirroring the card-face per-card path:
  serve `CARD_BIG_ART` by `load_file("CARD_BIG_ART.BIN")` then memcpy the
  requested card's 112x112 slice (or use `CARD_BIG_ART_CD.BIN`, the sector-padded
  per-card variant already generated and shipped), then CPY_TO_32X. The whole
  `CARD_BIG_ART.BIN` is ~903 KiB (won't fit Word RAM) — prefer
  `CARD_BIG_ART_CD.BIN` (sector-aligned per card) + a per-card private blob like
  the faces, OR read by LBA+offset slices.
- Re-enable a small big-art LRU on CD32X once offset reads work (currently
  `prewarm_list_add_card` is a CD32X no-op; `WAIFU_ASSET_BIG_CACHE_SLOTS=1`).

### Files: `src/game/assets.c`, `waifu_cd32x_cdrom.c`, `cd32x_boot_main.c`,
`Makefile.cd32x` (ensure `CARD_BIG_ART_CD.BIN` is laid out per-card if used).

---

## Issue 4 — Story-mode 2D portraits don't load

Portraits stream via `STORY_PORTRAITS.BIN` / `STORY_PORTRAIT_MASK.BIN` through
`read_blob_slice` with non-zero offsets and a CD sector stride
(`WAIFU_STORY_PORTRAIT_CD_STRIDE`). On CD32X the supervisor seam only supports
start-aligned whole reads + the card-face per-card path, so portrait slices fail.

Investigate / fix:
- `src/game/assets.c` — `load_requested_portrait_slot()` /
  `request_story_*` use `cd_read_blob_slice(STORY_PORTRAITS, off, plane_bytes)`.
- `src/platform/cd32x/waifu_cd32x_cdrom.c` + `cd32x_boot_main.c` — add an
  offset-aware portrait read (same approach as faces/big-art: `load_file` whole
  portrait file into Word RAM then memcpy the requested slice, or LBA+offset via
  `read_cd`-with-`load_file`-semantics). Mind the supervisor per-request transfer
  limit (~65535 words) — slice per portrait plane.
- Verify `WAIFU_STORY_PORTRAIT_CD_STRIDE` sector alignment matches what the
  supervisor reads.

### Files: `src/game/assets.c`, `waifu_cd32x_cdrom.c`, `cd32x_boot_main.c`.

---

## Issue 5 — Responsive HUD / status bar / hand for 320x240+

The battle HUD, status bar, and card-hand (and the hand's text description) are
positioned with constants tuned for 256x240. On 320x240 they are off-center and
waste the extra width.

Fix approach (drive layout off `WAIFU_FM_WIDTH`/`WAIFU_FM_HEIGHT`, not literal
256, gated so 256x240 and PC-FX are byte-for-byte unchanged and pay no cost):
- `src/main.c` — battle UI drawing (`step_battle_interactive` and the
  `draw_*` HUD/status/hand/description helpers). Replace hardcoded x-origins /
  widths (e.g. status bar `x`, panel widths, hand start x, centered-text x) with
  expressions derived from `WAIFU_FM_WIDTH` (e.g. center = `WAIFU_FM_WIDTH/2`,
  status bar spans `0..WAIFU_FM_WIDTH`). Where the existing literals already equal
  the 256-derived value, prefer `#if WAIFU_FM_WIDTH == 256` keep-old / `#else`
  responsive, OR pure arithmetic that reduces to the old values at 256 so there
  is provably no 256x240 regression.
- For >=320: status bar full width; hand + description block centered using the
  extra horizontal space.
- No PC-FX cost: PC-FX is 256x240, so the responsive arithmetic must reduce to
  the identical constants and not add per-pixel work. Keep changes to layout
  math only (compile-time constants where possible).

### Files: `src/main.c` (battle HUD/status/hand/description draw code), possibly
`src/engine/cfx_screen_config.h` if shared layout constants are introduced.

---

## Throwaway debug harness (do first, remove before finalizing)

- Add a compile-time `CD32X_DEBUG_AUTOBATTLE` (or similar) that, after
  `waifu_fm_init()` in `src/platform/cd32x/cd32x_sh2_main.c` (or in
  `waifu_fm_reset_interactive`), jumps straight into a battle (call the same
  entry the menu uses, e.g. `enter_battle_after_assets`-equivalent) so headless
  runs reach gameplay in ~11k frames instead of ~34k.
- Capture **adjacent frames (N and N+1)** to detect flashing; a single frame is
  not enough. Build + capture: `make -f Makefile.cd32x all`, then from `CD32X/`:
  `./blastem_headless -m 32xcd -b <frame> -p out.ppm ../waifucd32x.cue`
  (BIOS: `cdbios.bin`, `32X_{M,S,G}_BIOS.bin` must sit next to the binary).

---

## Testing gotcha
Every code change shifts boot timing, so the same `-b <frame>` lands on a
different game state between builds. Don't compare fixed frame numbers across
builds; instead drive to a known UI state (or use the auto-battle shortcut) and
compare **adjacent** frames within one build to detect the flash.
