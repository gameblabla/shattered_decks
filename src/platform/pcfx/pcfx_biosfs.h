#ifndef PCFX_BIOSFS_H
#define PCFX_BIOSFS_H

#include <eris/types.h>

#define PCFX_BIOSFS_PATH_INTERNAL "/SRAM"
#define PCFX_BIOSFS_PATH_EXTERNAL "/CARD"

#define PCFX_BIOSFS_OPEN_READ      0x00000001UL
#define PCFX_BIOSFS_OPEN_WRITE     0x00000002UL
#define PCFX_BIOSFS_OPEN_APPEND    0x00000008UL
#define PCFX_BIOSFS_OPEN_CREATE    0x00000200UL
#define PCFX_BIOSFS_OPEN_TRUNCATE  0x00000400UL
#define PCFX_BIOSFS_OPEN_EXCL      0x00000800UL

#define PCFX_BIOSFS_OPEN_READWRITE (PCFX_BIOSFS_OPEN_READ | PCFX_BIOSFS_OPEN_WRITE)
#define PCFX_BIOSFS_OPEN_SAVE      (PCFX_BIOSFS_OPEN_READ | PCFX_BIOSFS_OPEN_WRITE | PCFX_BIOSFS_OPEN_CREATE | PCFX_BIOSFS_OPEN_TRUNCATE)

#define PCFX_BIOSFS_SEEK_SET 0
#define PCFX_BIOSFS_SEEK_CUR 1
#define PCFX_BIOSFS_SEEK_END 2

#define PCFX_BIOSFS_ERR_NOT_FOUND      (-15)
#define PCFX_BIOSFS_ERR_PATH_UNAVAILABLE (-19)
#define PCFX_BIOSFS_ERR_ALREADY_EXISTS (-32)
#define PCFX_BIOSFS_ERR_DEVICE_UNAVAILABLE (-45)

int pcfx_biosfs_init(void *heap, u32 heap_size);
int pcfx_biosfs_format(const char *path);
int pcfx_biosfs_mkdir(const char *path);
int pcfx_biosfs_remove(const char *path);
int pcfx_biosfs_open(const char *path, u32 flags);
int pcfx_biosfs_read(int fd, void *dst, u32 len);
int pcfx_biosfs_write(int fd, const void *src, u32 len);
int pcfx_biosfs_seek(int fd, s32 offset, int whence);
int pcfx_biosfs_close(int fd);
int pcfx_biosfs_save_file(const char *path, const void *src, u32 len);
int pcfx_biosfs_load_file(const char *path, void *dst, u32 cap, u32 *len_out);
const char *pcfx_biosfs_strerror(int rc);

#endif
