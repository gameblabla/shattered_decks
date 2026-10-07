# CD32X animation clock and presentation

The game API consumes elapsed **60 Hz periods**, not rendered-frame counts.
The old CD32X durations (for example, `WAIFU_PCFX_TURN_FRAMES=12`) were tuned
when a moving camera advanced one pose per rendered frame. Shared commit
`d424cee` changed moving cameras to consume elapsed vblanks. The old values
then described a 0.2-second board turn. Adding delays around presentation or
the MD mailbox could not repair those duration units.

CD32X now uses the shared authored durations for placement, turns, camera
moves, draws, equip/fusion and battle reveals. A turn is 58 clock periods
(about 0.97 seconds), plus presentation/phase-boundary granularity. The shorter
72-period console opening remains intentional.

## Clock and hardware references

- `cd32x_crt0.s` enables only Master SH-2 VBI and increments a 32-bit BSS
  counter once per acknowledged VBI. Both IRQ stores and foreground reads use
  the uncached SDRAM alias. The new counter occupies four bytes of SDRAM BSS.
- All Master external IRQ vectors enter one dispatcher. It reads the actual
  interrupt level from SR, groups even/odd level pairs, masks interrupts during
  the FRT correction, toggles TOCR low/high with readbacks, then acknowledges
  the actual source. R0/R1/R2 are saved; other registers and PR are untouched;
  RTE restores the interrupted SR. VRES retains the existing reset handler.
- The FRT is initialized on both SH-2s, including the Slave with interrupts
  disabled. OCRA=1, clear-on-match, Fs/8; FRT interrupts remain disabled.
- `CD32X/d32xr/crt0.s` (`pri_start`, `pri_irq`, `pri_v_irq`) supplies the
  established pattern for interrupt-level dispatch and FRT correction.
  `CD32X/d32xr/marshw.c` (`pri_vbi_handler`, `Mars_FlipFrameBuffers`,
  `Mars_WaitFrameBuffersFlip`) keeps its vblank clock separate from FS latching.
- Sega's `32XTechnicalInformation+Atachment.pdf`, in
  `CD32X/32X_All_Documents_OCR.txt`, item 10 and attachment 1 pages 12–13,
  specifies FRT initialization and interrupt dispatch/correction. The same
  technical notes list revision-specific MD interface problems; removing the
  mailbox from the animation clock avoids making those communications a clock
  dependency. This does not claim to fix every documented ASIC defect.
- Sega's `32X_Introduction_and_System_Features_-_26_-_04_-_1994.pdf`, hardware
  manual page 47 in the same OCR file, documents FS/FEN/VBLK. Presentation
  requests a swap and waits for FS acknowledgement, as in d32xr. Palette writes
  retain their independent VBLK check. Both framebuffer pages remain redrawn.

The foreground event loop skips a game step when the VBI clock has not
advanced. Unsigned 32-bit subtraction handles clock wrap. Long loading stalls
are capped at eight periods, matching the existing game API policy. For PAL,
6/5 conversion with a carried remainder maps the 50 Hz display clock to the
60 Hz authored timeline. There are no iteration-count timeouts or separate
leave-vblank/enter-vblank throttle loops. Existing MD controller, PCM/CD-DA,
CD-ROM and SFX mailbox conventions remain in use.

## Verification (2026-10-08)

- `make headless-cdrom-assets-2mb` and
  `./waifu_fm_headless_cdrom_2mb --regression-story-all --frames 1 --no-png`:
  all existing story/save/battle/asset regressions passed.
- Clean normal CD32X disc build: passed; staged SH-2 image **130576 bytes**,
  below the strict 131072-byte limit.
- Normal title/menu/Battle Mode navigation through the capture wrapper reached
  the rendered duel. Adjacent frames extracted from the bundled recording
  emulator showed the board and hand on both pages.
- Temporary `WAIFU_DEBUG_AUTOTURN` build: 130304 bytes; standard headless
  capture showed a rotating occupied board. Native SH-2 debugger tracing logged
  382 game steps; every elapsed report matched the unsigned clock difference
  capped at eight, and no successive steps shared a tick. After initial asset
  loading, alternating turns took **63/64 vblanks** (about 1.05 seconds at
  60 Hz), including rendering and phase-boundary latency.
- Debug defines were removed with a clean normal rebuild. Final adjacent-frame
  captures use the normal disc and the standard Battle Mode navigation.

These are emulator observations. Actual pacing on physical 32X revisions and
PAL hardware still requires hardware confirmation.
