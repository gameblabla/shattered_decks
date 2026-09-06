# MSX2 port — status

Target: MSX2 (Z80A 3.58 MHz, V9938, 128 KB VRAM), NEO-16 mapper cartridge,
built with MSXgl + SDCC. Design: `MSX2_PORT_PLAN.md` at the repository root.

This port is **a fork, not a branch of the shared frontend**. It never compiles
`src/main.c`, and no MSX code exists outside `src/msx2/` and `tools/msx2/`.

---

## Where it stands

### MSX2+ portrait edge follow-up — 2026-09-06

The brightness-only edge merge could not reproduce a portrait colour through
the sand's shared YJK hue. `Msx2_StoryEdgeInk()` now compares a YAE palette
candidate against the YJK fallback in C, using weighted RGB distance. It emits
palette ink only when closer to the intended alpha composite; `Msx2_MergeRow()`
keeps the existing three chroma bits for either encoding, preserving neighboring
background pixels. The candidate search is baked to keep Z80 redraws short.

The baker also now decodes YAE pixels in the backdrop correctly. Fitted palette
pixels occur throughout the sand painting, not just in the text panel; treating
their palette index as brightness had produced incorrect edge composites.

Each edge record was six bytes: x, fallback Y, RGB555 low/high, fallback error,
and candidate index. (Two, since the pass above.) The tables plus the opaque
body fit the existing 32 KB portrait slot; `Msx2_RomRead()` handles records
across the segment seam. No portrait segment allocation changes.

`python3 tools/msx2/check_portrait_edges.py` checks all 48
character/lighting/stage combinations: 29,856 edges, 15,094 improved palette
choices, weighted error 340,193 -> 185,326 (45.5% lower). It also checks chroma
preservation and asset bounds. This metric describes the tested edge colours,
not whole-image quality or a physical-hardware measurement. (The selector it
checked in C moved into the baker in the pass above; the numbers are the same
because the decision is.)

The final shipping MSX2+ ROM builds and packs. Real openMSX (`C-BIOS_MSX2+`)
captures in `artifacts/msx2plus_edge_verified/` show the opening and both speaker
states; 3,334 captured edge bytes match the C selector exactly. Reproduce the
comparison with `python3 tools/msx2/check_portrait_edges.py
artifacts/msx2plus_edge_verified`. The ordinary MSX2 story bank also compiles
with MSX2_PLUS undefined; a full baseline ROM/soak was not rerun in this pass.
The ready-to-run plus cartridge is `src/msx2/out/waifu_msx2p.rom`.

### MSX2+ portrait redraw speed, and title words off the YJK layer — 2026-09-06

A speaker change on the MSX2+ story screen took **1.61 s**; it now takes
**0.72 s**, measured the same way both times (an openMSX breakpoint on
`Msx2_MergeRow`, timing the burst of calls a change makes:
`BURST 24.4288 26.0374` against `BURST 24.4212 25.1410`). Nothing about the
result changed -- the 3,334-byte capture comparison the previous pass shipped
still matches byte for byte, now over 5,392 bytes and six frames.

Where it went, from ablation builds of the same ROM:

* **The edge choice is baked.** A fringe record was six bytes -- YJK fallback,
  target RGB555, the fallback's weighted error, a nominated palette index --
  and `Msx2_StoryEdgeInk()` weighed them on the Z80 against a copy of the
  scene palette read back on every redraw. Every input to that is known when
  `gen_msx_plus.py` runs, so it decides there and a record is now `(x, ink)`:
  a third of the ROM to read and none of the arithmetic. The tables fell from
  4,942 to 1,730 bytes and the pixel offset from 21,504 to 9,216.
* **A row's tables are read in blocks.** `Msx2_RomRead()` is a call, two
  normalising loops, a seam check and two bank switches around a short `ldir`;
  the blitter made three of them per row for an average of fourteen bytes. The
  run records now arrive sixteen rows at a time and the fringe stream through a
  sliding 192-byte window -- about a thousand reads per speaker change become
  sixty, for 304 bytes of RAM.
* **`Msx2_MergeRow()` writes R#14 once per row**, not twice per pixel, and
  holds both forms of the address high byte ready; a merged pixel is about 240
  T-states instead of 335. It also adds the figure's x as it walks, so the
  cartridge bytes go straight to it instead of through a fix-up buffer.

What is left is mostly not reducible: of the 0.72 s, about 0.27 s is the
`outi`/`nop`/`jr nz` interior blit, which is paced by the VDP's ~29 T-state
GRAPHIC 7 access interval and not by the CPU. That is also why a Turbo R is
not much faster here.

`tools/msx2/check_portrait_edges.py` no longer host-compiles the C selector
(there is none); it recomputes the baker's decision from the source art and the
shipped backdrops and checks the table against it. Its capture comparison also
locates each figure by matching rather than by frame number, which is what
broke when the redraw got faster and the scene moved along.

**The title words are no longer fringed.** `SHATTERED DECKS`, the copyright
line and the `PUSH SPACE` prompt were already stamped as YAE palette pixels,
but the stamp overwrote the whole byte -- including the low three bits, which
are the group of four's shared YJK chroma and belong to the picture, not to the
letter. Every glyph therefore dragged the hue out of the sky or sand beside it.
`gen_msx_scenes.stamp*` take `keep_chroma` now and the plus title passes it,
which is what `stamp_yae` already did for the panels and what `Msx2_MergeRow`
does for a bust's edge at runtime. 1,880 bytes of the title change; the MSX2
cartridge, where a byte really is a colour, is untouched.

| Milestone | State |
|---|---|
| M1a — rules fit in RAM and a duel can be played blind | **done** (see below) |
| M1b — timing truth ROM (OUTI spacing, HMMM/LMMV throughput) | not started |
| M2 — VRAM map, compositor skeleton, glyphs, page flip | **done**: GRAPHIC 7 layer, double-buffered page flip, glyphs, fills, the duel board |
| M3 — asset pipeline, title screen | **done**: every picture the game shows is baked from `assets/source/` and streamed from the cartridge |
| M4 — the duel screen, played by a person | **done**: place, fuse, attack, end turn on real input; shared-renderer 3D arena and turn views, hidden COM hand covers, and board-free 2D attack cut-ins |
| M5 — story presentation | **done**: opening, sanctum map, dialogue, and ending, with both speakers composited over the shipped painting |
| M6 — full duel loop | **done**: person-playable placement, fusion, support, attacks, turn handoff, results, and the real shared-renderer board |
| M7 — story completion and continue codes | **implemented**: eight-letter name entry, five-duel frontier, rewards, a tabbed deck editor, 16-symbol password save/load, floppy save/load where a drive answers, and ending transition |

### `MSX2PLUS_specificissues.txt` pass — 2026-09-05

Three reports, all about the MSX2+ (SCREEN 10) cartridge.

**The cartridge did not build at all.** Segment 2 -- the duel screen -- had ONE
byte free in the shipping MSX2 link, and the plus build adds two
`Msx2_VideoModeG7()` calls to it, so `MSXhex` failed with "Data overwrite at
offset 00008000h". The attack cut-in (`msx2_battle_fx.c`, 528 bytes) has moved
out of the duel bank into the modal one, where the fusion cut-in already lives:
it reaches nothing but `_CODE` -- sprites, the video layer, `g_duel`, the string
table -- which is what makes the move legal (`msx2_bank.h`). It is reached
through ONE trampoline rather than five, because a trampoline is `_CODE` and
`_CODE` had two hundred bytes left; `msx2_battle_fx.h` turns the five entry
points into one `(op, x, y, value)` call and the bank side takes it apart again.
Segment 2 went from 16,383 bytes to 15,923 for 41 bytes of `_CODE`.

**The busts.** Two passes, because the first one traded a fringe for holes.
`tools/msx2/gen_msx_plus.py` encoded a bust with no idea which of its pixels
were real. The portrait art is cut out against black, so a pixel at
40% alpha carries almost no colour -- and in YJK a pixel is never written alone,
it hands its group of four a hue, so one black rim pixel blackened four. The
figures stood inside a chewed black outline with square notches in it.

`msx2_yjk.encode()` now takes the mask: the group chroma is fitted to the pixels
that exist, the real colours are spread outward before encoding, and the
dithering error is neither taken from nor pushed into a pixel nothing will draw
-- which is what the GRB332 bust path always did. The listening speaker is
dimmed to 0.62 rather than 0.45, because YJK's weak end is the dark end and 0.45
came out as a silhouette.

Raising the opacity threshold to 176 fixed the fringe and put HOLES in the
figures instead: this art's antialiased hair is a long way from opaque, and
every strand below the bar stopped being blitted. There are two thresholds now,
because they are two questions. **Is this pixel part of the figure** is the MSX2
build's question and keeps its answer, 96. **Does this pixel get a vote on the
hue its group shares** is the one YJK adds, and that bar is 176.

The other half of the holes was the rounding, and BOTH answers to it were
wrong. A rectangle blit has to cover whole chroma groups. Rounded INWARD, every
partial group at the end of a run was left to the backdrop and the silhouette
lost up to three pixels a side, per row, with any run too short to survive
dropped entirely. Rounded OUTWARD it WROTE those three pixels instead, and what
they carry is what `spread()` pushed out there -- which at the edge of this art
is its own black outline, one rim pixel smeared into a four-pixel block. That is
the black halo the second report was about, and it is why the figure was chewed
where the first one said it was thin.

So a run covers only the groups the figure fills COMPLETELY, and the pixels left
over are not the blitter's problem at all. `gen_msx_plus.py` emits them as a
second table -- a count and (column, Y byte) pairs, row after row, between the
run table and the pixels -- and `Msx2_MergeRow()` (`msx2_stream.c`) writes them
one at a time: it reads the byte back out of VRAM, keeps the low three bits, and
changes nothing but the brightness. Those three bits are the hue the group
shares with the backdrop, and leaving them alone is the only way a cut-out gets
a per-pixel edge in a mode whose colour is four pixels wide. The worst row in
the shipped art needs twenty of them and the whole figure about eight hundred:
one VRAM read and one write each, once per speaker change, on top of a blit that
is otherwise the same rectangle copy the paletted build does.

That left a third report, and it was right: a dark navy rim all round both
figures. Only the BRIGHTNESS of a merged pixel is the bust's to choose, and the
brightness being written was the figure's own -- which at the edge of this art
is half the transparent ground it was cut out of, because a plain RGBA resample
mixes that black into every partial pixel. Hanging a colour like that on the
sky's hue is a shadow the drawing never had.

So the edge is now COMPOSITED, at bake time, against the picture it stands on:
alpha over the backdrop, then the best Y for the result given the chroma that
group already carries -- and the baker knows both, because it encoded that
picture a moment earlier. `gen_msx_scenes.portrait(premul=True)` resamples the
art in premultiplied alpha first so the figure's own colour is its own colour
(the MSX2 cartridge does not ask for it and its bytes are unchanged), and the
skirt of pixels below the 96 threshold, which used to be thrown away, is merged
too at 40: it is anti-aliasing, and a blend has somewhere to put it. The edge
stops being a cut-out.

The table is therefore a function of the backdrop, so each bust carries one per
talk scene -- four of them, 2 KB each, in padding the 32 KB stride already had
-- and `msx2_story.c` picks by stage (`g_bust_stage`, a static rather than an
argument so the MSX2 build carries no parameter it has no table for).

**Interface drawn as YJK.** Two inks on SCREEN 10 screens were still GRAPHIC 7
bytes, and a GRAPHIC 7 byte there is a YJK pixel that takes its hue from the
painting under it:

* `MSX2_PLATE_COLOR` (0x24, bit 3 clear) -- the plate under every speaker's name
  and under every row of the sanctum road. It comes from the generated scene
  header rather than `msx2_video.h`, so it was the one ink `msx2_plus.h` had not
  overridden. It is now `MSX2_YAE(0)`: entries 8..15 are fitted to each picture,
  so an interface colour may only ever name one of 0..7.
* `msx2_title.c` drew the refused menu row in a literal `MSX2_RGB(3, 3, 1)`.

And the fringing the report described down the sides of the title's menu box was
a third thing again, in the fill itself. A YAE pixel's low three bits are still
read as part of its group's J or K, and a byte-mode fill writes them as zero, so
a panel whose left or right edge lands inside a group of four dragged the colour
out of the picture pixels beside it. `Msx2_Fill()` on a SCREEN 10 screen is two
logical commands now -- `dest = (dest AND 7) OR ink` -- which is the same thing
`gen_msx_plus.py` does when it stamps a box into a baked picture. The duel is
GRAPHIC 7 in both cartridges and keeps the one byte-mode fill.

And one more of the same class that the report did not name: **sprites**. In
GRAPHIC 7 the sprite plane reads the chip's own fixed sixteen colours, and in
SCREEN 10 it reads the PALETTE REGISTERS instead (openMSX picks `palBg` for
every mode but GRAPHIC7). The sanctum road's selector is drawn in fixed-table
indices 2/10/8, two of which are in the fitted half of the palette, so the gem
took a different colour on every backdrop. `Msx2_SpriteGem()` now has a second
tint table for the YJK screens, out of the interface's own eight entries.

Verified on `C-BIOS_MSX2+` through `./msx2.sh trace`, which decodes a dump as
YJK when R#25 says so: title, menu, opening narration, both talk scenes, the
sanctum road and the duel. The talk box is 100% YAE pixels (0x18) at every row.

**The turbo R.** It works: the port switches to the R800 and spends it.

* The main ROM's version byte is read at boot (`msx2_audio_probe.c`, the one
  place in the port where the BIOS is in page 0) into `g_msx2_msxver`.
* `Msx2_CpuFast()` (`msx2_bank.c`) calls CHGCPU with A=0x81 -- the R800 with
  external memory still read as ROM, and the machine's LED lit -- from `_CODE`,
  with the BIOS briefly over segment 2.
* **And puts the interrupt vector back, which is the whole trick.** CHGCPU
  reinitialises the machine's interrupt system, and this port does not use the
  machine's: crt0 runs interrupt mode 2 off a RAM vector table at 0xC000 with
  the ISR at 0xC101, because the ROM's own 0x0038 is the duel bank. Coming back
  in mode 1 sent the next V-blank into whatever byte the duel screen has at
  0x0038 -- the machine died inside the first cartridge read after the switch,
  found at PC=0x00CC on a stack that was not ours. `ld i,a` / `im 2` after the
  call is what makes the R800 usable at all.
* The turn strip is baked with NINE poses instead of five (`src/main.c`). The
  even ones are byte for byte the old five -- `2k/8` and `k/4` are the same
  point on the path -- so a Z80 steps by two and plays exactly the move it
  always played, and an R800 steps by one and gets the in-betweens.
* `Msx2_ArenaDrawStep()` draws every line of a moving board rather than the
  pose's baked stride when the R800 is running. Both of these ask
  `MSX2_TURBO_R()`, which is `g_msx2_r800` -- whether the fast processor is
  RUNNING, not which machine this is -- so a turbo R that never got switched
  draws what an MSX2 draws instead of taking four times as long over a nicer
  picture.

Measured: the same 90 s soak, same seed. On an MSX2 the run is at frame 881,
duel two turn 1, 55 steps in. On openMSX's FS-A1GT (`MSX2_MACHINE=turbor`) it
is at frame 940, turn 2, 98 steps -- with the full-resolution camera move and
the nine-pose turn both on. The MSX2+ cartridge was captured on the same
machine and its story screens are intact, so SCREEN 10 and the R800 are happy
together.

**Room, which is why three things moved.** Every fix above cost code in areas
that had none:

* the attack cut-in left the duel bank for the modal one (above);
* `Msx2_QuadOutline`, `Msx2_QuadOutlineXor` and `Msx2_FrameRectXor` left `_CODE`
  for the duel bank (`msx2_lines.c`) -- only `msx2_board.c` calls them --
  because the chroma-preserving fill needed forty bytes of `_CODE` and there
  were none. `Msx2_LineOp()` stays in `_CODE` and is public now, which is the
  half a bank is allowed to call.

Final: `_CODE` ends at 0xBF41 with 191 bytes free in the plus build (0xBE84,
380 free in the MSX2 one), segment 2 holds 16,238 of 16,384 bytes, and both
cartridges pack with 3.8 MB of tail padding.

### Reproducing the turbo R runs

`MSX2_MACHINE=turbor ./msx2.sh run|trace|verify` -- openMSX's FS-A1GT, which
needs `fs-a1gt_firmware.rom` and `fs-a1gt_kanjifont.rom` in
`~/.openMSX/share/systemroms`. A machine assembled from a bare `MSXTR.ROM` +
`FMBIOS.ROM` pair does NOT boot this cartridge (or anything else): 32 KB of
main ROM with no sub ROM is not an MSX2+ boot, and the failure looks exactly
like a ROM that hangs. Use the FS-A1GT.

### `msx2_bugs.txt` pass — 2026-09-05

Eight reports from the owner, worked through in source and rebuilt:

* **A chain is never refused for "those cards do not fuse".** A material slot
  that stopped being playable while the chain was being chosen -- SPACE on a
  hand card plays it and empties its slot -- used to make
  `Msx2_FusionPreview()` refuse the whole chain for the rest of the turn.
  Dead entries are skipped now and only real materials are spent, so an
  unfusable pick always goes through the cut-in and leaves the last card
  standing, as on PC. The cut-in titles itself FUSION SUMMON / FUSION FAILED /
  CARDS DISCARDED from `Msx2_FusionSucceeded()`.
* **The cards come back at the square-on turn pose.** Every quad in
  `MOVE_TURN_2` is a quarter turn round: 0->1 is the edge that runs down the
  screen, not 0->3, so the mapper measured a zero-height span and drew nothing
  for that whole frame. The turned art already exists (the defence set is the
  upright card rotated clockwise), so `Msx2_RasterCard()` relabels the corners
  and swaps texture sets. Checked against the baked capture: 40 quads at that
  pose went from zero rows to real ones, and the nine that stay under three
  rows are the far support cells, which are that small in the picture.
* The map's red selector is taken down on the frame the opponent is chosen,
  not on the frame after, so it no longer stands on the talk scene while the
  portrait streams in.
* A landing no longer blanks the display unless the whole hand strip is being
  wiped. The player's summon (strip already off) and the opponent's (one card
  erased) were each spending a black frame to hide a picture that does not
  change.
* Story duels 1-3 play the battle theme, duel 4 the boss theme and duel 5 the
  final-boss theme. Every story duel used to open on the boss theme.
* A monster in defence position is refused as an attacker on the card, with
  CHANGE POSITION as the prompt, instead of offering the target row and then
  silently doing nothing.
* The chair change takes the rules' turn-start step before it reads the deal
  mask, so the card drawn to replace what that side spent flies in with the
  rest instead of appearing when the deal is over.

Built and packaged (resident ROM ends at `0xBF13`, 237 bytes spare); a 90 s
blind soak completed one duel and was mid-second with the probe reporting OK.

### The stray sliver beside an equip — 2026-09-05

The last item on the list, and it is geometry rather than an overflow.

`Msx2_RasterDraw()` treated every projected slot as a trapezoid: it walked the
0->3 and 1->2 side edges over the rows the LEFT one spanned. That holds in a
chair, where a board row projects with horizontal ends. Half way through a turn
orbit it does not: the slot leans, and its 0-1 edge slopes by several rows. At
`MOVE_TURN_1` the player's first support cell is (52,45) (65,49) (42,61)
(29,55), so rows 45..55 were drawn with the 1->2 edge started four rows above
where it begins -- the card's top right corner was painted ABOVE its own top
edge, out over the black past the board's rim, and its bottom six rows were
never drawn at all. That overhang is the sliver, which is why it only showed
with a card in a support row (they sit nearest the rim) and only during a
swing.

The row range is now the whole quad and each side is a CHAIN of edges. Every
quad the capture emits is wound the same way -- all 920 checked -- so from the
topmost corner the forward walk is the right-hand boundary and the backward
walk the left; each is followed edge by edge to the bottom corner, and `v` is
whatever texture row the corner carries (0 and 1 the top, 2 and 3 the bottom).
On a chair pose the first edge of each chain is the flat 0-1 top and covers no
rows, which leaves exactly the two edges this used to walk, so the trapezoid is
now the degenerate case rather than the assumption. `Msx2_RasterEdge()` got
smaller in the process: a chain only descends, so the sign juggling for an
upward edge is gone.

Measured over all 920 baked quads: quads painting more than two pixels outside
their own outline went from 353 (worst 12.2 px) to none (worst 0.71 px, which
is rasterizer rounding), and the 550 that drew fewer rows than their height to
none. Confirmed on hardware-accurate openMSX by dumping the band every frame of
every turn orbit in a long soak: before, the support card at that pose covered
screen rows 45..54 with a run out to x=65 on its first row; after, it covers
46..59 and its first row is x=50..55, which is the quad's own left and top edge
to the pixel.

Cost 154 bytes: the shipping ROM now ends at `0xBFAD`, 83 bytes spare. A 90 s
blind soak completed one duel with the probe reporting OK.

Note for whoever picks this up next: `make -f Makefile.msx2 regression` does
not package and has not since before this pass -- the probe diagnostics push
resident ROM past `0xC000`. `MSX2_FIXTURE_TOP_PASS_TURN` now also puts an equip
on each support row, so it reproduces this report directly once there is room
to build it again.

### The green line off the rim — 2026-09-05

The quad fix above was not all of it, and the rest is not geometry at all.

A camera-move frame draws one row in every two or three and has the command
engine repeat it down the group (`Msx2_FloorDouble`). The group grid is fixed
to the band, not to the pose, so the group that straddles the top of the arena
leads on a row the board does not reach yet, fetches an empty span record and
comes out blank: at `MOVE_TURN_2` the board's own first row is band 10, but
nothing is painted above band 12. `Msx2_RasterCard()` maps a card row by row
and skips nothing, so a card in a far row -- which is where an equip lives --
put pixels down on rows the floor had left black.

Two things went wrong from that. The card's top edge hung over the board's rim
with black under it; and, worse, the erase bookkeeping (`g_ext_l`/`g_ext_r`)
records only what the FLOOR covered, so those rows were remembered as empty and
never blacked again. Page 0 draws poses 22, 20 and 18 of a five-pose orbit, and
the equip's top row from pose 22 -- 15 pixels of `0xA9`, bright green, at screen
row 28 -- was still sitting outside the board when pose 20 went up two frames
later. That is the line the owner reported: it wanted a support card (they are
the far rows), it only showed while the table was turning, and passing the turn
from the opponent's chair is what puts pose 22 and pose 20 on the same page.

`Msx2_FloorBand()` now publishes `g_msx2_arena_top`, the first screen row this
pose is actually painted on -- `BAND_Y + ceil(row0 / stride) * stride` -- and
`Msx2_RasterDraw()` steps its chains over any row above it instead of drawing
there. A resting pose has a stride of one, so the value is the board's own top
row and nothing is clipped; a moving frame gives up the one or two rows of a
far card that the coarse floor was never going to stand on. Nothing a card
draws can now land outside a row the silhouette knows about.

Confirmed by scripting the owner's setup into a real duel (the debugger writes
a green GUARD support into the player's support row and a monster on the
field, then the opponent's pass drives the orbit): before, band row 14 of the
half-way frame ends `... 95 00 20 A9 A9 A9 x14` -- the board's last pixel, then
the stray card edge fifteen pixels out over the black; after, the row ends at
the board and the far cards start level with the floor. A 400-frame sweep of
every turn orbit in a long soak has no detached fragment bigger than a single
floor-coloured pixel.

Resident ROM ends at `0xBF74`, 140 bytes spare (the row skip let SDCC emit less
than the chain walk cost); the duel bank has 73. A 90 s blind soak completed one
duel with the probe reporting OK.

### WIP build follow-up — 2026-09-05

The first WIP overflowed resident ROM by 155 bytes once SDCC's `_HOME` and
initialization sections were included. Title presentation now lives in the
modal bank behind the usual resident wrappers. Shipping and fixed-seed autoplay
ROMs both package successfully: shipping ends at `0xBE6F` (401 bytes spare),
autoplay at `0xBE44` (444 spare). The budget report and packer now include the
resident runtime sections in their bounds checks. Overhead geometry has been
regenerated through the Makefile.

A host harness passed 18 fusion/discard assertions, including valid recipes,
incompatible pairs, support-only discards and rejected selections. The owner
tried the game and reported no obvious issues. The longer emulator soak was
stopped at the owner's request; no completed soak result is claimed here.

### WIP bug fixes — 2026-09-05 (initial source-only pass)

Implemented by source inspection of `msx2_bugs.txt` and the Claude transcripts:

* Failed fusion pairs now consume the earlier material and retain the next
  monster. Support-only chains can be discarded without reading result art for
  an empty card. Hand-slot validation and the summon limit remain enforced.
* Overhead card coordinates move right four pixels in `gen_msx_views.py`, so
  generated upright/defence placement, repair boxes and cursors move together.
* Floor sliver cleanup resets the VRAM pointer before each separate fill;
  clipped repairs reset the fill height to one row. Initial repeated-row camera
  frames clear the band because the exact-row backdrop has different coverage.
  A full-width row repeat explicitly uses a 256-pixel command width.
* Card-edge Q8.8 slopes divide unsigned magnitudes before applying their sign,
  avoiding overflow in the signed scaled x difference.

No build, asset regeneration, tests or emulator runs were performed for this
pass, as requested. The next normal Makefile.msx2 build must regenerate the
overhead geometry; the ROM and earlier measurements below predate these edits.
The initial camera-page clears may cost more time and remain unmeasured.

### The textured floor, measured — 2026-09-05

The perspective arena and the overhead tiles sample the two supplied sandstone
materials in SCREEN 8 GRB332; the walls stay flat.  The duel bank includes
`msx2_floor.c` instead of the old per-quad polygon walker, and
`tools/msx2/gen_msx_floor.py` (run by `Makefile.msx2`) bakes, per authored pose,
what each of the band's 114 rows shows: 92 KB of span records in 106,750 packed
bytes over seven segments, plus 2,560 bytes of texture.  Records carry material
and UV coefficients, never destination pixels.

**Measured, real openMSX, `tools/msx2/bench_floor.py`, C-BIOS_MSX2, 60 Hz:**

| | camera move, empty board | populated turn move |
|---|---|---|
| before this pass | 0.77 fps (1306 ms a frame) | — |
| now | **5.13 fps** (195 ms mean, 232 worst) | 3.6 fps (275 ms mean) |

The populated figure is no longer the floor's: an arena step there is 164-247 ms
and the rest is `Msx2_RasterCard` drawing up to ten cards over it, which this
pass did not touch.

#### Where the 6.7x came from, in the order it was worth doing

1. **The C span walker was over half the frame** (2,800 T-states a record;
   SDCC keeps that many locals in an IX frame).  `Msx2_FloorRow`,
   `Msx2_FloorBandRows` and the row fetch are assembly now, and the samplers
   pop their state off the record with a borrowed SP instead of being handed it.
2. **The samplers.** A constant-v span is 33 T-states a pixel: the generator
   folds v's texture row into the address, so the accumulator's high byte *is*
   the texel's low address byte and `ld c,h / ld a,(bc) / out / add hl,de` is
   the whole mapper.  A rotating span walks v in IY scaled by sixteen with its
   step in SP, and writes each sample twice -- the second write is what fills
   the VDP's own 29-T-state spacing, so the doubling is free.  The old loop was
   215 T-states a pixel.
3. **Black stopped being painted per span.**  A pose carries a dozen backdrop
   rectangles that cover every black pixel (a V9938 command is ~300 T-states of
   set-up and then ~2.25 us a pixel, so one command is worth 360 filled pixels,
   and there were two hundred black spans a frame).  Inside a camera move it is
   not painted at all: each page remembers what its arena covered last time, and
   only the two ends of that silhouette that the new one no longer covers go
   back to black -- a few pixels a row.
4. **A moving frame draws every second or third row** and has the command
   engine repeat it (`Msx2_FloorDouble`, one HMMM whose overlap propagates down
   the group).  That divides the sampling, the record walk, the row fetch and
   the span set-up all at once.  Which of 2 or 3 a pose takes is baked by the
   generator: a distant board loses its slab wall to trebling, a near one does
   not, and a two-row pose trebles anyway below the band row where the tiles
   are tall.  **The pose the camera stops on is redrawn line by line**, so
   nothing the player sits and looks at is doubled.

Rotating poses sample a 16x16 mip of each material and resting poses the fine
32x32, whole-pose either way so no two tiles of one picture carry different
detail.

#### What was measured and does not work

* `YMMM` instead of `HMMM` for the row repeat: two orders of magnitude slower
  in openMSX (a full-width copy, and NX is not ignored the way the manual
  reads).  `HMMM` over the silhouette only.
* Painting the whole band black first: 66 ms of command engine, and every span
  after it waits.
* Horizontal replication beyond two for the moving-v sampler: the interval
  between writes is already the VDP's, so a third pixel buys 12%, not 33%.

#### State and limits

Shipping ROM: duel bank 16,127 of 16,384 bytes, static RAM 9,726 with 3,458 to
HIMEM (the blind soak reports 3,427 free between `data ends` and SP).  A 150 s
soak completes a duel and stays `status OK`.  **The `regression` variant does
not link**: it needs 524 bytes of `_CODE` and only 506 are free -- that ceiling
is not this pass's and no code moved into `_CODE` here.

Projection is still the 23 authored mesh poses; arbitrary runtime camera
projection and card occlusion rejection are not implemented.  UV is
perspective-correct at scanline ends and affine between them.  No physical
hardware test: everything above is stock-machine emulator timing.  The
measurement recipe is in `tools/msx2/FLOOR_BENCH.md`.

### Where the code lives, since 2026-09-03

The port ran out of both of its code areas at once, and the fix reshaped the
memory map, so this is the first thing to know about the build:

* `_CODE` is 0x4000-0xBFFF, a hard **32 KB** (cartridge segments 0 and 1).
* **Page 0 (0x0000-0x3FFF) is a switched window** over three more 16 KB code
  banks: segment 2 is the duel screen, segment 3 the modal screens (card check,
  fusion cut-in), segment 4 the story screens.  `src/msx2/msx2_bank.h` is the
  contract; `msx2_bank.c` holds a trampoline per entry point, and each restores
  whatever bank it displaced, so a modal screen opened from the story lands back
  in the story.
* **The one rule:** code in a page-0 bank may call `_CODE` and its own bank, and
  no other bank — those do not exist while it runs.  The trampolines are the
  only way in, which is what enforces it.
* This works only because the interrupt handler moved to **RAM page 3**
  (`InstallRAMISR = "RAMISR_PAGE3"`, IM 2, 452 bytes).  With the ISR at 0x0038
  in ROM, mapping any other segment at 0x0000 would take the handler with it.
* It is also why **the disk layer is not in a bank**: it needs the BIOS back at
  0x0000 for RDSLT and CALSLT, and a bank that has to disappear for the call to
  work cannot be the bank the call is made from.  `msx2_disk.c` is in `_CODE`
  and swaps page 0's primary slot around each BIOS call.
* Cartridge segments **0-7 are reserved for code**; assets start at 8, so adding
  a bank is not a re-bake of six megabytes of artwork.

`./msx2.sh ram` reports all four areas and fails on any of them.

### The title screen

`./msx2.sh shot --page 1` after the default key sequence:

* the backdrop is `assets/source/title/title256_msx2.png`, the same picture the
  other targets show, nearest-colour quantised to GRB332 offline and **streamed out of the
  cartridge** — segments 4-7, 54,272 bytes, never linked into the Z80's 32 KB;
* the logo is drawn over it transparently with a hard shadow (the 6x8 bitmap
  font expanded 2x as runs of fills, since the printer has no scaler);
* `PRESS SPACE TO START` and the copyright sit directly on the artwork, white
  with a one-pixel black outline, because a colour that reads on the bright sky
  does not read on the dark rock and vice versa;
* the prompt blinks without re-streaming anything: both states of its strip are
  baked once into the offscreen VRAM rows below the visible 212, so a blink is
  one `HMMM` and no Z80 work at all;
* **nothing is ever drawn on the page the VDP is scanning out.** 128 KB of VRAM
  is exactly two GRAPHIC 7 pages, so a repaint goes to the hidden page and the
  swap happens in V-blank (`Msx2_VideoFlipRequest` / `Msx2_VideoPresent`, called
  straight after the frame loop's `HALT`).  Drawing on the visible page is what
  used to make the menu flicker: an erase and the redraw over it are two
  separate command-engine jobs, and a scan line passing between them shows the
  gap.  The consequence a scene has to honour is that a partial repaint reaches
  only *one* page, so every change is held in a dirty mask for
  `MSX2_VIDEO_PAGES` frames and painted again on the next buffer;
* the second buffer is built with one `Msx2_VideoCopyPage` (a whole-page
  `HMMM`, offscreen stashes included) rather than by streaming the cartridge a
  second time;
* SPACE opens the three-row menu (`STORY MODE` / `BATTLE MODE` / `LOAD STORY`),
  and confirming a row enters the selected flow.  `LOAD STORY` accepts the
  16-symbol continue code described below.  The help line lives *inside* the
  panel: it changes with the cursor, and anything redrawn over artwork would
  have to restore it.

Input is real: `msx2.sh` drives the openMSX keyboard matrix from Tcl, so the
menu is walked exactly as a player would walk it. There is no self-play code in
the ROM.

### The duel screen

**The board is the game's board.** Nothing about the arena is drawn offline:
`tools/msx2/gen_msx_views.py` builds the capture tool out of `src/main.c`, runs
it with `--dump-msx2-views`, and bakes what `render_board()` drew. The floor,
the slab sides, the perspective and the slot layout are whatever the other five
targets render (`MSX2_PORT_PLAN.md` §0.3.1, §4.3). A view whose art disagrees
with the other targets is a capture bug, not a styling choice, which is the
whole point of taking this route.

The player pose is the shared `player_camera()` used by PC-FX/headless; the COM
pose is the shared `enemy_camera()`. The opening therefore lands on the same
player-chair view instead of an MSX-only near-overhead frame. The black surround
also comes directly from the PC-FX presentation: story-stage paintings no
longer sit behind or grade the arena.

**There is a third, tactical view.** The chair perspective squeezes a field
slot down to about 36x15 pixels, so walking up from the hand cuts straight
overhead. It is deliberately plain: twenty contiguous tiles alternating the
arena's captured dark-beige and beige face colours, with no bevel, gutter,
ring, or slot outline. Cards are sharp axis-aligned 32x42 copies in this view.
The table takes the hand rows because there is no hand HUD while reading it.

**The selector is a sprite.** It was a rectangle drawn into the bitmap, which
meant it had to be erased from the bitmap on each page separately; it is now the
PC build's spinning red gem (`draw_spin_cursor` in `src/main.c`), the same
octahedron projected offline into eight sprite patterns. A sprite floats over
both GRAPHIC 7 pages, so there is one cursor rather than two, and nothing under
it is ever touched — which is what lets it animate at all.

**The cards are drawn into those trapezoids by a live affine mapper.** Every
authored pose record contains the shared renderer's projected upright and
defence quads for all twenty field cells. The 1,920-byte texture is read to RAM,
then an inline-Z80 DDA samples it directly to the VDP data port. A backwards
quad walks the source pointer backwards, so the old mirrored chair texture set
is gone. Empty slots are repaired by redrawing the live arena through a clipped
box; there is no captured slot-art blob.

The selector is a sprite — the spinning gem, projected offline into eight
patterns — so it floats over both GRAPHIC 7 pages and nothing underneath it is
ever touched.  The generator used to bake a flat brown ring just outside every
quad so that a bracket drawn *into* the bitmap could be erased by redrawing a
known colour; with the sprite there is nothing to erase, and the ring was only
paint over the arena, so it is gone.

**A card in defence position is turned a quarter turn**, the way it is on a
table, rather than labelled.  The turn is in the art: the cartridge carries the
card set stored turned (48x40, and 42x32 for the overhead board), so a span
program still walks one texture row forwards per destination row.  In a chair
view the turned card is *inscribed* in its slot — the slots there sit shoulder
to shoulder, and every erase is the slot's own restore tile — while overhead it
is drawn at full size, because the tile pitch is 48 and the card is 42.

**The hand is not board geometry.** It is a flat HUD strip on every target, so
it stays five axis-aligned 40x48 blits in a baked band — which also keeps the
cards a player is choosing between at a readable size.

**A duel opens and changes chairs from live geometry.** Sixteen projected mesh
samples form the shared `opening_camera()` arc; five form the player↔COM orbit.
For every sample the hidden page receives the filled 3-D slab and every occupied
field card in that pose's projected quad before the V-blank flip. Cards therefore
stay attached to the table throughout rotation instead of disappearing between
the endpoint views. The COM hand still uses the common spiral cover.

Placement, summon/fusion/equip/support and position changes retain the arena.
For a placement the hand disappears and the real 40x48 card face travels in
**both axes** -- out of its hand position and up to the middle of the projected
slot it is going into. Each pose saves the arena underneath it into the drawing
page's own offscreen rows before the card is blitted, and puts those exact
pixels back when that page comes round again, so the flight is the opaque card
art rather than an XOR outline and scan-out only ever sees finished poses. The
board then settles -- the live resting arena and every card are redrawn into
their quads -- and the populated view holds with only
the HUD and information panel before the hand returns.

There is no camera lurch under the landing any more. That effect borrowed the
turn strip's neighbour of the resting pose, and because that strip is a half
orbit sampled five times the neighbour is a *quarter turn* away: what played was
a one-frame jump-cut to a completely different camera. Nothing on any other
target moves the camera when a card is placed, so the settle is now one restore
frame and a landing costs one 29 KB stream instead of two.
**Attacks do not.** Monster-versus-monster, direct-hit and trap-counter actions
switch to a black full-screen 2D cut-in, with one or two 88x120 cards rendered
from the same source paintings as the other targets plus live names and
ATK/DEF. The attacker advances through five page-flipped poses, clean and impact
pages alternate at contact, and then the attacker retraces all five poses before
the outcome/damage hold. If an attack-position defender wins, it answers with
its own five-pose counter-lunge, impact and retreat. Every destroyed large card
is then consumed by a six-stage red/gold burn wipe. The runtime reconstructs the
correct 3D resting view only after that; no card teleports from the contact point
back to the field or remains visibly whole after the rules destroyed it.

The player places monsters in attack or defence, plays supports and equips,
builds a multi-card fusion chain out of the hand, attacks, and ends the turn.
Both pages are tracked separately: this screen remembers what each buffer is
showing and paints the difference, at most one card a frame.

### Final issue pass

The seven issues in `MSX2_ISSUES_LAST_PLAN.md` are implemented. The hand deal
now has separate target and landed masks for each VRAM page, so a five-card COM
hand cannot be painted as three cards on one page and five on the other. The
overhead placement path composes one hidden page, copies it to the other page,
and presents the completed result; transition entry hides the selector, attack,
and destruction sprite layers before any stream or view cut. The old `SUMMON`,
result verdict, and title-return instructions are no longer emitted.

SAVE GAME stays in its save picker phase: disk absence and write failure are
messages in that phase, while B returns to the map. Human duel seeds combine
the RTC when valid, the Z80 refresh register, frame timing, and input timing;
debug, regression, and fixed-seed builds retain deterministic seeds. The
regression ROM records these boundaries in the RAM probe rather than relying on
an all-black screenshot or on an emulator process merely staying alive.

### Second pass on the same report

Three of the four issues re-reported after that pass were reproduced in real
openMSX and fixed.

**The result cues looped.**  `g_msx2_music_assets` is `const`, so the linker
puts it in the code area -- and the code area runs past 0x8000, which is the
NEO window `Msx2_AudioStartRequested()` maps the recording through.  The `loop`
flag was still being read from the table as the argument to `LVGM_Play()`,
*after* that mapping, so what reached the player was a byte of the music
stream: Victory and Fail started with `LVGM_STATE_LOOP` set and never stopped.
Every asset field is now taken before the window moves.  Measured through
`g_LVGM_State` at 0xC221 in the regression fixtures: win 0x82 -> 0x80 and the
player idle after 13 s, loss idle after 19 s, while the title track still shows
0x82 with an advancing pointer after 100 s.

**The selector survived a turn handoff.**  Passing from a chair takes the
overhead cut nobody makes -- there is none between two chairs -- so nothing hid
the gem before `Msx2_BoardStepCameraMove()` flipped its first pose in.
`Msx2_BoardShowCursor()` does hide it on `M_TURN`, but that is the top of the
NEXT frame.  Sprite attribute 29 measured at Y=144 for 0.45 s of swing before
the fix and hidden within one frame of the press after it;
`Msx2_BoardSwitchView()` now calls `Msx2_SpriteTransitionBegin()` first.

The gem leaks in two places, not one.  The other is the opponent's wind-up
beat: `FX_COM_CHOOSE` is the one effect that deliberately stands the selector on
the cover the opponent is holding, once a frame, so
`Msx2_BoardStartFx()`'s hide does not hold for it -- and the followup composed
in `Msx2_BoardFinishFx()` (a support card's full-screen cut-in, THUNDER among
them) arrived with the gem still on the screen.  `Msx2_BoardFinishFx()` now
hides it before anything composes.  This one is fixed by inspection of that
path, not by capture: a COM support play is not schedulable in the soak.

**The turn number could read two different values on the two pages.**  The page
mask says who owes a repaint; it cannot say what a page is showing, and several
paths level the buffers with a VRAM copy that can put an old strip back on a
page whose bit was already spent.  Each page now records the three figures it
was painted with and `Msx2_BoardHudSync()` re-owes any page that has drifted,
every frame, so the alternating state cannot persist.

**SAVE GAME leaving the road was an input-repeat chain.**  The screen itself is
correct -- the map row, the picker, both destinations and the continue code were
driven with scripted keys on `C-BIOS_MSX2` and on `C-BIOS_MSX2_DISK`, from a
fresh story and after a completed story duel, and every one opened.  What the
report describes is a HELD key: SAVE GAME is the only three-screen chain on the
road (picker -> continue code -> back to the road, each composed with a blanked
stream), so a confirm arriving once per transition walks the player straight
through it and out the far side, where the next press starts whatever the road's
cursor is now on.  The deck editor is a single screen and does not show it.
Reproduced by injecting a 20 Hz repeat for two seconds: before, it walked to the
road and past it; after, one press is one answer and the picker stays.

The first attempt at this rate-limited the confirm button by counting quiet
V-blanks, and it did not hold: it is a guess about how a host repeats a key, and
the report came back unchanged.  What is in the ROM now has no durations in it
at all.

* The V-blank scan records BOTH edges -- `g_msx2_kb_edge` for presses,
  `g_msx2_kb_up` for releases -- so a press and its release inside one long
  main-loop pass are both seen.
* A confirm counts only while the latch is armed.  Only a release arms it; an
  honoured confirm disarms it.  A key that is held is not released, so it cannot
  answer twice however long it is held, and a key that is tapped answers every
  time however fast.
* `Msx2_InputFlush()` drops everything the scan collected and leaves the latch
  disarmed.  Every screen calls it at the moment it becomes visible: a press
  made while a screen was being composed was aimed at the screen before it, and
  a blanked stream is long enough for several.  Map, save picker, continue code,
  deck editor, reward, dialogue, narration, load picker, title and the duel
  board all do this.
* The continue-code screen leaves on ESC only.  Every step into the save flow is
  the confirm button and the one step out of it is not, so no amount of confirm
  -- from a repeat the game cannot see, or from a player leaning on the key --
  can walk out of the flow and start something on the road.  The prompt reads
  `ESC: RETURN TO THE ROAD`.

Directions are deliberately not filtered: walking a menu with a held key is
reasonable, and a repeated direction only moves a cursor the screen then shows.
Three seconds of injected 20 Hz repeats on the SAVE row now stop at the continue
code; deliberate presses still walk the whole flow and ESC still returns.

**The earlier note, kept because it bounds the search:**  The map row, the
picker, FLOPPY/PASSWORD, and the continue-code screen were driven with scripted
keys on `C-BIOS_MSX2` and on `C-BIOS_MSX2_DISK`, from a fresh story and after a
completed story duel: the picker opens every time.  Whatever the reported
machine does differently -- a real BIOS in an expanded slot 0, or a real disk
interface answering the `Msx2_DiskFind()` probe -- is not visible here, and the
probe is the one thing that screen does which the deck editor does not.

Music is now banked lVGM for PSG, MSX-MUSIC/OPLL, and MSX-AUDIO. The title has
a native recording for each chip, and the boot probe selects the best detected
set. `tools/msx2/gen_msx_audio.py` validates the VGM sources in `msx_music/`,
runs `vgm_cmp -justtmr`, checks timed register events and rendered PCM, then
invokes MSXzip with `--simplify --split 16K`. All current optimizer candidates
were rejected because `vgm2wav` rendered their alternate long-wait encodings
differently; the
original recordings are therefore intentionally retained as the final lVGM
inputs. The resident player maps one 16 KB segment per V-blank tick, handles
segment and loop markers, writes the selected chip, and restores the code bank.
Its notification path calls the fixed-code handler directly from the ISR; it
does not use SDCC's indirect callback trampoline while the 0x8000 window is
banked. Duel entry also queues the new track only after both board pages have
finished composing.
The final openMSX trace reached the battle stream with valid segment progress,
loop/error counters, and a valid probe checksum.

### Story mode

`STORY MODE` from the title runs the whole thing:

* the **opening** — Serena's four remembered lines, typed into the text box over
  the desert painting with her bust standing on it;
* the **sanctum map** — the stage the frontier has reached, the five opponents
  with their titles, everything past the frontier shown `- SEALED -`, and
  `LEAVE THE ROAD` back to the title. A cleared opponent can be replayed without
  moving the frontier, which is `g_story_progress` versus `g_story_duel_index`
  in `src/main.c` by another name;
* the **dialogue** — the nine or ten lines before each duel, in the writing's own
  order;
* the **duel**, dealt against the selected opponent on the stage's arena;
* the **ending** — the four closing lines over the ending painting, once the
  fifth opponent falls.

**A dialogue beat is a composited visual-novel scene** (§14.2), not a flattened
picture of one. The backdrop is one of the shipped `assets/source/bg/`
paintings with the text box baked into it, streamed **once per scene**. Serena
stands on the left and the opponent on the right, both on screen at the same
time, blitted at runtime from baked run tables; the inactive speaker is dimmed
rather than removed. The two brightness variants are cut from the same alpha
mask, so they cover byte for byte the same pixels and a speaker change is a pure
overwrite of two rects — no stream, no flip, no background repair. That replaced
about 2.2 MB of per-(duel, speaker) composites with 393 KB of busts.

The run table and the pixels are stored apart rather than interleaved, which is
the one departure from §14.2's letter: a bust is then blitted with the rectangle
copy the port already had, one call per opaque run, instead of a new skip-list
inner loop. Same arithmetic, one fewer piece of assembly.

The text pipeline is untouched. Story prose is still a **fixed-stride record
table in the cartridge**, parsed straight out of `src/main.c` by
`gen_msx_scenes.py`: ten kilobytes of prose is ten kilobytes the 32 KB code
budget does not have, and a second hand-copy of five thousand words of dialogue
is a second copy that goes stale. The typewriter is still per page, so a line
appears character by character on a double-buffered screen without the box being
redrawn.

The story loop is now completeable on the shipping build.  A run starts with an
eight-letter name, advances through five frontier duels, stores one deterministic
reward after each win, and enters the ending after the fifth reward.  The map also
offers a deck editor and a continue-code screen.  Codes contain 3 progress
bits, 40 name bits, four 7-bit deck overrides, and an 8-bit checksum, packed into
16 symbols from a 32-character alphabet.  Loading reconstructs the starter deck
and the earned reward collection before applying the four saved overrides.

The deck editor is the one the other targets have -- a DECK tab and a STORAGE
tab, the whole of whichever is up listed on screen, an art panel for the card
under the cursor, and one button that trades a card between the two sides --
with two departures the hardware and the save format force:

* **A named list, not a grid of faces.** A card face is 1,920 bytes and
  `Msx2_StreamRect` has to pace itself at 32 T-states a byte with the display
  on, so the eighteen faces of the PC-FX grid would be a third of a second of
  repaint per keypress. One face beside a list of names costs one, fits a
  256-wide screen, and answers the joystick at once. The list is drawn in card
  order so a card's copies sit together, the window pages a screenful at a time
  rather than a row (a scroll is ten names and ten lines of glyphs, twice), and
  a cursor step repaints only the two rows that changed and the art panel.
* **An exchange, not an add and a remove.** The cartridge has no battery, so the
  save is the continue code, and the code carries four card ids against a deck
  the seed regenerates. A deck that could grow or shrink could not be written
  down at all, and no more than four of its cards may differ from the dealt one.
  So the deck is always forty cards and a swap always trades one of them for one
  in storage. The player may pick *any* of the forty: the editor moves the
  chosen card into one of the four slots the code can record before swapping it,
  which is invisible because the deck is shuffled before every duel and the list
  is sorted. The fifth distinct change is refused with a message rather than
  made and lost.

### Uninitialised statics are not zero on this target

SDCC puts every zero-initialised static in `_DATA`, and MSXgl's ROM crt0 never
clears it -- it only copies `_INITIALIZER` over `_INITIALIZED`. A static that is
not given a value explicitly therefore starts as whatever the machine left in
RAM. That is not a theoretical hazard: it froze the board. `g_hand_hidden`
booted non-zero, so a summon never took the hand off the screen and never
computed the card's destination; `g_fx_bend` booted at 102 instead of 2, so the
cleanup pass streamed a hundred camera poses -- half a minute of a board stuck
in a mid-orbit pose -- before the effect could end.

`main()` now wipes `_DATA` once, above the fifteen bytes crt0 has already filled
in (heap pointer, ROM slot id, ROM/MSX version, NEO segment shadow), with a
single `LDIR` and interrupts held off. `pack_msx_rom.py` fails the build if the
link ever puts a game variable inside that reserved prefix, or moves a crt0
variable out of it. `Msx2_BoardEnter()` separately resets the landing state, so
a *second* duel starts from rest as well -- the boot wipe only runs once.

The wipe also exposed a second latent bug it had been hiding: the soak deals a
story duel without ever walking into story mode, so it was dealing whatever
`g_story_deck` happened to contain. `Msx2_StoryPrepareDuelDeck()` now builds the
starter deck if one has never been built.

### M1a evidence

`./msx2.sh verify --seconds 300` builds the **soak** ROM (`make -f Makefile.msx2
soak`, i.e. `-DMSX2_DEBUG_AUTOPLAY`, which hands the player's turn to the COM's
own AI) and reads the state probe back out of a RAM dump:

```
status       OK
duels done   1   player 1  /  com 0
RAM          data ends 0xD1EF, SP 0xF361, 8562 bytes free between them
```

Complete duels, cycling the five story opponents and free battle, with no
watchdog trip and no failed state invariant. The duel rules, the deck builder
and the AI all run on a Z80 inside the RAM budget.

The duel count is far lower than the 417 the renderer-less M1a build managed in
900 seconds, and that is the soak measuring the *presented* game: it takes one
rules step every frame (a person takes one every few seconds), streams a fresh
54 KB arena, plays the opening and turn presentation strips, and rasterises
cards into perspective quads underneath all of it. The number to watch here is
`status`, not the rate.

Footprint (`./msx2.sh ram`, 2026-09-03): 30,800 bytes in `_CODE`, and the three
page-0 banks at 9,414 (duel), 1,831 (modal) and 9,391 (story) of 16,384 each.
Before the split, the duel and story screens shared segment 2 and were within a
couple of hundred bytes of filling it, which is not a budget you can add a
feature to; the soak and story-soak variants are larger still, so every change
has to be checked against all three builds.  Its RAM report is 6,538 bytes used
from `0xC000` through `0xD98A`, with 6,646 bytes free to `HIMEM` (`0xF380`);
the runtime probe measured 6,615 bytes between static data and the live stack.
The RAM ISR accounts for 452 of the difference from the earlier figure and the
disk sector buffer for 512.  `pack_msx_rom.py` rejects any of the four code
areas if its linker-reported end crosses the mapped bank boundary.

### M7 evidence

`make -f Makefile.msx2 story-soak` builds a test-only variant that enters the
real story scene, advances its dialogue and reward screens, and crosses each
requested fight boundary as a win.  Complete rules/board duels remain covered
by the ordinary soak; keeping those gates separate makes story completion
deterministic.  A real openMSX run with `./msx2.sh run --seconds 2000 --no-keys`
completed all five story fights and entered the ending with `status OK` and
`duels done 5`.
The shipping path is separately exercised through real keyboard-matrix captures
of the name-entry, visual-novel, map, deck-editor, and continue-code screens.

### Final issue verification — 2026-09-04

The final regression ROM was run with `/usr/local/bin/openmsx` 20.0-rc1, not
the bundled broken headless Z80. The 20-second probe reported battle turn 4,
`deal_target_mask=0x1F`, `deal_landed0=0x1F`, `deal_landed1=0x1F`, 44 page
flips, three full-view streams, three blank pairs, music track 5 in segment
487, decoder pointer 486, 792 audio ticks, zero loops/errors, and
`checksum_ok=true`. The 60-second probe remained checksummed and advanced the
same stream to pointer 4265 after 3,176 audio ticks. Both decoded PNG frames
were non-black and `tools/msx2/compare_sequence.py` accepted the capture.

The final regression link measured `_CODE=32,253`, segment 2 at 12,727 bytes,
segment 3 at 2,671 bytes, and segment 4 at 9,374 bytes. The shipping link
measured `_CODE=31,782`, segment 2 at 12,502 bytes, segment 3 at 2,977 bytes,
and segment 4 at 9,361 bytes. The mapper check passed with
`./msx2.sh neo-test`.

This is emulator evidence only. Physical MSX2 verification of audio tempo and
segment crossing, RTC entropy, selector/hand transitions, and floppy DSKIO is
still pending; no physical-hardware result is inferred from the openMSX run.

After the diagnostic pass was removed, the clean shipping-source soak was run
again with `./msx2.sh verify --seconds 300`. Real openMSX ended with `status OK`,
one completed duel (`duels done 1`, player 1 / COM 0), turn 10, and 1,234
current-duel steps; the mapper check passed in the same run. The soak ROM was
then replaced by the shipping build before the final title capture.

---

## Build and verify

```
make -f Makefile.msx2            # regenerate tables, build the ROM
make -f Makefile.msx2 soak       # ... the same ROM, playing itself
./msx2.sh verify --seconds 300   # build the soak ROM, run blind, read the probe
./msx2.sh ram                    # code/RAM footprint against the budgets
```

**`verify` builds the soak ROM, and leaves it in `out/`.** The shipping build
waits for a hand on the joystick, so a blind run against it sits on turn 1 for
the whole run and reports a hang that is really an empty chair. Rebuild with
plain `make -f Makefile.msx2` before taking screenshots of the played game. A
variant stamp now makes that command rebuild automatically when `out/` contains
the soak ROM; MSXgl's incremental builder does not track changed `-D` flags by
itself.

`MSXGL_PATH` selects the MSXgl tree (default `MSXgl-main`), `ROM_SIZE_KB` the
cartridge size. The default is now 8192: the packed assets use 6,004 KB after
adding the sixteen-pose four-stage opening and 72 large battle cards. The
shipping cartridge can remain 16384.

`./msx2.sh shot` photographs the game by dumping VRAM and decoding it
(`tools/msx2/vram_png.py`) rather than by asking the emulator for a screenshot:
openMSX needs a display for that and its GL renderer returns an empty frame
under Xvfb. `--page 0|1` chooses which GRAPHIC 7 page to decode, including the
hidden one. Note that GRAPHIC 6/7 interleave VRAM across two 64 KB banks, so a
raw read of the emulator's `physical VRAM` shows every picture twice at half
width; the decoder undoes that.

`./msx2.sh neo-test` runs the NEO Mapper 1.2 large-ROM regression through the
project OpenMSX command layer. `./msx2.sh header`, `./msx2.sh asm`, and
`./msx2.sh cov` expose ROM analysis through the same layer. Scripted keyboard
input is available with `./msx2.sh input`; it uses the bundled deterministic
backend's active-low MSX PPI keyboard matrix.

### Do not use the bundled openmsx-headless to run this ROM

`MSXgl-main/openmsx-headless-.../openmsx` has a **broken Z80 core**: it does not
implement `LD r,(IX+d)` and `LD (IX+d),r` for `r != A` — the load or store is
silently skipped, registers keep their old values. SDCC uses IX as its frame
pointer and emits those two instructions constantly, so every C function with
stack locals computes garbage and the ROM dies within a few hundred
instructions. This was proved with a 20-instruction hand-written test ROM:

```
ld ix,#C300 / ld hl,#1234 / ld (ix-3),l / ld (ix-2),h
ld hl,#0000 / ld l,(ix-3)  / ld h,(ix-2)     ->  HL = 0000, should be 1234
```

Real openMSX gives `1234`. `openmsx selftest` still reports `z80 execution: ok`,
so the selftest does not cover these opcodes. Use that build only for what it
was patched for — NEO mapper and SCREEN 7/8 capture experiments — never to run
compiled C.

`msx2.sh` therefore drives **real openMSX** (20.0-rc1 at `/usr/local/bin/openmsx`,
which knows the `NEO-16` romtype), windowless via `set renderer none`.

### How a blind run is observed

The game consumes the input layer, so the *shipping* ROM is driven the way a
player drives it: `msx2.sh` presses keys through openMSX's keyboard matrix from
Tcl (`--keys "6.0:space 6.8:space"`), and `./msx2.sh shot --page 0|1` decodes
either GRAPHIC 7 page to a PNG. That is how every screen below was checked.

The blind probe is for the *soak* ROM, which plays itself: it stamps its state
into RAM every frame (`msx2_probe.c`), the run script dumps all 64 KiB of
CPU-visible memory, and `tools/msx2/read_probe.py` finds the struct by its magic
and prints it. `MSX2_STAGE(n)` marks how far `main()` got, which turns a boot
crash into a number instead of a black screen.

---

## Layout

| File | What |
|---|---|
| `msx2_main.c` | boot, vblank ISR, the scene loop, the blind autoplay driver |
| `msx2_title.c/.h` | title screen: streamed art, logo, attract prompt, menu |
| `msx2_board.c/.h` | the duel screen: the ten projected slots, both chair views, the hand strip, cursor, HUD, turn strip, and action cels |
| `msx2_battle_fx.c/.h` | resident 2-D cut-in primitives: impact burst, result text and the staged destruction wipe |
| `msx2_raster.c/.h` | live affine card mapper with an inline-Z80 texel DDA |
| `msx2_story.c/.h` | story mode: name entry, map, dialogue, deck editor, rewards, continue codes, ending |
| `msx2_story_utils.c` | resident story hashing, card thumbnails, grid navigation, and password codec |
| `msx2_video.c/.h` | GRAPHIC 7 layer: pages, fills, glyphs, 2x text |
| `msx2_input.c/.h` | joystick + keyboard, latched once per frame |
| `msx2_stream.c/.h` | cartridge segment -> VRAM streaming through the 0x8000 window |
| `msx2_duel.c/.h` | the duel rules: board, LP, battle, supports, fusion, turn order |
| `msx2_cards.c/.h` | card stat tables (generated) |
| `msx2_probe.c/.h` | the headless observation channel |
| `msx2_bank.c/.h` | the switchable page-0 code window, and the trampoline per banked entry point |
| `msx2_screens.c/.h` | the card check screen and the fusion cut-in (modal bank) |
| `msx2_sprite.c/.h` | the V9938 sprite layer: burst, result word, the spinning selector |
| `msx2_disk.c/.h` | the continue code on a floppy, through the disk ROM's DSKIO |
| `msx2_story_load.c/.h` | LOAD STORY: is the save on a disk or on paper? |
| `waifu_msx2_s2_b0.c` | page-0 bank, segment 2: the duel screen |
| `waifu_msx2_s3_b0.c` | page-0 bank, segment 3: the modal screens |
| `waifu_msx2_s4_b0.c` | page-0 bank, segment 4: the story screens |
| `msx2_audio.c/.h`, `msx2_lvgm.c`, `msx2_psg.c` | resident V-blank multi-chip lVGM playback, bank seam, and probe counters |
| `msx2_libc.c` | `time()`/`clock()` for the shared deck builder |
| `compat/waifu_assets.h` | shim so `src/game/deck.c` compiles without the 5.3 MB asset header |
| `project_config.js`, `msxgl_config.h` | MSXgl build configuration |

Shared code compiled unmodified: `src/game/deck.c`, `src/game/ai.c`, plus
`card_ids.h` and `deck_pools.h`.

Generated: `src/generated/msx2_card_tables.h` from `tools/msx2/gen_msx_tables.py`
(ATK/DEF/attribute/tribe lifted out of `waifu_assets.h`, 432 bytes of ROM), and
`src/generated/msx2_scenes.h` plus `src/msx2/assets/*.bin` from
`tools/msx2/gen_msx_scenes.py` (full-screen art nearest-colour quantised to
GRB332, without dithering).

`tools/msx2/gen_msx_views.py` is the board's own generator, imported by
`gen_msx_scenes.py` so the cartridge segment map stays owned by one tool. It
compiles `src/main.c` with `-DWAIFU_MSX2_VIEW_DUMP` into
`build/msx2_capture/waifu_msx2_dump`, runs `--dump-msx2-views`, and serialises
the projected arena mesh plus upright/defence card quads for every resting and
camera-move pose. **`src/main.c` is therefore a build input of the cartridge**:
a change to the arena, card geometry, or camera re-bakes those records, and
`Makefile.msx2` says so. The former board pictures, empty-slot tiles, span
programs, camera strips, and mirrored chair textures are no longer packed.
That moves the last asset from segment 493 to 227 and reduces packed cartridge
use from about 7.5 MiB to 3.2 MiB (the ROM container remains 8 MiB).
`tools/msx2/pack_msx_rom.py` writes those binaries into the built cartridge at
the segments the header names, and fails the build if the streamer has drifted
above 0x8000.

### How streaming works, and the two rules it lives by

A GRAPHIC 7 screen is 54,272 bytes, so scenes are never linked — they are packed
into whole 16 KB NEO segments and pushed at the VDP through the 0x8000 window
(`msx2_stream.c`). Two constraints make it work:

1. **The copy runs with interrupts off.** While the window holds picture data,
   none of the code up there exists — including the ISR. Interrupts come back
   between segments, so the longest blackout is one 16 KB chunk (~70 ms), and
   the streamer itself must be linked *below* 0x8000. `pack_msx_rom.py` checks
   that on every build rather than trusting it.
2. **The display is blanked for the duration.** In GRAPHIC 7 with the screen on
   the VDP wants ~29 T-states between VRAM writes and `OTIR` gives it 21, so
   bytes would be dropped. This is also why key presses are ignored while a
   scene streams, and why `msx2.sh`'s default key sequence starts at six
   seconds.

---

## Open issues, in priority order

1. **M1b timing truth is not started.** The current budgets are still engineering
   estimates; a dedicated timing ROM must measure OUTI spacing, VDP commands, the
   affine-card path, and eventual sound replay before those figures can be called
   hardware measurements.

2. **The AI is slow.** Roughly 80 ms per rules step, most of it in
   `waifu_ai_choose_com_*` plus rebuilding `WaifuAiState` (226 bytes) for every
   query. It shows as the COM taking a beat to answer, which reads as thinking
   rather than as a stall, and the board keeps repainting underneath it — but it
   must be measured properly at M1b, and the state build should be made
   incremental if a turn ever visibly hangs.

3. **The floppy save is written but has never run against a drive.**  LOAD STORY
   offers FLOPPY DISK or PASSWORD, the slot probe is exercised (it correctly
   reports "no drive answered" on C-BIOS), and the BIOS-in-page-0 swap that the
   probe needs is proven by the fact that selecting LOAD STORY no longer hangs.
   What is NOT proven is DSKIO itself: no emulator configuration here has a disk
   ROM, so the read and the write have never executed.  The password path
   remains the guaranteed persistence mechanism.

4. **Physical sound remains unverified.** The shipping build now contains
   PSG, OPLL, and MSX-AUDIO lVGM tracks, and `Msx2_SfxPlay()` now synthesizes
   nine short PSG cues on channel C: selection, confirmation, card placement,
   destruction, draw, turn hand-off, laser, direct hit, and loss.  The effect
   temporarily borrows channel C, then restores the music register state; no
   physical MSX2 audio test has been run here, so its tempo and mix still need
   confirmation on hardware.

5. **Deck-editor UX is intentionally compact.** The plan's 5x4 paginated icon
   grid is reduced to a four-slot thumbnail row plus a one-card collection
   carousel to stay inside the current ROM/RAM and page-flip budget.  It still
   supports selecting a deck slot, swapping a reward card, and returning to the
   map.

---

## The music does not stop for the screen any more — 2026-09-05

The lVGM decoder has always run in the V-blank handler, so "the tune hangs on a
note whenever the screen changes" was never about the player: it was about the
handler not being *reached*.  Two places held interrupts off for far longer than
a frame.

* **The scene streamer.** `Msx2_StreamChunk()` disabled interrupts for a whole
  16 KB OTIR -- about 344,000 T-states, six V-blanks -- because the ISR executes
  out of the same 0x8000 window the picture is mapped into.  It now gives the
  window back every 256-byte page and opens one instruction of EI there, so a
  pending V-blank is taken between pages.  The VDP's write pointer survives it
  untouched, because the ISR does no VRAM work at all -- that rule is what makes
  this legal.
* **The command-engine wait.** `Msx2_CmdOut()` polled CE inside the same DI as
  its fifteen-register burst.  A line of text drawn straight after a page copy
  waits on an LMMM over 54,000 pixels, tens of milliseconds, every one of them a
  frame the music never got.  The poll now closes and reopens the window each
  turn; S#2 still never outlives a DI, which is the debt it owed all along.

Measured off `g_msx2_ticks`, which the ISR increments and nothing else touches,
on the same scripted run before and after:

| Window | Frames expected | Before | After |
|---|---|---|---|
| Title into the story, 8.0-12.6 s | 276 | 256 | **276** |
| The whole run, 8.0-20.0 s | 720 | 699 | **719** |
| Duel entry and the first cut-ins, 22-30 s | 480 | 462 | **479** |

---

## The MSX2+ cartridge (`make -f Makefile.msx2 plus`)

A second ROM, `src/msx2/out/waifu_msx2p.rom`, built from the same sources with
`-DMSX2_PLUS` and `Machine = "2P"`.  It runs on an MSX2+ or turboR and puts
every 2-D picture screen in **SCREEN 10 (YJK + YAE)**.

**Why it is a separate cartridge and not a runtime check.** The difference IS
the pictures: a YJK byte and a GRAPHIC 7 byte are the same byte read two ways,
so one image cannot hold both encodings of the title, the story and the busts.
Everything else -- the code, the segment map, the streamer, the page flip, the
command engine, the sprite plane -- is identical, and `gen_msx_plus.py` writes
its assets at exactly the sizes and segments the MSX2 set uses so the same
packer places them.

**What is YJK and what is not.**

| Screen | Mode | Why |
|---|---|---|
| Title, ending, story narration, talks, sanctum road, continue code, save/load, deck editor, reward | SCREEN 10 | They are paintings and text |
| The duel board, the attack cut-in, the fusion cut-in, the card check | GRAPHIC 7 | The board is rasterised live by the Z80 and its 40x48 card textures are shared with the hand; a chroma group four pixels wide would cost every card edge for nothing the eye gets back.  This is also what the owner asked for: in game it stays SCREEN 8, thumbnails included |

The mode is decided by the door a screen comes in at, not by a flag each screen
sets: `Msx2_StreamScene()` is the switch to a picture screen and declares YJK;
the duel and its cut-ins stream through `Msx2_StreamSceneBlanked()` and declare
GRAPHIC 7 (`Msx2_VideoModeG7()` in `Msx2_BoardEnter`).

**Text is YAE, and that is the point.** In SCREEN 10 a byte with bit 3 set is a
palette colour at full accuracy owing its neighbours nothing; every other byte
is a brightness plus a share of its group's hue.  Interface has to be the first
kind or a word takes its colour from the painting three pixels away, so the
SCREEN 10 translation units get their ink from `msx2_plus.h`, which redefines
`MSX2_GOLD` and the rest as YAE pixels.  The panels behind the words are stamped
flat in the baked picture for the same reason.

**The palette.** Sixteen entries: 0..7 are the interface's, identical in every
scene, and 8..15 are fitted to each picture by k-means over the pixels YJK
reproduces worst -- which is the dark end, where Y moves in steps of two.  That
fit is worth about 3.3 dB and is the whole difference between this encoder and a
good one.  A scene carries its own 32 bytes in the slack after it (a 54,272-byte
picture inside four 16 KB segments leaves eleven kilobytes), so no asset,
segment or header symbol had to be added to hold them; `Msx2_VideoScenePalette()`
reads them straight after the stream.  Sprites are unaffected either way: in
GRAPHIC 7 and its YJK derivatives the V9938/58 colours sprites from a fixed
table, not the palette.

**The one asset it adds.** `cards_yjk` -- a YJK copy of the 40x48 thumbnails --
because the deck editor and the reward reveal are SCREEN 10 screens that show a
card and the duel still needs the GRAPHIC 7 originals.  It is emitted into a
plus-only manifest and header (`src/generated/msx2_plus_scenes.h`) so the MSX2
cartridge never carries it; it is also why the plus ROM is 8 MB where the MSX2
one is 4.

**Busts.** A bust is blitted over a picture, and a YJK pixel cannot be written
alone -- three of its neighbours share its hue.  Every opaque run is therefore
rounded INWARD to a chroma group, and the pixels left over are merged one at a
time, keeping the hue already in VRAM and carrying a brightness baked from the
figure COMPOSITED over that backdrop -- one edge table per talk scene, chosen at
runtime by stage.  Both bust positions are pinned to multiples of four so a run
aligned inside the rectangle is aligned on the screen.

**The converter.** `tools/msx2/msx2_yjk.py` encodes and decodes YJK/YJK+YAE with
optional Floyd-Steinberg (`--dither 0.9`, what the assets are baked with), an
optional per-picture palette fit (`--fit-yae 8`), SCREEN 12 support, and a
`compare` mode.  Checked against the owner's Dadither reference conversion of
the same title art (`assets/source/msx2/title256_msx2plus.gla`): 37.07 dB
against 37.17 dB, and visually indistinguishable.

**Is a YJK 3-D rasteriser possible?** In theory yes, and it would be slower than
what is there.  The span filler writes one byte per pixel either way, so a flat
span costs the same -- but a card edge or a floor tile boundary that falls
inside a group of four cannot change hue there, so either the quad snaps its
edges to multiples of four (visible stair-stepping on every rotation) or the
filler reads back the group, merges the chroma and writes it again, which turns
a `outi` stream into a read-modify-write and roughly doubles the cost of the one
loop the duel's frame time is already made of.  The board's own art would also
have to be re-baked per rotation pose, because a texture's chroma groups only
line up with the screen's if the quad lands on a multiple of four.  A YJK
*background* (the floor, which is flat-shaded bands) would be affordable; a YJK
board with cards on it is not, at 3.58 MHz.

---

## Deliberate divergences from `src/main.c`

Noted here so they are decisions, not drift:

* Card ids are `u8` with `0xFF` for empty, not `int` with `-1`.
* Traps are owner-generic rather than player-only. The AI never sets one, so the
  two are behaviourally identical.
* The 25-entry `WaifuBattlePhase` collapses to five phases: everything in the
  original that exists to time an animation belongs to presentation here.
* One portability fix landed in shared code: `src/game/ai.c` used `-999999` as a
  score sentinel, which does not fit a 16-bit `int`. It is now `-30000`; real
  scores stay inside ±10000, so no target changes behaviour.
