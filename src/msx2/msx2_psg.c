// MSXgl's PSG implementation is linked explicitly in the resident project
// list.  The streaming window may replace 0x8000 during both VRAM copies and
// the audio ISR, so the library archive must not leave PSG_Apply below the
// 0x8000 ceiling by accident.
#include "../../MSXgl-main/engine/src/psg.c"
