// ─────────────────────────────────────────────────────────────────────────────
//  msx2_story.c — the opening, the sanctum map, the dialogue, the ending
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_story.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_stream.h"
#include "msx2_audio.h"
#include "msx2_duel.h"
#include "msx2_probe.h"
#include "msx2_scenes.h"

// ── Phases ───────────────────────────────────────────────────────────────────
#define PH_NARRATE   0   // the opening or the ending: one voice, no portrait
#define PH_TALK      1   // pre-duel dialogue, two speakers, two composites
#define PH_MAP       2   // the sanctum road: pick an opponent, or leave
#define PH_NAME      3   // eight-letter player name entry
#define PH_CODE_IN   4   // continue-code entry
#define PH_CODE_OUT  5   // continue code shown at the sanctum
#define PH_DECK      6   // compact deck editor
#define PH_REWARD    7   // a story-win card reveal

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
#define MAP_CURSOR_X 34
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
#define EDIT_DECK_Y           44
#define EDIT_STORAGE_Y        139
#define EDIT_CARD_X(n)        (u8)(8 + (u8)(n) * 49)

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

static c8  g_player_name[STORY_NAME_LEN + 1];
static u8  g_name_len;
static u8  g_name_cursor;
static c8  g_code[STORY_CODE_LEN + 1];
static u8  g_code_len;
static u8  g_code_cursor;
static u8  g_code_error;
static u8  g_editor_target;
static u8  g_editor_storage_cursor;
static u8  g_editor_storage_mode;
static u8  g_story_deck[STORY_DECK_SIZE];
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
static void Msx2_StoryReadLine(u16 offset)
{
	Msx2_RomRead(MSX2_TEXT_SEGMENT, offset, (u8*)g_text, MSX2_LINE_STRIDE);
	g_text[MSX2_LINE_STRIDE - 1] = 0;
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
static void Msx2_StoryBlitBust(u8 chr, u8 x, u8 y, bool lit)
{
	u16 seg = MSX2_PORTRAIT_SEG(chr, lit);
	u16 pix = MSX2_PORTRAIT_INDEX_BYTES;   // the pixels follow the run table
	u8  rec[MSX2_PORTRAIT_ROW_STRIDE];
	u8  row, k;

	for(row = 0; row < MSX2_PORTRAIT_H; ++row)
	{
		Msx2_RomRead(seg, (u16)((u16)row * MSX2_PORTRAIT_ROW_STRIDE), rec,
		             MSX2_PORTRAIT_ROW_STRIDE);
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
			Msx2_StreamRect(s, o, (u8)(x + rx), (u8)(y + row), head, 1);
			if(head != rn)
				Msx2_StreamRect((u16)(s + 1), 0, (u8)(x + rx + head),
				                (u8)(y + row), (u8)(rn - head), 1);
			pix = (u16)(pix + rn);
		}
	}
}

// Both busts, with `speaker` lit and the other dimmed.  The two brightness
// variants are cut from the same alpha mask, so this covers byte for byte the
// pixels the previous pair covered and nothing behind them has to be repaired.
static void Msx2_StoryBusts(u8 speaker)
{
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
	if(g_phase != PH_TALK)
	{
		// Narration has no second speaker: the opening is Serena remembering
		// over her own scene, the ending is a voice over the closing painting.
		u16 segment = (g_narr_which == NARR_INTRO)
		            ? MSX2_TALK_SEGMENT(0) : MSX2_SCENE_ENDING_SEGMENT;
		Msx2_VideoDrawPage(MSX2_PAGE_1);
		Msx2_StreamScene(segment, MSX2_PAGE_1);
		if(g_narr_which == NARR_INTRO)
			Msx2_StoryBlitBust(0, MSX2_PORTRAIT_LEFT_X, MSX2_PORTRAIT_LEFT_Y,
			                   TRUE);
		Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
		Msx2_VideoShowPage(MSX2_PAGE_1);
		g_shot = shot;
		return;
	}

	if(g_shot == 0xFF)
	{
		// First line of the scene: the painting, then both figures on it.
		Msx2_VideoDrawPage(MSX2_PAGE_1);
		Msx2_StreamScene(MSX2_TALK_SEGMENT(g_msx2_stage_for_duel[g_duel_index]),
		                 MSX2_PAGE_1);
		Msx2_StoryBusts(shot);
		Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
		Msx2_VideoShowPage(MSX2_PAGE_1);
	}
	else
	{
		u8 shown = Msx2_VideoGetDrawPage();
		Msx2_VideoDrawPage((u8)(shown ^ 1));
		Msx2_StoryBusts(shot);
		Msx2_VideoDrawPage(shown);
		Msx2_StoryBusts(shot);
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
	g_phase = PH_TALK;
	g_line = 0;
	g_line_count = g_msx2_dialogue_count[g_duel_index];
	g_shot = 0xFF;                       // nothing is up yet, so force a stream
	Msx2_MusicPlay(MSX2_MUSIC_OPENING);
	// The scene is built once, here, whoever speaks first -- including a
	// narrator line, which changes no portrait and so would never build it.
	Msx2_StoryShowShot(0);
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
	Msx2_StoryLoadNarrLine();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Name and continue-code screens
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_StoryUiDirty(void)
{
	g_map_dirty = ALL_PAGES;
}

static void Msx2_StoryNamePaint(void)
{
	u8 i;
	c8 one[2];

	Msx2_Fill(18, 22, 220, 164, MSX2_PANEL_COLOR);
	Msx2_FrameRect(18, 22, 220, 164, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(30, "NAME YOUR DUELIST");
	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextCenter(48, "CHOOSE EIGHT LETTERS");
	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextAt(70, 68, "NAME:");
	for(i = 0; i < STORY_NAME_LEN; ++i)
	{
		one[0] = (i < g_name_len) ? g_player_name[i] : '_';
		one[1] = 0;
		Msx2_TextColor((i == g_name_len) ? MSX2_GOLD : MSX2_WHITE,
		               MSX2_PANEL_COLOR);
		Msx2_TextAt((u8)(111 + i * 12), 68, one);
	}

	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextAt(45, 100, "ABCDEFGHIJKLM");
	Msx2_TextAt(45, 120, "NOPQRSTUVWXYZ");
	one[0] = (c8)('A' + g_name_cursor);
	one[1] = 0;
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextAt((u8)(45 + (g_name_cursor % 13) * 13),
	            (u8)(100 + (g_name_cursor / 13) * 20), one);
	Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(140, "SPACE: LETTER / RETURN: ACCEPT");
	Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
	Msx2_TextCenter(153, "ESC: DELETE");
}

static void Msx2_StoryEnterName(void)
{
	g_phase = PH_NAME;
	g_name_len = 0;
	g_name_cursor = 0;
	g_player_name[0] = 0;
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_SCENE_TITLE_SEGMENT, MSX2_PAGE_1);
	Msx2_StoryNamePaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_MusicPlay(MSX2_MUSIC_OPENING);
	g_map_dirty = 0;
}

static bool Msx2_StoryAcceptName(void)
{
	if(g_name_len == 0)
		return FALSE;
	while(g_name_len < STORY_NAME_LEN)
		g_player_name[g_name_len++] = 'A';
	g_player_name[STORY_NAME_LEN] = 0;
	g_progress = 0;
	g_duel_index = 0;
	Msx2_StoryBuildStarterDeck();
	Msx2_StoryEnterNarration(NARR_INTRO);
	return TRUE;
}

static void Msx2_StoryCodePaint(void)
{
	c8 one[2];
	Msx2_Fill(14, 20, 228, 170, MSX2_PANEL_COLOR);
	Msx2_FrameRect(14, 20, 228, 170, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(30, (g_phase == PH_CODE_OUT) ? "CONTINUE CODE" : "ENTER CONTINUE CODE");
	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextCenter(48, g_code);
	if(g_phase == PH_CODE_OUT)
	{
		Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
		Msx2_TextCenter(68, "WRITE THIS DOWN");
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextCenter(139, "ENTER IT ON THE TITLE SCREEN");
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextCenter(153, "SPACE / ESC: RETURN TO ROAD");
		return;
	}
	if(g_phase != PH_CODE_OUT)
	{
		Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
		Msx2_TextAt(27, 82, "ABCDEFGH");
		Msx2_TextAt(27, 96, "JKLMNPQR");
		Msx2_TextAt(27, 110, "STUVWXYZ");
		Msx2_TextAt(27, 124, "23456789");
		one[0] = (c8)CODE_ALPHABET[g_code_cursor];
		one[1] = 0;
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextAt((u8)(27 + (g_code_cursor % 8) * 27),
		            (u8)(82 + (g_code_cursor / 8) * 14), one);
	}
	Msx2_TextColor(g_code_error ? MSX2_RED : MSX2_DARK_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(143, g_code_error ? "INVALID CODE - TRY AGAIN" : "SPACE: LETTER / RETURN: LOAD");
	Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
	Msx2_TextCenter(155, "ESC: DELETE / EMPTY ESC: BACK");
}

static void Msx2_StoryEnterCodeInput(void)
{
	g_phase = PH_CODE_IN;
	g_code_len = 0;
	g_code[0] = 0;
	g_code_cursor = 0;
	g_code_error = 0;
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_SCENE_TITLE_SEGMENT, MSX2_PAGE_1);
	Msx2_StoryCodePaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	g_map_dirty = 0;
}

static void Msx2_StoryEnterCodeOutput(void)
{
	g_phase = PH_CODE_OUT;
	Msx2_StoryBuildCode(g_code, g_progress, g_player_name, g_story_deck);
	Msx2_StoryUiDirty();
}

static void Msx2_StoryEnterDeck(void);

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

		if(i == g_cursor)
		{
			Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
			Msx2_TextAt(MAP_CURSOR_X, y, ">");
		}

		if(!open)
		{
			Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
			Msx2_TextAt(MAP_NAME_X, y, "- SEALED -");
			continue;
		}

		Msx2_StoryReadOpp(i, 0);
		Msx2_TextColor((i == g_cursor) ? MSX2_WHITE
		                              : ((i < g_progress) ? MSX2_SAND : MSX2_TEAL),
		               MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_NAME_X, y, g_opp);
		Msx2_StoryReadOpp(i, 1);
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_TITLE_X, y, g_opp);
	}

	if(g_cursor == MAP_DECK_ROW)
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_CURSOR_X, MAP_DECK_Y, ">");
	}
	Msx2_TextColor((g_cursor == MAP_DECK_ROW) ? MSX2_WHITE : MSX2_SAND,
	               MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_DECK_Y, "DECK EDITOR");

	if(g_cursor == MAP_CODE_ROW)
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_CURSOR_X, MAP_CODE_Y, ">");
	}
	Msx2_TextColor((g_cursor == MAP_CODE_ROW) ? MSX2_WHITE : MSX2_SAND,
	               MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_CODE_Y, "CONTINUE CODE");

	if(g_cursor == MAP_BACK_ROW)
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_CURSOR_X, MAP_BACK_Y, ">");
	}
	Msx2_TextColor((g_cursor == MAP_BACK_ROW) ? MSX2_WHITE : MSX2_SAND,
	               MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_BACK_Y, "LEAVE THE ROAD");

	Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(MAP_HELP_Y, "SPACE CHOOSES  -  UP/DOWN MOVES");
}

static void Msx2_StoryEnterMap(void)
{
	g_phase = PH_MAP;
	g_cursor = g_duel_index;
	if(g_cursor > g_progress)
		g_cursor = g_progress;
	g_map_dirty = ALL_PAGES;

	Msx2_MusicPlay(MSX2_MUSIC_TITLE);
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStageForProgress(g_progress)), MSX2_PAGE_1);
	Msx2_StoryMapPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	g_map_dirty = 0;
}

static u8 Msx2_StoryMapStep(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 before = g_cursor;
	u8 page = Msx2_VideoGetDrawPage();

	if((pressed & MSX2_BTN_UP) && (g_cursor != 0))
		--g_cursor;
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
	{
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		g_map_dirty = ALL_PAGES;
	}

	if(pressed & MSX2_BTN_A)
	{
		Msx2_SfxPlay(MSX2_SFX_CONFIRM);
		if(g_cursor == MAP_DECK_ROW)
		{
			Msx2_StoryEnterDeck();
			return MSX2_STORY_BUSY;
		}
		if(g_cursor == MAP_CODE_ROW)
		{
			Msx2_StoryEnterCodeOutput();
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
			Msx2_TextCenter(MSX2_TALK_PROMPT_Y, "PUSH SPACE");
			g_prompt |= (u8)(1u << page);
			painted = TRUE;
		}
	}

	if(painted)
		Msx2_VideoFlipRequest();
	return FALSE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Compact deck editor
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_StoryDeckPaint(void)
{
	u8 i;

	Msx2_Fill(2, 18, 252, 176, MSX2_PANEL_COLOR);
	Msx2_FrameRect(2, 18, 252, 176, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(25, "DECK EDITOR");
	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextCenter(35, "CHOOSE A CARD TO REPLACE");

	for(i = 0; i < STORY_EDIT_SLOTS; ++i)
	{
		u8 x = EDIT_CARD_X(i);
		Msx2_StoryDrawCardThumb(g_story_deck[i], x, EDIT_DECK_Y);
	}
	Msx2_FrameRect((u8)(EDIT_CARD_X(g_editor_target) - 1), (u8)(EDIT_DECK_Y - 1),
	               MSX2_CARD_W + 2, MSX2_CARD_H + 2,
	               g_editor_storage_mode ? MSX2_GOLD : MSX2_RED);

	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextAt(8, 118, "STORAGE");
	if(g_story_storage_count == 0)
	{
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(61, 118, "EMPTY - WIN DUELS TO EARN CARDS");
	}
	else
	{
		/* The collection is a carousel rather than a second grid.  It keeps the
		   whole reward card visible and makes left/right work even after a
		   continue code has restored a longer collection. */
		Msx2_StoryDrawCardThumb(g_story_storage[g_editor_storage_cursor], 8,
		                       (u8)(EDIT_STORAGE_Y - 5));
		Msx2_FrameRect(7, (u8)(EDIT_STORAGE_Y - 6), (u16)(MSX2_CARD_W + 2),
		               (u8)(MSX2_CARD_H + 2),
		               g_editor_storage_mode ? MSX2_RED : MSX2_GOLD);
		Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
		Msx2_TextAt(61, 135, "CARD");
		Msx2_NumAt(91, 135, (i16)g_story_storage[g_editor_storage_cursor]);
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(61, 151, "ITEM");
		Msx2_NumAt(91, 151, (i16)(g_editor_storage_cursor + 1));
		Msx2_TextAt(111, 151, "OF");
		Msx2_NumAt(128, 151, (i16)g_story_storage_count);
	}

	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextCenter(197, g_editor_storage_mode
		? "L/R PICK  SPACE SWAP  ESC BACK"
		: "L/R TARGET  DOWN PICK  ESC EXIT");
}

static void Msx2_StoryEnterDeck(void)
{
	g_phase = PH_DECK;
	g_editor_target = 0;
	g_editor_storage_cursor = 0;
	g_editor_storage_mode = FALSE;
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStageForProgress(g_progress)), MSX2_PAGE_1);
	Msx2_StoryDeckPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	g_map_dirty = 0;
	Msx2_MusicPlay(MSX2_MUSIC_DECK_EDITOR);
}

static void Msx2_StoryDeckStep(void)
{
	u8 pressed = Msx2_InputPressed();

	if(g_editor_storage_mode)
	{
		if(pressed & MSX2_BTN_LEFT)
			g_editor_storage_cursor = (g_editor_storage_cursor == 0)
			                         ? (u8)(g_story_storage_count - 1)
			                         : (u8)(g_editor_storage_cursor - 1);
		if(pressed & MSX2_BTN_RIGHT)
			g_editor_storage_cursor = (u8)((g_editor_storage_cursor + 1 ==
			                               g_story_storage_count) ? 0
			                              : (g_editor_storage_cursor + 1));
		if(pressed & MSX2_BTN_UP)
			g_editor_storage_mode = FALSE;
		if((pressed & MSX2_BTN_A) && (g_story_storage_count != 0))
		{
			u8 old = g_story_deck[g_editor_target];
			g_story_deck[g_editor_target] = g_story_storage[g_editor_storage_cursor];
			g_story_storage[g_editor_storage_cursor] = old;
			Msx2_SfxPlay(MSX2_SFX_CONFIRM);
			g_editor_storage_mode = FALSE;
		}
	}
	else
	{
		if(pressed & MSX2_BTN_LEFT)
		{
			g_editor_target = (g_editor_target == 0) ? (STORY_EDIT_SLOTS - 1)
			                                      : (g_editor_target - 1);
		}
		if(pressed & MSX2_BTN_RIGHT)
		{
			g_editor_target = (u8)((g_editor_target + 1) % STORY_EDIT_SLOTS);
		}
		if((pressed & MSX2_BTN_DOWN) && (g_story_storage_count != 0))
		{
			g_editor_storage_mode = TRUE;
		}
		if(pressed & MSX2_BTN_A && (g_story_storage_count != 0))
		{
			g_editor_storage_mode = TRUE;
		}
	}

	if(pressed & MSX2_BTN_B)
	{
		if(g_editor_storage_mode)
		{
			g_editor_storage_mode = FALSE;
		}
		else
		{
			Msx2_StoryEnterMap();
			return;
		}
	}
	if(pressed & (MSX2_BTN_LEFT | MSX2_BTN_RIGHT | MSX2_BTN_UP |
	              MSX2_BTN_DOWN | MSX2_BTN_A | MSX2_BTN_B))
	{
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		Msx2_StoryUiDirty();
	}
	if(g_map_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
	{
		Msx2_StoryDeckPaint();
		g_map_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
		Msx2_VideoFlipRequest();
	}
}

static void Msx2_StoryCodeInputStep(void)
{
	u8 pressed = Msx2_InputPressed();
	u8 changed = (pressed & (MSX2_BTN_LEFT | MSX2_BTN_RIGHT |
	                         MSX2_BTN_UP | MSX2_BTN_DOWN)) ? TRUE : FALSE;
	g_code_cursor = Msx2_StoryGridCursor(g_code_cursor, 8, 32, pressed);
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
			/* The title is still underneath; redraw the map in full on the hidden
			   page before handing control back to the story scene. */
			Msx2_StoryEnterMap();
			return;
		}
	}
	if(changed)
		Msx2_StoryUiDirty();
	if(g_map_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
	{
		Msx2_StoryCodePaint();
		g_map_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
		Msx2_VideoFlipRequest();
	}
}

static void Msx2_StoryCodeOutputStep(void)
{
	u8 pressed = Msx2_InputPressed();
	if(pressed & (MSX2_BTN_A | MSX2_BTN_B))
	{
		Msx2_StoryEnterMap();
		return;
	}
	if(g_map_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
	{
		Msx2_StoryCodePaint();
		g_map_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
		Msx2_VideoFlipRequest();
	}
}

static void Msx2_StoryRewardPaint(void)
{
	Msx2_Fill(30, 22, 196, 166, MSX2_PANEL_COLOR);
	Msx2_FrameRect(30, 22, 196, 166, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(34, "DUEL CLEARED");
	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextCenter(48, "NEW CARD EARNED");
	Msx2_StoryDrawCardThumb(g_reward_card, 108, 62);
	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextCenter(119, "CARD");
	Msx2_NumAt(140, 119, (i16)g_reward_card);
	Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(145, "STORED IN YOUR COLLECTION");
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(170, "SPACE: CONTINUE");
}

static void Msx2_StoryEnterReward(void)
{
	g_phase = PH_REWARD;
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStageForProgress(g_progress)), MSX2_PAGE_1);
	Msx2_StoryRewardPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	g_map_dirty = 0;
	Msx2_MusicPlay(MSX2_MUSIC_RESULT);
}

// ─────────────────────────────────────────────────────────────────────────────
//  The scene
// ─────────────────────────────────────────────────────────────────────────────

void Msx2_StoryBegin(void)
{
	/* A new run starts with the same name-entry affordance as the other
	   targets.  Collection/deck creation happens only after the name is
	   accepted, so a cancelled entry cannot leave stale save data behind. */
	g_progress = 0;
	g_duel_index = 0;
	g_reward_card = MSX2_CARD_NONE;
	MSX2_STAGE(MSX2_STAGE_STORY);
	Msx2_StoryEnterName();
}

void Msx2_StoryBeginAutoplay(void)
{
#ifdef MSX2_DEBUG_STORY_AUTOPLAY
	u8 i;
	g_progress = 0;
	g_duel_index = 0;
	g_reward_card = MSX2_CARD_NONE;
	for(i = 0; i < STORY_NAME_LEN; ++i)
		g_player_name[i] = "AUTOPLAY"[i];
	g_player_name[STORY_NAME_LEN] = 0;
	g_name_len = STORY_NAME_LEN;
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

void Msx2_StoryBeginLoad(void)
{
	Msx2_StoryEnterCodeInput();
}

void Msx2_StoryPrepareDuelDeck(void)
{
	if(!g_story_deck_ready)
		Msx2_StoryBuildStarterDeck();
	Msx2_DuelSetPlayerDeck(g_story_deck, STORY_DECK_SIZE);
}

u8 Msx2_StoryDuelIndex(void)
{
	return g_duel_index;
}

void Msx2_StoryDuelDone(bool won)
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

u8 Msx2_StoryStep(void)
{
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
	if(g_phase == PH_NAME)
	{
		u8 pressed = Msx2_InputPressed();
		u8 changed = (pressed & (MSX2_BTN_LEFT | MSX2_BTN_RIGHT |
		                         MSX2_BTN_UP | MSX2_BTN_DOWN)) ? TRUE : FALSE;
		g_name_cursor = Msx2_StoryGridCursor(g_name_cursor, 13, 26, pressed);
		if(pressed & MSX2_BTN_A)
		{
			if(g_name_len < STORY_NAME_LEN)
			{
				g_player_name[g_name_len++] = (c8)('A' + g_name_cursor);
				g_player_name[g_name_len] = 0;
				changed = TRUE;
			}
			else if(Msx2_StoryAcceptName())
				return MSX2_STORY_BUSY;
		}
		if(pressed & MSX2_BTN_B)
		{
			if(g_name_len != 0)
			{
				--g_name_len;
				g_player_name[g_name_len] = 0;
				changed = TRUE;
			}
			else
				return MSX2_STORY_QUIT;
		}
		if(changed)
			Msx2_StoryUiDirty();
		if(g_map_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
		{
			Msx2_StoryNamePaint();
			g_map_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
			Msx2_VideoFlipRequest();
		}
		return MSX2_STORY_BUSY;
	}
	if(g_phase == PH_CODE_IN)
	{
		Msx2_StoryCodeInputStep();
		return MSX2_STORY_BUSY;
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
