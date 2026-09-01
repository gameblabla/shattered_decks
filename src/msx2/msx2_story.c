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
#define MAP_HEAD_Y   38
#define MAP_ROW_Y(n) (u8)(58 + (n) * 20)
#define MAP_BACK_Y   160
#define MAP_HELP_Y   174
#define MAP_CURSOR_X 34
#define MAP_NAME_X   48
#define MAP_TITLE_X  132
#define MAP_ROWS     (MSX2_STORY_MAX_DUELS + 1)   // the opponents, then LEAVE

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
static u8  g_shot;          // which composite is on screen, 0 or 1

// Bitmasks of pages that still owe a repaint.  A partial repaint only ever
// reaches the page it was drawn on, so every change is carried until both
// buffers have seen it -- per page rather than as a countdown, because the
// per-page character cursor (g_drawn) has to be reset with it.
static u8  g_fresh;         // the box, the plate and the name
static u8  g_prompt;        // the "line finished" marker
static u8  g_map_dirty;

static u8  g_cursor;

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
		return "SERENA";
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

	Msx2_Fill(BOX_X, (u8)BOX_Y, (u16)BOX_W, (u8)BOX_H, MSX2_BLACK);
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
	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
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

// The picture a speaker needs.  A narrator line keeps whichever composite is
// already up: re-streaming 54 KB to say one sentence in nobody's voice would
// black the screen for half a second for no gain.
static void Msx2_StoryShowShot(u8 shot)
{
	u16 segment = (g_phase == PH_TALK)
	            ? MSX2_TALK_SEGMENT(g_duel_index, shot)
	            : (u16)((g_narr_which == NARR_INTRO) ? MSX2_SCENE_TALK_INTRO_SEGMENT
	                                                 : MSX2_SCENE_ENDING_SEGMENT);

	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(segment, MSX2_PAGE_1);
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
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
//  The map
// ─────────────────────────────────────────────────────────────────────────────

static u8 Msx2_StoryStage(void)
{
	// story_scene_for_progress() in src/main.c: the road changes under the
	// player as the frontier moves, not as the selection does.
	if(g_progress >= 4) return 3;
	if(g_progress >= 3) return 2;
	if(g_progress >= 2) return 1;
	return 0;
}

static const c8* Msx2_StoryStageName(void)
{
	switch(Msx2_StoryStage())
	{
	case 1:  return "STONE TEMPLE";
	case 2:  return "EMBER CRATER";
	case 3:  return "THE VOID";
	default: return "DESERT ROAD";
	}
}

static void Msx2_StoryMapPaint(void)
{
	u8 i;

	Msx2_Fill((u8)(MSX2_MAP_PANEL_X + 1), (u8)(MSX2_MAP_PANEL_Y + 1),
	          (u16)(MSX2_MAP_PANEL_W - 2), (u8)(MSX2_MAP_PANEL_H - 2), MSX2_BLACK);

	Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
	Msx2_TextCenter(MAP_HEAD_Y, Msx2_StoryStageName());

	for(i = 0; i < MSX2_STORY_MAX_DUELS; ++i)
	{
		u8 y = MAP_ROW_Y(i);
		bool open = (i <= g_progress);

		if(i == g_cursor)
		{
			Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
			Msx2_TextAt(MAP_CURSOR_X, y, ">");
		}

		if(!open)
		{
			Msx2_TextColor(MSX2_DARK_SAND, MSX2_BLACK);
			Msx2_TextAt(MAP_NAME_X, y, "- SEALED -");
			continue;
		}

		Msx2_StoryReadOpp(i, 0);
		Msx2_TextColor((i == g_cursor) ? MSX2_WHITE
		                              : ((i < g_progress) ? MSX2_SAND : MSX2_TEAL),
		               MSX2_BLACK);
		Msx2_TextAt(MAP_NAME_X, y, g_opp);
		Msx2_StoryReadOpp(i, 1);
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_BLACK);
		Msx2_TextAt(MAP_TITLE_X, y, g_opp);
	}

	if(g_cursor == MSX2_STORY_MAX_DUELS)
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
		Msx2_TextAt(MAP_CURSOR_X, MAP_BACK_Y, ">");
	}
	Msx2_TextColor((g_cursor == MSX2_STORY_MAX_DUELS) ? MSX2_WHITE : MSX2_SAND,
	               MSX2_BLACK);
	Msx2_TextAt(MAP_NAME_X, MAP_BACK_Y, "LEAVE THE ROAD");

	Msx2_TextColor(MSX2_DARK_SAND, MSX2_BLACK);
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
	Msx2_StreamScene(MSX2_MAP_SEGMENT(Msx2_StoryStage()), MSX2_PAGE_1);
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
	if((pressed & MSX2_BTN_DOWN) && (g_cursor < MSX2_STORY_MAX_DUELS))
	{
		// A locked opponent is shown but never reachable, so the cursor steps
		// straight from the frontier to LEAVE.
		g_cursor = (g_cursor < g_progress) ? (u8)(g_cursor + 1)
		                                   : MSX2_STORY_MAX_DUELS;
	}
	if(g_cursor != before)
	{
		Msx2_SfxPlay(MSX2_SFX_SELECT);
		g_map_dirty = ALL_PAGES;
	}

	if(pressed & MSX2_BTN_A)
	{
		Msx2_SfxPlay(MSX2_SFX_CONFIRM);
		if(g_cursor == MSX2_STORY_MAX_DUELS)
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

	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
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
			Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
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
//  The scene
// ─────────────────────────────────────────────────────────────────────────────

void Msx2_StoryBegin(void)
{
	g_progress = 0;
	g_duel_index = 0;
	MSX2_STAGE(MSX2_STAGE_STORY);
	Msx2_StoryEnterNarration(NARR_INTRO);
}

u8 Msx2_StoryDuelIndex(void)
{
	return g_duel_index;
}

void Msx2_StoryDuelDone(bool won)
{
	if(won && (g_duel_index == g_progress))
	{
		++g_progress;
		if(g_progress >= MSX2_STORY_MAX_DUELS)
		{
			// The road is walked.  Progress stays at the end so a replay of the
			// last opponent cannot advance it again.
			g_progress = MSX2_STORY_MAX_DUELS - 1;
			g_duel_index = MSX2_STORY_MAX_DUELS - 1;
			Msx2_StoryEnterNarration(NARR_ENDING);
			return;
		}
		g_duel_index = g_progress;
	}
	Msx2_StoryEnterMap();
}

u8 Msx2_StoryStep(void)
{
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
