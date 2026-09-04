// ─────────────────────────────────────────────────────────────────────────────
//  msx2_entropy_bank.c — RTC/R/tick/input collection in page-0 segment 3
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_entropy.h"
#include "clock.h"

extern volatile u16 g_msx2_ticks;

static u32 g_entropy_state;
u8 g_msx2_entropy_r;
u8 g_msx2_entropy_flags;

static u8 Msx2_EntropyReadR(void)
{
	__asm
		ld		a, r
		ld		(_g_msx2_entropy_r), a
	__endasm;
	return g_msx2_entropy_r;
}

static u8 Msx2_EntropyRtcPair(u8 units, u8 tens)
{
	u8 lo = RTC_Read(units);
	u8 hi = RTC_Read(tens);
	if((lo > 9) || (hi > 9))
		return 0xFF;
	return (u8)(lo + (hi << 3) + (hi << 1));
}

static u32 Msx2_EntropyReadRtc(u8* valid)
{
	u32 value = 0;
	u8 second;
	u8 minute;
	u8 hour;
	u8 day;
	u8 month;
	u8 year;
	u8 weekday;
	u8 before;
	u8 after;
	u8 retry;

	*valid = FALSE;
	if(!RTC_IsSettingOK())
		return 0;

	// The RTC exposes each BCD nibble independently.  A read that straddles a
	// second boundary can otherwise produce an impossible time, so take a
	// coherent sample and retry the small window when seconds changed.
	for(retry = 0; retry < 3; ++retry)
	{
		RTC_SetMode(RTC_MODE_TIME);
		before = RTC_Read(RTC_REG_TIME_SEC);
		second = (u8)(before + 10u * RTC_Read(RTC_REG_TIME_10SEC));
		minute = Msx2_EntropyRtcPair(RTC_REG_TIME_MIN, RTC_REG_TIME_10MIN);
		hour = Msx2_EntropyRtcPair(RTC_REG_TIME_HOUR, RTC_REG_TIME_10HOUR);
		weekday = RTC_Read(RTC_REG_TIME_WEEKDAY);
		day = Msx2_EntropyRtcPair(RTC_REG_TIME_DAY, RTC_REG_TIME_10DAY);
		month = Msx2_EntropyRtcPair(RTC_REG_TIME_MONTH, RTC_REG_TIME_10MONTH);
		year = Msx2_EntropyRtcPair(RTC_REG_TIME_YEAR, RTC_REG_TIME_10YEAR);
		RTC_SetMode(RTC_MODE_TIME);
		after = RTC_Read(RTC_REG_TIME_SEC);
		if(before > 9 || RTC_Read(RTC_REG_TIME_10SEC) > 9 ||
		   before != after || second > 59 || minute > 59 || hour > 23 ||
		   weekday > 6 || day == 0 || day > 31 || month == 0 || month > 12 ||
		   year == 0xFF)
			continue;

		value = ((u32)second << 24) ^ ((u32)minute << 18)
		      ^ ((u32)hour << 12) ^ ((u32)day << 7)
		      ^ ((u32)month << 3) ^ (u32)year ^ weekday;
		*valid = TRUE;
		return value;
	}
	return 0;
}

static u32 Msx2_EntropyStep(void)
{
	g_entropy_state ^= g_entropy_state << 13;
	g_entropy_state ^= g_entropy_state >> 17;
	g_entropy_state ^= g_entropy_state << 5;
	if(g_entropy_state == 0)
		g_entropy_state = 0xA5C3E17Du;
	return g_entropy_state;
}

static u32 Msx2_EntropyAvalanche(u32 value)
{
	value ^= value >> 16;
	value ^= value << 7;
	value ^= value >> 15;
	value ^= value << 11;
	value ^= value >> 16;
	return value;
}

void Msx2_EntropyInit_In(void)
{
#if defined(MSX2_TEST_SEED)
	g_entropy_state = (u32)MSX2_TEST_SEED;
	g_msx2_entropy_flags = MSX2_ENTROPY_FIXED;
#elif defined(MSX2_DEBUG_AUTOPLAY) || defined(MSX2_DEBUG_STORY_AUTOPLAY) || \
      defined(MSX2_DEBUG_REGRESSION)
	// Direct debug builds remain deterministic even without a make-time seed.
	g_entropy_state = 0x6D534832u;
	g_msx2_entropy_flags = MSX2_ENTROPY_FIXED;
#else
	u8 rtc_valid;
	u32 rtc = Msx2_EntropyReadRtc(&rtc_valid);
	u8 z80_r_before = Msx2_EntropyReadR();
	u16 ticks = g_msx2_ticks;
	u8 z80_r_after = Msx2_EntropyReadR();
	g_entropy_state = rtc ^ ((u32)z80_r_before << 24)
	                ^ ((u32)z80_r_after << 16) ^ (u32)ticks;
	g_entropy_state = Msx2_EntropyAvalanche(g_entropy_state ^ 0x6D534832u);
	g_msx2_entropy_flags = MSX2_ENTROPY_Z80_R | MSX2_ENTROPY_TICKS;
	if(rtc_valid)
		g_msx2_entropy_flags |= MSX2_ENTROPY_RTC;
#endif
	if(g_entropy_state == 0)
		g_entropy_state = 0x1F2E3D4Cu;
	g_msx2_initial_seed = g_entropy_state;
	g_msx2_duel_seed = 0;
}

void Msx2_EntropyMixInput_In(u8 held, u8 pressed, c8 typed)
{
#if !defined(MSX2_TEST_SEED) && !defined(MSX2_DEBUG_AUTOPLAY) && \
    !defined(MSX2_DEBUG_STORY_AUTOPLAY) && !defined(MSX2_DEBUG_REGRESSION)
	g_entropy_state ^= (u32)g_msx2_ticks;
	g_entropy_state ^= (u32)held << 24;
	g_entropy_state ^= (u32)pressed << 16;
	g_entropy_state ^= (u32)(u8)typed << 8;
	Msx2_EntropyStep();
	g_msx2_entropy_flags |= MSX2_ENTROPY_INPUT;
#else
	held; pressed; typed;
#endif
}

u32 Msx2_EntropyNextSeed_In(void)
{
#if !defined(MSX2_TEST_SEED) && !defined(MSX2_DEBUG_AUTOPLAY) && \
    !defined(MSX2_DEBUG_STORY_AUTOPLAY) && !defined(MSX2_DEBUG_REGRESSION)
	u8 rtc_valid;
	u32 rtc = Msx2_EntropyReadRtc(&rtc_valid);
	g_entropy_state ^= (u32)Msx2_EntropyReadR() << 24;
	g_entropy_state ^= (u32)Msx2_EntropyReadR() << 16;
	g_entropy_state ^= rtc;
	g_entropy_state ^= (u32)g_msx2_ticks;
	if(rtc_valid)
		g_msx2_entropy_flags |= MSX2_ENTROPY_RTC;
	g_msx2_entropy_flags |= MSX2_ENTROPY_Z80_R | MSX2_ENTROPY_TICKS;
#endif
	g_msx2_duel_seed = Msx2_EntropyStep();
	return g_msx2_duel_seed;
}
