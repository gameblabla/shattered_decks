# CD32X improvement roadmap & constraints

Use this skill when planning performance, memory, or feature work on the CD32X build. It records what is DONE, what measured as a dead-end, and which levers remain — so effort isn't wasted re-deriving them.

## Hard constraints (design around these first)

- **SH-2 staged image < 131072 bytes** (BlastEm staging limit). Currently ~129,408 — ~1.6 KiB free. Oversized image hangs at "Uploading SH2 app...".
- **32X SDRAM 256 KiB**: ~149 KiB code + 224 KiB-budget uncached asset arena; no room for a 75 KiB shadow framebuffer today, none for the 512 KiB floor cache ever.
- **Word RAM 256 KiB** (Sub-CPU side): staging for CD reads and (planned) PCM music ring; big blobs (CARD_BIG_ART_CD.BIN ~903 KiB) must be read offset-aware, never whole.
- Framebuffer DRAM bandwidth is **shared by both SH-2s** and the frame cost is **store-bound**, not compute-bound.

## Already implemented — do not redo (docs/cd32x/CD32X_RENDER_PERF.md status header)

- Floor: reciprocal texel stepping + packed halfword stores; per-frame opaque-UI floor occluder rect (`story_scene_set_floor_occluder`).
- Sky/stripes: 32X VDP auto-fill rows (`waifu_cd32x_video_fill_rows_index`) — zero SH-2 stores; story skies are MD VDP plane-B gradients (§7.6 done).
- Portraits: pre-clipped color-key blit with halfword pair packing (`draw_masked_bitmap`).
- Dual-SH-2 story-quad rasterization in disjoint scanline bands (`cfx_renderer3d_draw_quad_board_band`).
- Back-face culling (axis-plane tests for pillar walls; `story_face_visible` cross/dot for pyramid/volcano — cross points INTO the solid, visible = dot < 0).
- Image-size levers already spent: `___divdi3` (620 B) → `lldiv_trunc` (**must keep its 32-bit fast path** — plain bit-loop regressed void scene 6→11 VB), const LUTs rebuilt into BSS at init (BSS isn't staged), dedup per-TU `static` arrays in headers (n2DLib_font).
- Perf levers commit: 4df3183. Measured: temple 9→7 vblanks/frame; void 6.

## Measured dead-ends — don't retry

- More compute optimization of span fills (already asm; cost is stores on contended DRAM).
- Renderer caches on CD32X (~1% class wins on PC-FX; here they don't fit anyway).
- Closed-loop PCM play-pointer pacing (regression; keep the open-loop GET_TICKS pump from Sub-CPU INT2).
- Locking the framebuffer page (no flip) → permanent back page → black screen.

## Remaining levers (priority order)

1. **§7.1 SDRAM shadow framebuffer + DMA present** — the next big FPS win (removes contended-DRAM stores), also unlocks **§7.4 dirty rects**. Blocked on SDRAM: needs ~75 KiB → must first shrink the asset arena budget or stream more.
2. **§7.5 / §7.7** in CD32X_RENDER_PERF.md — still open.
3. **PCM music streaming + on-demand card art** (docs/cd32x/CD32X_STREAMING_AUDIO_AND_LZ4W.md — verdict: feasible at 1x drive). Frees CD-DA↔data-read contention: music ring in Word RAM + RF5C164 wave RAM (64 KiB, off both budgets); 2-slot ~24.5 KiB SDRAM card-art cache (attacker+defender, no double buffer); card art uncompressed; LZ4W only for big 2D assets (title/ending/portraits) staged in Word RAM.
4. **Offset-aware supervisor read** for big-art + story portraits (CD32X_FIXES_PLAN.md issues 3/4 remnant) — prerequisite shared with (3).
5. Known cosmetic bug: volcano cone faces indistinguishable from floor (only wire outlines show) — confirmed NOT caused by culling/batching (`CD32X_STORY_PERF_OFF` A/B).

## Working practices

- Measure before/after with `-DWAIFU_CD32X_DEBUG_FPS` vblank overlay + `CD32X_DEBUG_BOOT_SCENE` (see cd32x-build-verify skill); compare vblanks/frame per scene, not wall-clock.
- Any new `.data`/`.rodata`/code costs staged-image bytes; prefer BSS + runtime init.
- Verify no white-flash regressions by diffing adjacent captured frames N/N+1 (page-flip hardware shows single-page draws as flashes).
- Keep PC-FX and headless builds byte-identical in behavior: CD32X-only switches live in Makefile.cd32x.
