# CD32X build & headless verification

Use this skill whenever building or verifying the Sega Mega CD32X (Mega-CD + 32X) target of the waifu card game.

## Build

```sh
make -f Makefile.cd32x            # default target: `cue` → waifucd32x.cue / waifucd32x.iso
make -f Makefile.cd32x program    # just toolchain-check + the M68K/SH-2 binaries
```

- Toolchain root: `ROOTDIR` (defaults to `$(GENDEV)`), expected at `/opt/toolchains/sega` (`m68k-elf-` + `sh-elf-` prefixes, `ldscripts/`, `bootblocks/US_BOOT.BIN`).
- The image is two programs: a resident Mega-CD/MD **supervisor** (M68K) plus the **SH-2 game binary** uploaded to 32X SDRAM. ISO built by `tools/cd32x_iso/bin/genisoimage`.
- **`EXTRA_CFLAGS` changes require `make -f Makefile.cd32x clean-build` first** — dependencies are not flag-aware.
- **Hard image-size limit: the SH-2 staged image must stay < 131072 bytes** (128 KiB BlastEm staging limit). An oversized image hangs at "Uploading SH2 app...". Current release image is ~129,408 bytes — only ~1.6 KiB of headroom. See the `cd32x-improvements` skill for size levers before adding code.

## Headless verify (BlastEm)

```sh
HOME=/tmp/cd32x_home BLASTEM=CD32X/blastem_headless BLASTEM_TIMEOUT=160 \
  scripts/cd32x/blastem_headless_capture.sh waifucd32x.cue /tmp/myimage.png 12500 /tmp/cd32x_random_battle_input4.txt
```

Args: `<cue> <out.png> <frames> [input-script]`. Env: `CD32X_BIOS_START_END` (default 5400) bounds the auto-generated Sega-CD BIOS-window START presses.

Gotchas (learned the hard way):
- **Custom input scripts MUST include the BIOS-window STARTs** or boot never skips (see `scripts/cd32x/bios_skip_only_input.txt`).
- Do **not** spam START past the BIOS window — it enters the menu and captures the wrong state.
- Menu navigation in scripts: by frame ~7000 the game is on the MENU; `+gamepads.1.down`@7000 then `+gamepads.1.a`@7400 reaches Battle Mode. Shorthand like `DOWN` never registers.
- Every code change shifts boot timing → fixed-frame captures are not comparable across builds. Prefer driving to a known UI state, and capture adjacent frames N / N+1 to detect page-flip flashing (32X has two framebuffer pages; anything not redrawn every frame lives in only one page).
- Fast iteration: build with `EXTRA_CFLAGS=-DCD32X_DEBUG_AUTOBATTLE` (throwaway; auto-enters battle, cuts iteration ~8 min → ~3 min). For story-scene perf: temporary `CD32X_DEBUG_BOOT_SCENE=N` (0 desert / 2 temple / 3 volcano / 4 void) + `-DWAIFU_CD32X_DEBUG_FPS` vblank overlay, capture with `CD32X_BIOS_START_END=3000`.
- BIOS files: `bios_CD_U/E/J.bin` + `32X_M/S/G_BIOS.BIN` must be next to the BlastEm executable; the script writes a minimal `blastem.cfg` there if missing.

## Docs

`docs/cd32x/`: `PORT_SCAFFOLD.md` (structure), `CD32X_RENDER_PERF.md` (perf analysis + status), `CD32X_FIXES_PLAN.md` (known-issue plan), `CDDA.md` (music track map), `CD32X_STREAMING_AUDIO_AND_LZ4W.md` (feasibility study), `ISO_TOOLS.md`.
