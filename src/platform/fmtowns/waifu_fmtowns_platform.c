/* src/engine/platform.h seam for the FM TOWNS Marty port.
 *
 * Story saves go to the machine's 8 KiB battery-backed CMOS RAM -- see the
 * storage section below for how it is reached and why the game's block sits
 * where it does.  An IC memory card (common/icm.c) was the other candidate
 * and was rejected: it is optional hardware a player may simply not have,
 * while the CMOS is on every FM TOWNS including the Marty.
 *
 * Every other seam here has no dedicated hardware behind it either
 * (no sprite plane, no hardware text/tile overlay wired up beyond the
 * milestone-4/5 debug strips), so this mirrors the "no hardware layer"
 * shape CD32X's waifu_cd32x_video.c uses for the same functions: return 0/
 * no-op and let the common code composite everything into the software
 * framebuffer, exactly like the host build does. */
#include "platform.h"
#include <string.h>
#include "io.h"
#include "machine.h"

/* ---- Storage: the machine's battery-backed CMOS RAM -------------------
 *
 * A Marty has no cartridge and no Backup RAM chip the way the CD32X does,
 * but every FM TOWNS -- Marty included -- has 8 KiB of battery-backed CMOS
 * RAM (Technical Databook I-1: "8KB, battery-backed").  That is real
 * persistence, present on every machine, and it needs no IC memory card in
 * the slot, so this is where story progress goes.
 *
 * Reaching it takes two steps that are easy to get wrong:
 *
 *  1. The CMOS is not visible by default.  Bit 0 of the memory switch
 *     register at I/O 0x0480 (Databook table I-3-40) swaps the
 *     dictionary/learning RAM -- which the CMOS lives in the top of -- into
 *     the 0xD0000 region.  With that bit clear, writes to 0xD8000 land in
 *     nothing at all and read back as whatever was there; that is exactly
 *     what happened on the first attempt at this.  Verified against
 *     Tsugaru's own CMOSSAVE dump: with the bit set, 0xD8000 + i is CMOS
 *     byte i, 1:1.
 *
 *  2. The BIOS owns the low part of the CMOS -- boot device list, setup
 *     settings, the machine's own state.  A dump of a stock Marty CMOS has
 *     data scattered from 0x0000 up to about 0x10B0 and nothing above
 *     that, so the game's block sits at the very top (FMTOWNS_CMOS_SAVE_OFF)
 *     where it cannot tread on any of it.  Clobbering that region is not a
 *     harmless bug: it is the user's machine settings.
 *
 * The block is self-describing -- magic, length, checksum -- so a CMOS that
 * has never held a save (or whose battery died, or that another program
 * scribbled on) reads back as "no save" instead of as a corrupt one.
 */

#define FMTOWNS_MEMORY_SWITCH_PORT  0x0480
#define FMTOWNS_MEMORY_SWITCH_DIC   0x01    /* map dictionary/learning RAM */
#define FMTOWNS_CMOS_BASE           ((volatile unsigned char *)0x000D8000u)
#define FMTOWNS_CMOS_BYTES          8192u

/* Last 256 bytes of the 8 KiB, clear of everything the BIOS uses. */
#define FMTOWNS_CMOS_SAVE_OFF       (FMTOWNS_CMOS_BYTES - 256u)
#define FMTOWNS_SAVE_HEADER         8
#define FMTOWNS_SAVE_CAP            (256 - FMTOWNS_SAVE_HEADER)

/* 'W','F','S','1' -- bumping the last byte invalidates old saves on a
 * format change instead of misreading them. */
static const unsigned char k_save_magic[4] = { 0x57, 0x46, 0x53, 0x31 };

static unsigned short save_checksum(const unsigned char *data, int len)
{
    unsigned short sum = 0;
    int i;
    for (i = 0; i < len; ++i) {
        sum = (unsigned short)(sum + data[i]);
        sum = (unsigned short)((sum << 1) | (sum >> 15));   /* order matters */
    }
    return sum;
}

/* The CMOS window is only mapped while bit 0 of 0x0480 is set, and the same
 * register also decides whether 0xF8000 is ROM or RAM, so it is opened for
 * exactly as long as an access takes and then put back.  Nothing here is
 * re-entrant: this payload runs with interrupts disabled. */
static void cmos_open(void)  { outb(FMTOWNS_MEMORY_SWITCH_DIC, FMTOWNS_MEMORY_SWITCH_PORT); }
static void cmos_close(void) { outb(0x00, FMTOWNS_MEMORY_SWITCH_PORT); }

/* Reads the block's header, and its payload into `out` when one is given.
 * Returns the payload length, or -1 when the CMOS holds no valid save. */
static int cmos_read_block(unsigned char *out, int max_len)
{
    unsigned char header[FMTOWNS_SAVE_HEADER];
    unsigned char body[FMTOWNS_SAVE_CAP];
    volatile unsigned char *cmos;
    int len, i;
    unsigned short stored;

    cmos_open();
    cmos = FMTOWNS_CMOS_BASE + FMTOWNS_CMOS_SAVE_OFF;
    for (i = 0; i < FMTOWNS_SAVE_HEADER; ++i) {
        header[i] = cmos[i];
    }
    len = (int)header[4] | ((int)header[5] << 8);
    if (memcmp(header, k_save_magic, sizeof(k_save_magic)) != 0 ||
        len <= 0 || len > FMTOWNS_SAVE_CAP) {
        cmos_close();
        return -1;
    }
    for (i = 0; i < len; ++i) {
        body[i] = cmos[FMTOWNS_SAVE_HEADER + i];
    }
    cmos_close();

    stored = (unsigned short)((unsigned short)header[6] |
                              ((unsigned short)header[7] << 8));
    if (stored != save_checksum(body, len)) {
        return -1;
    }

    if (out) {
        int n = (len > max_len) ? max_len : len;
        memcpy(out, body, (size_t)n);
        return n;
    }
    return len;
}

int waifu_platform_storage_exists_dev(int device, const char *name)
{
    (void)name;
    /* One save device: the machine's own CMOS.  Unlike PC-FX/CD32X there is
     * no second (external) device to choose between, and the game only
     * compiles its device-picker states for those two targets. */
    if (device != 0) return 0;
    return cmos_read_block(0, 0) > 0;
}

int waifu_platform_storage_write_dev(int device, const char *name, const void *data, int len)
{
    const unsigned char *src = (const unsigned char *)data;
    volatile unsigned char *cmos;
    unsigned short sum;
    int i;

    (void)name;
    if (device != 0) return 0;
    if (len <= 0 || len > FMTOWNS_SAVE_CAP || !src) return 0;

    sum = save_checksum(src, len);

    cmos_open();
    cmos = FMTOWNS_CMOS_BASE + FMTOWNS_CMOS_SAVE_OFF;
    /* Body first, header last: a power cut part-way through leaves the old
     * magic over a half-written body, which reads back as "no save" rather
     * than as a valid header describing garbage. */
    for (i = 0; i < len; ++i) {
        cmos[FMTOWNS_SAVE_HEADER + i] = src[i];
    }
    for (i = 0; i < (int)sizeof(k_save_magic); ++i) {
        cmos[i] = k_save_magic[i];
    }
    cmos[4] = (unsigned char)(len & 0xff);
    cmos[5] = (unsigned char)((len >> 8) & 0xff);
    cmos[6] = (unsigned char)(sum & 0xff);
    cmos[7] = (unsigned char)((sum >> 8) & 0xff);
    cmos_close();

    return len;
}

int waifu_platform_storage_read_dev(int device, const char *name, void *data, int max_len)
{
    (void)name;
    if (device != 0) return -1;
    if (!data || max_len <= 0) return -1;
    return cmos_read_block((unsigned char *)data, max_len);
}

int waifu_platform_storage_exists(const char *name) { return waifu_platform_storage_exists_dev(0, name); }
int waifu_platform_storage_write(const char *name, const void *data, int len) { return waifu_platform_storage_write_dev(0, name, data, len); }
int waifu_platform_storage_read(const char *name, void *data, int max_len) { return waifu_platform_storage_read_dev(0, name, data, max_len); }

/* ---- No hardware background/text/sprite layers yet ------------------- */

int waifu_platform_ui_extra_w(void) { return 0; }
void waifu_platform_ui_hud(int on) { (void)on; }
int waifu_platform_performance_tier(void)
{
    int cpu_class = fmt_machine_cpu_class();
    if (cpu_class == FMT_MACHINE_ID_CPU_80486) return 2;
    if (cpu_class == FMT_MACHINE_ID_CPU_80386DX) return 1;
    return 0; /* 386SX-class Marty/UX, plus any unknown value as a safe default. */
}

int waifu_platform_glyph(int x, int y, int cell_w, unsigned char ch,
                         unsigned char fg, unsigned char shadow)
{ (void)x; (void)y; (void)cell_w; (void)ch; (void)fg; (void)shadow; return 0; }

void waifu_platform_prewarm_ending(void) {}

int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll)
{ (void)kind; (void)hscroll; return 0; }

int waifu_platform_text_overlay(WaifuTextOverlayKind kind, const WaifuTextOverlayParams *params)
{ (void)kind; (void)params; return 0; }

void waifu_platform_text_overlay_clear(void) {}

int waifu_platform_text_overlay_is_hardware(void) { return 0; }

void waifu_platform_story_layers_begin(void) {}
int waifu_platform_story_portrait(int portrait_id, int x, int y)
{ (void)portrait_id; (void)x; (void)y; return 0; }
