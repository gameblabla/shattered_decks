/* The card stat tables.
 *
 * src/generated/msx2_card_tables.h emits the four ATK/DEF/attribute/tribe
 * arrays only in the translation unit that defines MSX2_CARD_TABLES_IMPL.  The
 * MSX2 fork does that in msx2_cards.c, which also carries scene geometry this
 * target has no use for, so the ST build owns this one-line TU instead.  Four
 * hundred and thirty-two bytes, and both forks stay on the same generated
 * numbers.
 */
#define MSX2_CARD_TABLES_IMPL
#include "msx2_cards.h"
