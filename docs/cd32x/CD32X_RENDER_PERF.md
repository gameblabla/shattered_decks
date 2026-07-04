# CD32X rendering & performance analysis (why it runs at ~1 FPS)

> **Implementation status (2026-07-04).** The no-SDRAM-space levers below have
> been implemented:
>
> - **§7.3 floor**: `render_floor_row_range`'s direct-sample path no longer
>   calls `floor_sample_direct` (which did TWO 64-bit multiply+divides per
>   pixel); texels come from a per-call reciprocal (`tex_scale`) and are
>   emitted as packed halfword stores. The floor also skips pixels under one
>   opaque UI rect per frame (`story_scene_set_floor_occluder`; armed by the
>   map/plaza/sanctum composers, consumed by `draw_floor_tiled`).
> - **§7.6-lite sky**: full-width scanline bands go through the 32X VDP
>   auto-fill (`waifu_cd32x_video_fill_rows_index` / `fill_rows` in main.c) —
>   zero SH-2 stores for the sky, deck-editor/name-entry/fusion stripes.
> - **§7.3 portraits**: `draw_masked_bitmap` on CD32X is a pre-clipped
>   color-key blit that packs adjacent opaque texels into halfword stores.
> - **§7.2 extension**: story-scene quads (pillars, pyramid/volcano faces) are
>   collected into `Cd32xStoryQuad` batches and rasterized by BOTH SH-2s in
>   disjoint scanline bands (`cfx_renderer3d_draw_quad_board_band`,
>   `CD32X_BOARD_JOB_RENDER_QUAD_BAND`) — painter order preserved per band,
>   no write overlap.
> - **Back-face culling** (all platforms): temple pillar walls use exact
>   axis-plane camera-side tests; pyramid/volcano faces use the world-space
>   `story_face_visible` test (cross product points INTO the solid for the
>   face tables' winding). Back faces used to be fully textured then 100%
>   overdrawn.
> - **Image-size wins** (the 128 KiB BlastEm staging limit was 80 bytes from
>   full): libgcc `___divdi3` (620 B) replaced by `lldiv_trunc` (32-bit fast
>   path + rare bit-loop), the 2 KiB `cfx_recip_*` LUTs are built at
>   `cfx_renderer3d_init` into BSS, and `n2DLib_font` is one shared const
>   copy instead of one per including TU. Release image: 129,408 bytes.
>
> Measured on the temple sanctum map (debug `WAIFU_CD32X_DEBUG_FPS` overlay,
> BlastEm): **9 vblanks/frame before culling+banding, 7 after** (~6.7 → 8.6
> fps); the void scene measures 6 vblanks/frame. The remaining frame cost is
> store-bound on contended framebuffer DRAM (both CPUs share its bandwidth),
> so the next big lever is still **§7.1 (SDRAM shadow + DMA present)**, which
> also unlocks §7.4 dirty rects. §7.5/§7.7 remain open.
>
> **§7.6 is now fully implemented**: the story skies are Mega Drive VDP
> plane-B gradients/starfield behind the 32X bitmap
> (`CD32X_MD_CMD_SET_BG`, Sub-CPU compositor in `cd32x_boot_main.c`; tiles
> are synthesized at runtime, scene switches are palette-line-3 swaps).  The
> 32X marks CRAM entry 0 as MD-priority and the SH-2 only auto-fills the
> rows above the floor horizon to index 0 (`story_sky_clear_rows`), so the
> sky costs the SH-2s nothing beyond that partial clear.

This is an analysis-only document. It describes how the Sega CD 32X build draws a
frame with the two SH-2s, the exact code paths involved, the hardware limits that
make them slow, and what would have to change to get from ~1 FPS toward a
playable frame rate. No code is changed here.

Line references are `file:line` at the time of writing.

---

## 1. Executive summary

The single dominant cost is that **every pixel of every frame is written
directly into the 32X framebuffer DRAM, one halfword (often one byte) at a time,
from a 23 MHz SH-2**, with almost no use of caching, dirty rectangles, DMA, or
the second CPU outside of three narrow 3D sub-tasks.

- `src/main.c:198` — on CD32X the game's `framebuffer` pointer *is* the 32X
  framebuffer aperture:
  ```c
  static uint8_t *const framebuffer = (uint8_t *)WAIFU_CD32X_FRAMEBUFFER_PIXELS;
  ```
  `WAIFU_CD32X_FRAMEBUFFER_PIXELS = 0x24000000 + 0x200`
  (`src/platform/cd32x/waifu_cd32x_video.h:7-8`). There is **no SDRAM shadow
  framebuffer** — all 2D blits, text, card art, the 3D floor and the 3D quads
  store their pixels straight into 32X VDP DRAM.
- The frame is **fully re-rendered every frame**; the frame/scene caches are
  compiled out on CD32X (`Makefile.cd32x` passes `-DWAIFU_BG_CACHE_DISABLE
  -DWAIFU_BATTLE_BASE_CACHE_DISABLE -DWAIFU_FLOOR_SAMPLE_CACHE_DISABLE`).
- The **second SH-2 is idle for the large majority of every frame**. It only
  does work for three hot 3D pieces (story floor, battle board-top rows, battle
  X-walls); everything else — all 2D UI, card blits, hand, HUD, portraits, text,
  fades, the pyramid/pillar quads — runs on the master alone.

320×224 = **71,680 pixels**. Re-emitting all of them every frame into contended
32X DRAM from one 23 MHz core, plus a per-pixel perspective floor, is what pins
the frame time into the ~1 FPS range.

---

## 2. Per-frame control flow

Master SH-2 main loop — `src/platform/cd32x/cd32x_sh2_main.c:22-53`:

```c
for (;;) {
    waifu_cd32x_input_poll(input, &in);
    waifu_fm_step(&in);                       // <-- ALL game logic + ALL rendering
    waifu_cd32x_video_present_8bpp(...);      // palette upload; no pixel copy (see below)
    waifu_cd32x_video_wait_vblank(video);     // request FS flip, spin until it lands
    ... music pump ...
}
```

Everything expensive is inside `waifu_fm_step()`. `present_8bpp` is essentially
free on CD32X because rendering already happened in the framebuffer:
`waifu_cd32x_video_present_8bpp` returns immediately once it sees the source is
the framebuffer aperture itself (`waifu_cd32x_video.c:566`), only refreshing the
256-entry CRAM palette (`cd32x_rgb_to_cram`, `waifu_cd32x_video.c:503-505`).

Slave SH-2 — `cd32x_sh2_main.c:70-76`:

```c
void slave(void) {
    for (;;) { waifu_cd32x_slave_service(); __asm__ volatile ("nop"); }
}
```

The slave spins forever polling one uncached mailbox and only acts when the
master posts a job (§4).

---

## 3. The framebuffer target — the core bottleneck

### 3.1 Direct-to-VRAM, no shadow

The 32X has two physical framebuffer pages in its own DRAM. The SH-2 can only
touch the *back* page through the `0x24000000` aperture; `cd32x_wait_fb_flip`
flips `FS` every frame (`waifu_cd32x_video.c:158-165`). Because the game renders
directly into that aperture, **every store the renderer issues is an uncached
write into 32X DRAM that is simultaneously being scanned out by the 32X VDP for
display**. These accesses are arbitrated against the VDP's display fetch and are
much slower than SDRAM (`0x06000000`) or cache.

`docs/cd32x/CD32X_FIXES_PLAN.md` records that a real CPU shadow framebuffer was
rejected earlier for **memory** reasons ("~149 KiB code + asset staging + story
portraits leave < 75 KiB" of the 256 KiB SDRAM). That was a correctness/space
decision, but it is precisely why the renderer pays full VRAM-write latency on
every pixel: there is no fast scratch buffer to compose into.

### 3.2 Byte vs. halfword writes

The framebuffer is 16-bit wide. A **byte** store to it is the worst case (the
DRAM wants halfword granularity). The backend has packed-halfword helpers for
this reason:

- `copy_u8_fast` / `cd32x_copy_pairs_to_back` pack two 8-bit texels into one
  16-bit store (`waifu_cd32x_video.c:66-85`, `main.c:~1560` copy loop). The
  same-size card blit path uses it (`draw_card_raw`, `main.c:2709-2713`).
- Full-screen solid fills use the **32X VDP auto-fill** hardware, not the CPU
  (`cd32x_auto_fill_back_words` → `MARS_VDP_FILLEN/FILADR/FILDAT`,
  `waifu_cd32x_video.c:109-123`; reached via `clear_screen` →
  `waifu_cd32x_video_clear_back_index`, `main.c:1552`). This is good and cheap.

But several hot paths still emit **byte** stores straight to VRAM:

- **The 3D floor** (`render_floor_row_range`, `main.c:10934-10947`):
  ```c
  for (int x = 0; x < WAIFU_FM_WIDTH; ++x) {
      ...
      row[x] = src[(vz & 31) * tw + (ux & 31)];   // byte store to 0x2400xxxx
      px += dx; ...
  }
  ```
  Up to 320 byte writes per row for every row below the horizon (~130 rows), plus
  two sample lookups and two phase wraps per pixel. This is the single most
  expensive routine in the story scenes and is done with **byte** stores.
- **Scaled card art** generic fallback (`draw_card_raw`, `main.c:2723-2733`) and
  gray variant (`main.c:2742+`) write one byte per destination pixel with a
  per-pixel integer multiply/divide for the source coordinate. CD32X has a
  faster scaler (`cd32x_blit_scaled_fast`, `main.c:2718`) but the underlying
  stores are still into VRAM.
- `put_px` / rect fills done pixel-at-a-time in gameplay code (`main.c:1642`,
  `main.c:2731`, dither fade `main.c:4193`).

### 3.3 The 3D span rasterizer

The board/quad rasterizer is genuinely optimized: `renderer3d_spans.inc` has an
SH-2 asm inner loop that fetches four texels and emits two packed **halfword**
stores per iteration (`cfx_draw_scanline_fast_pair_sh1_nowrap`,
`renderer3d_spans.inc:98-160`; board tile fill `cfx_board_fill`
`renderer3d_spans.inc:900-1015`). These are as good as it gets for
CPU-driven texturing into VRAM — the remaining cost there is fundamental VRAM
write latency, not the code.

So the renderer is split in quality: the **triangle/board spans are packed
halfword asm**, but the **floor and much of the 2D UI are byte-at-a-time** and
all of it lands in slow VRAM.

---

## 4. How the two SH-2s are (under)used

Parallelism exists only through one uncached mailbox struct
(`Cd32xBoardJob`, `main.c:1233-1250`) accessed through the uncached SDRAM mirror
(`cd32x_board_job_uncached`, `main.c:1252-1259`). The master posts a job, does
its own half, then **spins** until the slave flips `command` to `DONE`:

- Story floor split — `cd32x_render_floor_parallel` (`main.c:1328-1354`):
  master draws the top band, slave the bottom band, master busy-waits
  (`while (job->command != CD32X_BOARD_JOB_DONE) {}`, `main.c:1350`).
- Battle board-top rows — `cd32x_render_board_top_parallel` (`main.c:1302-1322`).
- Battle side X-walls — `cd32x_render_board_sides_parallel` (`main.c:1261-1282`).

The slave dispatcher handles exactly those three jobs and nothing else
(`waifu_cd32x_slave_service`, `main.c:1356-1378`).

Consequences:

1. **The slave is idle for everything else in the frame**: hand cards, field
   card faces, HUD/status bar, LP panels, portraits, dialogue text, cut-in
   animations, fades, the pyramid and stone-pillar quads (drawn on the master
   *after* the floor completes), and the whole 2D menu/title/deck-editor. In a
   typical battle or story frame that "everything else" is most of the pixels,
   so the second CPU buys at best a ~2× speedup on a minority of the frame.
2. **Blocking hand-off**: the master cannot overlap its own next work with the
   slave — it spins. There is no job queue, no double buffering of jobs, and the
   slave cannot start the next frame's work early.
3. **Mailbox is uncached + polled**: both sides hammer the uncached SDRAM mirror
   in tight spin loops (`main.c:1277`, `main.c:1318`, `main.c:1350`, and the
   slave's `for(;;)` at `cd32x_sh2_main.c:72-75`), which also consumes bus
   bandwidth.

---

## 5. Hardware limits that bound this

- **Two Hitachi SH-2 @ ~23.01 MHz (NTSC).** No hardware triangle/blit unit for
  the framebuffer; the 32X VDP only offers auto-fill (solid) and the page flip.
  All texturing/scaling is software on the SH-2s.
- **Framebuffer is 32X DRAM, shared with display scan-out.** SH-2 writes to
  `0x24000000` are uncached and arbitrated against the VDP; effective write
  throughput is a small fraction of SDRAM. Byte writes are worse than halfword
  writes. The `0x24020000` "overwrite image" aperture only skips index-0 pixels
  on *display*, it is not a second CPU-writable page (see
  `waifu_cd32x_video.c:167-178` and `CD32X_FIXES_PLAN.md`).
- **256 KiB SH-2 SDRAM total** (and the linked image must stay ≤ 131072 bytes
  for the BlastEm loader — `waifu_cd32x_memory.h`, AGENTS.md). This is why a full
  SDRAM shadow framebuffer (~70 KiB per page) and large caches were dropped, and
  why the renderer composes directly in VRAM.
- **SH-2 cache is 4 KiB, 2-way.** It is enabled for the master (`cd32x_crt0.s`
  "purge cache, turn it on, and run main()" at :206), but the framebuffer region
  is not usefully cacheable for writes, and the texture LUTs / card art thrash a
  4 KiB cache quickly.
- **SH-2 has hardware DMA (2 channels)** — `SH2_DMA_*` at
  `cd32x_32x.h:92-106` — currently **unused for rendering**. There is a
  `fast_memcpy` in `cd32x_crt0.s` but the present path does not DMA anything
  because it renders in place.

---

## 6. Per-frame cost, where it goes

Rough ordering of cost in a **story** frame (worst case):

1. **Textured floor**, per-pixel, byte stores to VRAM, ~320×130 texels with two
   LUT lookups + two wraps each (`render_floor_row_range`). Halved by the
   dual-SH-2 split but still the top cost.
2. **Pyramid / stone-pillar quads** via the affine span rasterizer (master only,
   after the floor).
3. **Two story portraits** (~124×200 each) blitted with masking, plus dialogue
   panel + wrapped text — master only.
4. **Sky** — currently composed into the framebuffer as well (the brief calls
   for moving this to the MD VDP, which would remove it from the SH-2 entirely).

In a **battle** frame:

1. **Board**: side walls (split), camera-facing wall (master), board-top rows
   (split) — textured spans.
2. **Field card faces + hand + HUD + LP panels + status bar**, all 2D blits and
   text on the **master only**.
3. Cut-in / animation frames when active (big-art blits).

Full-screen work every frame regardless: the mandatory page flip wait.  (Note:
`apply_black_dither_fade` is NOT a per-pixel cost on CD32X — it applies fades
as palette intensity and returns before the dither walk; only host builds pay
the per-pixel dither.)

---

## 7. What must be done to improve performance (prioritized)

These are ordered by expected payoff vs. effort. The first two attack the core
bottleneck (VRAM write latency and single-CPU rendering); the rest are additive.

### 7.1 Compose into cached SDRAM, copy to VRAM once per frame (biggest win)

Render the frame into an **SDRAM back buffer** (cacheable `0x06000000` region)
and move it to the 32X framebuffer once per frame with **SH-2 DMA**
(`SH2_DMA_SAR0/DAR0/TCR0/CHCR0`, already declared) or a tight halfword copy.

- Writes during rendering become cached SDRAM writes (fast, no VDP contention)
  instead of uncached VRAM writes. This is the difference between "slow" and
  "very slow" for *every* pixel the CPU touches.
- The copy is one linear 35,840-halfword transfer that the DMA engine can do
  largely off the CPU, ideally during/after vblank.
- Cost: ~70 KiB SDRAM for one shadow page. The blocker historically was space
  (§5). Mitigations that make it fit: only shadow the region that actually
  changes; free the title/portrait staging arenas during battle; or shadow at
  the working set granularity (board area) rather than full screen. Even a
  **partial** SDRAM shadow for the hottest region (the floor band, or the board)
  would recover most of the loss because that is where the per-pixel writes are.

If a full shadow truly cannot fit, the fallback is 7.2 + making 100% of VRAM
writes halfword-packed (7.3).

### 7.2 Use the second SH-2 across the whole frame, not just 3 jobs

Today the slave helps only floor/board-top/X-walls and blocks the master. Move
to a model where the slave carries a real share of the frame:

- **Split the framebuffer by scanline band** and have each CPU own the top/bottom
  half of *all* rendering for its band (2D and 3D), not just the floor. The 2D
  UI (hand, HUD, portraits, cards) is currently 100% master.
- Replace the single blocking job with a **small job queue / double-buffered
  mailbox** so the master can keep composing while the slave drains work, instead
  of spinning (`main.c:1277/1318/1350`).
- Keep the slave busy during the long 2D/portrait phase — currently its most
  idle window.

Ceiling here is ~2×, but it applies to the *whole* frame instead of a slice.

### 7.3 Eliminate byte stores to VRAM

Convert the remaining byte-store hot paths to packed halfword writes (the board
spans already do this; the floor and scaled 2D blits do not):

- **Floor** (`render_floor_row_range`, `main.c:10934-10947`): emit two texels per
  halfword store (mirror `cfx_board_fill`'s SH-1 asm pair loop). This roughly
  halves the store count on the dominant routine.
- **Scaled/gray card blits** (`draw_card_raw` generic path, `main.c:2723-2733`):
  ensure the CD32X scaler always packs pairs and never falls back to per-pixel
  byte stores; precompute source-x per column to drop the per-pixel divide.
- Any `put_px`/rect fills in gameplay code that still hit VRAM one byte at a time.

### 7.4 Re-enable frame/region caching (the UI is mostly static)

The full-redraw-every-frame policy (`WAIFU_BG_CACHE_DISABLE`,
`WAIFU_BATTLE_BASE_CACHE_DISABLE`) exists because the page flips each frame and a
compose-once screen would land in only one page (see `CD32X_FIXES_PLAN.md`
Issue 1). With an SDRAM shadow (7.1) that constraint disappears: the shadow is a
single stable buffer, so **dirty-rectangle / cached backgrounds become legal
again**. Most battle/menu frames only change a few small regions (cursor, LP
numbers, a moving card); redrawing only those instead of all 71,680 pixels is a
large win independent of write speed. This is the highest-leverage change *after*
the shadow buffer exists.

### 7.5 Cut 3D per-pixel cost (floor is the target)

- **Cache the projected floor rows.** For a mostly-static camera the per-row
  world-space start/step and the texel pattern repeat; the floor sample cache was
  compiled out (`WAIFU_FLOOR_SAMPLE_CACHE_DISABLE`) for space, but a small
  per-row cache or a reduced vertical resolution (render every other floor row
  and duplicate) would cut the dominant cost.
- **Lower the floor's active band or texel rate** during camera motion; the
  brief already asks for simpler skies drawn on the MD VDP, which removes the sky
  from the SH-2 entirely.

### 7.6 Offload the background to the Mega Drive VDP (per the project brief)

The sanctum/story skies are currently composed into the SH-2 framebuffer. Drawing
them on the **MD VDP** layer behind the 32X image (as the brief requests) removes
those pixels from the SH-2 write budget completely, and lets the 32X clear only
the region it actually draws (the fast VDP auto-fill already exists,
`waifu_cd32x_video.c:109-123`).

### 7.7 Present via DMA / overlap with vblank

Once there is an SDRAM shadow, kick the SDRAM→VRAM DMA and let it run while the
CPUs start next-frame logic, rather than the current in-place render + spin-wait
flip (`cd32x_sh2_main.c:32-33`).

---

## 8. Summary table

| Lever | Where | Payoff | Effort/Risk |
|---|---|---|---|
| SDRAM shadow + DMA to VRAM | new; `SH2_DMA_*`, `cd32x_32x.h:92` | Very high (every pixel) | High (SDRAM space) |
| Full-frame dual-SH-2 split + job queue | `main.c:1233-1378` | High (~2× whole frame) | Medium |
| Kill byte stores to VRAM (floor, scaled blits) | `main.c:10934`, `main.c:2723` | High on floor/2D | Medium |
| Dirty-rect / cached UI (needs shadow) | `WAIFU_*_CACHE_DISABLE` | High on UI frames | Medium (depends on 7.1) |
| Floor row cache / reduced res | `render_floor_row_range` `main.c:10900` | Medium | Low/Medium |
| Sky on MD VDP + clear-only-needed | `waifu_platform_background_request` `main.c` / video backend | Medium | Medium |
| Present via DMA overlapped with vblank | `cd32x_sh2_main.c:32` | Medium | Medium |

The load-bearing change is **§7.1 (compose in SDRAM, DMA to VRAM)**: it directly
removes the per-pixel VRAM-write penalty that dominates the frame and it unlocks
§7.4 (dirty-rect UI). §7.2 and §7.3 are the best wins that do not require finding
SDRAM space first.
