/* SNES fork shim.
 *
 * src/game/deck.c and src/game/ai.c include "waifu_assets.h" only for the card
 * count; the real generated header is 5.3 MB of pixel data with no place in a
 * cartridge build.  Card stats come from src/generated/msx2_card_tables.h. */
#ifndef WAIFU_ASSETS_H
#define WAIFU_ASSETS_H

#define WAIFU_CARD_COUNT 72

#endif /* WAIFU_ASSETS_H */
