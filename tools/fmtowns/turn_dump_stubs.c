/* Host-side stubs for the FM TOWNS turn-board-cache dump build.
 *
 * Building src/main.c with -DWAIFU_FM_FMTOWNS on the host gives the same board
 * geometry and the same i386 rasterizer the console payload uses, without the
 * FM TOWNS platform layer.  Only the audio seam is left dangling; the dump
 * renders board pixels and never plays a sound.  See dump_turn_board_frames()
 * in src/main.c for the full regeneration recipe.
 */

void waifu_fmtowns_sfx_play(int effect)
{
    (void)effect;
}
