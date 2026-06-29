#ifndef WAIFU_FM_PLATFORM_H
#define WAIFU_FM_PLATFORM_H

/* ---------------------------------------------------------------------------
 * Platform service seams.
 *
 * Common game code depends only on the declarations in this header; each
 * target supplies the implementation in its own translation unit (host stdio,
 * PC-FX BackupRAM/CD, and — in the future — ROM consoles with SRAM/EEPROM or
 * RAM-only computers). The intent is that nothing platform specific
 * (<stdio.h>, console headers, file APIs) leaks into the common game logic;
 * it talks to the platform exclusively through these functions.
 *
 * This is the first seam to be carved out. Future seams (asset backend,
 * video/text surface, background layers, input, audio mixer pull) will be
 * declared here too as they are migrated off their current #ifdef-based
 * coupling.
 * ------------------------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Persistent storage --------------------------------------------------
 * A named byte-blob persistence service. The blob contents (format and
 * versioning) are owned entirely by the common game code; the platform only
 * decides *where* the bytes live:
 *   - host (SDL 1.2 / headless): a file on disk
 *   - PC-FX: BackupRAM / FX-BMP volumes (handled by its own save path today)
 *   - ROM consoles: SRAM / EEPROM / FRAM
 *   - RAM-only: an in-memory buffer
 *
 * `name` is an opaque identifier chosen by the caller (e.g. a save slot name).
 */

/* Returns 1 if a blob with this name exists, 0 otherwise. */
int waifu_platform_storage_exists(const char *name);

/* Writes `len` bytes. Returns the number of bytes written on success
 * (== len), or 0 on failure. */
int waifu_platform_storage_write(const char *name, const void *data, int len);

/* Reads up to `max_len` bytes into `data`. Returns the number of bytes read
 * (>= 0), or -1 if the blob could not be opened. */
int waifu_platform_storage_read(const char *name, void *data, int max_len);

/* ---- Background layer -----------------------------------------------------
 * Some platforms can present a scrolling background *behind* the game's
 * framebuffer using dedicated hardware (PC-FX RAINBOW). Platforms without one
 * composite the background into the framebuffer themselves (the host software
 * sky). The common scene code expresses its intent with a platform-agnostic
 * kind + horizontal scroll, and lets the platform decide how to realize it.
 */
typedef enum WaifuBackgroundKind {
    WAIFU_BACKGROUND_NONE = 0,
    WAIFU_BACKGROUND_DESERT,
    WAIFU_BACKGROUND_STONE,
    WAIFU_BACKGROUND_EMBER,
    WAIFU_BACKGROUND_SKY
} WaifuBackgroundKind;

/* Requests a background layer for this frame.
 *   returns 1: the platform presented `kind` on a dedicated hardware layer;
 *              the caller should leave the framebuffer background transparent.
 *   returns 0: the platform has no hardware background layer; the caller should
 *              composite the background into the framebuffer itself.
 * The host returns 0 today, but the seam is in place so a host background layer
 * can later present `kind`/`hscroll` and return 1 without touching game code. */
int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_PLATFORM_H */
