/* Atari ST fork shim.
 *
 * The shared logic modules (src/game/deck.c, src/game/ai.c) include
 * "waifu_assets.h" only for the card-count constants -- the real generated
 * header is 5.3 MB of pixel data that has no place in a 512 KB floppy build.  This
 * stand-in is placed first on the Atari ST include path so the shared sources
 * compile unmodified.  Card stats live in src/generated/msx2_card_tables.h.
 */
#ifndef WAIFU_ASSETS_H
#define WAIFU_ASSETS_H

#include <stdint.h>

#define WAIFU_CARD_COUNT 72

#endif /* WAIFU_ASSETS_H */
