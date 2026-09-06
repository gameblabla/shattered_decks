// ─────────────────────────────────────────────────────────────────────────────
//  msx2_story.c — the opening, the sanctum map, the dialogue, the ending
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_story.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_stream.h"
#include "msx2_sprite.h"
#include "msx2_audio.h"
#include "msx2_duel.h"
#include "msx2_cards.h"
#include "msx2_probe.h"
#include "msx2_scenes.h"
#include "msx2_disk.h"
#include "msx2_story_load.h"
#include "msx2_bank.h"
// SCREEN 10 in the MSX2+ cartridge: this unit's ink is YAE (msx2_plus.h).
#include "msx2_plus.h"
#ifdef MSX2_PLUS
// The plus cartridge's own asset symbols: the busts carry a fringe table the
// paletted ones have no use for (Msx2_StoryBlitBust).
#include "msx2_plus_scenes.h"
#endif

// The MSX2 cartridge's busts are GRAPHIC 7 bytes and every pixel of one is its
// own colour, so there is no fringe table between the runs and the pixels.
#ifndef MSX2_PORTRAIT_PIXELS_OFF
#define MSX2_PORTRAIT_PIXELS_OFF  MSX2_PORTRAIT_INDEX_BYTES
#endif
#ifdef MSX2_DEBUG_REGRESSION
#include "msx2_regression.h"
#endif

// ── Phases ───────────────────────────────────────────────────────────────────
#define PH_NARRATE   0   // the opening or the ending: one voice, no portrait
#define PH_TALK      1   // pre-duel dialogue, two speakers, two composites
#define PH_MAP       2   // the sanctum road: pick an opponent, or leave
#define PH_CODE_IN   4   // continue-code entry
#define PH_CODE_OUT  5   // continue code shown at the sanctum
#define PH_DECK      6   // compact deck editor
#define PH_REWARD    7   // a story-win card reveal
#define PH_LOAD_PICK 8   // LOAD STORY: is the save on a disk or on paper?
#define PH_SAVE_PICK 9   // SAVE GAME: floppy or password

// Which run of records the narration is reading, so the end of it knows where
// to go: the opening leads to the map, the ending leads back to the title.
#define NARR_INTRO   0
#define NARR_ENDING  1

// The text box, drawn over the baked panel.  The panel is a flat colour that
// went through the ditherer, so it is *not* a flat byte on screen; anything
// written over it is written over a fill first, exactly as the duel screen
// does with its own two panels.
#define BOX_X        1
#define BOX_Y        (MSX2_TALK_BOX_Y + 1)
#define BOX_W        (MSX2_SCREEN_W - 2)
#define BOX_H        (MSX2_SCREEN_H - MSX2_TALK_BOX_Y - 2)

// The name plate is baked into the composites only; the ending screen has no
// panel at all, so the narration draws its own box there.
#define PLATE_X      7
#define PLATE_Y      (MSX2_TALK_NAME_Y - 2)
#define PLATE_W      143
#define PLATE_H      11

#define REVEAL_RATE  2   // characters per frame: a full three-row line in a second

// Map geometry, inside the panel the backdrop was baked with.
#define MAP_HEAD_Y   36
#define MAP_ROW_Y(n) (u8)(46 + (n) * 15)
#define MAP_DECK_Y   127
#define MAP_CODE_Y   145
#define MAP_BACK_Y   163
#define MAP_HELP_Y   176
// The selector is a sprite now, so what used to be the ">" column is the
// square the gem stands in: 20 pixels of it, ending where the names start.
#define MAP_GEM_X    28
// A row's text is eight pixels tall and the gem is twenty, so it sits six
// above the line to be centred on it.
#define MAP_GEM_RISE 6
#define MAP_NAME_X   48
#define MAP_TITLE_X  132
#define MAP_DECK_ROW (MSX2_STORY_MAX_DUELS)
#define MAP_CODE_ROW (MSX2_STORY_MAX_DUELS + 1)
#define MAP_BACK_ROW (MSX2_STORY_MAX_DUELS + 2)
#define MAP_ROWS     (MSX2_STORY_MAX_DUELS + 3)

#define STORY_DECK_SIZE       WAIFU_DECK_SIZE
#define STORY_STORAGE_SIZE    64
#define STORY_NAME_LEN        8
#define STORY_CODE_LEN        16
#define STORY_EDIT_SLOTS      4

// ── The deck editor, laid out the way the PC-FX one is ───────────────────────
//
// Two tabs over one browsable list, an art panel for whatever the cursor is
// on, and one button that moves a card between the two sides.  What the other
// targets draw as a 6x3 grid of card faces is a NAMED LIST here, and that is
// the only real departure: a card face is 1,920 bytes streamed at the padded
// 32 T-state pitch Msx2_StreamRect has to use with the display on, so eighteen
// of them is a third of a second of repaint per keypress.  One face beside a
// list of names costs one, reads better on a 256-wide screen, and is the only
// version of this screen that answers the joystick at once.
#define ED_TITLE_Y            6
#define ED_TAB_Y              17
#define ED_TAB_H              13
#define ED_TAB0_X             6
#define ED_TAB1_X             134
#define ED_TAB_W              116
#define ED_STATUS_Y           35
#define ED_LIST_X             6
#define ED_LIST_W             190
#define ED_LIST_Y             48
#define ED_ROW_H              13
#define ED_ROWS               10
// The gem's column, and the text pushed clear of it.  20 pixels of selector
// sit at ED_GEM_X; a name starts after them.
#define ED_GEM_X              8
#define ED_TEXT_X             34
#define ED_TEXT_COLS          30
#define ED_ART_X              204
#define ED_ART_Y              50
#define ED_INFO_X             202
#define ED_STAT_Y             104
#define ED_COUNT_Y            126
#define ED_MSG_Y              180
#define ED_HINT_Y             192
#define ED_HINT2_Y            202
#define ED_NONE               0xFFu

// How long a held direction waits before it repeats, and how often after that.
// Forty cards is four screenfuls; one press per row would be eighty presses to
// walk the deck and back.
#define ED_REP_DELAY          20
#define ED_REP_RATE           4

// How many quiet frames the cursor has to stand still before the card art
// under it is redrawn.  The art is the ONE thing a cursor step still costs --
// a 40x48 stream, twice, once per page -- and it is worth nothing at all while
// the list is scrolling past.  Longer than ED_REP_RATE on purpose: a held
// direction therefore draws no art until it is let go.
#define ED_ART_SETTLE         7

// Repaint levels, in the order they cost.  A settled cursor owes the art panel
// and nothing else -- the selector itself is a sprite and owes no page any
// pixels; a scroll or a tab owes the whole list; the tabs, the status line and
// the hints only change when the mode does.
#define ED_DIRTY_ART          1
#define ED_DIRTY_LIST         2
#define ED_DIRTY_ALL          3

#define CODE_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

extern u8 Msx2_StoryGridCursor(u8 cursor, u8 cols, u8 count, u8 pressed);
extern u32 Msx2_StoryNameHash(const c8* name, u8 len);
extern u8 Msx2_StoryStageForProgress(u8 progress);
extern const c8* Msx2_StoryStageNameForProgress(u8 progress);
extern void Msx2_StoryDrawCardThumb(u8 card, u8 x, u8 y);
extern void Msx2_StoryBuildCode(c8* code, u8 progress, const c8* name,
	                             const u8* deck);
extern bool Msx2_StoryDecodeCode(const c8* code, u8 len, u8* progress,
	                                c8* name, u8* swaps);
extern bool Msx2_StoryTakeStorageCard(u8* storage, u8* count, u8 card);

static u8  g_phase;
static u8  g_progress;      // the frontier: the furthest opponent unlocked
static u8  g_duel_index;    // the opponent selected on the map
static u8  g_narr_which;

// The line being spoken, wrapped to the width of the box.
static c8  g_row[MSX2_TALK_LINES][MSX2_TALK_COLS + 1];
static u8  g_row_len[MSX2_TALK_LINES];
static u8  g_total;         // characters in the whole line
static u8  g_reveal;        // how many of them the player has been shown
static u8  g_drawn[MSX2_VIDEO_PAGES];

static u8  g_line;          // which record of the run is up
static u8  g_line_count;
static u8  g_speaker;       // 0 Serena, 1 the opponent, 2 the narrator
static u8  g_shot;          // which speaker is lit, 0 or 1; 0xFF before the scene is built

// Bitmasks of pages that still owe a repaint.  A partial repaint only ever
// reaches the page it was drawn on, so every change is carried until both
// buffers have seen it -- per page rather than as a countdown, because the
// per-page character cursor (g_drawn) has to be reset with it.
static u8  g_fresh;         // the box, the plate and the name
static u8  g_prompt;        // the "line finished" marker
static u8  g_map_dirty;

static u8  g_cursor;

// ── The selector ────────────────────────────────────────────────────────────
// The sanctum road and the deck editor both used to mark the cursor by drawing
// into the picture: a ">" in the panel, a filled row behind a name.  Both then
// owed a repaint on every step -- and on this machine the map's repaint was
// the WHOLE panel, five opponent names read back out of the cartridge
// included, and the editor's was two rows of glyphs, twice, once per page.
//
// It is the duel screen's spinning gem instead.  A V9938 sprite floats over
// both bitmap pages and is placed by writing a handful of bytes, so a cursor
// step now costs nothing in the picture at all, and the only thing either
// screen still repaints when the cursor moves is the editor's card art --
// which is deferred until the cursor settles (see ED_ART_SETTLE).
static u8  g_gem_tick;
static u8  g_gem_up;

// The duelist's name is fixed on this target.  Typing eight letters in on a
// V9938 text screen cost a full repaint per keypress and was the slowest,
// fiddliest screen in the port, so the story simply runs as SERENA -- who the
// prose calls her, and who every other target defaults to.
static c8  g_player_name[STORY_NAME_LEN + 1] = "SERENA  ";
static c8  g_code[STORY_CODE_LEN + 1];
static u8  g_code_len;
static u8  g_code_cursor;
static u8  g_code_error;
// What the disk last did, so the screen can say it: 0 nothing, 1 saved,
// 2 failed, 3 loaded, 4 nothing to load.
static u8  g_disk_msg;
static u8  g_save_pick;
// The flag the code screen raises when the player backs out of an empty code:
// the phase steppers return void, and this one has to leave the whole scene.
static u8  g_want_quit;
// The editor: one cursor and one scroll per tab, so stepping across and back
// lands where it left.
static u8  g_ed_tab;
static u8  g_ed_cursor[2];
static u8  g_ed_scroll[2];
// The half-finished exchange: the row picked on the other tab, or ED_NONE.
static u8  g_ed_pending;
static u8  g_ed_pending_tab;
// A refusal the screen is still showing; cleared by the next button.
static u8  g_ed_msg;
static u8  g_ed_rep;
static u8  g_ed_rep_dir;
static u8  g_ed_dirty[MSX2_VIDEO_PAGES];
// Frames left before the settled cursor's card art is drawn, or 0 for "the art
// on the screen is the card the cursor is on".
static u8  g_ed_art_wait;
// The deck is browsed in card order, not in the order the slots happen to hold:
// the deck is shuffled before every duel, so slot order means nothing to the
// game, and a sorted list puts a card's copies together where they can be
// counted.  It also hides the one thing the save format forces -- see
// Msx2_StoryDeckHostSlot.
static u8  g_ed_view[STORY_DECK_SIZE];
// The ten names on screen, read once per scroll rather than once per page: a
// repaint has to happen twice, and the cartridge read is the same both times.
static c8  g_ed_names[ED_ROWS][MSX2_NAME_STRIDE];
static u8  g_story_deck[STORY_DECK_SIZE];
// The first four cards of the generated starter deck.  The continue code has
// room for exactly four replaced slots (STORY_EDIT_SLOTS x 7 bits), so a slot
// still holding its starter card is a slot a change can be saved in.
static u8  g_story_base[STORY_EDIT_SLOTS];
// Whether g_story_deck holds a real deck yet.  The soak deals a story duel
// without ever walking into story mode, and the RAM it reads is now genuinely
// zero rather than accidentally card-shaped.
static u8  g_story_deck_ready;
static u8  g_story_storage[STORY_STORAGE_SIZE];
static u8  g_story_storage_count;
static u8  g_reward_card;

static c8  g_text[MSX2_LINE_STRIDE];
static c8  g_opp[MSX2_OPP_STRIDE / 2];

#define ALL_PAGES  (u8)((1u << MSX2_VIDEO_PAGES) - 1u)

// ─────────────────────────────────────────────────────────────────────────────
//  Reading the cartridge
// ─────────────────────────────────────────────────────────────────────────────

// One dialogue/narration record: a speaker byte then a NUL-terminated line.
// The prose calls the heroine SERENA by name, and the player may have called
// her something else.  The other targets substitute at draw time; here the
// record is rewritten once, in the buffer, right after it is read -- the
// typewriter downstream then reveals the player's own name character by
// character with nothing else changed.
static void Msx2_StoryNameSub(void)
{
	c8* p = &g_text[1];
	u8 nlen = STORY_NAME_LEN;
	u8 len, i, j;

	while((nlen != 0) && (g_player_name[nlen - 1] == ' '))
		--nlen;
	if(nlen == 0)
		return;
	for(len = 0; p[len] != 0; ++len)
		;
	for(i = 0; (u8)(i + 6) <= len; ++i)
	{
		if((p[i] != 'S') || (p[i + 1] != 'E') || (p[i + 2] != 'R') ||
		   (p[i + 3] != 'E') || (p[i + 4] != 'N') || (p[i + 5] != 'A'))
			continue;
		if(nlen > 6)
		{
			u8 grow = (u8)(nlen - 6);
			if((u16)(len + grow) >= MSX2_LINE_STRIDE - 2)
				return;
			for(j = (u8)(len + grow); j > (u8)(i + 5); --j)
				p[j] = p[j - grow];
			len = (u8)(len + grow);
		}
		else if(nlen < 6)
		{
			u8 shrink = (u8)(6 - nlen);
			for(j = (u8)(i + nlen); j <= (u8)(len - shrink); ++j)
				p[j] = p[j + shrink];
			len = (u8)(len - shrink);
		}
		for(j = 0; j < nlen; ++j)
			p[i + j] = g_player_name[j];
		i = (u8)(i + nlen - 1);
	}
}

static void Msx2_StoryReadLine(u16 offset)
{
	Msx2_RomRead(MSX2_TEXT_SEGMENT, offset, (u8*)g_text, MSX2_LINE_STRIDE);
	g_text[MSX2_LINE_STRIDE - 1] = 0;
	Msx2_StoryNameSub();
}

// `field` 0 is the opponent's name, 1 their title.
static void Msx2_StoryReadOpp(u8 duel, u8 field)
{
	Msx2_RomRead(MSX2_TEXT_SEGMENT,
	             (u16)(MSX2_OPP_OFFSET + (u16)duel * MSX2_OPP_STRIDE
	                   + (u16)field * (MSX2_OPP_STRIDE / 2)),
	             (u8*)g_opp, MSX2_OPP_STRIDE / 2);
	g_opp[MSX2_OPP_STRIDE / 2 - 1] = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Story collection and continue codes
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_StoryBuildStarterDeck(void)
{
	WaifuDeck deck;
	WaifuDeckRng rng;
	u8 i;

	g_story_deck_ready = TRUE;
	waifu_deck_rng_seed(&rng, Msx2_StoryNameHash(g_player_name, STORY_NAME_LEN));
	waifu_deck_build_random(&deck, &rng, 0);
	for(i = 0; i < STORY_DECK_SIZE; ++i)
		g_story_deck[i] = (u8)deck.cards[i];
	/* Remembered before any swap is applied: the code writes slots 0-3 out
	   raw, so "this slot still holds what the seed dealt" is what tells the
	   editor a change can still be recorded there. */
	for(i = 0; i < STORY_EDIT_SLOTS; ++i)
		g_story_base[i] = g_story_deck[i];
	g_story_storage_count = 0;
	for(i = 0; i < STORY_STORAGE_SIZE; ++i)
		g_story_storage[i] = MSX2_CARD_NONE;
	/* Rewards use the duel index as their stable identity, so the collection
	   can be reconstructed from the progress bits in a continue code. */
	for(i = 0; i < g_progress && i < MSX2_STORY_MAX_DUELS; ++i)
		g_story_storage[g_story_storage_count++] =
			(u8)((i * 13 + 7) % MSX2_CARD_COUNT);
}

static bool Msx2_StoryParseCode(void)
{
	u8 swaps[STORY_EDIT_SLOTS];
	u8 i;
	if(!Msx2_StoryDecodeCode(g_code, g_code_len, &g_progress,
	                         g_player_name, swaps))
		return FALSE;
	if(g_progress > MSX2_STORY_MAX_DUELS)
		g_progress = MSX2_STORY_MAX_DUELS;
	Msx2_StoryBuildStarterDeck();
	for(i = 0; i < STORY_EDIT_SLOTS; ++i)
	{
		if(swaps[i] != g_story_deck[i])
		{
			/* A saved override is an exchange with the collection, not a free
			   card injection.  Reject a checksum-valid code that asks for a
			   card the player could not have earned or stored. */
			if(!Msx2_StoryTakeStorageCard(g_story_storage,
			                              &g_story_storage_count, swaps[i]))
				return FALSE;
			if(g_story_storage_count < STORY_STORAGE_SIZE)
				g_story_storage[g_story_storage_count++] = g_story_deck[i];
		}
		g_story_deck[i] = swaps[i];
	}
	g_duel_index = (g_progress >= MSX2_STORY_MAX_DUELS)
		? (MSX2_STORY_MAX_DUELS - 1) : g_progress;
	return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  The typewriter
// ─────────────────────────────────────────────────────────────────────────────

// Greedy word wrap into the three rows the box was baked for.  A line that
// would overflow them is cut rather than scrolled: the box is part of the
// picture, so there is nothing to scroll it in.
static void Msx2_StoryWrap(const c8* text)
{
	const c8* p = text;
	u8 row = 0;
	u8 col = 0;
	u8 i;

	for(i = 0; i < MSX2_TALK_LINES; ++i)
	{
		g_row[i][0] = 0;
		g_row_len[i] = 0;
	}

	while((*p != 0) && (row < MSX2_TALK_LINES))
	{
		u8 n = 0;
		while((p[n] != 0) && (p[n] != ' '))
			++n;
		if(n > MSX2_TALK_COLS)
			n = MSX2_TALK_COLS;

		if((col != 0) && ((u8)(col + 1 + n) > MSX2_TALK_COLS))
		{
			g_row[row][col] = 0;
			g_row_len[row] = col;
			++row;
			col = 0;
			if(row >= MSX2_TALK_LINES)
				break;
		}
		if(col != 0)
			g_row[row][col++] = ' ';
		for(i = 0; i < n; ++i)
			g_row[row][col++] = p[i];

		p += n;
		while(*p == ' ')
			++p;
	}

	if(row < MSX2_TALK_LINES)
	{
		g_row[row][col] = 0;
		g_row_len[row] = col;
	}

	g_total = 0;
	for(i = 0; i < MSX2_TALK_LINES; ++i)
		g_total = (u8)(g_total + g_row_len[i]);
}

// Put one character of the line on the draw page.  The reveal index counts
// through the rows end to end, so a page that is behind catches up simply by
// drawing the characters between its own cursor and the reveal.
static void Msx2_StoryDrawChar(u8 index)
{
	c8 one[2];
	u8 row = 0;

	while((row < MSX2_TALK_LINES) && (index >= g_row_len[row]))
	{
		index = (u8)(index - g_row_len[row]);
		++row;
	}
	if(row >= MSX2_TALK_LINES)
		return;

	one[0] = g_row[row][index];
	one[1] = 0;
	Msx2_TextAt((u8)(MSX2_TALK_TEXT_X + index * 6),
	            (u8)(MSX2_TALK_LINE0_Y + row * MSX2_TALK_LINE_STEP), one);
}

static const c8* Msx2_StorySpeakerName(void)
{
	if(g_speaker == 0)
		return g_player_name;
	if(g_speaker == 1)
	{
		Msx2_StoryReadOpp(g_duel_index, 0);
		return g_opp;
	}
	return "";
}

// Everything about a line that is not a character of it: the emptied box, the
// name plate, and (on the ending screen, which has no baked panel) the box
// frame itself.
static void Msx2_StoryDressLine(void)
{
	const c8* who;

	Msx2_Fill(BOX_X, (u8)BOX_Y, (u16)BOX_W, (u8)BOX_H, MSX2_PANEL_COLOR);
	if(g_phase == PH_NARRATE)
		Msx2_FrameRect(0, MSX2_TALK_BOX_Y, (u16)MSX2_SCREEN_W,
		               (u8)(MSX2_SCREEN_H - MSX2_TALK_BOX_Y), MSX2_GOLD);

	who = Msx2_StorySpeakerName();
	if(who[0] != 0)
	{
		Msx2_Fill(PLATE_X, (u8)PLATE_Y, (u16)PLATE_W, PLATE_H, MSX2_PLATE_COLOR);
		Msx2_FrameRect((u8)(PLATE_X - 1), (u8)(PLATE_Y - 1), (u16)(PLATE_W + 2),
		               (u8)(PLATE_H + 2), MSX2_GOLD);
		Msx2_TextColor(MSX2_GOLD, MSX2_PLATE_COLOR);
		Msx2_TextAt(MSX2_TALK_NAME_X, MSX2_TALK_NAME_Y, who);
	}
	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
}

// Start speaking the wrapped line.  Both pages owe the whole of it.
static void Msx2_StoryBeginLine(void)
{
	g_reveal = 0;
	g_drawn[0] = 0;
	g_drawn[1] = 0;
	g_fresh = ALL_PAGES;
	g_prompt = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Dialogue
// ─────────────────────────────────────────────────────────────────────────────

// ── §14.2: the composited visual-novel scene ─────────────────────────────────
//
// The backdrop is one of the shipped paintings with the text box baked into it,
// and it is streamed ONCE per scene.  Both speakers stand on it at the same
// time -- Serena on the left, the opponent on the right, the six-pixel stagger
// the other targets use -- and the inactive one is dimmed rather than removed.
// A speaker change is therefore two rect overwrites and nothing else: no
// stream, no flip, no page divergence, and two characters on screen where the
// flattened composite this replaces could only ever show one.

// One bust, from its baked run table.  A row is a handful of opaque runs, and a
// transparent pixel costs nothing at all -- not a VRAM write, not an address
// re-set -- which is what makes a 124-wide figure affordable (§1.2).
#ifdef MSX2_PORTRAIT_FRINGE_OFF
// WHICH BACKDROP THE FIGURES ARE STANDING ON.
// Edge pixels keep the chroma already in VRAM, so the byte written over one is
// a function of the picture behind it and the baker holds one table per talk
// scene.  This says which of them to read; it is a static rather than an
// argument so that the MSX2 cartridge, which has no such table, carries no
// such parameter.
//
// The byte itself is finished in the table -- YJK brightness or a YAE palette
// pixel, whichever the baker measured as closer to the intended composite
// (tools/msx2/gen_msx_plus.py, bust_edge_bytes).  It used to be decided here,
// out of a six-byte record carrying the fallback, the target RGB555, the
// fallback's error and a nominated palette index, against a copy of the
// scene's palette read back at every redraw.  All of that is bake-time
// knowledge, and paying for it per pixel per page cost more than the figure's
// whole interior did: nineteen kilobytes of table read out of the cartridge
// per bust per page, plus a weighted colour distance in C on each of some six
// hundred edge pixels.  Two bytes a record, and none of the arithmetic.
static u8 g_bust_stage;
#endif

// A ROM READ COSTS FAR MORE THAN THE BYTES IT MOVES.
// Msx2_RomRead is a call, two window-normalising loops, a seam check, a bank
// switch to the cartridge and another back; the `ldir` in the middle of it is
// the cheap part.  This blitter used to make three of them a row -- the run
// record, the fringe count, the fringe record -- 124 rows a figure, twice a
// figure, on both pages, for an average of about fourteen bytes each.  The
// calls cost more than everything else in a row put together.
//
// So both tables are read in blocks instead.  The run records are fixed-width
// and a block is sixteen rows of them; the fringe stream is variable-length, so
// a window slides along it and is refilled whenever less than a whole row's
// worst case is left.  Two hundred bytes of RAM turns roughly a thousand reads
// per speaker change into about sixty.
#define BUST_INDEX_ROWS   16     // must be a power of two: the row masks it
#define BUST_INDEX_BYTES  (BUST_INDEX_ROWS * MSX2_PORTRAIT_ROW_STRIDE)
static u8 g_bust_index[BUST_INDEX_BYTES];

#ifdef MSX2_PORTRAIT_FRINGE_OFF
// The count byte plus the widest row any bust has, which is what must be in
// hand before a row can be walked.
#define BUST_FRINGE_ROW   (1 + MSX2_PORTRAIT_FRINGE_MAX * MSX2_PORTRAIT_FRINGE_BYTES)
#define BUST_FRINGE_BUF   192
static u8 g_bust_fringe[BUST_FRINGE_BUF];
#endif

static void Msx2_StoryBlitBust(u8 chr, u8 x, u8 y, bool lit)
{
	u16 seg = MSX2_PORTRAIT_SEG(chr, lit);
	u16 pix = MSX2_PORTRAIT_PIXELS_OFF;    // the pixels follow the tables
	// The row cursor is lifted out of the loop: this runs 124 times a figure
	// and four times a speaker change, and a multiply per row is worth more
	// than it looks at that count.
	const u8* rec = g_bust_index;
	u8  row, k;
#ifdef MSX2_PORTRAIT_FRINGE_OFF
	// The draw page, likewise: it was a call per fringe row.
	u16 line = (u16)((u16)Msx2_VideoGetDrawPage() << 8) + y;
	// The edge of the figure, one pixel at a time (msx2_stream.c): a bust is
	// blitted as whole chroma groups here, and every solid pixel that shares
	// its group with the backdrop is in this table instead.  A row is a count
	// and then that many (x, ink) pairs, and the rows are walked in order --
	// which is what lets a window slide along the stream instead of seeking.
	u16 fringe = (u16)(MSX2_PORTRAIT_FRINGE_OFF
	                   + (u16)g_bust_stage * MSX2_PORTRAIT_FRINGE_STRIDE);
	u8  held = 0;                          // bytes of the window not yet walked
	u8  fp = 0;                            // ... and where they start in it
	u8  edge_n;
#endif

	for(row = 0; row < MSX2_PORTRAIT_H; ++row)
	{
		// The right-hand bust stands six pixels lower than the left one, so its
		// last rows fall inside the text box.  They are clipped here rather
		// than by moving the figure: the run table still has to be walked to
		// keep the pixel cursor in step, only the blit is skipped.
		u8 sy = (u8)(y + row);
		bool visible = (sy < MSX2_TALK_BOX_Y);
		if(!(row & (BUST_INDEX_ROWS - 1)))
		{
			Msx2_RomRead(seg, (u16)((u16)row * MSX2_PORTRAIT_ROW_STRIDE),
			             g_bust_index, BUST_INDEX_BYTES);
			rec = g_bust_index;
		}
		for(k = 0; k < rec[0]; ++k)
		{
			u8  rx = rec[1 + k * 2];
			u8  rn = rec[2 + k * 2];
			u16 s = seg;
			u16 o = pix;
			u8  head;

			while(o >= 0x4000u) { o -= 0x4000u; ++s; }
			// A run may straddle the segment boundary, so the tail is a second
			// blit rather than a read that runs off the end of the window.
			head = ((u16)rn > (0x4000u - o)) ? (u8)(0x4000u - o) : rn;
			if(visible)
			{
				Msx2_StreamRect(s, o, (u8)(x + rx), sy, head, 1);
				if(head != rn)
					Msx2_StreamRect((u16)(s + 1), 0, (u8)(x + rx + head),
					                sy, (u8)(rn - head), 1);
			}
			pix = (u16)(pix + rn);
		}
#ifdef MSX2_PORTRAIT_FRINGE_OFF
		// Top the window up if this row could outrun it.  The tail that is
		// left keeps its place at the front; the read that follows it may run
		// past the end of the table and into whatever is next, which is never
		// walked and so never matters.
		if(held < BUST_FRINGE_ROW)
		{
			for(k = 0; k < held; ++k)
				g_bust_fringe[k] = g_bust_fringe[(u8)(fp + k)];
			Msx2_RomRead(seg, fringe, g_bust_fringe + held,
			             (u8)(BUST_FRINGE_BUF - held));
			fringe = (u16)(fringe + (BUST_FRINGE_BUF - held));
			held = BUST_FRINGE_BUF;
			fp = 0;
		}
		edge_n = g_bust_fringe[fp++];
		--held;
		if(edge_n)
		{
			if(visible)
				// The columns are the figure's own, so the blitter adds x as
				// it walks rather than a second pass over the record doing it.
				Msx2_MergeRow(g_bust_fringe + fp, edge_n, line, x);
			edge_n = (u8)(edge_n * MSX2_PORTRAIT_FRINGE_BYTES);
			fp = (u8)(fp + edge_n);
			held = (u8)(held - edge_n);
		}
#endif
		rec += MSX2_PORTRAIT_ROW_STRIDE;
#ifdef MSX2_PORTRAIT_FRINGE_OFF
		++line;
#endif
	}
}

// Both busts, with `speaker` lit and the other dimmed.  The two brightness
// variants are cut from the same alpha mask, so this covers byte for byte the
// pixels the previous pair covered and nothing behind them has to be repaired.
static void Msx2_StoryBusts(u8 speaker)
{
#ifdef MSX2_PORTRAIT_FRINGE_OFF
	g_bust_stage = g_msx2_stage_for_duel[g_duel_index];
#endif
	Msx2_StoryBlitBust(0, MSX2_PORTRAIT_LEFT_X, MSX2_PORTRAIT_LEFT_Y,
	                   speaker != 1);
	Msx2_StoryBlitBust((u8)(1 + g_duel_index), MSX2_PORTRAIT_RIGHT_X,
	                   MSX2_PORTRAIT_RIGHT_Y, speaker == 1);
}

// Change who is speaking.  Both pages are painted here and now rather than
// journalled: it is an eighth of a second, it happens once a line at most, and
// carrying it in a dirty mask would mean the two buffers disagreed about which
// character was lit for a frame.
static void Msx2_StoryShowShot(u8 shot)
{
	u8 show = Msx2_VideoGetShowPage();
	u8 page = (u8)(show ^ 1);

	if(g_phase != PH_TALK)
	{
		// Narration has no second speaker: the opening is Serena remembering
		// over her own scene, the ending is a voice over the closing painting.
		u16 segment = (g_narr_which == NARR_INTRO)
		            ? MSX2_TALK_SEGMENT(0) : MSX2_SCENE_ENDING_SEGMENT;
		// Stream and compose only while the display is blank.  The old opening
		// and Kasem paths streamed page 1, re-enabled the display, and then
		// blitted the portrait into whichever page happened to be visible.  A
		// scan could therefore catch half a backdrop and half a bust.
		Msx2_VideoDisplayBlank();
		Msx2_VideoModeYjk();
		Msx2_VideoDrawPage(page);
		Msx2_StreamSceneBlanked(segment, page);
		Msx2_VideoScenePalette(segment);
		if(g_narr_which == NARR_INTRO)
		{
			// The opening narration stands Serena on stage 0's picture, which
			// is the segment streamed just above.
#ifdef MSX2_PORTRAIT_FRINGE_OFF
			g_bust_stage = 0;
#endif
			Msx2_StoryBlitBust(0, MSX2_PORTRAIT_LEFT_X, MSX2_PORTRAIT_LEFT_Y,
			                   TRUE);
		}
		Msx2_VideoCopyPage(page, show);
		Msx2_VideoShowPage(page);
		Msx2_VideoDisplayRestore();
		g_shot = shot;
		return;
	}

	if(g_shot == 0xFF)
	{
		// First line of the scene: the painting, then both figures on it.
		Msx2_VideoDisplayBlank();
		Msx2_VideoModeYjk();
		Msx2_VideoDrawPage(page);
		Msx2_StreamSceneBlanked(
			MSX2_TALK_SEGMENT(g_msx2_stage_for_duel[g_duel_index]), page);
		Msx2_VideoScenePalette(
			MSX2_TALK_SEGMENT(g_msx2_stage_for_duel[g_duel_index]));
		Msx2_StoryBusts(shot);
		Msx2_VideoCopyPage(page, show);
		Msx2_VideoShowPage(page);
		Msx2_VideoDisplayRestore();
	}
	else
	{
		// A SPEAKER CHANGE IS A REDRAW OF TWO FIGURES, NOT OF THE SCREEN.
		// It used to blank the output for the whole of it -- and relighting two
		// 124-wide busts out of the cartridge is an eighth of a second, so the
		// scene went black between every pair of lines.  Nothing here touches
		// the backdrop, and the port has two pages: the new pair is composed on
		// the hidden one with the output up, and the flip is the only moment
		// anything changes on the screen.
		//
		// The hidden page is levelled off the visible one FIRST.  It is a whole
		// frame behind -- the typewriter draws one page a frame -- so composing
		// straight onto it published the PREVIOUS line's text, half-typed,
		// under the new speaker's name.  That is the flicker that looked like a
		// page-flip fault on the way from one speaker to the next.
		Msx2_VideoDrawPage(page);
		Msx2_VideoCopyPage(show, page);
		Msx2_StoryBusts(shot);
		// The box goes with them.  The caller is about to type a new line into
		// it, one page a frame, and without this the old line stayed on screen
		// underneath the new name plate until the frame after the flip.
		Msx2_StoryDressLine();
		VDP_CommandWait();
		Msx2_VideoShowPage(page);
		Msx2_VideoCopyPage(page, show);
	}
	g_shot = shot;
}

static void Msx2_StoryLoadTalkLine(void)
{
	Msx2_StoryReadLine((u16)(MSX2_DIALOGUE_OFFSET
	                         + ((u16)g_duel_index * MSX2_LINES_PER_DUEL
	                            + (u16)g_line) * MSX2_LINE_STRIDE));
	g_speaker = (u8)g_text[0];
	if(g_speaker < 2)
	{
		if(g_shot != g_speaker)
			Msx2_StoryShowShot(g_speaker);
	}
	Msx2_StoryWrap(&g_text[1]);
	Msx2_StoryBeginLine();
}

static void Msx2_StoryEnterTalk(void)
{
	// The seal, once more at the door: no path into a duel may name an
	// opponent past the frontier, whatever the map did with its cursor.
	if((g_duel_index >= MSX2_STORY_MAX_DUELS) || (g_duel_index > g_progress))
		g_duel_index = (g_progress >= MSX2_STORY_MAX_DUELS)
			? (MSX2_STORY_MAX_DUELS - 1) : g_progress;
	g_phase = PH_TALK;
	g_line = 0;
	g_line_count = g_msx2_dialogue_count[g_duel_index];
	g_shot = 0xFF;                       // nothing is up yet, so force a stream
	Msx2_MusicPlay(MSX2_MUSIC_OPENING);
	// The scene is built once, here, whoever speaks first -- including a
	// narrator line, which changes no portrait and so would never build it.
	Msx2_StoryShowShot(0);
	Msx2_InputFlush();
	Msx2_StoryLoadTalkLine();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Narration
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_StoryLoadNarrLine(void)
{
	u16 base = (g_narr_which == NARR_INTRO) ? MSX2_INTRO_OFFSET : MSX2_ENDING_OFFSET;

	Msx2_StoryReadLine((u16)(base + (u16)g_line * MSX2_LINE_STRIDE));
	// The opening is Serena remembering, over her own composite, so it fills
	// the name plate that picture was baked with.  The ending is narration over
	// artwork that has no plate at all.
	g_speaker = (g_narr_which == NARR_INTRO) ? 0 : 2;
	Msx2_StoryWrap(&g_text[1]);
	Msx2_StoryBeginLine();
}

static void Msx2_StoryEnterNarration(u8 which)
{
	g_phase = PH_NARRATE;
	g_narr_which = which;
	g_line = 0;
	g_line_count = (which == NARR_INTRO) ? MSX2_INTRO_COUNT : MSX2_ENDING_COUNT;
	Msx2_MusicPlay((which == NARR_INTRO) ? MSX2_MUSIC_OPENING : MSX2_MUSIC_RESULT);
	Msx2_StoryShowShot(0);
	Msx2_InputFlush();
	Msx2_StoryLoadNarrLine();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Continue-code screens
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_StoryUiDirty(void)
{
	g_map_dirty = ALL_PAGES;
}

// THE CODE, WRITTEN OUT THE WAY A PASSWORD SCREEN WRITES IT.
// Sixteen characters at the font's own six-pixel pitch are a solid word: a
// player copying one off the screen onto paper cannot tell where one letter
// ends and the next begins.  So the code is laid out as two rows of eight
// cells, each character alone in its own sunken box, exactly as the cartridge
// password screens this is borrowing from do it.
#define CODE_COLS     8
#define CODE_CELL_W   24
#define CODE_CELL_H   14
#define CODE_GRID_X   ((MSX2_SCREEN_W - CODE_COLS * CODE_CELL_W) / 2)
#define CODE_GRID_Y   44
#define CODE_ROW_STEP 18

// THE FLOPPY, WHEN THERE IS ONE.
// The cartridge has no battery, so the save is a code the player copies down.
// A machine with a drive can copy it down instead: one sector, written through
// the disk ROM (msx2_disk.c).  The offer only appears when a drive answered,
// and it is on F1 rather than on a letter because both of these screens are
// screens the player types into.
static void Msx2_StoryDiskLine(u8 y)
{
	u8 id;

	if(g_disk_msg == 1)      id = MSX2_S_SAVED_TO_DISK;
	else if(g_disk_msg == 2) id = MSX2_S_DISK_ERROR_USE_A_BLANK_DISK;
	else if(g_disk_msg == 4) id = MSX2_S_NO_DRIVE_ANSWERED;
	else if(g_disk_msg == 3) id = MSX2_S_NO_SAVE_ON_THIS_DISK;
	else if(!Msx2_DiskPresent())
		return;
	else if(g_phase == PH_CODE_OUT) id = MSX2_S_F1_SAVES_TO_DISK;
	else                     id = MSX2_S_F1_LOADS_FROM_DISK;
	Msx2_TextColor((g_disk_msg == 2) || (g_disk_msg == 3) || (g_disk_msg == 4)
	               ? MSX2_RED : MSX2_TEAL,
	               MSX2_PANEL_COLOR);
	Msx2_TextCenter(y, Msx2_UiText(id));
}

// THE GRID IS DRAWN AT THE PITCH THE MARKER MOVES AT.
// The four alphabet rows were one solid eight-character string at the font's
// own six-pixel pitch, while the gold marker was placed at twenty-seven pixels
// a column: they agreed on the first letter of a row and on nothing after it,
// so the letter the marker sat on was never the letter the button would take --
// and the marker landed clean off the end of the row it belonged to, where a
// partial repaint then had nothing to erase it with.  One pitch now serves
// both: a space after every letter, which is twelve pixels.
#define CODE_GRID_COLS 8
#define CODE_GRID_ROWS 4
#define CODE_PITCH     12
#define CODE_ALPHA_X   27
#define CODE_ALPHA_Y   90

static void Msx2_StoryCodeAlphaRow(u8 first, u8 y)
{
	c8 row[CODE_GRID_COLS * 2];
	u8 i;

	for(i = 0; i < CODE_GRID_COLS; ++i)
	{
		row[i * 2] = (c8)CODE_ALPHABET[first + i];
		row[i * 2 + 1] = ' ';
	}
	row[CODE_GRID_COLS * 2 - 1] = 0;
	Msx2_TextAt(CODE_ALPHA_X, y, row);
}

// ONE CELL, AND ONLY THE CELLS A KEYSTROKE TOUCHES.
// Sixteen cells is sixteen fills, sixteen borders and sixteen single-character
// draws, and this screen redraws on every letter of a code -- on each of two
// pages.  On this machine that is long enough for the next letter to be pressed
// and released before the keyboard is looked at again, so typing a code at an
// ordinary speed lost letters.  A keystroke moves exactly two cells: the one
// that gains the character and the one the marker moves to.  Each page
// remembers the length it was last drawn at, so a page that missed a beat
// redraws the whole span it missed rather than just the last cell.
static u8 g_code_seen[MSX2_VIDEO_PAGES];
// The picker letter and the message state each page was last drawn with, so a
// refresh knows the two letters and the one line it actually has to touch.
static u8 g_code_cur_seen[MSX2_VIDEO_PAGES];
static u8 g_code_msg_seen[MSX2_VIDEO_PAGES];

static void Msx2_StoryCodeCell(u8 i)
{
	u8 x = (u8)(CODE_GRID_X + (i % CODE_COLS) * CODE_CELL_W);
	u8 y = (u8)(CODE_GRID_Y + (i / CODE_COLS) * CODE_ROW_STEP);
	bool here = (g_phase == PH_CODE_IN) && (i == g_code_len);
	// THE CODE SCREEN HAS TO SHOW THE CODE.
	// `g_code_len` counts what the player has TYPED, and nothing types on the
	// way out -- so the screen that exists to be copied onto paper drew sixteen
	// empty cells.  Coming out, every cell is filled by definition:
	// Msx2_StoryBuildCode() wrote all STORY_CODE_LEN of them.
	bool filled = (g_phase == PH_CODE_OUT) || (i < g_code_len);
	c8 one[2];

	one[1] = 0;
	// Two fills, not five: the border is the whole cell painted in the frame
	// colour with the plate laid back on top of it.  Msx2_FrameRect is four
	// separate commands.
	Msx2_Fill(x, y, CODE_CELL_W - 2, CODE_CELL_H,
	          here ? MSX2_GOLD : MSX2_DARK_SAND);
	Msx2_Fill((u8)(x + 1), (u8)(y + 1), CODE_CELL_W - 4, CODE_CELL_H - 2,
	          MSX2_PLATE_COLOR);
	if(filled)
	{
		one[0] = g_code[i];
		Msx2_TextColor(MSX2_WHITE, MSX2_PLATE_COLOR);
	}
	else
	{
		one[0] = '-';
		Msx2_TextColor(here ? MSX2_GOLD : MSX2_DARK_SAND, MSX2_PLATE_COLOR);
	}
	Msx2_TextAt((u8)(x + (CODE_CELL_W - 2 - 6) / 2), (u8)(y + 3), one);
}

static void Msx2_StoryCodeCells(bool all)
{
	u8 page = Msx2_VideoGetDrawPage();
	u8 first = 0;
	u8 last = STORY_CODE_LEN - 1;
	u8 i;

	if(!all)
	{
		u8 seen = g_code_seen[page];
		first = (seen < g_code_len) ? seen : g_code_len;
		last = (seen > g_code_len) ? seen : g_code_len;
		if(last >= STORY_CODE_LEN)
			last = STORY_CODE_LEN - 1;
	}
	for(i = first; i <= last; ++i)
		Msx2_StoryCodeCell(i);
	g_code_seen[page] = g_code_len;
}

// One letter of the picker grid, in the colour it is wanted.
static void Msx2_StoryCodeAlphaCell(u8 i, u8 color)
{
	c8 one[2];

	one[0] = (c8)CODE_ALPHABET[i];
	one[1] = 0;
	Msx2_TextColor(color, MSX2_PANEL_COLOR);
	Msx2_TextAt((u8)(CODE_ALPHA_X + (i % CODE_GRID_COLS) * CODE_PITCH),
	            (u8)(CODE_ALPHA_Y + (i / CODE_GRID_COLS) * 12), one);
}

// EVERYTHING A KEYPRESS CHANGES, AND NOTHING ELSE.
// A code screen used to be repainted whole for every letter -- the panel fill,
// the gold frame, the title, two help lines that never change, all sixteen
// cells and all thirty-two picker letters -- and each of those help lines is a
// string read out of the cartridge with interrupts held off.  On this machine
// that adds up to longer than a person leaves a key down, which is why typing a
// continue code lost letters.  A keystroke moves two cells and two picker
// letters; a message line changes only when a result does.  `all` is the
// compose on entry, which of course draws the lot.
static void Msx2_StoryCodeRefresh(bool all)
{
	u8 page = Msx2_VideoGetDrawPage();
	u8 msg = (u8)((g_code_error ? 0x80 : 0) | g_disk_msg);

	Msx2_StoryCodeCells(all);

	if(g_phase == PH_CODE_IN)
	{
		if(all)
		{
			u8 row;
			Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
			for(row = 0; row < CODE_GRID_ROWS; ++row)
				Msx2_StoryCodeAlphaRow((u8)(row * CODE_GRID_COLS),
				                       (u8)(CODE_ALPHA_Y + row * 12));
		}
		else if(g_code_cur_seen[page] != g_code_cursor)
			Msx2_StoryCodeAlphaCell(g_code_cur_seen[page], MSX2_WHITE);
		Msx2_StoryCodeAlphaCell(g_code_cursor, MSX2_GOLD);
		g_code_cur_seen[page] = g_code_cursor;
	}

	if(all || (g_code_msg_seen[page] != msg))
	{
		if(g_phase == PH_CODE_IN)
		{
			Msx2_Fill(15, 141, 226, 10, MSX2_PANEL_COLOR);
			Msx2_TextColor(g_code_error ? MSX2_RED : MSX2_DARK_SAND,
			               MSX2_PANEL_COLOR);
			Msx2_TextCenter(143, g_code_error
			                ? Msx2_UiText(MSX2_S_INVALID_CODE_TRY_AGAIN)
			                : Msx2_UiText(MSX2_S_TYPE_IT_OR_PICK_AND_PRESS_SP));
		}
		Msx2_Fill(15, 162, 226, 10, MSX2_PANEL_COLOR);
		Msx2_StoryDiskLine(167);
		g_code_msg_seen[page] = msg;
	}
}

static void Msx2_StoryCodePaint(void)
{
	Msx2_Fill(14, 20, 228, 170, MSX2_PANEL_COLOR);
	Msx2_FrameRect(14, 20, 228, 170, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(30, (g_phase == PH_CODE_OUT) ? Msx2_UiText(MSX2_S_CONTINUE_CODE) : Msx2_UiText(MSX2_S_ENTER_CONTINUE_CODE));
	if(g_phase == PH_CODE_OUT)
	{
		Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
		Msx2_TextCenter(90, Msx2_UiText(MSX2_S_WRITE_THIS_DOWN));
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextCenter(139, Msx2_UiText(MSX2_S_ENTER_IT_ON_THE_TITLE_SCREEN));
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextCenter(153, Msx2_UiText(MSX2_S_SPACE_ESC_RETURN_TO_ROAD));
	}
	else
	{
		Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
		Msx2_TextCenter(155, Msx2_UiText(MSX2_S_ESC_DELETE_EMPTY_ESC_BACK));
	}
	Msx2_StoryCodeRefresh(TRUE);
}

// The compose draws one page and copies it to the other, so tell both pages
// what they are now showing -- otherwise the copied page's first incremental
// refresh would repair a span that is already right, or worse, miss one.
static void Msx2_StoryCodeSeenAll(void)
{
	u8 i;
	for(i = 0; i < MSX2_VIDEO_PAGES; ++i)
	{
		g_code_seen[i] = g_code_len;
		g_code_cur_seen[i] = g_code_cursor;
		g_code_msg_seen[i] = (u8)((g_code_error ? 0x80 : 0) | g_disk_msg);
	}
}

static void Msx2_StoryEnterCodeInput(void)
{
	g_phase = PH_CODE_IN;
	g_code_len = 0;
	g_code[0] = 0;
	g_code_cursor = 0;
	g_code_error = 0;
	g_disk_msg = 0;
	g_want_quit = 0;
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_SCENE_TITLE_SEGMENT, MSX2_PAGE_1);
	Msx2_StoryCodePaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_InputFlush();
	Msx2_StoryCodeSeenAll();
	g_map_dirty = 0;
}

static void Msx2_StoryEnterCodeOutput(void)
{
	g_phase = PH_CODE_OUT;
	Msx2_StoryBuildCode(g_code, g_progress, g_player_name, g_story_deck);
	// It used to mark the pages dirty and leave the backdrop to whatever the
	// previous screen happened to have streamed -- which was the title picture
	// the save picker brought with it.  Compose it here, on the road, exactly
	// as the map and the picker do, so both pages are finished before either is
	// shown.
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStageForProgress(g_progress)),
	                 MSX2_PAGE_1);
	Msx2_StoryCodePaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_InputFlush();
	Msx2_StoryCodeSeenAll();
	g_map_dirty = 0;
}

// Saving is an explicit choice, rather than the old map item that silently
// dropped straight into a password screen.  The code is built before the
// choice so either destination produces the same save, and the code screen is
// always shown afterwards as a paper backup.
#define SAVE_ROW_Y(n)  (u8)(84 + (n) * 22)

static void Msx2_StoryEnterMap(void);

static void Msx2_StorySavePickPaint(void)
{
	bool disk = Msx2_DiskPresent();
	u8 i;

	Msx2_Fill(38, 52, 180, 108, MSX2_PANEL_COLOR);
	Msx2_FrameRect(38, 52, 180, 108, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(62, Msx2_UiText(MSX2_S_SAVE_GAME));
	for(i = 0; i < 2; ++i)
	{
		u8 y = SAVE_ROW_Y(i);
		bool live = (i != 0) || disk;
		Msx2_Fill(52, (u8)(y - 2), 12, 11, MSX2_PANEL_COLOR);
		if(i == g_save_pick)
		{
			Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
			Msx2_TextAt(54, y, ">");
		}
		Msx2_TextColor(live ? ((i == g_save_pick) ? MSX2_WHITE : MSX2_SAND)
		                    : MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(72, y, Msx2_UiText((i == 0) ? MSX2_S_FLOPPY_DISK
		                                        : MSX2_S_PASSWORD));
	}

	Msx2_Fill(40, 128, 176, 10, MSX2_PANEL_COLOR);
	if(!disk && g_save_pick == 0)
	{
		Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
		Msx2_TextCenter(130, Msx2_UiText(MSX2_S_NO_DRIVE_ANSWERED));
	}
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(146, Msx2_UiText(MSX2_S_SPACE_PICKS_ESC_RETURNS));
}

static void Msx2_StoryEnterSavePick(void)
{
	g_phase = PH_SAVE_PICK;
	g_save_pick = Msx2_DiskPresent() ? 0 : 1;
	g_disk_msg = 0;
	// The A edge that selected SAVE GAME belongs to the map phase.  Consume it
	// at the destination boundary so a slow frame cannot also activate the
	// picker (or a future phase handler) with the same edge.
	Msx2_InputConsume(MSX2_BTN_A | MSX2_BTN_ENTER | MSX2_BTN_B);
	Msx2_StoryBuildCode(g_code, g_progress, g_player_name, g_story_deck);
	// THE ROAD, NOT THE TITLE SCREEN.
	// SAVE GAME is reached from the sanctum road and returns to it, so it wears
	// the road's own artwork.  Streaming the title picture put the logo and the
	// copyright line -- and whatever the title screen had left in the sprite
	// planes -- behind a panel that belongs to the middle of a run.
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStageForProgress(g_progress)),
	                 MSX2_PAGE_1);
	Msx2_StorySavePickPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_InputFlush();
	g_map_dirty = 0;
}

static void Msx2_StorySavePickStep(void)
{
	u8 pressed = Msx2_InputPressed();

	if(pressed & (MSX2_BTN_UP | MSX2_BTN_DOWN))
	{
		g_save_pick ^= 1;
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		Msx2_StoryUiDirty();
	}
	if(pressed & MSX2_BTN_B)
	{
		Msx2_StoryEnterMap();
		return;
	}
	if(pressed & (MSX2_BTN_A | MSX2_BTN_ENTER))
	{
		Msx2_SfxPlay(MSX2_SFX_CONFIRM);
		if(g_save_pick == 0)
		{
			if(!Msx2_DiskPresent())
			{
				g_disk_msg = 4;
				Msx2_StoryUiDirty();
				return;
			}
			g_disk_msg = Msx2_DiskSave(g_code) ? 1 : 2;
			if(g_disk_msg == 2)
			{
				Msx2_StoryUiDirty();
				return;
			}
		}
		else
			g_disk_msg = 0;
		Msx2_StoryEnterCodeOutput();
		return;
	}
	if(g_map_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
	{
		Msx2_StorySavePickPaint();
		g_map_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
		Msx2_VideoFlipRequest();
	}
}

static void Msx2_StoryEnterDeck(void);

// The selector, at (x, y) -- the top-left of the gem itself.  Call it every
// frame the screen is up: `frame >> 2` is the same quarter-turn every four
// V-blanks the duel screen spins it at, so the two screens agree.
static void Msx2_StoryGem(u8 x, u8 y)
{
	++g_gem_tick;
	Msx2_SpriteGem(x, y, (u8)(g_gem_tick >> 2), MSX2_SPR_RED);
	g_gem_up = TRUE;
}

// Take it down on the way to a screen that has no list.  Msx2_StreamScene()
// clears the whole sprite plane, so this is only for the screens that do not
// stream -- and it is one test, not three attribute writes, on every other
// frame of the run.
static void Msx2_StoryGemHide(void)
{
	if(g_gem_up)
	{
		Msx2_SpriteHideGem();
		g_gem_up = FALSE;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  The map
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_StoryMapPaint(void)
{
	u8 i;

	Msx2_Fill((u8)(MSX2_MAP_PANEL_X + 1), (u8)(MSX2_MAP_PANEL_Y + 1),
	          (u16)(MSX2_MAP_PANEL_W - 2), (u8)(MSX2_MAP_PANEL_H - 2), MSX2_PANEL_COLOR);

	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(MAP_HEAD_Y, Msx2_StoryStageNameForProgress(g_progress));

	for(i = 0; i < MSX2_STORY_MAX_DUELS; ++i)
	{
		u8 y = MAP_ROW_Y(i);
		bool open = (i <= g_progress);

		if(!open)
		{
			Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
			Msx2_TextAt(MAP_NAME_X, y, Msx2_UiText(MSX2_S_SEALED));
			continue;
		}

		Msx2_StoryReadOpp(i, 0);
		// Cleared opponents in sand, the frontier in teal.  Nothing here
		// depends on where the cursor is any more -- that is what lets the
		// panel be painted once and then left alone.
		Msx2_TextColor((i < g_progress) ? MSX2_SAND : MSX2_TEAL,
		               MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_NAME_X, y, g_opp);
		Msx2_StoryReadOpp(i, 1);
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_TITLE_X, y, g_opp);
	}

	Msx2_TextColor(MSX2_SAND, MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_DECK_Y, Msx2_UiText(MSX2_S_DECK_EDITOR));

	Msx2_TextColor(MSX2_SAND, MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_CODE_Y, Msx2_UiText(MSX2_S_SAVE_GAME));

	Msx2_TextColor(MSX2_SAND, MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_BACK_Y, Msx2_UiText(MSX2_S_LEAVE_THE_ROAD));

	Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(MAP_HELP_Y, Msx2_UiText(MSX2_S_SPACE_CHOOSES_UP_DOWN_MOVES));
}

// IS THIS ROW A SEALED OPPONENT?
// The list shows every opponent, cleared, current and sealed, because the road
// ahead is part of what the screen is for -- but a sealed one is not a place
// the cursor may stand.  DOWN has always stepped over them; UP walked straight
// into them from DECK EDITOR / SAVE GAME / LEAVE THE ROAD, and SPACE there
// started the duel, so the whole story could be played out of order from the
// bottom of the list upwards.  Both directions and the confirm ask this now.
static bool Msx2_StoryRowSealed(u8 row)
{
	return (row < MSX2_STORY_MAX_DUELS) && (row > g_progress);
}

// The screen line the cursor's row is written on.  The three utility rows are
// not on the duel rows' pitch, so this is a lookup and not arithmetic.
static u8 Msx2_StoryMapRowY(void)
{
	if(g_cursor == MAP_DECK_ROW)
		return MAP_DECK_Y;
	if(g_cursor == MAP_CODE_ROW)
		return MAP_CODE_Y;
	if(g_cursor == MAP_BACK_ROW)
		return MAP_BACK_Y;
	return MAP_ROW_Y(g_cursor);
}

static void Msx2_StoryEnterMap(void)
{
	g_phase = PH_MAP;
	g_cursor = g_duel_index;
	if(g_cursor > g_progress)
		g_cursor = g_progress;
	g_map_dirty = ALL_PAGES;

	Msx2_MusicPlay(MSX2_MUSIC_OVERWORLD);
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStageForProgress(g_progress)), MSX2_PAGE_1);
	Msx2_StoryMapPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_InputFlush();
	g_map_dirty = 0;
}

static u8 Msx2_StoryMapStep(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 before = g_cursor;
	u8 page = Msx2_VideoGetDrawPage();

	if((pressed & MSX2_BTN_UP) && (g_cursor != 0))
	{
		--g_cursor;
		// Up out of the utility rows lands on the frontier, never past it.
		if(Msx2_StoryRowSealed(g_cursor))
			g_cursor = g_progress;
	}
	if((pressed & MSX2_BTN_DOWN) && (g_cursor < MAP_BACK_ROW))
	{
		// A locked opponent is shown but never reachable, so the cursor steps
		// straight from the frontier to the utility rows.
		if(g_cursor < g_progress)
			++g_cursor;
		else if(g_cursor == g_progress)
			g_cursor = MAP_DECK_ROW;
		else
			++g_cursor;
	}
	if(g_cursor != before)
		Msx2_SfxPlay(MSX2_SFX_SELECT);

	if(pressed & MSX2_BTN_A)
	{
		// Nothing may open a sealed opponent, whatever put the cursor there.
		if(Msx2_StoryRowSealed(g_cursor))
		{
			g_cursor = g_progress;
			Msx2_SfxPlay(MSX2_SFX_SELECT);
			return MSX2_STORY_BUSY;
		}
		Msx2_SfxPlay(MSX2_SFX_CONFIRM);
		// TAKE THE SELECTOR DOWN BEFORE THE NEXT SCREEN IS BUILT.
		// Msx2_StoryStep_In() hides it on the frame AFTER the phase changes,
		// and the screens below compose themselves inside this call -- so the
		// red gem was still standing on the talk scene while its portrait was
		// being streamed in, for as long as that took.  The two screens that
		// own a selector put it back up on their own frame.
		Msx2_StoryGemHide();
		if(g_cursor == MAP_DECK_ROW)
		{
			Msx2_StoryEnterDeck();
			return MSX2_STORY_BUSY;
		}
		if(g_cursor == MAP_CODE_ROW)
		{
			Msx2_StoryEnterSavePick();
			return MSX2_STORY_BUSY;
		}
		if(g_cursor == MAP_BACK_ROW)
			return MSX2_STORY_QUIT;
		g_duel_index = g_cursor;
		Msx2_StoryEnterTalk();
		return MSX2_STORY_BUSY;
	}
	if(pressed & MSX2_BTN_B)
		return MSX2_STORY_QUIT;

	// The whole of the cursor: a few attribute writes, no flip, and the panel
	// underneath untouched since the screen was composed.
	Msx2_StoryGem(MAP_GEM_X, (u8)(Msx2_StoryMapRowY() - MAP_GEM_RISE));

	if(g_map_dirty & (u8)(1u << page))
	{
		Msx2_StoryMapPaint();
		g_map_dirty &= (u8)~(1u << page);
		Msx2_VideoFlipRequest();
	}
	return MSX2_STORY_BUSY;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Speaking a line
// ─────────────────────────────────────────────────────────────────────────────

// Returns TRUE when the run of lines is over.
static bool Msx2_StoryTextStep(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 page = Msx2_VideoGetDrawPage();
	bool painted = FALSE;

#ifdef MSX2_DEBUG_STORY_AUTOPLAY
	/* The story soak advances every text beat without adding input behavior to
	   the shipping build. */
	pressed |= MSX2_BTN_A;
#endif

	if(pressed & (MSX2_BTN_A | MSX2_BTN_B))
	{
		if(g_reveal < g_total)
		{
			// A player who reads faster than the typewriter gets the rest at
			// once; the same press must not also advance the line.
			g_reveal = g_total;
		}
		else
		{
			++g_line;
			if(g_line >= g_line_count)
				return TRUE;
			if(g_phase == PH_TALK)
				Msx2_StoryLoadTalkLine();
			else
				Msx2_StoryLoadNarrLine();
			Msx2_SfxPlay(MSX2_SFX_SELECT);
		}
	}
	else if(g_reveal < g_total)
	{
		g_reveal = (u8)(g_reveal + REVEAL_RATE);
		if(g_reveal > g_total)
			g_reveal = g_total;
	}

	if(g_fresh & (u8)(1u << page))
	{
		Msx2_StoryDressLine();
		g_drawn[page] = 0;
		g_fresh &= (u8)~(1u << page);
		painted = TRUE;
	}

	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	while(g_drawn[page] < g_reveal)
	{
		Msx2_StoryDrawChar(g_drawn[page]);
		++g_drawn[page];
		painted = TRUE;
	}

	if(g_reveal == g_total)
	{
		if(!(g_prompt & (u8)(1u << page)))
		{
			Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
			Msx2_TextCenter(MSX2_TALK_PROMPT_Y, Msx2_UiText(MSX2_S_PUSH_SPACE));
			g_prompt |= (u8)(1u << page);
			painted = TRUE;
		}
	}

	if(painted)
		Msx2_VideoFlipRequest();
	return FALSE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  The deck editor
//
//  The same screen the other targets have: a DECK tab and a STORAGE tab, the
//  whole of whichever one is up listed on screen, an art panel showing the card
//  under the cursor, and one button that moves a card from one side to the
//  other.  Left and right change tabs -- the list is one column, so the two
//  directions the cursor does not need are the tab keys.
//
//  THE ONE RULE THIS SCREEN HAS AND THE OTHERS DO NOT: an exchange, not an
//  add and a remove.  The cartridge has no battery, so the save is the
//  sixteen-symbol continue code, and the code carries four card ids
//  (STORY_EDIT_SLOTS x 7 bits) against a deck the seed regenerates.  A deck
//  that could grow or shrink could not be written down at all, and no more than
//  four of its cards can differ from the dealt one.  So the deck is always
//  forty cards, a swap always trades one of them for one in storage, and the
//  editor refuses the fifth change rather than making one it cannot save.
// ─────────────────────────────────────────────────────────────────────────────

static u8 Msx2_StoryDeckCount(void)
{
	return g_ed_tab ? g_story_storage_count : (u8)STORY_DECK_SIZE;
}

static u8 Msx2_StoryDeckCardAt(u8 idx)
{
	return g_ed_tab ? g_story_storage[idx] : g_story_deck[g_ed_view[idx]];
}

// Card order for the deck list.  Insertion sort over an index array: the deck
// itself must keep its slots where they are, because slots 0-3 are what the
// continue code writes out.
static void Msx2_StoryDeckSortView(void)
{
	u8 i, j, v;

	for(i = 0; i < STORY_DECK_SIZE; ++i)
		g_ed_view[i] = i;
	for(i = 1; i < STORY_DECK_SIZE; ++i)
	{
		v = g_ed_view[i];
		j = i;
		while((j != 0) && (g_story_deck[g_ed_view[j - 1]] > g_story_deck[v]))
		{
			g_ed_view[j] = g_ed_view[j - 1];
			--j;
		}
		g_ed_view[j] = v;
	}
}

// Storage has no slot identity at all -- the code decoder finds a card in it by
// value -- so it is sorted in place.
static void Msx2_StoryDeckSortStorage(void)
{
	u8 i, j, v;

	for(i = 1; i < g_story_storage_count; ++i)
	{
		v = g_story_storage[i];
		j = i;
		while((j != 0) && (g_story_storage[j - 1] > v))
		{
			g_story_storage[j] = g_story_storage[j - 1];
			--j;
		}
		g_story_storage[j] = v;
	}
}

// Which physical deck slot a change to `phys` can be recorded in, moving the
// picked card into it if it is not already there.  Slots 0-3 are the four the
// continue code has room for; a slot still holding its dealt card is free.
// The exchange of two deck slots is invisible: the deck is a bag that gets
// shuffled before every duel, and the list is drawn in card order.
static u8 Msx2_StoryDeckHostSlot(u8 phys)
{
	u8 j;

	if(phys < STORY_EDIT_SLOTS)
		return phys;
	for(j = 0; j < STORY_EDIT_SLOTS; ++j)
	{
		if(g_story_deck[j] == g_story_base[j])
		{
			u8 tmp = g_story_deck[j];
			g_story_deck[j] = g_story_deck[phys];
			g_story_deck[phys] = tmp;
			return j;
		}
	}
	return ED_NONE;
}

static void Msx2_StoryDeckDirty(u8 level)
{
	u8 i;
	for(i = 0; i < MSX2_VIDEO_PAGES; ++i)
		if(g_ed_dirty[i] < level)
			g_ed_dirty[i] = level;
}

// The ten names the window is showing, out of the cartridge and into RAM.
static void Msx2_StoryDeckCacheNames(void)
{
	u8 count = Msx2_StoryDeckCount();
	u8 i;

	for(i = 0; i < ED_ROWS; ++i)
	{
		u8 idx = (u8)(g_ed_scroll[g_ed_tab] + i);
		g_ed_names[i][0] = 0;
		if(idx >= count)
			continue;
		Msx2_RomRead(MSX2_TEXT_SEGMENT,
		             (u16)Msx2_StoryDeckCardAt(idx) * MSX2_NAME_STRIDE,
		             (u8*)g_ed_names[i], MSX2_NAME_STRIDE);
		g_ed_names[i][ED_TEXT_COLS] = 0;
	}
}

// A row of the list.  NOTHING here depends on where the cursor is: that is the
// point of the sprite selector, and it is what lets a cursor step repaint no
// rows whatever.  The one row that is still coloured differently is the one
// holding a half-finished exchange, which changes at most twice a swap.
static void Msx2_StoryDeckRow(u8 i)
{
	u8 y = (u8)(ED_LIST_Y + i * ED_ROW_H);
	u8 idx = (u8)(g_ed_scroll[g_ed_tab] + i);
	bool held = ((g_ed_pending != ED_NONE) && (g_ed_pending_tab == g_ed_tab) &&
	             (g_ed_pending == idx));

	Msx2_Fill(ED_LIST_X, y, ED_LIST_W, (u8)(ED_ROW_H - 1), MSX2_BLACK);
	if((idx >= Msx2_StoryDeckCount()) || (g_ed_names[i][0] == 0))
		return;
	Msx2_TextColor(held ? MSX2_GOLD : MSX2_SAND, MSX2_BLACK);
	Msx2_TextAt(ED_TEXT_X, (u8)(y + 2), g_ed_names[i]);
}

// The art panel: the card the cursor is on, its two figures, and where it sits
// in the list.  One 40x48 stream, which is the whole per-keypress cost.
static void Msx2_StoryDeckInfo(void)
{
	u8 count = Msx2_StoryDeckCount();
	u8 cursor = g_ed_cursor[g_ed_tab];
	u8 card;

	Msx2_Fill((u8)(ED_ART_X - 1), (u8)(ED_ART_Y - 1), MSX2_CARD_W + 2,
	          (u8)(MSX2_CARD_H + 2), MSX2_BLACK);
	Msx2_Fill(ED_INFO_X, ED_STAT_Y, 52, (u8)(ED_COUNT_Y + 8 - ED_STAT_Y),
	          MSX2_BLACK);
	if(count == 0)
		return;

	card = Msx2_StoryDeckCardAt(cursor);
	Msx2_StoryDrawCardThumb(card, ED_ART_X, ED_ART_Y);
	Msx2_FrameRect((u8)(ED_ART_X - 1), (u8)(ED_ART_Y - 1), MSX2_CARD_W + 2,
	               (u8)(MSX2_CARD_H + 2), MSX2_GOLD);

	if(Msx2_IsMonster(card))
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
		Msx2_TextAt(ED_INFO_X, ED_STAT_Y, "ATK");
		Msx2_TextAt(ED_INFO_X, (u8)(ED_STAT_Y + 10), "DEF");
		Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
		Msx2_NumAt((u8)(ED_INFO_X + 24), ED_STAT_Y, (i16)Msx2_CardAtk(card));
		Msx2_NumAt((u8)(ED_INFO_X + 24), (u8)(ED_STAT_Y + 10),
		           (i16)Msx2_CardDef(card));
	}
	else
	{
		Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
		Msx2_TextAt(ED_INFO_X, ED_STAT_Y, "SUPPORT");
	}

	Msx2_TextColor(MSX2_DARK_SAND, MSX2_BLACK);
	Msx2_NumAt(ED_INFO_X, ED_COUNT_Y, (i16)(cursor + 1));
	Msx2_TextAt((u8)(ED_INFO_X + 18), ED_COUNT_Y, "/");
	Msx2_NumAt((u8)(ED_INFO_X + 26), ED_COUNT_Y, (i16)count);
}

static void Msx2_StoryDeckList(void)
{
	u8 i;

	for(i = 0; i < ED_ROWS; ++i)
		Msx2_StoryDeckRow(i);
	if(g_ed_tab && (g_story_storage_count == 0))
	{
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_BLACK);
		Msx2_TextAt(ED_TEXT_X, (u8)(ED_LIST_Y + 2),
		            Msx2_UiText(MSX2_S_EMPTY_WIN_DUELS_TO_EARN_CARD));
	}
	Msx2_StoryDeckInfo();
}

static void Msx2_StoryDeckTab(u8 which, u8 x, u8 id, u8 count)
{
	bool on = (g_ed_tab == which);
	u8 bg = on ? MSX2_DEEP_BLUE : MSX2_BLACK;

	Msx2_Fill(x, ED_TAB_Y, ED_TAB_W, ED_TAB_H, bg);
	Msx2_FrameRect(x, ED_TAB_Y, ED_TAB_W, ED_TAB_H,
	               on ? MSX2_GOLD : MSX2_DARK_SAND);
	Msx2_TextColor(on ? MSX2_WHITE : MSX2_DARK_SAND, bg);
	Msx2_TextAt((u8)(x + 10), (u8)(ED_TAB_Y + 3), Msx2_UiText(id));
	Msx2_NumAt((u8)(x + 70), (u8)(ED_TAB_Y + 3), (i16)count);
}

static void Msx2_StoryDeckPaint(void)
{
	bool pending = (g_ed_pending != ED_NONE);

	Msx2_ClearPage(MSX2_BLACK);

	Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
	Msx2_TextCenter(ED_TITLE_Y, Msx2_UiText(MSX2_S_DECK_EDITOR));

	Msx2_StoryDeckTab(0, ED_TAB0_X, MSX2_S_DECK, STORY_DECK_SIZE);
	Msx2_StoryDeckTab(1, ED_TAB1_X, MSX2_S_STORAGE, g_story_storage_count);

	Msx2_TextColor(MSX2_TEAL, MSX2_BLACK);
	Msx2_TextCenter(ED_STATUS_Y, Msx2_UiText(pending
		? MSX2_S_PICK_THE_CARD_TO_SWAP
		: MSX2_S_CHOOSE_A_CARD_TO_REPLACE));

	Msx2_StoryDeckList();

	if(g_ed_msg)
	{
		Msx2_TextColor(MSX2_RED, MSX2_BLACK);
		Msx2_TextCenter(ED_MSG_Y, Msx2_UiText(g_ed_msg));
	}

	Msx2_TextColor(MSX2_DARK_SAND, MSX2_BLACK);
	Msx2_TextCenter(ED_HINT_Y, Msx2_UiText(MSX2_S_UP_DOWN_PICKS_L_R_CHANGES_TAB));
	Msx2_TextColor(MSX2_SAND, MSX2_BLACK);
	Msx2_TextCenter(ED_HINT2_Y, Msx2_UiText(pending
		? MSX2_S_SPACE_SWAPS_HERE_ESC_CANCELS
		: MSX2_S_SPACE_EXCHANGES_ESC_LEAVES));
}

static void Msx2_StoryEnterDeck(void)
{
	g_phase = PH_DECK;
	g_ed_tab = 0;
	g_ed_cursor[0] = 0;
	g_ed_cursor[1] = 0;
	g_ed_scroll[0] = 0;
	g_ed_scroll[1] = 0;
	g_ed_pending = ED_NONE;
	g_ed_pending_tab = 0;
	g_ed_msg = 0;
	g_ed_rep = 0;
	g_ed_rep_dir = 0;
	g_ed_art_wait = 0;
	Msx2_StoryDeckSortStorage();
	Msx2_StoryDeckSortView();
	Msx2_StoryDeckCacheNames();

	/* Black, not the road: the list wants the whole screen, and a picture
	   under it would have to be restored under every row that changes. */
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StoryDeckPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_InputFlush();
	g_ed_dirty[0] = 0;
	g_ed_dirty[1] = 0;
	g_map_dirty = 0;
	Msx2_MusicPlay(MSX2_MUSIC_DECK_EDITOR);
}

// Keep the cursor inside the window.  The window moves a whole screenful at a
// time rather than a row: a scroll costs ten names and ten lines of glyphs on
// both pages, and paying that on every step past the edge is what made the
// list feel stuck.  Stepping off the bottom starts the next page at the cursor;
// stepping off the top ends the previous page at it.
static bool Msx2_StoryDeckClamp(u8 dir)
{
	u8 cur = g_ed_cursor[g_ed_tab];
	u8 was = g_ed_scroll[g_ed_tab];
	u8 sc = was;

	if(cur < sc)
		sc = (dir & MSX2_BTN_UP)
			? ((cur >= (u8)(ED_ROWS - 1)) ? (u8)(cur - ED_ROWS + 1) : 0)
			: cur;
	else if(cur >= (u8)(sc + ED_ROWS))
		sc = cur;
	if(sc == was)
		return FALSE;
	g_ed_scroll[g_ed_tab] = sc;
	return TRUE;
}

static void Msx2_StoryDeckSwitchTab(void)
{
	u8 count;

	g_ed_tab ^= 1;
	count = Msx2_StoryDeckCount();
	if(count == 0)
	{
		g_ed_cursor[g_ed_tab] = 0;
		g_ed_scroll[g_ed_tab] = 0;
	}
	else
	{
		if(g_ed_cursor[g_ed_tab] >= count)
			g_ed_cursor[g_ed_tab] = (u8)(count - 1);
		if(g_ed_scroll[g_ed_tab] >= count)
			g_ed_scroll[g_ed_tab] = 0;
		(void)Msx2_StoryDeckClamp(0);
	}
	Msx2_StoryDeckCacheNames();
	Msx2_StoryDeckDirty(ED_DIRTY_ALL);
}

// Trade the deck card at view row `deck_row` for the storage card at
// `store_row`.  Both lists are re-sorted afterwards, which is also what hides
// the slot shuffle Msx2_StoryDeckHostSlot may have done.
static void Msx2_StoryDeckExchange(u8 deck_row, u8 store_row)
{
	u8 slot = Msx2_StoryDeckHostSlot(g_ed_view[deck_row]);
	u8 old;

	if(slot == ED_NONE)
	{
		g_ed_msg = MSX2_S_ONLY_FOUR_CARDS_CAN_BE_SWAPPED;
		Msx2_StoryDeckDirty(ED_DIRTY_ALL);
		return;
	}
	old = g_story_deck[slot];
	g_story_deck[slot] = g_story_storage[store_row];
	g_story_storage[store_row] = old;
	Msx2_StoryDeckSortStorage();
	Msx2_StoryDeckSortView();
	Msx2_SfxPlay(MSX2_SFX_CONFIRM);
}

static void Msx2_StoryDeckStep(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 held = Msx2_InputHeld();
	u8 page = Msx2_VideoGetDrawPage();
	u8 move = (u8)(pressed & (MSX2_BTN_UP | MSX2_BTN_DOWN));
	u8 count;

	// A held direction repeats, because forty cards is four screenfuls.
	if(move != 0)
	{
		g_ed_rep_dir = move;
		g_ed_rep = 0;
	}
	else if((g_ed_rep_dir != 0) && (held & g_ed_rep_dir))
	{
		++g_ed_rep;
		if((g_ed_rep >= ED_REP_DELAY) &&
		   (((u8)(g_ed_rep - ED_REP_DELAY) % ED_REP_RATE) == 0))
			move = g_ed_rep_dir;
	}
	else
		g_ed_rep_dir = 0;

	if(g_ed_msg && (pressed != 0))
	{
		g_ed_msg = 0;
		Msx2_StoryDeckDirty(ED_DIRTY_ALL);
	}

	count = Msx2_StoryDeckCount();
	if((move != 0) && (count != 0))
	{
		u8 cur = g_ed_cursor[g_ed_tab];
		if(move & MSX2_BTN_UP)
			cur = (cur == 0) ? (u8)(count - 1) : (u8)(cur - 1);
		else
			cur = (u8)((cur + 1 == count) ? 0 : (cur + 1));
		if(cur != g_ed_cursor[g_ed_tab])
		{
			g_ed_cursor[g_ed_tab] = cur;
			Msx2_SfxPlay(MSX2_SFX_SELECT);
			// The selector answers on this frame whatever else happens: it is
			// a sprite, and it is placed at the bottom of this function.  The
			// art is what waits.
			g_ed_art_wait = ED_ART_SETTLE;
			if(Msx2_StoryDeckClamp(move))
			{
				Msx2_StoryDeckCacheNames();
				Msx2_StoryDeckDirty(ED_DIRTY_LIST);
			}
		}
	}

	if(pressed & (MSX2_BTN_LEFT | MSX2_BTN_RIGHT))
	{
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		Msx2_StoryDeckSwitchTab();
	}

	if(pressed & MSX2_BTN_A)
	{
		if((g_story_storage_count == 0) || (count == 0))
		{
			g_ed_msg = MSX2_S_EMPTY_WIN_DUELS_TO_EARN_CARD;
			Msx2_StoryDeckDirty(ED_DIRTY_ALL);
		}
		else if((g_ed_pending != ED_NONE) && (g_ed_pending_tab != g_ed_tab))
		{
			// The other half.  Which row is which follows from the side the
			// first pick was made on, not from the side we are standing on --
			// the player is free to walk back and forth in between.
			u8 deck_row = g_ed_tab ? g_ed_pending : g_ed_cursor[0];
			u8 store_row = g_ed_tab ? g_ed_cursor[1] : g_ed_pending;
			g_ed_pending = ED_NONE;
			Msx2_StoryDeckExchange(deck_row, store_row);
			// Land on the deck, which is the side the change is judged on.
			if(g_ed_tab != 0)
				Msx2_StoryDeckSwitchTab();
			if(g_ed_cursor[1] >= g_story_storage_count)
				g_ed_cursor[1] = 0;
			g_ed_scroll[1] = 0;
			Msx2_StoryDeckCacheNames();
			Msx2_StoryDeckDirty(ED_DIRTY_ALL);
		}
		else
		{
			// Half an exchange: hold this card and cross to the other side.
			g_ed_pending = g_ed_cursor[g_ed_tab];
			g_ed_pending_tab = g_ed_tab;
			Msx2_SfxPlay(MSX2_SFX_SELECT);
			Msx2_StoryDeckSwitchTab();
		}
	}

	if(pressed & MSX2_BTN_B)
	{
		if(g_ed_pending != ED_NONE)
		{
			g_ed_pending = ED_NONE;
			Msx2_SfxPlay(MSX2_SFX_SELECT);
			Msx2_StoryDeckSwitchTab();
		}
		else
		{
			Msx2_StoryEnterMap();
			return;
		}
	}

	// The cursor has stood still long enough: the card it is on may now be
	// drawn, on both pages.  A walk down the list draws none of them.
	if(g_ed_art_wait != 0)
	{
		--g_ed_art_wait;
		if(g_ed_art_wait == 0)
			Msx2_StoryDeckDirty(ED_DIRTY_ART);
	}

	// The selector, every frame, wherever the cursor is: twenty pixels of gem
	// in the margin the names were moved out of.
	Msx2_StoryGem(ED_GEM_X,
	              (u8)(ED_LIST_Y +
	                   (u8)(g_ed_cursor[g_ed_tab] - g_ed_scroll[g_ed_tab]) *
	                   ED_ROW_H - 4));

	if(g_ed_dirty[page] != 0)
	{
		if(g_ed_dirty[page] == ED_DIRTY_ART)
			Msx2_StoryDeckInfo();
		else if(g_ed_dirty[page] == ED_DIRTY_LIST)
			Msx2_StoryDeckList();
		else
			Msx2_StoryDeckPaint();
		g_ed_dirty[page] = 0;
		Msx2_VideoFlipRequest();
	}
}

// Is `ch` one of the letters the code alphabet uses, and where?
static u8 Msx2_StoryCodeSlot(c8 ch)
{
	u8 i;
	for(i = 0; i < 32; ++i)
		if(CODE_ALPHABET[i] == ch)
			return i;
	return 0xFF;
}

static void Msx2_StoryCodeInputStep(void)
{
	u8 pressed = Msx2_InputPressed();
	c8 typed = Msx2_InputTyped();
	u8 changed = (pressed & (MSX2_BTN_LEFT | MSX2_BTN_RIGHT |
	                         MSX2_BTN_UP | MSX2_BTN_DOWN)) ? TRUE : FALSE;
	g_code_cursor = Msx2_StoryGridCursor(g_code_cursor, 8, 32, pressed);
	if((typed != 0) && (g_code_len < STORY_CODE_LEN))
	{
		// A typed character that is not in the alphabet is ignored rather than
		// substituted: I and O are absent on purpose, so silently turning them
		// into 1 and 0 would hand the player a code they never wrote down.
		u8 slot = Msx2_StoryCodeSlot(typed);
		if(slot != 0xFF)
		{
			g_code_cursor = slot;
			g_code[g_code_len++] = typed;
			g_code[g_code_len] = 0;
			g_code_error = 0;
			changed = TRUE;
			pressed &= (u8)~MSX2_BTN_A;
		}
	}
	if(Msx2_InputDiskKey())
	{
		if(Msx2_DiskLoad(g_code) && (g_code[STORY_CODE_LEN - 1] != 0))
		{
			g_code_len = STORY_CODE_LEN;
			g_disk_msg = 0;
			if(Msx2_StoryParseCode())
			{
				Msx2_StoryEnterMap();
				return;
			}
			g_code_error = TRUE;
		}
		else
			g_disk_msg = 3;
		changed = TRUE;
	}
	if(pressed & MSX2_BTN_ENTER)
	{
		// RETURN submits a full code; on a short one it does nothing, which is
		// what stops a stray press from reporting the code invalid.
		pressed &= (u8)~MSX2_BTN_A;
		if(g_code_len == STORY_CODE_LEN)
		{
			if(Msx2_StoryParseCode())
			{
				Msx2_StoryEnterMap();
				return;
			}
			g_code_error = TRUE;
			changed = TRUE;
		}
	}
	if(pressed & MSX2_BTN_A)
	{
		if(g_code_len < STORY_CODE_LEN)
		{
			g_code[g_code_len++] = (c8)CODE_ALPHABET[g_code_cursor];
			g_code[g_code_len] = 0;
			g_code_error = 0;
			changed = TRUE;
		}
		else if(Msx2_StoryParseCode())
		{
			Msx2_StoryEnterMap();
			return;
		}
		else
		{
			g_code_error = TRUE;
			changed = TRUE;
		}
	}
	if(pressed & MSX2_BTN_B)
	{
		if(g_code_len != 0)
		{
			--g_code_len;
			g_code[g_code_len] = 0;
			g_code_error = 0;
			changed = TRUE;
		}
		else
		{
			// Backing out of an empty code is backing out of LOAD STORY, and
			// LOAD STORY was reached from the title.  It used to fall into the
			// map instead, which started a run the player had not loaded.
			g_want_quit = TRUE;
			return;
		}
	}
	if(changed)
		Msx2_StoryUiDirty();
	if(g_map_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
	{
		Msx2_StoryCodeRefresh(FALSE);
		g_map_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
		Msx2_VideoFlipRequest();
	}
}

static void Msx2_StoryCodeOutputStep(void)
{
	u8 pressed = Msx2_InputPressed();
	if(Msx2_InputDiskKey())
	{
		g_disk_msg = Msx2_DiskSave(g_code) ? 1 : 2;
		Msx2_StoryUiDirty();
	}
	// ESC, AND NOT THE CONFIRM BUTTON.
	// This screen is the last of the three SAVE GAME composes, and the confirm
	// button is the one that got the player here.  Leaving on it too means a
	// key that is repeating -- by the host, by the emulator's keyboard mapping,
	// by anything the game cannot see -- walks out of the save flow and starts
	// whatever the road's cursor is on.  Every step INTO the flow is the
	// confirm button and the one step out of it is ESC, so no amount of
	// confirm can leave.  The prompt says so.
	if(pressed & MSX2_BTN_B)
	{
		Msx2_StoryEnterMap();
		return;
	}
	if(g_map_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
	{
		Msx2_StoryCodeRefresh(FALSE);
		g_map_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
		Msx2_VideoFlipRequest();
	}
}

static void Msx2_StoryRewardPaint(void)
{
	Msx2_Fill(30, 22, 196, 166, MSX2_PANEL_COLOR);
	Msx2_FrameRect(30, 22, 196, 166, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(34, Msx2_UiText(MSX2_S_DUEL_CLEARED));
	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextCenter(48, Msx2_UiText(MSX2_S_NEW_CARD_EARNED));
	Msx2_StoryDrawCardThumb(g_reward_card, 108, 62);
	/* The card's own name, not "CARD 37": the id was a placeholder from before
	   the cartridge carried the name table, and the deck editor now names every
	   card the player owns, so the screen that hands one over must too.  g_text
	   is the story's line buffer and holds nothing between a duel and the map. */
	Msx2_RomRead(MSX2_TEXT_SEGMENT, (u16)g_reward_card * MSX2_NAME_STRIDE,
	             (u8*)g_text, MSX2_NAME_STRIDE);
	g_text[MSX2_NAME_STRIDE - 1] = 0;
	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextCenter(119, g_text);
	Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(145, Msx2_UiText(MSX2_S_STORED_IN_YOUR_COLLECTION));
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(170, Msx2_UiText(MSX2_S_SPACE_CONTINUE));
}

static void Msx2_StoryEnterReward(void)
{
	g_phase = PH_REWARD;
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStageForProgress(g_progress)), MSX2_PAGE_1);
	Msx2_StoryRewardPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_InputFlush();
	g_map_dirty = 0;
	Msx2_MusicPlay(MSX2_MUSIC_RESULT);
}

// ─────────────────────────────────────────────────────────────────────────────
//  The scene
// ─────────────────────────────────────────────────────────────────────────────

void Msx2_StoryBegin_In(void)
{
	/* No name entry on this target: the run starts as SERENA and goes
	   straight into the opening narration. */
	g_progress = 0;
	g_duel_index = 0;
	g_reward_card = MSX2_CARD_NONE;
	MSX2_STAGE(MSX2_STAGE_STORY);
	Msx2_StoryBuildStarterDeck();
	Msx2_MusicPlay(MSX2_MUSIC_OPENING);
	Msx2_StoryEnterNarration(NARR_INTRO);
}

void Msx2_StoryBeginAutoplay_In(void)
{
#ifdef MSX2_DEBUG_STORY_AUTOPLAY
	u8 i;
	g_progress = 0;
	g_duel_index = 0;
	g_reward_card = MSX2_CARD_NONE;
	for(i = 0; i < STORY_NAME_LEN; ++i)
		g_player_name[i] = Msx2_UiText(MSX2_S_AUTOPLAY)[i];
	g_player_name[STORY_NAME_LEN] = 0;
	Msx2_StoryBuildStarterDeck();
	/* Exercise the exact builder/parser pair used by the title's load screen;
	   a bad checksum or a non-canonical spare bit makes the soak fail early. */
	Msx2_StoryBuildCode(g_code, g_progress, g_player_name, g_story_deck);
	g_code_len = STORY_CODE_LEN;
	if(!Msx2_StoryParseCode())
	{
		g_stat_status = MSX2_PROBE_BADSTATE;
		return;
	}
	MSX2_STAGE(MSX2_STAGE_STORY);
	/* Story-soak validates state transitions, not the renderer.  Avoid streaming
	   the same map/reward paintings ten times so the five-duel gate completes in
	   a bounded emulator run; the shipping build still enters every screen. */
	g_phase = PH_MAP;
#endif
}

void Msx2_StoryBeginLoad_In(void)
{
	g_phase = PH_LOAD_PICK;
	g_want_quit = 0;
	g_map_dirty = 0;
	Msx2_MusicPlay(MSX2_MUSIC_OPENING);
	Msx2_StoryLoadPickEnter(g_code);
}

void Msx2_StoryPrepareDuelDeck_In(void)
{
	if(!g_story_deck_ready)
		Msx2_StoryBuildStarterDeck();
	Msx2_DuelSetPlayerDeck(g_story_deck, STORY_DECK_SIZE);
}

u8 Msx2_StoryDuelIndex_In(void)
{
	return g_duel_index;
}

void Msx2_StoryDuelDone_In(bool won)
{
	if(won && (g_duel_index == g_progress))
	{
		/* Rewards are deliberately deterministic on this target: a continue code
		   can reproduce the exact collection without a 64-bit RNG payload. */
		g_reward_card = (u8)((g_duel_index * 13 + 7) % MSX2_CARD_COUNT);
		if(g_story_storage_count < STORY_STORAGE_SIZE)
			g_story_storage[g_story_storage_count++] = g_reward_card;
		++g_progress;
		if(g_progress >= MSX2_STORY_MAX_DUELS)
		{
			// The road is walked.  Progress stays at the end so a replay of the
			// last opponent cannot advance it again.
			g_duel_index = MSX2_STORY_MAX_DUELS - 1;
#ifdef MSX2_DEBUG_STORY_AUTOPLAY
			g_phase = PH_REWARD;
#else
			Msx2_StoryEnterReward();
#endif
			return;
		}
		g_duel_index = g_progress;
#ifdef MSX2_DEBUG_STORY_AUTOPLAY
		g_phase = PH_REWARD;
#else
		Msx2_StoryEnterReward();
#endif
		return;
	}
	Msx2_StoryEnterMap();
}

u8 Msx2_StoryStep_In(void)
{
	// One place, rather than a hide in each of the eight Enter functions: the
	// two screens that own the selector put it up again on their own frame.
	if((g_phase != PH_MAP) && (g_phase != PH_DECK))
		Msx2_StoryGemHide();
#ifdef MSX2_DEBUG_STORY_AUTOPLAY
	if(g_phase == PH_MAP)
	{
		g_duel_index = (g_progress >= MSX2_STORY_MAX_DUELS)
			? (MSX2_STORY_MAX_DUELS - 1) : g_progress;
		return MSX2_STORY_FIGHT;
	}
	if(g_phase == PH_REWARD)
	{
		if(g_progress >= MSX2_STORY_MAX_DUELS &&
		   g_duel_index == MSX2_STORY_MAX_DUELS - 1)
			Msx2_StoryEnterNarration(NARR_ENDING);
		else
			g_phase = PH_MAP;
		return MSX2_STORY_BUSY;
	}
#endif
	if(g_phase == PH_LOAD_PICK)
	{
		u8 what = Msx2_StoryLoadPickStep();
		if(what == MSX2_LOADPICK_QUIT)
			return MSX2_STORY_QUIT;
		if(what == MSX2_LOADPICK_PASSWORD)
			Msx2_StoryEnterCodeInput();
		else if(what == MSX2_LOADPICK_LOADED)
		{
			// The disk carries the same sixteen characters the player would
			// have typed, so it goes through the same parser: a sector holding
			// something else fails exactly where a mistyped code fails.
			g_code_len = STORY_CODE_LEN;
			if(Msx2_StoryParseCode())
				Msx2_StoryEnterMap();
			else
				Msx2_StoryLoadPickRefused();
		}
		return MSX2_STORY_BUSY;
	}
	if(g_phase == PH_SAVE_PICK)
	{
		Msx2_StorySavePickStep();
		return MSX2_STORY_BUSY;
	}
	if(g_phase == PH_CODE_IN)
	{
		Msx2_StoryCodeInputStep();
		return g_want_quit ? MSX2_STORY_QUIT : MSX2_STORY_BUSY;
	}
	if(g_phase == PH_CODE_OUT)
	{
		Msx2_StoryCodeOutputStep();
		return MSX2_STORY_BUSY;
	}
	if(g_phase == PH_DECK)
	{
		Msx2_StoryDeckStep();
		return MSX2_STORY_BUSY;
	}
	if(g_phase == PH_REWARD)
	{
		if(Msx2_InputPressed() & (MSX2_BTN_A | MSX2_BTN_B))
		{
			if(g_progress >= MSX2_STORY_MAX_DUELS &&
			   g_duel_index == MSX2_STORY_MAX_DUELS - 1)
			{
				Msx2_StoryEnterNarration(NARR_ENDING);
			}
			else
				Msx2_StoryEnterMap();
		}
		return MSX2_STORY_BUSY;
	}
	if(g_phase == PH_MAP)
		return Msx2_StoryMapStep();

	if(!Msx2_StoryTextStep())
		return MSX2_STORY_BUSY;

	if(g_phase == PH_TALK)
		return MSX2_STORY_FIGHT;
	if(g_narr_which == NARR_INTRO)
	{
		Msx2_StoryEnterMap();
		return MSX2_STORY_BUSY;
	}
	return MSX2_STORY_QUIT;
}

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_StoryRegressionStamp_In(void)
{
	g_msx2_regression_diag.story_phase = g_phase;
	g_msx2_regression_diag.save_row =
		(g_phase == PH_SAVE_PICK) ? g_save_pick : g_cursor;
}

void Msx2_StoryRegressionFixture_In(u8 fixture)
{
	if(fixture != MSX2_FIXTURE_STORY_SAVE_ROW)
		return;

	// Enter the map through the same composed path used after opening, a reward,
	// or a loaded code.  The next A edge therefore exercises the real map-to-save
	// phase boundary rather than a synthetic picker screen.
	Msx2_StoryEnterMap();
	g_cursor = MAP_CODE_ROW;
	g_map_dirty = ALL_PAGES;
}
#endif
