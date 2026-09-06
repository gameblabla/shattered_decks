// ─────────────────────────────────────────────────────────────────────────────
//  msx2_mapper.h — logical cartridge-window operations
//
//  NEO-16 has an independent page-0 window.  ASCII16-X mirrors its page-2
//  window into page 0, so ASCII code banks and streamed data share one shadow.
//  Callers use the logical page-2 operation below; mapper encoding stays here
//  and in the target-specific inline paths in msx2_stream.c.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

#define MSX2_MAPPER_WINDOW       0x8000
#define MSX2_MAPPER_SEGMENT_SIZE 0x4000
#define MSX2_MAPPER_CODE_SEGMENT 1
#define MSX2_MAPPER_RULES_SEGMENT 7

#ifdef MSX2_ASCII16X

// ASCII16-X exposes the upper four segment bits on the mapper write address.
// The published 8 MiB hardware uses segments 0..0x1FF; the encoding supports
// the full SDK range, so no high byte is silently discarded here.
#define MSX2_MAPPER_MAX_SEGMENT  0x0FFF
#define MSX2_MAPPER_PAGE2_ADDR   0x7000

void Msx2_MapperSetPage2(u16 segment);

#else

#define MSX2_MAPPER_MAX_SEGMENT  0xFFFF
#define MSX2_MAPPER_PAGE2_ADDR   0x7000

#endif
