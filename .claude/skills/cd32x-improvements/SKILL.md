# CD32X improvement roadmap & constraints

Use this skill when planning performance, memory, or feature work on the CD32X build. It records what is DONE, what measured as a dead-end, and which levers remain — so effort isn't wasted re-deriving them. Treat the dead-end list as binding: an idea listed there needs new evidence before it may be retried.

## Decision procedure (follow in order)

1. Is the idea in "Already implemented"? → do nothing, it's done.
2. Is it in "Measured dead-ends"? → reject it; explain why to the user; propose a remaining lever instead.
3. Does it add SH-2 code/const data? → estimate staged-image bytes first (limit 131072, headroom ≈ 1–8 KiB depending on tree state; check `stat -c %s build/cd32x/sh2/waifucd32x.sh2.bin`).
4. Does it add a RAM buffer? → name the region it lives in (SDRAM arena / Word RAM / wave RAM / BSS) and the byte count BEFORE writing code.
5. Measure before/after with the vblank overlay (below). No measurement → no perf claim.

## Hard constraints (design around these first)

- **SH-2 staged image < 131072 bytes** (BlastEm staging limit). Oversized image hangs at "Uploading SH2 app...". BSS is free — prefer BSS + runtime init over const tables.
- **32X SDRAM 256 KiB**: ~150 KiB code + asset arena (`WAIFU_ASSET_RAM_BUDGET=196608`); no room for a ~75 KiB shadow framebuffer today, none for the 512 KiB floor cache ever.
- **Word RAM 256 KiB** (Sub-CPU side): CD read staging and (planned) PCM music ring; big blobs (`CARD_BIG_ART_CD.BIN` ~903 KiB) must be read offset-aware, never whole.
- Framebuffer DRAM bandwidth is **shared by both SH-2s**; measured frame cost is **store-bound**, not compute-bound.

## Already implemented — do not redo (docs/cd32x/CD32X_RENDER_PERF.md status header; perf commit 4df3183)

- Floor: reciprocal texel stepping + packed halfword stores; per-frame opaque-UI floor occluder rect (`story_scene_set_floor_occluder`).
- Sky/stripes: 32X VDP auto-fill rows (`waifu_cd32x_video_fill_rows_index`, Master-only) — zero SH-2 stores; story skies are MD VDP plane-B gradients/starfield synthesized at runtime by the Sub-CPU (§7.6 done).
- Portraits: pre-clipped color-key blit with halfword pair packing (`draw_masked_bitmap`).
- Dual-SH-2 story-quad rasterization in disjoint scanline bands (`cfx_renderer3d_draw_quad_board_band`, `CD32X_BOARD_JOB_RENDER_QUAD_BAND`) — painter order preserved per band.
- Back-face culling (axis-plane tests for pillar walls; `story_face_visible` cross/dot for pyramid/volcano — cross points INTO the solid, visible = dot < 0).
- Image-size levers already spent: `___divdi3` (620 B) → `lldiv_trunc` (**must keep its 32-bit fast path** — the plain bit-loop regressed void scene 6→11 VB via line_i clipping), const LUTs (`cfx_recip_*`) built into BSS at `cfx_renderer3d_init`, dedup of per-TU `static` arrays in headers (`n2DLib_font`).
- White-flash class fixed: title resident in CPU asset arena + software full-redraw route; palette entry-0 priority-bit rule; vblank-held MD CRAM writes; CD-DA re-assert after data reads; per-card face LRU battle loading; BRAM story saves.
- Measured baseline: temple map 9 → 7 vblanks/frame after culling+banding; void map 6.

## Measured dead-ends — DO NOT retry without new evidence

| Idea | Why it's dead |
|---|---|
| More compute optimization of span fills (asm floor, etc.) | fills are already asm; cost is stores on contended framebuffer DRAM |
| Renderer caches (floor/bg/battle-base) on CD32X | ~1%-class wins even on PC-FX; here they don't fit in SDRAM at all |
| Closed-loop PCM play-pointer pacing | shipped regression; keep the open-loop GET_TICKS pump from Sub-CPU INT2 |
| Locking the framebuffer page (no flip) | CPU sees only the back page → permanent black screen (verified) |
| Compressing card art | barely compresses; a 112x112 card is ~93 ms at 1x — latency is seek, not transfer |
| Plain bit-loop 64-bit divide | regressed void scene 6→11 vblanks (line_i clipping) |
| "Compose once" screens on the hardware overlay | two-page flip hardware → single-page content flashes |

## Remaining levers (priority order)

1. **§7.1 SDRAM shadow framebuffer + DMA present** — the next big FPS win (removes contended-DRAM stores) and unlocks **§7.4 dirty rects**. Blocked on SDRAM: needs ~75 KiB → first shrink the asset arena budget or stream more from CD.
2. **§7.5 / §7.7** in `CD32X_RENDER_PERF.md` — still open.
3. **PCM music streaming + on-demand card art** (`CD32X_STREAMING_AUDIO_AND_LZ4W.md`; verdict: feasible at conservative 1x drive). Frees the CD-DA↔data-read contention: music ring in Word RAM + RF5C164 wave RAM (64 KiB, off both budgets); 2-slot ~24.5 KiB SDRAM card-art cache (attacker+defender pair, no double buffer); card art uncompressed; LZ4W reserved for big 2D assets (title/ending/portraits) staged in Word RAM then `CPY_TO_32X`.
4. **Offset-aware supervisor read** for big-art + story portraits (`CD32X_FIXES_PLAN.md` issues 3/4 remnant) — prerequisite shared with (3); the load-whole-file trick cannot work (~903 KiB > Word RAM).
5. Known cosmetic bug: volcano cone faces indistinguishable from the floor (only wire outlines show) — confirmed NOT caused by culling/batching (`CD32X_STORY_PERF_OFF` A/B).

## Measurement protocol (mandatory for perf claims)

1. `make -f Makefile.cd32x clean-build`
2. Build with `EXTRA_CFLAGS="-DWAIFU_CD32X_DEBUG_FPS -DCD32X_DEBUG_BOOT_SCENE=N"` (re-add the boot-scene hack ad hoc; N: 0 desert / 2 temple / 3 volcano / 4 void).
3. Capture with `CD32X_BIOS_START_END=3000` (see cd32x-build-verify skill) and read vblanks/frame off the overlay in the PNG.
4. Compare vblanks/frame per scene, never wall-clock or frame numbers.
5. Repeat for at least temple (worst case) and void (best case) before claiming a win.
6. Remove the debug defines and `clean-build` before committing.

## What NOT to do

- Do NOT trade PC-FX or headless behavior for CD32X wins — CD32X-only switches live in `Makefile.cd32x`.
- Do NOT add `.data`/`.rodata` casually; every byte counts against the 131072 staging limit.
- Do NOT re-architect the two-CPU seam (SH-2 requests / M68K supervisor serves) — extend the mailbox command set instead.
- Do NOT claim a fix for flashing/palette/CD-DA issues without an adjacent-frame (N, N+1) capture comparison.
- Do NOT delete the "throwaway" debug hooks list — future sessions need them.
