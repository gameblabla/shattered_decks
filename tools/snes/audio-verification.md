# SNES scene audio regression

The game uses one resident SPC700 player. Boot reconstructs the sequence pointer
and voice masks that an SPC snapshot player would normally restore from direct
page. The runtime command counter continues after the IPL's last acknowledgement;
otherwise the first block can mistake that stale echo for its own acknowledgement.

During replacement the SPC stops its timer, keys off voices, mutes the DSP, and
receives blocks without returning to the sequencer. Only the restart command
resets the sequence pointer and resumes playback. Commands are remembered before
acknowledgement, and SFX calls wait for acceptance before another call can reuse
the payload ports. The CPU restores the game's NMI/auto-joypad setting explicitly:
$4200 is write-only.

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
overworld, duplicate requests, invalid IDs, consecutive SFX immediately after
restart, command-token wraparound, and reuse of the resident SFX bank. Each stage
must advance its WRAM stamp, retain the correct resident code, match the selected
music and directory byte for byte in SPC RAM, preserve SFX samples, and produce
non-silent PCM. Captures and WAV files go to `build/snes/audio-probe/verify/`.

The patch also supports capturing a normal game run: set `SNES_AUDIO_PCM` to the
output filename for raw stereo 48 kHz signed 16-bit little-endian PCM and
`SNES_AUDIO_ARAM` to the final 64 KiB SPC RAM dump filename.

These checks are emulator evidence; they do not establish physical SNES timing.
