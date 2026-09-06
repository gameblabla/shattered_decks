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

/* Read `len` bytes from `offset` in a file.  Returns the byte count, which the
 * caller should check against `len`: a short read is a truncated or missing
 * file and every user of this treats that as "do without".
 *
 * This exists because DAT/BIG.CRD is 359 KB of battle cards and only two of
 * them are ever wanted at once -- a machine that can hold the whole file could
 * not also hold the board art, the hand art and a music stream. */
int32_t Atarist_DiskLoadAt(const char *path, int32_t offset, void *buf,
                           int32_t len);

/* The same, split so a caller that wants SEVERAL records out of one file pays
 * for the open once.  Opening a file on a floppy is not cheap -- GEMDOS walks
 * the directory and then the FAT chain -- and DAT/BIG.CRD is read from 300 KB
 * in, so the open dominates the read.  Returns a handle, or negative. */
int16_t Atarist_DiskOpen(const char *path);
int32_t Atarist_DiskReadAt(int16_t handle, int32_t offset, void *buf,
                           int32_t len);
void    Atarist_DiskClose(int16_t handle);

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
