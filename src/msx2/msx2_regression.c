// ─────────────────────────────────────────────────────────────────────────────
//  msx2_regression.c — fixed-seed/fixture plumbing for transient regressions
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_regression.h"

#ifdef MSX2_DEBUG_REGRESSION

#ifndef MSX2_TEST_FIXTURE
#define MSX2_TEST_FIXTURE MSX2_FIXTURE_NONE
#endif

static u8 g_fixture;

void Msx2_RegressionInit(void)
{
	g_fixture = MSX2_TEST_FIXTURE;
}

u8 Msx2_RegressionFixture(void)
{
	return g_fixture;
}

#else

void Msx2_RegressionInit(void) {}
u8 Msx2_RegressionFixture(void) { return MSX2_FIXTURE_NONE; }

#endif
