#ifndef CFX_SCREEN_CONFIG_H
#define CFX_SCREEN_CONFIG_H

/* Single authoritative framebuffer resolution for the whole build. Change these
   two values (and provide matching 2D asset layout) to retarget a different
   screen size; nothing else hardcodes the width/height. */
#if defined(WAIFU_FM_CD32X)
#define WAIFU_FM_WIDTH 320
#define WAIFU_FM_HEIGHT 224
#else
#define WAIFU_FM_WIDTH 256
#define WAIFU_FM_HEIGHT 240
#endif

#define ORAM_DATA
#endif
