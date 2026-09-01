// Small story helpers are compiled with the normal 32 KB code area.  They do
// not touch the cartridge window, so they need none of the page-0 residency
// guarantee that keeps the streamer and board in waifu_msx2_s2_b0.c.

#include "msx2_story.h"
#include "msx2_input.h"
#include "msx2_video.h"
#include "msx2_stream.h"
#include "msx2_duel.h"
#include "msx2_cards.h"
#include "msx2_scenes.h"

#define CODE_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define STORY_NAME_LEN 8
#define STORY_CODE_LEN 16
#define STORY_EDIT_SLOTS 4

bool Msx2_StoryTakeStorageCard(u8* storage, u8* count, u8 card)
{
	u8 i;
	for(i = 0; i < *count; ++i)
	{
		if(storage[i] == card)
		{
			u8 j;
			for(j = i; j + 1 < *count; ++j)
				storage[j] = storage[j + 1];
			--*count;
			storage[*count] = MSX2_CARD_NONE;
			return TRUE;
		}
	}
	return FALSE;
}

u32 Msx2_StoryNameHash(const c8* name, u8 len)
{
	u32 h = 0x811C9DC5u;
	u8 i;
	for(i = 0; i < len; ++i)
	{
		h ^= (u8)name[i];
		h *= 16777619u;
	}
	return h;
}

u8 Msx2_StoryStageForProgress(u8 progress)
{
	if(progress >= 4) return 3;
	if(progress >= 2) return (u8)(progress - 1);
	return 0;
}

const c8* Msx2_StoryStageNameForProgress(u8 progress)
{
	switch(Msx2_StoryStageForProgress(progress))
	{
	case 1:  return "STONE TEMPLE";
	case 2:  return "EMBER CRATER";
	case 3:  return "THE VOID";
	default: return "DESERT ROAD";
	}
}

void Msx2_StoryDrawCardThumb(u8 card, u8 x, u8 y)
{
	if(card == MSX2_CARD_NONE)
	{
		Msx2_Fill(x, y, MSX2_CARD_W, MSX2_CARD_H, MSX2_BLACK);
		return;
	}
	Msx2_StreamRect((u16)(MSX2_CARD_ART_SEGMENT + card / MSX2_CARD_ART_PER_SEG),
	                (u16)((card % MSX2_CARD_ART_PER_SEG) * MSX2_CARD_ART_STRIDE),
	                x, y, MSX2_CARD_W, MSX2_CARD_H);
}

void Msx2_StoryCodeSetBits(u8* bytes, u8 bit, u8 count, u16 value)
{
	u8 i;
	for(i = 0; i < count; ++i)
	{
		u8 pos = (u8)(bit + i);
		if(value & (u8)(1u << i))
			bytes[pos >> 3] |= (u8)(1u << (pos & 7));
		else
			bytes[pos >> 3] &= (u8)~(1u << (pos & 7));
	}
}

u16 Msx2_StoryCodeGetBits(const u8* bytes, u8 bit, u8 count)
{
	u8 i;
	u16 value = 0;
	for(i = 0; i < count; ++i)
	{
		u8 pos = (u8)(bit + i);
		if(bytes[pos >> 3] & (u8)(1u << (pos & 7)))
			value |= (u8)(1u << i);
	}
	return value;
}

u8 Msx2_StoryCodeIndex(c8 ch)
{
	u8 i;
	for(i = 0; i < 32; ++i)
		if(CODE_ALPHABET[i] == (c8)ch)
			return i;
	return 0xFF;
}

void Msx2_StoryBuildCode(c8* code, u8 progress, const c8* name,
	                       const u8* deck)
{
	u8 bytes[10];
	u8 i;
	u8 checksum = 0;

	for(i = 0; i < 10; ++i)
		bytes[i] = 0;
	Msx2_StoryCodeSetBits(bytes, 0, 3, (u8)(progress & 7));
	for(i = 0; i < STORY_NAME_LEN; ++i)
		Msx2_StoryCodeSetBits(bytes, (u8)(3 + i * 5), 5,
		                      (u8)((name[i] >= 'A' && name[i] <= 'Z')
		                           ? (name[i] - 'A') : 0));
	for(i = 0; i < STORY_EDIT_SLOTS; ++i)
		Msx2_StoryCodeSetBits(bytes, (u8)(43 + i * 7), 7, deck[i]);
	for(i = 0; i < 9; ++i)
		checksum = (u8)(checksum + bytes[i]);
	Msx2_StoryCodeSetBits(bytes, 71, 8, checksum);
	for(i = 0; i < STORY_CODE_LEN; ++i)
		code[i] = (c8)CODE_ALPHABET[Msx2_StoryCodeGetBits(bytes, (u8)(i * 5), 5)];
	code[STORY_CODE_LEN] = 0;
}

bool Msx2_StoryDecodeCode(const c8* code, u8 len, u8* progress,
	                         c8* name, u8* swaps)
{
	u8 bytes[10];
	u8 i;
	u8 checksum = 0;
	u8 stored;

	if(len != STORY_CODE_LEN)
		return FALSE;
	for(i = 0; i < 10; ++i)
		bytes[i] = 0;
	for(i = 0; i < STORY_CODE_LEN; ++i)
	{
		u8 value = Msx2_StoryCodeIndex(code[i]);
		if(value == 0xFF)
			return FALSE;
		Msx2_StoryCodeSetBits(bytes, (u8)(i * 5), 5, value);
	}
	/* Sixteen base-32 symbols carry one spare bit beyond the 79-bit payload.
	   Reject non-zero values there so every valid save has one canonical code. */
	if(Msx2_StoryCodeGetBits(bytes, 79, 1) != 0)
		return FALSE;
	stored = (u8)Msx2_StoryCodeGetBits(bytes, 71, 8);
	Msx2_StoryCodeSetBits(bytes, 71, 8, 0);
	for(i = 0; i < 9; ++i)
		checksum = (u8)(checksum + bytes[i]);
	if(checksum != stored)
		return FALSE;

	*progress = Msx2_StoryCodeGetBits(bytes, 0, 3);
	for(i = 0; i < STORY_NAME_LEN; ++i)
		name[i] = (c8)('A' + Msx2_StoryCodeGetBits(bytes, (u8)(3 + i * 5), 5) % 26);
	name[STORY_NAME_LEN] = 0;
	for(i = 0; i < STORY_EDIT_SLOTS; ++i)
	{
		swaps[i] = Msx2_StoryCodeGetBits(bytes, (u8)(43 + i * 7), 7);
		if(swaps[i] >= MSX2_TOTAL_CARDS)
			return FALSE;
	}
	return TRUE;
}

u8 Msx2_StoryGridCursor(u8 cursor, u8 cols, u8 count, u8 pressed)
{
	if(pressed & MSX2_BTN_LEFT)
		cursor = (cursor == 0) ? (u8)(count - 1) : (u8)(cursor - 1);
	if(pressed & MSX2_BTN_RIGHT)
		cursor = (u8)((cursor + 1 == count) ? 0 : (cursor + 1));
	if(pressed & MSX2_BTN_UP)
		cursor = (cursor < cols) ? (u8)(cursor + count - cols)
		                         : (u8)(cursor - cols);
	if(pressed & MSX2_BTN_DOWN)
		cursor = (cursor + cols >= count) ? (u8)(cursor + cols - count)
		                                 : (u8)(cursor + cols);
	return cursor;
}
