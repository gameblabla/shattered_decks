// ─────────────────────────────────────────────────────────────────────────────
//  msx2_title.c — title screen: streamed artwork, logo, prompt and menu
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_title.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_audio.h"
#include "msx2_stream.h"
#include "msx2_scenes.h"

// ── Geometry, lifted from src/main.c so the two builds frame the same screen ──
#define TITLE_LOGO_Y1     20
#define TITLE_LOGO_Y2     40

#define MENU_PANEL_X      41
#define MENU_PANEL_Y      122
#define MENU_PANEL_W      174
#define MENU_PANEL_H      56
#define MENU_ROW_X        72
#define MENU_ROW_Y(n)     (133 + (n) * 18)
#define MENU_CURSOR_X     54
#define MENU_PROMPT_Y     188
#define TITLE_COPY_Y      203

// THE BLINKING PROMPT, WITH NOTHING BEHIND IT.
// The prompt sits directly on the artwork, so turning it off means putting the
// picture back -- and re-streaming a strip of it out of the cartridge every 48
// frames is absurd for a blink.  Instead both states are baked once, into the
// offscreen VRAM rows below the visible 212 (the page is 256 rows tall), and a
// blink is a single HMMM: the command engine moves the strip and the Z80 does
// nothing.  Rows 186-197 cover the glyphs plus the pixel of outline around them.
#define TITLE_STRIP_Y     186
#define TITLE_STRIP_H     12
#define TITLE_STASH_CLEAN 216   // the artwork strip, untouched
#define TITLE_STASH_TEXT  228   // the same strip with the prompt drawn on it
                                // (both below row 240: the sprite tables live
                                //  there and a page copy stops short of them)

// Phases.  ATTRACT is the logo and the blinking prompt; MENU is the panel.
#define TITLE_ATTRACT     0
#define TITLE_MENU        1

#define BLINK_PERIOD      48   // frames per on/off pair, as on the other targets

// Repainting on a double-buffered screen.
// A partial repaint only reaches the page it was drawn on, so every change is
// held in a dirty mask and painted again on the next page, for as many frames
// as there are buffers.  Nothing here is ever drawn on the visible page --
// that, not the amount of drawing, is what made the menu flicker.
#define TITLE_DIRTY_PROMPT  0x01
#define TITLE_DIRTY_PANEL   0x02
#define TITLE_DIRTY_ROWS    0x04

static u8  g_phase;
static u8  g_cursor;
static u16 g_frame;
static u8  g_blink_on;
static u8  g_dirty;
static u8  g_dirty_left;

// The row labels are cartridge strings, so the table holds their ids: a C
// initialiser cannot call anything, and there is no reason for these three
// words to sit in the code budget when every other line of the game does not.
static const u8 g_rows[MSX2_TITLE_ROWS] =
{
	MSX2_S_STORY_MODE,
	MSX2_S_BATTLE_MODE,
	MSX2_S_LOAD_STORY,
};

static bool Msx2_TitleRowEnabled(u8 row)
{
	return row < MSX2_TITLE_ROWS;
}

// ── The backdrop ─────────────────────────────────────────────────────────────

// The title art is the same picture the other targets show
// (assets/source/title/title256_msx2.png), dithered to GRB332 offline and
// streamed straight out of the cartridge.  It costs no Z80 work beyond the
// copy, which is the whole reason this port is a 16 MB cartridge.
static void Msx2_TitleBackdrop(void)
{
	Msx2_StreamScene(MSX2_SCENE_TITLE_SEGMENT, Msx2_VideoGetDrawPage());
}

// ── The attract prompt ───────────────────────────────────────────────────────

// Bake the two states of the prompt strip into offscreen VRAM.  Called once,
// on the composed page, before it is shown.  Neither state is drawn: the clean
// one is the artwork as it arrived, and the lit one is twelve rows of the same
// artwork with the words already painted into them, baked by the scene
// generator and streamed straight out of the cartridge.
static void Msx2_TitleBakePrompt(void)
{
	Msx2_CopyRect(0, TITLE_STRIP_Y, 0, TITLE_STASH_CLEAN, MSX2_SCREEN_W, TITLE_STRIP_H);
	Msx2_StreamBand(MSX2_TITLE_PROMPT_SEGMENT, TITLE_STASH_TEXT, TITLE_STRIP_H);
}

static void Msx2_TitlePrompt(bool on)
{
	Msx2_CopyRect(0, on ? TITLE_STASH_TEXT : TITLE_STASH_CLEAN,
	              0, TITLE_STRIP_Y, MSX2_SCREEN_W, TITLE_STRIP_H);
}

// ── The menu ─────────────────────────────────────────────────────────────────

static void Msx2_TitleMenuRow(u8 row)
{
	u8 y = MENU_ROW_Y(row);
	u8 fg;

	if(!Msx2_TitleRowEnabled(row))
		fg = MSX2_RGB(3, 3, 1);                 // dim: present but refused
	else if(row == g_cursor)
		fg = MSX2_GOLD;
	else
		fg = MSX2_WHITE;

	Msx2_TextColor(fg, MSX2_BLACK);
	Msx2_TextAt(MENU_ROW_X, y, Msx2_UiText(g_rows[row]));

	// A right-pointing triangle, nine rows tall so it centres on the 8-pixel
	// glyph.  The erase covers the whole nine rows, or the tail of the previous
	// cursor is left behind on the row it moved off.
	Msx2_Fill(MENU_CURSOR_X, (u8)(y - 1), 6, 9, MSX2_BLACK);
	if(row == g_cursor)
	{
		u8 i;
		for(i = 0; i < 9; ++i)
		{
			u8 w = (i < 5) ? (u8)(i + 1) : (u8)(9 - i);
			Msx2_Fill(MENU_CURSOR_X, (u8)(y - 1 + i), w, 1, MSX2_RED);
		}
	}
}

static void Msx2_TitleMenuPanel(void)
{
	u8 row;

	Msx2_Fill(MENU_PANEL_X - 2, MENU_PANEL_Y - 2, MENU_PANEL_W + 4,
	          MENU_PANEL_H + 4, MSX2_BLACK);
	Msx2_FrameRect(MENU_PANEL_X, MENU_PANEL_Y, MENU_PANEL_W, MENU_PANEL_H,
	               MSX2_GOLD);

	for(row = 0; row < MSX2_TITLE_ROWS; ++row)
		Msx2_TitleMenuRow(row);
}

// ── Scene ────────────────────────────────────────────────────────────────────

// Mark work to be done, and keep it queued long enough to reach every buffer.
static void Msx2_TitleDirty(u8 mask)
{
	g_dirty |= mask;
	g_dirty_left = MSX2_VIDEO_PAGES;
}

// Repaint the queued pieces on whichever page is currently hidden.  Order
// matters: the prompt strip overlaps the bottom of the menu panel, so restoring
// artwork there has to happen before the panel is redrawn over it.
static void Msx2_TitlePaint(void)
{
	if(g_dirty & TITLE_DIRTY_PROMPT)
		Msx2_TitlePrompt(g_blink_on);

	if(g_dirty & TITLE_DIRTY_PANEL)
	{
		Msx2_TitleMenuPanel();   // draws the rows itself
	}
	else if(g_dirty & TITLE_DIRTY_ROWS)
	{
		u8 row;
		for(row = 0; row < MSX2_TITLE_ROWS; ++row)
			Msx2_TitleMenuRow(row);
	}
}

// Build the whole screen on the draw page, from the streamed backdrop up.
static void Msx2_TitleCompose(void)
{
	// The logo and the copyright line are painted into the picture itself
	// (tools/msx2/gen_msx_scenes.py), so the backdrop arrives finished.
	Msx2_TitleBackdrop();
	Msx2_TitleBakePrompt();
	Msx2_TitlePrompt(g_blink_on);
	if(g_phase == TITLE_MENU)
		Msx2_TitleMenuPanel();
}

void Msx2_TitleEnter(void)
{
	g_phase = TITLE_ATTRACT;
	g_cursor = MSX2_TITLE_STORY;
	g_frame = 0;
	g_blink_on = TRUE;
	g_dirty = 0;
	g_dirty_left = 0;

	// Compose page 1 -- streaming the artwork and laying the text over it takes
	// far longer than a frame -- then hand the second buffer a copy of it with a
	// single HMMM rather than streaming the cartridge a second time.  Both pages
	// then hold the identical picture, which is what lets a per-frame repaint
	// touch only the hidden one.
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_TitleCompose();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);   // ... and drawing moves to page 0

	Msx2_MusicPlay(MSX2_MUSIC_TITLE);
}

u8 Msx2_TitleStep(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 choice = MSX2_TITLE_BUSY;

	++g_frame;

	if(g_phase == TITLE_ATTRACT)
	{
		bool on = ((g_frame / BLINK_PERIOD) & 1) == 0;
		if(on != g_blink_on)
		{
			g_blink_on = on;
			Msx2_TitleDirty(TITLE_DIRTY_PROMPT);
		}

		if(pressed & (MSX2_BTN_A | MSX2_BTN_B))
		{
			g_blink_on = FALSE;
			Msx2_SfxPlay(MSX2_SFX_CONFIRM);
			g_phase = TITLE_MENU;
			Msx2_TitleDirty(TITLE_DIRTY_PROMPT | TITLE_DIRTY_PANEL);
		}
	}
	else
	{
		if(pressed & (MSX2_BTN_UP | MSX2_BTN_DOWN))
		{
			if(pressed & MSX2_BTN_UP)
				g_cursor = (u8)((g_cursor == 0) ? (MSX2_TITLE_ROWS - 1) : (g_cursor - 1));
			else
				g_cursor = (u8)((g_cursor + 1) % MSX2_TITLE_ROWS);
			Msx2_SfxPlay(MSX2_SFX_SELECT);
			// Every row, not just the two that changed: the page being painted
			// is a frame behind, so it may not be showing the cursor where this
			// page was showing it.
			Msx2_TitleDirty(TITLE_DIRTY_ROWS);
		}

		if(pressed & MSX2_BTN_B)
		{
			// Back out of the menu to the attract prompt, so the button always
			// means the same thing.
			Msx2_TitleEnter();
			return MSX2_TITLE_BUSY;
		}

		if(pressed & MSX2_BTN_A)
		{
			// A refused row is silent on purpose: there is no deny sound in the
			// SFX set, and inventing one here would be a decision made in the
			// wrong file.
			if(Msx2_TitleRowEnabled(g_cursor))
			{
				Msx2_SfxPlay(MSX2_SFX_CONFIRM);
				choice = g_cursor;
			}
		}
	}

	if(g_dirty_left != 0)
	{
		Msx2_TitlePaint();
		Msx2_VideoFlipRequest();
		if(--g_dirty_left == 0)
			g_dirty = 0;
	}

	return choice;
}

u8 Msx2_TitleCursor(void)
{
	return (u8)((g_phase == TITLE_ATTRACT) ? 0xFF : g_cursor);
}
