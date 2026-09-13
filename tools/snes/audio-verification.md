# SNES scene audio regression

The game uses one resident SPC700 player. Boot reconstructs the sequence pointer
and voice masks that an SPC snapshot player would normally restore from direct
page. The runtime command counter continues after the IPL's last acknowledgement;
otherwise the first block can mistake that stale echo for its own acknowledgement.

During replacement the SPC stops its timer, keys off voices, mutes the DSP, and
receives blocks without returning to the sequencer. Only the restart command
resets the sequence pointer, applies the requested looping/finite playback mode,
and resumes playback. Commands are remembered before acknowledgement, and SFX
calls wait for acceptance before another call can reuse the payload ports. The
CPU restores the game's NMI/auto-joypad setting explicitly: $4200 is write-only.

Regenerate the snapshots and build the game:

```sh
make -f Makefile.snes audio-assets
make -f Makefile.snes
python3 tools/snes/verify.py
```

The supplied accurate headless Mednafen discards audio. To inspect actual PCM and
SPC RAM, apply the small capture patch to a **copy** of its source tree. It changes
only output/inspection plumbing, leaving CPU, SPC, and DSP emulation unchanged:

```sh
cp -a SNES/snes-headless /tmp/snes-audio-capture
patch -d /tmp/snes-audio-capture -p1 < tools/snes/audio_capture.patch
JOBS=8 /tmp/snes-audio-capture/build-snes-headless.sh
make -f Makefile.snes SNES_AUDIO_PROBE=1 BUILD=build/snes/audio-probe
python3 tools/snes/verify_audio.py --emulator /tmp/snes-audio-capture/snes-mednafen
```

The separate probe cartridge uses the production loader, driver and snapshots.
It exercises title → battle → overworld → victory → fail → title → battle →
overworld (one phase per 400 fields), duplicate requests, invalid IDs,
command-token wraparound, and reuse of the resident SFX bank.  In each phase
two SFX slots are played by delayed B presses -- 150 fields after the track
change, then 100 fields later; the last phase plays its two 15 fields apart so
the second voice streams over the first.  Each phase must advance its WRAM
stamp, retain the exact resident code, match the selected music and directory
byte for byte in SPC RAM, preserve the packed SFX bank, and show each effect in
the PCM as its own reference decode (normalised cross-correlation against the
host expansion of the same 2-bit stream; the overlap phase's second effect is
matched on first differences).  The SPC's own diagnostics are asserted too:
zero ring underruns on either voice, and the late-tick count, worst timer
backlog and minimum decode lead are reported.  The live direct-page state must
also show looping mode for scene music and finite mode for the victory/failure
cues.  Captures, WAV files and a result manifest (`result-manifest.json`, with the
ROM/symbol/layout hashes and `complete` only after every requested phase) go
to `build/snes/audio-probe/verify/`.

The ARAM budget: music from `$0C10`, two 216-byte BRR decode rings at
`$94B0`/`$9588`, the 20,640-byte packed SFX bank at `$9660` (all ten PC
effects at 12.8 kHz, 2-bit residuals, 5 bytes per 16 samples, expanded into
the rings by the SPC as they play), the DSP directory at `$E700`.  Music is
ordinary 9-byte BRR; title and battle use the converter's perceptual sample
tier to fit under the rings, the other three songs are exact.
`src/snes/assets/audio/audio-layout-report.json` is regenerated with the
snapshots and holds the per-slot and per-track numbers.

The patch also supports capturing a normal game run: set `SNES_AUDIO_PCM` to the
output filename for raw stereo 48 kHz signed 16-bit little-endian PCM and
`SNES_AUDIO_ARAM` to the final 64 KiB SPC RAM dump filename.

These checks are emulator evidence; they do not establish physical SNES timing.
