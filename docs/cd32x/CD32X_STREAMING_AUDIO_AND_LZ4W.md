# CD32X card streaming + PCM music (+ LZ4W for 2D assets) — feasibility study

**Question (user idea):** Free the CD drive from CD-DA during deck-editor / in-duel
so it can stream *card* data with no perceptible disruption, by (a) streaming music
as PCM instead of CD-DA, and (b) reading full-size card art on demand into a small
32X cache. Keep CD-DA for the rest of the game (title / map / results).

**Refined scope (per follow-up):**
- **Card art is NOT compressed** — it barely compresses on this data, and a single
  112x112 card is only ~93 ms of 1x transfer, so compression buys nothing here.
- **Compression (LZ4W) is reserved for the big 2D assets** — title screen, ending,
  2D story portraits — which *do* compress and can be staged in Sega-CD RAM, then
  pushed to the 32X. See "LZ4W for large 2D assets" below.
- The card cache on the 32X holds **two** full-size arts (battle animation needs the
  attacker+defender pair resident at once) and uses **no double buffer**.
- Target the conservative **1x** drive; the card-flip / face-down window is the
  latency cover.

**Verdict: yes, comfortably.** The card transfer is tiny; the only real cost is
seek latency, and the flip animation hides it. Nothing here needs hardware we don't
already drive.

---

## What we already have (confirmed in tree)

- **RF5C164 Sega-CD PCM chip is already working** for SFX
  (`src/platform/cd32x/cd32x_pcm.c`): wave-RAM upload (`pcm_cpy`/`pcm_load_samples`),
  per-channel start/loop/freq/env/pan, 8 channels, currently 4 used for SFX
  one-shots at `WAIFU_CD32X_SFX_PCM_FREQ_DELTA`.
- **Sub-CPU (Sega-CD 68000) owns the CD + audio** already: `cd32x_boot_main.c`
  does `init_cd` / `load_file` / RF5C164 SFX / CD-DA via `cd32x_cdda.s`, and the
  SH-2↔68000 bridge (`MARS_SYS_COMMx`) is the command path.
- **The m68k assembler path builds supervisor `.s` files**
  (`Makefile.cd32x`: `M68K_AS -m68000 --register-prefix-optional`,
  `CD32X_M68K_SRCS_S`). `x68000_playground/lz4w.s` is exactly this dialect
  (`.global lz4w_unpack`, mixed `a0`/`%sp`), so it assembles into the sub-CPU
  build essentially unchanged.
- **`lz4w.jar` compressor** runs under the installed **Java 17** → build-time
  compression is available.
- **The slave SH-2 is idle** (`cd32x_sh2_main.c: slave()` is a `nop` loop) — it is
  the natural home for a 32X PWM driver *if* we ever need the non-CD 32X target.

## The numbers

- **CD read rate is 1x**: **150 KB/s raw = 75 sectors/s** (2048 B/sector). This is
  the conservative target; do **not** assume 2x.
- **Seek time dominates, not transfer.** A small read's wall-clock cost is
  `seek + rotational latency + transfer`, and for card-sized reads the seek term is
  the largest and the most variable: roughly **~100–300 ms** for a short seek and
  up to **~1 s** worst-case full-stroke. Contiguous on-disc layout is therefore the
  single most important lever (keeps inter-read seeks short). Being off CD-DA in
  these screens also stops the head bouncing to an audio track, which shortens card
  seeks further.
- **LZ4W decompression** (large 2D assets only): 550–950 KB/s on a **7.67 MHz**
  68000 (SGDK benchmark). The Sega-CD sub-CPU is a **12.5 MHz** 68000 → expect
  **~0.9–1.5 MB/s**, far faster than the 150 KB/s the CD delivers, so decompression
  is never the bottleneck when staging title/ending/portraits.
- **Streamed music bandwidth is tiny**: 8-bit PCM at 11 kHz = 11 KB/s
  (~5.5 of 75 sectors/s), at ~19 kHz ≈ 19 KB/s. So music is a small slice of the
  1x budget. The binding constraint is **seek contention**, not bandwidth — solved
  by a Word-RAM read-ahead buffer (below), not by raw throughput.

## Card streaming — the concrete plan (1x, hidden behind the flip)

**Sizes (confirmed in tree):** one big card art is `112*112 = 12544 B` (256 colors).
`CARD_BIG_ART_CD.BIN` is already padded **per card to 7 sectors = 14336 B**,
sector-aligned, so a card is a **direct LBA read — no offset slicing, no
decompression**. The whole file is 72 cards * 14336 = ~1.03 MB.

**Per-card cost at 1x:** 7 sectors / 75 sectors/s = **~93 ms transfer**, plus the
seek. The card-flip / face-down dwell gives a natural **~300–800 ms** cover window,
so `seek + 93 ms` fits inside the flip in the normal case; only a pathological long
seek would risk it, and contiguous layout removes that.

**Two-card 32X cache (battle animation):** 2 * 12544 = **~24.5 KB** of 32X SDRAM —
trivial. The hard rule: the attacker+defender pair must be **prefetched into the two
slots *before* the battle animation starts** (during selection / lead-in). Never
stream during an animation — a mid-animation seek cannot be guaranteed. The 2-slot
cache exists precisely so the animation only ever reads resident data.

**No double buffer.** The cache slot is the *final* destination; it is filled while
the card is **face-down** and revealed only after the fill completes (flip finishes).
The display consumer never reads a half-filled slot, so there is no tearing and no
ping-pong staging — **one slot per card**. (This is independent of the framebuffer
page-flip; the card cache lives in SDRAM, not the framebuffer, so it costs nothing
toward the Issue-1 redraw budget.)

**Card-check flow:** start the LBA read the instant the card-back is shown; the
face-down dwell plus the rotation (face foreshortened/hidden) covers seek + 93 ms.
By the time the card faces the player, the slot is full.

**Coexisting with PCM music — the key trick:** do **not** sector-stream music
concurrently with card reads (one head can't seek to the audio region and the card
region at once). Instead keep a **multi-second music read-ahead buffer in Sega-CD
Word RAM**: during a card burst the CD is dedicated to the card while music plays
from the buffer with **zero CD access**; top the buffer back up afterward. Bandwidth
is a non-issue (music ~5.5 of 75 sectors/s) — the read-ahead buffer is what removes
the *seek* contention. Buffer depth must cover `seek-to-card + 93 ms + seek-back`
plus margin; ~1 s is ample and fits Word RAM easily.

**Worst-case seek removal:** lay out the current duel's working-set cards
**contiguously** on disc so inter-card seeks stay short, covering even the
no-flip-cover cases (a surprise reveal lands in the ~100–200 ms range).

This per-card direct-LBA read into a small resident cache is also exactly the
offset-aware read Issues 3 (card-check art) and 4 (portraits) were missing.

## LZ4W for large 2D assets (title / ending / portraits)

Cards skip compression, but the big 2D fullscreen assets *do* compress and are not
latency-critical (loaded on a screen transition, not behind a flip). For those:
stage the compressed blob into Sega-CD RAM, `lz4w_unpack` on the sub-CPU 68000
(~0.9–1.5 MB/s), then `CPY_TO_32X`. This gives them a faster-than-1x *effective*
load and keeps their raw size off the disc. Keep raw variants for PC-FX/host.

## Recommended architecture (CD32X)

Keep *all* audio + CD on the **sub-CPU**, mirroring today's design. The SH-2 SDRAM
(256 KB, ~149 KB code + 224 KB-budget asset arena) pays **zero** new cost — the
music ring buffer lives in **Word RAM** (sub-CPU, 256 KB) and **PCM wave RAM**
(64 KB, off both SDRAM and Word RAM).

```
Sub-CPU (68000, 12.5MHz)             Master SH-2                 RF5C164 PCM
─────────────────────────           ───────────                ───────────
read-ahead music ──► WordRAM ring ─────────────► wave RAM refill ──► music ch (1-2)
card LBA read (raw) ─► WordRAM ─► CPY_TO_32X ─► 2-slot SDRAM cache    SFX ch (2-4)
2D assets (lz4w) ──► WordRAM ─► unpack ─► CPY_TO_32X (on transitions)
CD is dedicated to one job at a time; the music ring read-ahead covers card bursts
```

- **Music = RF5C164 streaming**, not CD-DA, during deck-editor / in-duel. Split a
  channel's wave-RAM region into a double buffer; set the loop register to wrap it;
  the sub-CPU watches playback position and refills the consumed half from a
  **Word-RAM PCM ring read ahead from CD** (raw PCM; compression optional and not
  required at these bitrates). Standard Sega-CD PCM-streaming pattern (proven).
- **Card data = raw direct-LBA** (no compression): read the card's 7 sectors from
  `CARD_BIG_ART_CD.BIN` into Word RAM, `CPY_TO_32X` into a **2-slot SDRAM cache**.
  Drops into the existing `cd32x_service_cd_request` seam and is exactly the
  offset-aware read **Issues 3/4** (big-art + portraits) were missing. See the
  "Card streaming" section for the cache/flip/no-double-buffer rules.
- **CD is single-tasked**: it does music read-ahead **or** a card read, never both
  at once. The multi-second Word-RAM music ring absorbs the card-read gap, so there
  is no underrun and no CD-DA-vs-data conflict (Issue 2 dissolves entirely here).
- **CD-DA stays** for title / overworld-map / results (no concurrent data reads
  there), so we don't re-encode the whole soundtrack as PCM — only the
  deck-editor + battle themes need PCM stream variants.

For a **cartridge-only 32X** (no Sega CD, no RF5C164) the same scheme uses the
**slave SH-2 + PWM** with the ring buffer in SDRAM — heavier on SDRAM, so it's a
separate future track; CD32X should use RF5C164.

## Risks / open questions

1. **RF5C164 streaming timing** — reading the chip's current play position to time
   the half-buffer refill is the classic hard part. Mitigate with a generous
   Word-RAM ring (≥1 s) and refill on a vblank-rate cadence rather than tight
   polling.
2. **Sub-CPU budget** — decompress + ring refill + CD service + SFX all on one
   68000. The numbers say there's headroom (decompress ≫ CD rate), but the
   music-refill cadence must be guaranteed even during a burst of card reads;
   that's the whole point of the watermark/priority.
3. **PCM quality** — RF5C164 mono-ish 8-bit at ~11–19 kHz. Acceptable for these
   chiptune-style themes; pre-render the CD-DA WAVs down to the chosen rate at
   build time.
4. **Word-RAM map** — must lay out {card LBA-read staging, music PCM read-ahead
   ring, optional lz4w output for 2D assets} within 256 KB alongside the existing
   `0x0C0000`/`0x200000` card staging. Small, but must be explicit.
5. **Asset pipeline** — a WAV→PCM tool for the streamed themes (resample to the
   chosen RF5C164 rate), and `lz4w.jar` only for the large 2D assets (title /
   ending / portraits). Cards stay raw (`CARD_BIG_ART_CD.BIN` already sector-padded
   per card). Keep raw/host variants for PC-FX.

## Suggested phasing

1. **Per-card direct-LBA read into a 2-slot cache (no audio change).** Add an
   offset/LBA-aware `cd32x_service_cd_request` path that reads a card's 7 sectors
   from `CARD_BIG_ART_CD.BIN` and `CPY_TO_32X` into a 2-slot SDRAM cache; drive it
   behind the card-back/flip window. Verify a streamed big-art matches the raw one
   headless. This alone fixes Issues 3/4 and is independently shippable.
   **Implemented:** the SH-2 CD32X blob-slice backend maps exact card big-art and
   story portrait/mask slice requests to private supervisor IDs; the Sega-CD
   supervisor seeks inside `CARD_BIG_ART_CD.BIN`, `STORY_PORTRAITS.BIN`, and
   `STORY_PORTRAIT_MASK.BIN`, reads only the requested sector-padded record into
   Word RAM, then transfers it with `CPY_TO_32X`. `Makefile.cd32x` now builds with
   `WAIFU_ASSET_BIG_CACHE_SLOTS=2`, matching the planned attacker+defender cache.
   Battle cut-ins prewarm that exact pair before the phase starts; hand, field,
   and deck card-check previews prewarm the selected monster before entering the
   preview state; CD32X rendering uses cached-only 112x112 art so a missed prewarm
   drops art for the frame instead of issuing a render-time CD read.
   **Related presentation fix:** the 32X video backend no longer treats title/menu
   as a one-shot hardware overlay, and after each FS page flip it clears the newly
   hidden CPU-visible back page (line table preserved). This is separate from card
   cache memory, but it addresses stale-page flashes in title/loading/game
   transitions when a state draws only a partial/black frame.
2. **RF5C164 music streaming, CD idle.** Stream one deck-editor theme from a
   preloaded Word-RAM buffer (CD quiet) to validate the double-buffer/refill and
   quality before adding CD contention.
3. **Single-task the CD** between music read-ahead and card reads under the
   ring watermark; soak-test in-duel with frequent card loads + battle animations.
4. Switch deck-editor / battle music routing from CD-DA to the PCM stream; keep
   CD-DA elsewhere. Then the Issue-2 CD-DA-resume hack can be retired for those
   states.

## Bottom line

Yes — possible at 1x with no perceptible disruption. The card transfer is only
~93 ms; seek is the variable and the card-flip / face-down window hides it, with a
2-slot SDRAM cache (~24.5 KB) holding the animation pair and **no double buffer**.
Phase 1 (per-card direct-LBA read into that cache) is low-risk, reuses the existing
supervisor seam, and resolves Issues 3/4 on its own. The PCM-streaming half
(Phases 2–4) is the real engineering — RF5C164 refill timing is the one genuinely
tricky piece, decoupled from card reads by a Word-RAM music read-ahead — but it
removes the CD-DA/data conflict by design rather than papering over it. LZ4W stays
reserved for the large 2D assets (title / ending / portraits), not cards.
