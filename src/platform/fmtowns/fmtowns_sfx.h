#ifndef FMTOWNS_SFX_H
#define FMTOWNS_SFX_H

/* Sound effects through the RF5C68 wave-table PCM chip -- see fmtowns_sfx.c
 * for why effects use the PCM chip while music uses CD-DA. */

/* Resets the PCM chip and clears its wave RAM.  Call once at boot. */
void fmtowns_sfx_init(void);

/* Queues an effect (a WaifuSoundEffect value).  Called by the game core
 * through sounds.c's waifu_sound_play(); safe to call several times in one
 * frame, including with the same effect. */
void waifu_fmtowns_sfx_play(int effect);

/* Keys queued effects onto the chip.  Call once a frame, after the frame's
 * present. */
void fmtowns_sfx_update(void);

#endif /* FMTOWNS_SFX_H */
