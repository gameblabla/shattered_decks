#include "snes_stamp.h"

SnesStamp g_stamp;

void snesStampInit(void)
{
    u16 i;
    u8 *p = (u8 *)&g_stamp;
    for (i = 0; i < sizeof(SnesStamp); ++i) p[i] = 0;
    g_stamp.magic = SNES_STAMP_MAGIC;
    snesStampCommit();
}

void snesStampCommit(void)
{
    u16 sum = 0;
    u16 i;
    const u16 *w = (const u16 *)&g_stamp;
    for (i = 0; i < (sizeof(SnesStamp) / 2) - 1; ++i) sum += w[i];
    g_stamp.checksum = sum;
}
