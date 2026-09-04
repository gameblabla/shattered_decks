// ─────────────────────────────────────────────────────────────────────────────
//  msx2_story_load.c — the LOAD STORY source picker
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_story_load.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_stream.h"
#include "msx2_disk.h"
#include "msx2_audio.h"
#include "msx2_scenes.h"

#define LOAD_ROWS      2
#define LOAD_ROW_Y(n)  (u8)(84 + (n) * 22)
#define LOAD_CODE_LEN  16

static c8* g_pick_code;
static u8  g_pick;
static u8  g_refused;
static u8  g_dirty;          // pages that still owe a repaint

static void Msx2_StoryLoadPickPaint(void)
{
	u8 i;
	bool disk = Msx2_DiskPresent();

	Msx2_Fill(38, 52, 180, 108, MSX2_PANEL_COLOR);
	Msx2_FrameRect(38, 52, 180, 108, MSX2_GOLD);
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(62, Msx2_UiText(MSX2_S_WHERE_IS_THE_SAVE));

	for(i = 0; i < LOAD_ROWS; ++i)
	{
		u8   y = LOAD_ROW_Y(i);
		bool live = (i != 0) || disk;
		Msx2_Fill(52, (u8)(y - 2), 12, 11, MSX2_PANEL_COLOR);
		if(i == g_pick)
		{
			Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
			Msx2_TextAt(54, y, ">");
		}
		// A machine with no drive still sees the row -- the offer is what
		// explains the feature -- and is told below why it is greyed out.
		Msx2_TextColor(live ? ((i == g_pick) ? MSX2_WHITE : MSX2_SAND)
		                    : MSX2_DARK_SAND, MSX2_PANEL_COLOR);
		Msx2_TextAt(72, y, Msx2_UiText((i == 0) ? MSX2_S_FLOPPY_DISK
		                                        : MSX2_S_PASSWORD));
	}

	Msx2_Fill(40, 128, 176, 10, MSX2_PANEL_COLOR);
	Msx2_TextColor(MSX2_RED, MSX2_PANEL_COLOR);
	if(g_refused)
		Msx2_TextCenter(130, Msx2_UiText(MSX2_S_NO_SAVE_ON_THIS_DISK));
	else if(!disk)
		Msx2_TextCenter(130, Msx2_UiText(MSX2_S_NO_DRIVE_ANSWERED));
	Msx2_TextColor(MSX2_GOLD, MSX2_PANEL_COLOR);
	Msx2_TextCenter(146, Msx2_UiText(MSX2_S_SPACE_PICKS_ESC_RETURNS));
}

void Msx2_StoryLoadPickEnter(c8* code)
{
	g_pick_code = code;
	g_pick = Msx2_DiskPresent() ? 0 : 1;
	g_refused = 0;
	g_dirty = 0;
	Msx2_VideoDrawPage(MSX2_PAGE_1);
	Msx2_StreamScene(MSX2_SCENE_TITLE_SEGMENT, MSX2_PAGE_1);
	Msx2_StoryLoadPickPaint();
	Msx2_VideoCopyPage(MSX2_PAGE_1, MSX2_PAGE_0);
	Msx2_VideoShowPage(MSX2_PAGE_1);
	Msx2_InputFlush();
}

void Msx2_StoryLoadPickRefused(void)
{
	g_refused = 1;
	g_dirty = (u8)((1u << MSX2_VIDEO_PAGES) - 1u);
}

u8 Msx2_StoryLoadPickStep(void)
{
	u8 pressed = Msx2_InputPressed();

	if(pressed & (MSX2_BTN_UP | MSX2_BTN_DOWN))
	{
		g_pick = (u8)(g_pick ^ 1);
		g_refused = 0;
		g_dirty = (u8)((1u << MSX2_VIDEO_PAGES) - 1u);
	}
	if(pressed & MSX2_BTN_B)
		return MSX2_LOADPICK_QUIT;
	if(pressed & (MSX2_BTN_A | MSX2_BTN_ENTER))
	{
		if(g_pick != 0)
			return MSX2_LOADPICK_PASSWORD;
		if(Msx2_DiskLoad(g_pick_code) && (g_pick_code[LOAD_CODE_LEN - 1] != 0))
			return MSX2_LOADPICK_LOADED;
		Msx2_StoryLoadPickRefused();
	}

	if(g_dirty & (u8)(1u << Msx2_VideoGetDrawPage()))
	{
		Msx2_StoryLoadPickPaint();
		g_dirty &= (u8)~(1u << Msx2_VideoGetDrawPage());
		Msx2_VideoFlipRequest();
	}
	return MSX2_LOADPICK_BUSY;
}
