#include "pcfx_biosfs.h"

/* Public BIOS filesystem dispatcher indices recovered from the BIOS table. */
#define FSYS_INIT   0
#define FSYS_FORMAT 4
#define FSYS_OPEN   7
#define FSYS_READ   8
#define FSYS_WRITE  9
#define FSYS_SEEK   10
#define FSYS_CLOSE  12
#define FSYS_REMOVE 13
#define FSYS_MKDIR  15

extern int pcfx_biosfs_call0(int index);
extern int pcfx_biosfs_call1(int index, u32 a0);
extern int pcfx_biosfs_call2(int index, u32 a0, u32 a1);
extern int pcfx_biosfs_call3(int index, u32 a0, u32 a1, u32 a2);

int pcfx_biosfs_init(void *heap, u32 heap_size)
{
    u32 start = (u32)heap;
    u32 end = start + heap_size;
    if(!heap || heap_size < 1024U) return -1;
    return pcfx_biosfs_call3(FSYS_INIT, start, end, 0);
}

int pcfx_biosfs_format(const char *path)
{
    static const char ok[] = "FSYS_FORMAT_OK";
    if(!path) return -1;
    return pcfx_biosfs_call2(FSYS_FORMAT, (u32)path, (u32)ok);
}

int pcfx_biosfs_mkdir(const char *path)
{
    if(!path) return -1;
    return pcfx_biosfs_call1(FSYS_MKDIR, (u32)path);
}

int pcfx_biosfs_remove(const char *path)
{
    if(!path) return -1;
    return pcfx_biosfs_call1(FSYS_REMOVE, (u32)path);
}

int pcfx_biosfs_open(const char *path, u32 flags)
{
    if(!path) return -1;
    return pcfx_biosfs_call2(FSYS_OPEN, (u32)path, flags);
}

int pcfx_biosfs_read(int fd, void *dst, u32 len)
{
    if(fd < 0 || (!dst && len)) return -1;
    return pcfx_biosfs_call3(FSYS_READ, (u32)fd, (u32)dst, len);
}

int pcfx_biosfs_write(int fd, const void *src, u32 len)
{
    if(fd < 0 || (!src && len)) return -1;
    return pcfx_biosfs_call3(FSYS_WRITE, (u32)fd, (u32)src, len);
}

int pcfx_biosfs_seek(int fd, s32 offset, int whence)
{
    if(fd < 0) return -1;
    return pcfx_biosfs_call3(FSYS_SEEK, (u32)fd, (u32)offset, (u32)whence);
}

int pcfx_biosfs_close(int fd)
{
    if(fd < 0) return -1;
    return pcfx_biosfs_call1(FSYS_CLOSE, (u32)fd);
}

int pcfx_biosfs_save_file(const char *path, const void *src, u32 len)
{
    int fd;
    int rc;
    fd = pcfx_biosfs_open(path, PCFX_BIOSFS_OPEN_SAVE);
    if(fd < 0) return fd;
    rc = pcfx_biosfs_write(fd, src, len);
    if(rc >= 0 && (u32)rc != len) rc = -31;
    {
        int close_rc = pcfx_biosfs_close(fd);
        if(rc >= 0 && close_rc < 0) rc = close_rc;
    }
    return rc < 0 ? rc : 0;
}

int pcfx_biosfs_load_file(const char *path, void *dst, u32 cap, u32 *len_out)
{
    int fd;
    int rc;
    int total = 0;
    fd = pcfx_biosfs_open(path, PCFX_BIOSFS_OPEN_READ);
    if(fd < 0) return fd;
    while((u32)total < cap) {
        rc = pcfx_biosfs_read(fd, (u8 *)dst + total, cap - (u32)total);
        if(rc < 0) {
            pcfx_biosfs_close(fd);
            return rc;
        }
        if(rc == 0) break;
        total += rc;
    }
    rc = pcfx_biosfs_close(fd);
    if(rc < 0) return rc;
    if(len_out) *len_out = (u32)total;
    return 0;
}

const char *pcfx_biosfs_strerror(int rc)
{
    switch(rc) {
    case 0: return "OK";
    case -1: return "invalid argument";
    case -15: return "not found";
    case -17: return "not file or path type mismatch";
    case -19: return "path/device unavailable";
    case -24: return "bad file mode";
    case -25: return "bad seek";
    case -28: return "unsupported operation";
    case -30: return "out of BIOS heap";
    case -31: return "short I/O";
    case -32: return "already exists";
    case -45: return "device unavailable";
    default: return "BIOS FS error";
    }
}
