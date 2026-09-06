/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_hw.h — the ST/STE hardware the port touches, in one place.
 *
 *  Only registers this port actually programs are listed; a full ST hardware
 *  map belongs in a document, not in a header that every translation unit
 *  reads.  Everything is a volatile lvalue macro so the call sites read like
 *  assignments rather than casts.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_HW_H
#define WAIFU_ATARIST_HW_H

#include <stdint.h>

#define ST_R8(a)  (*(volatile uint8_t  *)(a))
#define ST_R16(a) (*(volatile uint16_t *)(a))
#define ST_R32(a) (*(volatile uint32_t *)(a))

/* ── System variables (TOS) ──────────────────────────────────────────────── */
#define ST_HZ200        ST_R32(0x4ba)   /* 200 Hz counter kept by Timer C */
#define ST_FRCLOCK      ST_R32(0x466)   /* VBL counter kept by TOS */
#define ST_VBLQUEUE     ST_R32(0x456)   /* -> array of VBL routine pointers */
#define ST_NVBLS        ST_R32(0x454)   /* how many entries that array has */
#define ST_PHYSTOP      ST_R32(0x42e)   /* top of ST RAM */
#define ST_SCREENPT     ST_R32(0x45e)   /* TOS's own logical screen pointer */
#define ST_VBLSEM       ST_R16(0x452)   /* VBL semaphore; 0 disables the queue */

/* ── Interrupt vectors ───────────────────────────────────────────────────── */
#define ST_VEC_VBL      ST_R32(0x70)
#define ST_VEC_HBL      ST_R32(0x68)
#define ST_VEC_MFP_ACIA ST_R32(0x118)   /* MFP channel 6: IKBD/MIDI ACIA */
#define ST_VEC_MFP_TIMB ST_R32(0x120)   /* MFP channel 8: Timer B */

/* ── Shifter / video ─────────────────────────────────────────────────────── */
#define ST_VIDBASE_HI   ST_R8(0xffff8201)
#define ST_VIDBASE_MID  ST_R8(0xffff8203)
#define ST_VIDBASE_LO   ST_R8(0xffff820d)   /* STE only */
#define ST_VCOUNT_HI    ST_R8(0xffff8205)
#define ST_VCOUNT_MID   ST_R8(0xffff8207)
#define ST_VCOUNT_LO    ST_R8(0xffff8209)
#define ST_SYNCMODE     ST_R8(0xffff820a)   /* 0 = 60 Hz, 2 = 50 Hz */
#define ST_LINEWIDTH    ST_R8(0xffff820f)   /* STE: extra words per line */
#define ST_HSCROLL      ST_R8(0xffff8265)   /* STE: fine horizontal scroll */
#define ST_PALETTE      ((volatile uint16_t *)0xffff8240)
#define ST_RESOLUTION   ST_R8(0xffff8260)   /* 0 = 320x200x4bpp */

/* ── MFP 68901 ───────────────────────────────────────────────────────────── */
#define ST_MFP_IERA     ST_R8(0xfffffa07)
#define ST_MFP_IERB     ST_R8(0xfffffa09)
#define ST_MFP_IPRA     ST_R8(0xfffffa0b)
#define ST_MFP_IPRB     ST_R8(0xfffffa0d)
#define ST_MFP_ISRA     ST_R8(0xfffffa0f)
#define ST_MFP_ISRB     ST_R8(0xfffffa11)
#define ST_MFP_IMRA     ST_R8(0xfffffa13)
#define ST_MFP_IMRB     ST_R8(0xfffffa15)
#define ST_MFP_TACR     ST_R8(0xfffffa19)
#define ST_MFP_TBCR     ST_R8(0xfffffa1b)
#define ST_MFP_TADR     ST_R8(0xfffffa1f)
#define ST_MFP_TBDR     ST_R8(0xfffffa21)

#define ST_MFP_IERA_TIMB  0x01
#define ST_MFP_IERB_ACIA  0x40

/* ── Sound ───────────────────────────────────────────────────────────────── */
#define ST_YM_SELECT    ST_R8(0xffff8800)
#define ST_YM_WRITE     ST_R8(0xffff8802)
#define ST_YM_READ      ST_R8(0xffff8800)

/* STE DMA sound */
#define ST_DMA_CTRL     ST_R8(0xffff8901)
#define ST_DMA_BASE_HI  ST_R8(0xffff8903)
#define ST_DMA_BASE_MID ST_R8(0xffff8905)
#define ST_DMA_BASE_LO  ST_R8(0xffff8907)
#define ST_DMA_END_HI   ST_R8(0xffff890f)
#define ST_DMA_END_MID  ST_R8(0xffff8911)
#define ST_DMA_END_LO   ST_R8(0xffff8913)
#define ST_DMA_MODE     ST_R8(0xffff8921)

/* ── IKBD ACIA ───────────────────────────────────────────────────────────── */
#define ST_ACIA_CTRL    ST_R8(0xfffffc00)
#define ST_ACIA_DATA    ST_R8(0xfffffc02)

/* ── Blitter (present on STE, optional on ST) ────────────────────────────── */
#define ST_BLIT_HALFTONE ((volatile uint16_t *)0xffff8a00)
#define ST_BLIT_SRC_XINC ST_R16(0xffff8a20)
#define ST_BLIT_SRC_YINC ST_R16(0xffff8a22)
#define ST_BLIT_SRC_ADDR ST_R32(0xffff8a24)
#define ST_BLIT_ENDMASK1 ST_R16(0xffff8a28)
#define ST_BLIT_ENDMASK2 ST_R16(0xffff8a2a)
#define ST_BLIT_ENDMASK3 ST_R16(0xffff8a2c)
#define ST_BLIT_DST_XINC ST_R16(0xffff8a2e)
#define ST_BLIT_DST_YINC ST_R16(0xffff8a30)
#define ST_BLIT_DST_ADDR ST_R32(0xffff8a32)
#define ST_BLIT_XCOUNT   ST_R16(0xffff8a36)
#define ST_BLIT_YCOUNT   ST_R16(0xffff8a38)
#define ST_BLIT_HOP      ST_R8(0xffff8a3a)
#define ST_BLIT_OP       ST_R8(0xffff8a3b)
#define ST_BLIT_CTRL     ST_R8(0xffff8a3c)
#define ST_BLIT_SKEW     ST_R8(0xffff8a3d)

#endif /* WAIFU_ATARIST_HW_H */
