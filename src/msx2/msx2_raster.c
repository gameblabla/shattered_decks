// ─────────────────────────────────────────────────────────────────────────────
//  msx2_raster.c — §8 Tier A: the baked span-program card rasterizer
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_raster.h"
#include "msx2_video.h"
#include "msx2_stream.h"
#include "msx2_scenes.h"

// The card being drawn, and the program that draws it.  Both live in RAM for
// the duration: a COPY is then a RAM-to-VDP block push, so the inner loop never
// touches the mapper and the interpreter never pays a bank switch per run.
static u8 g_tex[MSX2_CARD_W * MSX2_CARD_H];
static u8 g_prog[MSX2_SPAN_MAX];

// Which texture is already in g_tex, so a repaint that redraws the same card
// into the same slot -- the common case, once per page -- skips the 1,920-byte
// read.  0xFFFF is "nothing".
static u16 g_tex_loaded;
static u8  g_prog_loaded;
static u8  g_view;

#define TEX_KEY(card, mirror, def) \
	(u16)(((u16)(card) << 2) | ((u16)(def) << 1) | (mirror))
#define TEX_NONE               0xFFFF
// The program cache key is the slot and the position it is holding: a slot
// whose card lies down uses a different program in the same slot, so the slot
// number alone would have kept the upright one.
#define PROG_KEY(slot, def)    (u8)(((slot) << 1) | (def))
#define PROG_NONE              0xFF

// Slots 0..4 are the physical COM row.  Which row is mirrored is selected
// below from the camera chair: the far row faces the current viewer's
// opponent, while the near row faces the viewer.
#define COM_SLOTS  (MSX2_FIELD_SLOTS / 2)

void Msx2_RasterInit(void)
{
	g_tex_loaded = TEX_NONE;
	g_prog_loaded = PROG_NONE;
	g_view = MSX2_VIEW_TOP;
}

void Msx2_RasterSetView(u8 view)
{
	if(view >= MSX2_BOARD_VIEWS)
		view = MSX2_VIEW_TOP;
	if(g_view != view)
	{
		g_view = view;
		// A view owns a different span table.  The card texture is reusable,
		// but the loaded program is not.
		g_prog_loaded = PROG_NONE;
	}
}

void Msx2_RasterLoad(u8 card_index, u8 slot, u8 defense)
{
	// The COM row is drawn from the mirrored blob (see the header): its span
	// programs walk their texels backwards, and reading a mirrored texture
	// forwards is the same picture with no second inner loop.
    // The camera view changes which side is "ours".  In the player view the
    // COM row is the far row and is rotated away from the player; after the
    // turn orbit the COM row is nearest the camera and the player's row is the
    // one that must be read upside down.  Key this from the view, not from the
    // physical row, or the opponent would sit at its own chair looking at its
    // cards backwards.
    u8  mirror = (g_view == MSX2_VIEW_TOP)
               ? ((slot < COM_SLOTS) ? 1 : 0)
               : ((slot < COM_SLOTS) ? 0 : 1);
	u16 key = TEX_KEY(card_index, mirror, defense);

	if(g_tex_loaded != key)
	{
		// A card in defence position is turned a quarter turn on the board.
		// The turn is in the ART -- the defence set is stored 48 wide by 40
		// tall -- so the interpreter below is untouched by it: it still walks
		// one texture row forwards per destination row.
		u16 base = defense
		         ? (mirror ? MSX2_CARD_DEF_MIRROR_SEGMENT : MSX2_CARD_DEF_SEGMENT)
		         : (mirror ? MSX2_CARD_MIRROR_SEGMENT : MSX2_CARD_ART_SEGMENT);
		Msx2_RomReadLong((u16)(base + card_index / MSX2_CARD_ART_PER_SEG),
		                 (u16)((card_index % MSX2_CARD_ART_PER_SEG)
		                       * MSX2_CARD_ART_STRIDE),
		                 g_tex, MSX2_CARD_W * MSX2_CARD_H);
		g_tex_loaded = key;
	}

	if(g_prog_loaded != PROG_KEY(slot, defense))
	{
		u8 record = defense ? g_msx2_span_def_record[g_view][slot]
		                    : g_msx2_span_record[g_view][slot];
		Msx2_RomReadLong((u16)(MSX2_SPAN_SEGMENT + record / MSX2_SPAN_PER_SEG),
		                 (u16)((record % MSX2_SPAN_PER_SEG) * MSX2_SPAN_STRIDE),
		                 g_prog, MSX2_SPAN_MAX);
		g_prog_loaded = PROG_KEY(slot, defense);
	}
}

void Msx2_RasterDraw(void)
{
	const u8* p = g_prog;

	while(*p != MSX2_OP_END)
	{
		u8  dy = *p++;
		u8  x0 = *p++;
		u16 src = (u16)p[0] | ((u16)p[1] << 8);
		p += 3;                       // the mirror flag is baked into `src`
		Msx2_PokeAt(x0, dy);

		// One row, one destination run: the generator splits a broken row into
		// two records rather than emitting a gap, so from here it is nothing
		// but pushing bytes at an address the VDP advances by itself.
		for(;;)
		{
			u8 op = *p++;
			u8 n = (u8)(op & MSX2_OP_RUN_MASK);

			if(op == MSX2_OP_ENDROW)
				break;

			switch(op & 0x60)
			{
			case MSX2_OP_COPY:
				Msx2_PokeBlock(&g_tex[src], n);
				src = (u16)(src + n);
				break;
			case MSX2_OP_DUP:
				// Magnification: the texel is the last one written, and the
				// VDP's write pointer is already sitting after it.
				Msx2_PokeRun(g_tex[src - 1], n);
				break;
			default:                  // MSX2_OP_ADV: the quad is smaller here
				src = (u16)(src + n);
				break;
			}
		}
	}
}

void Msx2_RasterCard(u8 card_index, u8 slot, u8 defense)
{
	Msx2_RasterLoad(card_index, slot, defense);
	Msx2_RasterDraw();
}
