# Deferred textured-floor benchmark

Implementation was compiled before any emulator tests. The 5 fps target remains
an acceptance gate. Use real `/usr/local/bin/openmsx`, stock MSX2 timing; never
the bundled incomplete headless Z80. No savestate is loaded.

For the shipping ROM already built:

```sh
python3 tools/msx2/bench_floor.py --seconds 40 --out artifacts/msx2-floor-opening
```

For populated turns, build the soak variant, then run:

```sh
make -f Makefile.msx2 soak
python3 tools/msx2/bench_floor.py --seconds 150 --out artifacts/msx2-floor-populated
```

The harness resolves the linked symbols of the exact build, guards banked
breakpoints with ROM bytes, times ArenaDraw through its return (including final
CE completion), and times each camera step through VideoPresent's return. This
includes pose loading, live cards, and presentation synchronization. It reports
continuous presentation intervals separately from work rate, excluding gameplay
pauses. The shipping opening is empty; the soak's turns are naturally populated,
not a guaranteed twenty-card worst case. No turn samples means that coverage is
missing, not that populated rendering passed.

Outputs: `metadata.json` with ROM/map hashes and build configuration,
`frames.csv` with pose timings and VDP registers, `summary.json`, per-frame
physical VRAM dumps and decoded visible-page PNGs. PNGs show the bitmap; they
do not composite hardware sprites. R#1/R#8/R#9 record display/sprite/video mode
state; a nonzero `busy_at_presentation` invalidates completion claims.
The harness itself has only been syntax-compiled, not emulator-validated.

Report back to the coding agent:

- Exact ROM hash, variant, machine, R#9 frequency, sprite state, audio behavior.
- Opening and populated-turn continuous presented fps, mean/worst work ms,
  arena ms, and whether every measured frame completes within 200 ms.
- Inspect PNGs for visible sandstone detail, flat walls, cracks, texture wrapping,
  clipped edges, correct card attachment and absence of card trails.
- Separately exercise card removal, defense changes, overhead view, and returns
  from modal scenes; this timing harness does not certify those paths.
- Missing coverage, Tcl errors, and any bank/audio/stack regression.

Run both supported 50/60 Hz configurations separately for eventual acceptance;
the script observes R#9 and does not override the game's frequency. Changing
only the machine name does not prove the game retained that machine's default.
Physical V9938 results must be reported separately from emulator timing.

After revisions, rebuild and repeat with a new output directory. Restore the
shipping build when done:

```sh
make -f Makefile.msx2 rom
```
