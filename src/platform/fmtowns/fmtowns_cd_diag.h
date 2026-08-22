#ifndef FMTOWNS_CD_DIAG_H
#define FMTOWNS_CD_DIAG_H

#include <stdint.h>

/*
 * FM TOWNS CD-ROM diagnostics and read resilience -- game code only.
 *
 * libfmt's cdrom.c is a plain driver: it issues the handshake and returns 0
 * or -1.  Everything here is the game's policy on top of it -- what to do
 * when a read fails, and what to leave on screen if one never comes back.
 *
 * It exists because of a failure that could not be debugged remotely.  The
 * port froze on a real FM TOWNS Marty at the core's `LOADING... TITLE 0%`
 * screen, and that screen was the entire bug report: it did not say which
 * file, which sector, how far the handshake got, or what the CDC's status
 * register held.  The A0-setup-command fix (see STATUS.md) is the believed
 * cause, but it is a deduction from the boot ROM's traffic, not something
 * anyone has measured on hardware -- so this port has to assume it can still
 * hang there, and be built so that the next hang reports itself.
 *
 * Three parts:
 *
 *   1. A boot self-test that walks the CD path one step at a time and paints
 *      the result of each.  Each step's label is drawn *before* the step
 *      runs, so if a step never returns, the screen names it.
 *   2. A retrying, instrumented read wrapper.  Before every attempt it
 *      stamps a breadcrumb line (LBA, sector count, attempt, what is being
 *      loaded) into the bottom of *both* VRAM pages -- both, because a hang
 *      stops the page flip, so writing only the draw page can leave the
 *      evidence on the page nobody is looking at.
 *   3. A failure report screen: the last read's parameters, the CDC master
 *      status register, the running counters, and a retry prompt, so a dead
 *      drive is a readable diagnosis instead of a frozen picture.
 *
 * Nothing here is on the fast path of a running duel: breadcrumbs turn
 * themselves off once the asset load is done (see waifu_fmtowns_cdrom.c).
 */

/* Runs the boot self-test and paints its report.  Call once, after
 * fmt_machine_detect() (it writes VRAM) and before the game core starts.
 * Sets the video mode itself if nothing has yet.
 *
 * Returns 1 if every check passed.  On failure it holds the report on
 * screen with a visible countdown and retries the whole self-test when it
 * expires, so an unattended machine is never left dead on a transient
 * fault; A continues into the game anyway and returns 0. */
int fmtowns_cd_diag_selftest(void);

/* Sector read with the same contract as fmt_cdrom_read() -- 0 on success,
 * -1 after every retry has been used.  Records what it did either way. */
int fmtowns_cd_diag_read(uint32_t lba, uint16_t count, void *buf);

/* Names what is being loaded ("CARDFACE.BIN", ...), for the breadcrumb and
 * the failure report.  The string is not copied: pass a literal. */
void fmtowns_cd_diag_stage(const char *what);

/* Breadcrumbs cost ~2 KB of VRAM stores per read, which is worth it while
 * the loading screen is up and pure overhead once the game is running.  Off
 * until switched on. */
void fmtowns_cd_diag_breadcrumbs(int on);

/* Paints the failure report for the last failed read and waits.  Returns 1
 * if the player asked to retry (RUN, or the countdown expiring), 0 to give
 * up on the read (A). */
int fmtowns_cd_diag_report_failure(void);

#endif /* FMTOWNS_CD_DIAG_H */
