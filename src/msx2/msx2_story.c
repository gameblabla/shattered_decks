// ─────────────────────────────────────────────────────────────────────────────
//  msx2_story.c — the opening, the sanctum map, the dialogue, the ending
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_story.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_stream.h"
#include "msx2_audio.h"
#include "msx2_duel.h"
#include "msx2_cards.h"
#include "msx2_probe.h"
#include "msx2_scenes.h"
#include "msx2_disk.h"
#include "msx2_story_load.h"

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
		// The right-hand bust stands six pixels lower than the left one, so its
		// last rows fall inside the text box.  They are clipped here rather
		// than by moving the figure: the run table still has to be walked to
		// keep the pixel cursor in step, only the blit is skipped.
		bool visible = ((u8)(y + row) < MSX2_TALK_BOX_Y);
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
			if(visible)
			{
				Msx2_StreamRect(s, o, (u8)(x + rx), (u8)(y + row), head, 1);
				if(head != rn)
					Msx2_StreamRect((u16)(s + 1), 0, (u8)(x + rx + head),
					                (u8)(y + row), (u8)(rn - head), 1);
			}
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
		VDP_EnableDisplay(FALSE);
		Msx2_VideoDrawPage(page);
		Msx2_StreamSceneBlanked(segment, page);
		if(g_narr_which == NARR_INTRO)
			Msx2_StoryBlitBust(0, MSX2_PORTRAIT_LEFT_X, MSX2_PORTRAIT_LEFT_Y,
		                   TRUE);
		Msx2_VideoCopyPage(page, show);
		Msx2_VideoShowPage(page);
		VDP_EnableDisplay(TRUE);
		g_shot = shot;
		return;
	}

	if(g_shot == 0xFF)
	{
		// First line of the scene: the painting, then both figures on it.
		VDP_EnableDisplay(FALSE);
		Msx2_VideoDrawPage(page);
		Msx2_StreamSceneBlanked(
			MSX2_TALK_SEGMENT(g_msx2_stage_for_duel[g_duel_index]), page);
		Msx2_StoryBusts(shot);
		Msx2_VideoCopyPage(page, show);
		Msx2_VideoShowPage(page);
		VDP_EnableDisplay(TRUE);
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
			Msx2_TextAt(MAP_NAME_X, y, Msx2_UiText(MSX2_S_SEALED));
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
	Msx2_TextAt(MAP_NAME_X, MAP_DECK_Y, Msx2_UiText(MSX2_S_DECK_EDITOR));

	if(g_cursor == MAP_CODE_ROW)
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_CURSOR_X, MAP_CODE_Y, ">");
	}
	Msx2_TextColor((g_cursor == MAP_CODE_ROW) ? MSX2_WHITE : MSX2_SAND,
	               MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_CODE_Y, Msx2_UiText(MSX2_S_SAVE_GAME));

	if(g_cursor == MAP_BACK_ROW)
	{
		Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
		Msx2_TextAt(MAP_CURSOR_X, MAP_BACK_Y, ">");
	}
	Msx2_TextColor((g_cursor == MAP_BACK_ROW) ? MSX2_WHITE : MSX2_SAND,
	               MSX2_PANEL_COLOR);
	Msx2_TextAt(MAP_NAME_X, MAP_BACK_Y, Msx2_UiText(MSX2_S_LEAVE_THE_ROAD));

	Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
	Msx2_TextCenter(MAP_HELP_Y, Msx2_UiText(MSX2_S_SPACE_CHOOSES_UP_DOWN_MOVES));
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
//  Compact deck editor
// ─────────────────────────────────────────────────────────────────────────────

static void Msx2_StoryDeckPaint(void)
{
	u8 i;

	Msx2_Fill(2, 18, 252, 176, MSX2_PANEL_COLOR);
	Msx2_FrameRect(2, 18, 252, 176, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(25, Msx2_UiText(MSX2_S_DECK_EDITOR));
	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextCenter(35, Msx2_UiText(MSX2_S_CHOOSE_A_CARD_TO_REPLACE));

	for(i = 0; i < STORY_EDIT_SLOTS; ++i)
	{
		u8 x = EDIT_CARD_X(i);
		Msx2_StoryDrawCardThumb(g_story_deck[i], x, EDIT_DECK_Y);
	}
	Msx2_FrameRect((u8)(EDIT_CARD_X(g_editor_target) - 1), (u8)(EDIT_DECK_Y - 1),
	               MSX2_CARD_W + 2, MSX2_CARD_H + 2,
	               g_editor_storage_mode ? MSX2_GOLD : MSX2_RED);

	Msx2_TextColor(MSX2_TEAL, MSX2_PANEL_COLOR);
	Msx2_TextAt(8, 118, Msx2_UiText(MSX2_S_STORAGE));
	if(g_story_storage_count == 0)
	{
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(61, 118, Msx2_UiText(MSX2_S_EMPTY_WIN_DUELS_TO_EARN_CARD));
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
		Msx2_TextAt(61, 135, Msx2_UiText(MSX2_S_CARD));
		Msx2_NumAt(91, 135, (i16)g_story_storage[g_editor_storage_cursor]);
		Msx2_TextColor(MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(61, 151, Msx2_UiText(MSX2_S_ITEM));
		Msx2_NumAt(91, 151, (i16)(g_editor_storage_cursor + 1));
		Msx2_TextAt(111, 151, "OF");
		Msx2_NumAt(128, 151, (i16)g_story_storage_count);
	}

	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextCenter(197, g_editor_storage_mode
		? Msx2_UiText(MSX2_S_L_R_PICK_SPACE_SWAP_ESC_BACK)
		: Msx2_UiText(MSX2_S_L_R_TARGET_DOWN_PICK_ESC_EXI));
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
	if(pressed & (MSX2_BTN_A | MSX2_BTN_B))
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
	Msx2_TextColor(MSX2_WHITE, MSX2_PANEL_COLOR);
	Msx2_TextCenter(119, Msx2_UiText(MSX2_S_CARD));
	Msx2_NumAt(140, 119, (i16)g_reward_card);
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
