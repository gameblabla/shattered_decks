/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_os.h — the TOS calls the port makes, without MiNTlib.
 *
 *  The build links -nostdlib, so <osbind.h> (which is header-only but drags in
 *  MiNTlib's types) is not used.  These are the six GEMDOS/XBIOS entries the
 *  game needs: memory, files, supervisor mode and the two video/blitter
 *  queries.  TOS stays resident throughout — the game owns the screen and the
 *  interrupts, not the machine — so floppy loading keeps working mid-duel.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_OS_H
#define WAIFU_ATARIST_OS_H

#include <stdint.h>

#define ST_TRAP_CLOBBER "d1", "d2", "a0", "a1", "a2", "cc", "memory"

/* GEMDOS Malloc(). -1 asks for the size of the largest free block. */
static inline void *st_malloc(int32_t size)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.l %1,-(%%sp)\n\t"
        "move.w #0x48,-(%%sp)\n\t"
        "trap #1\n\t"
        "addq.l #6,%%sp"
        : "=r"(ret) : "g"(size) : ST_TRAP_CLOBBER);
    return (void *)ret;
}

static inline int32_t st_mfree(void *p)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.l %1,-(%%sp)\n\t"
        "move.w #0x49,-(%%sp)\n\t"
        "trap #1\n\t"
        "addq.l #6,%%sp"
        : "=r"(ret) : "g"(p) : ST_TRAP_CLOBBER);
    return ret;
}

static inline int16_t st_fopen(const char *name, int16_t mode)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.w %2,-(%%sp)\n\t"
        "move.l %1,-(%%sp)\n\t"
        "move.w #0x3d,-(%%sp)\n\t"
        "trap #1\n\t"
        "lea 8(%%sp),%%sp"
        : "=r"(ret) : "g"(name), "g"(mode) : ST_TRAP_CLOBBER);
    return (int16_t)ret;
}

static inline int32_t st_fread(int16_t handle, int32_t count, void *buf)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.l %3,-(%%sp)\n\t"
        "move.l %2,-(%%sp)\n\t"
        "move.w %1,-(%%sp)\n\t"
        "move.w #0x3f,-(%%sp)\n\t"
        "trap #1\n\t"
        "lea 12(%%sp),%%sp"
        : "=r"(ret) : "g"(handle), "g"(count), "g"(buf) : ST_TRAP_CLOBBER);
    return ret;
}

static inline int32_t st_fseek(int32_t offset, int16_t handle, int16_t mode)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.w %3,-(%%sp)\n\t"
        "move.w %2,-(%%sp)\n\t"
        "move.l %1,-(%%sp)\n\t"
        "move.w #0x42,-(%%sp)\n\t"
        "trap #1\n\t"
        "lea 10(%%sp),%%sp"
        : "=r"(ret) : "g"(offset), "g"(handle), "g"(mode) : ST_TRAP_CLOBBER);
    return ret;
}

static inline int16_t st_fclose(int16_t handle)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.w %1,-(%%sp)\n\t"
        "move.w #0x3e,-(%%sp)\n\t"
        "trap #1\n\t"
        "addq.l #4,%%sp"
        : "=r"(ret) : "g"(handle) : ST_TRAP_CLOBBER);
    return (int16_t)ret;
}

static inline int16_t st_fcreate(const char *name, int16_t attr)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.w %2,-(%%sp)\n\t"
        "move.l %1,-(%%sp)\n\t"
        "move.w #0x3c,-(%%sp)\n\t"
        "trap #1\n\t"
        "lea 8(%%sp),%%sp"
        : "=r"(ret) : "g"(name), "g"(attr) : ST_TRAP_CLOBBER);
    return (int16_t)ret;
}

static inline int32_t st_fwrite(int16_t handle, int32_t count, const void *buf)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.l %3,-(%%sp)\n\t"
        "move.l %2,-(%%sp)\n\t"
        "move.w %1,-(%%sp)\n\t"
        "move.w #0x40,-(%%sp)\n\t"
        "trap #1\n\t"
        "lea 12(%%sp),%%sp"
        : "=r"(ret) : "g"(handle), "g"(count), "g"(buf) : ST_TRAP_CLOBBER);
    return ret;
}

/* XBIOS Setscreen(logical, physical, rez); -1 leaves a field alone. */
static inline void st_setscreen(void *log, void *phys, int16_t rez)
{
    __asm__ volatile(
        "move.w %2,-(%%sp)\n\t"
        "move.l %1,-(%%sp)\n\t"
        "move.l %0,-(%%sp)\n\t"
        "move.w #5,-(%%sp)\n\t"
        "trap #14\n\t"
        "lea 12(%%sp),%%sp"
        : : "g"(log), "g"(phys), "g"(rez) : "d0", ST_TRAP_CLOBBER);
}

/* XBIOS Vsync(). */
static inline void st_vsync(void)
{
    __asm__ volatile(
        "move.w #37,-(%%sp)\n\t"
        "trap #14\n\t"
        "addq.l #2,%%sp"
        : : : "d0", ST_TRAP_CLOBBER);
}

/* XBIOS Blitmode(): -1 queries.  Bit 1 of the result is "Blitter fitted". */
static inline int16_t st_blitmode(int16_t mode)
{
    register int32_t ret __asm__("d0");
    __asm__ volatile(
        "move.w %1,-(%%sp)\n\t"
        "move.w #64,-(%%sp)\n\t"
        "trap #14\n\t"
        "addq.l #4,%%sp"
        : "=r"(ret) : "g"(mode) : ST_TRAP_CLOBBER);
    return (int16_t)ret;
}

/* Supervisor mode.  Implemented in crt0.S because the stack switch Super()
 * performs must not happen behind GCC's back inside a function that has
 * anything on its own stack frame. */
void *atarist_supervisor_enter(void);
void atarist_supervisor_leave(void *old_ssp);

#endif /* WAIFU_ATARIST_OS_H */
