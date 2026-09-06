/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_disk.h — loading things off the floppy.
 *
 *  The game keeps TOS resident: it owns the screen, the palette and the
 *  interrupt vectors, but GEMDOS is untouched and still owns the disk.  So
 *  loading is Fopen/Fread/Fclose and nothing more exotic, which is also what
 *  makes the same build work from a hard disk or a Hatari GEMDOS drive.
 *
 *  Paths are given relative to the program's own drive, with backslashes,
 *  because that is what GEMDOS takes.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_DISK_H
#define WAIFU_ATARIST_DISK_H

#include <stdint.h>

/* Read a whole file into `buf`.  Returns the byte count, or -1 if the file is
 * missing or bigger than `max`. */
int32_t Atarist_DiskLoad(const char *path, void *buf, int32_t max);

/* Bring one music track into the resident music buffer and hand it to the
 * player.  Only one track is resident at a time -- the streams are up to 29 KB
 * and a 512 KB machine has better uses for the other 200 -- so this stops the
 * player first: the VBL is reading the very bytes Fread is about to overwrite.
 * Returns non-zero on success; a missing file is not fatal, the game just
 * plays silently. */
int  Atarist_MusicLoadTrack(int track);

/* Called once at boot, before any track is asked for. */
int  Atarist_DiskInit(void);

#endif /* WAIFU_ATARIST_DISK_H */
