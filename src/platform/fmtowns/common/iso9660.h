#ifndef FMT_ISO9660_H
#define FMT_ISO9660_H

#include <stdint.h>

/*
 * Minimal read-only ISO9660 (level 1, no Joliet/Rock Ridge) filesystem
 * reader for the CD image built by this project's Makefile (mkisofs
 * -iso-level 1). Built on fmt_cdrom_read() (cdrom.[ch]/scsi.[ch]) - call
 * fmt_cdrom_init() once before using anything here.
 *
 * Only handles flat lookups in the root directory (this project's CD/
 * layout doesn't use subdirectories), by plain 8.3-style name with or
 * without the ";1" ISO9660 version suffix - fmt_iso9660_find() accepts
 * either and matches case-insensitively.
 */

/* Looks up `name` (e.g. "IMAGE.RAW", with or without ";1") in the root
 * directory. On success, returns 0 and sets `lba`/`size` to the file's
 * starting LBA and byte length. Returns -1 if not found or the PVD
 * couldn't be read. */
int fmt_iso9660_find(const char *name, uint32_t *lba, uint32_t *size);

/* Convenience wrapper: looks up `name` and reads its whole contents
 * into `buf` (which must be at least as large as the file - use
 * fmt_iso9660_find() first if the size isn't already known). Returns
 * the file's byte length on success, -1 if not found, the read failed,
 * or the file doesn't fit in `bufsize`. */
int32_t fmt_iso9660_load(const char *name, void *buf, uint32_t bufsize);

#endif
