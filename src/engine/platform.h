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

/* Multi-device variants. Some platforms expose more than one save device
 * (CD32X: 0 = internal Backup RAM, 1 = Backup RAM cartridge); the common code
 * offers a device picker over them. Single-device platforms ignore `device`.
 * The non-`_dev` calls above are equivalent to `device == 0`. */
int waifu_platform_storage_exists_dev(int device, const char *name);
int waifu_platform_storage_write_dev(int device, const char *name, const void *data, int len);
int waifu_platform_storage_read_dev(int device, const char *name, void *data, int max_len);

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

/* ---- Widescreen HUD -------------------------------------------------------
 * On a widescreen presentation (the SDL3/PC build) the 3D view fills the frame,
 * but the fixed 2D UI is otherwise centered in a game-aspect column with side
 * bars. These let the duel HUD reach the true screen edges:
 *   - ui_extra_w() is the extra game-x width available beyond WAIFU_FM_WIDTH
 *     (0 when there is no widescreen room); the HUD adds it to right-anchored
 *     positions and to the full-width bar so they span the wider frame.
 *   - ui_hud(1) .. ui_hud(0) brackets the HUD draw calls; the platform renders
 *     everything between them across the FULL width (mapping game-x [0,
 *     WAIFU_FM_WIDTH+extra] to the whole frame at native scale) instead of the
 *     centered column, so edge-anchored HUD lands at the screen edges.
 * Non-widescreen platforms return 0 / no-op, leaving the layout unchanged. */
int waifu_platform_ui_extra_w(void);
void waifu_platform_ui_hud(int on);

/* Arena backdrop for the 3D duel board.  The board is drawn over a flat black
 * clear on every console; a platform with a real colour pipeline may instead
 * fill the frame with a graded backdrop, which is what keeps a widescreen duel
 * from being a small board floating in a black void.  Called immediately after
 * the field clear, before any board geometry, so whatever it draws lands
 * underneath.  Returns 1 when the platform drew one (purely informational —
 * the caller draws the board either way); consoles return 0 and nothing
 * changes. */
int waifu_platform_arena_backdrop(void);

/* Coarse CPU/rendering-performance tier for optional visual fidelity scaling.
 * 0 = baseline/constrained, 1 = intermediate, 2 = fast.  Game timing must not
 * depend on this value; it may only select how many intermediate visual poses
 * are rendered or retained during a fixed-duration animation. */
int waifu_platform_performance_tier(void);

/* Per-character text rendering seam. A platform that can render high-resolution
   glyphs (SDL3/PC via FreeType) draws the character at cell origin (x,y) with
   the given fixed cell advance and returns 1; the common text primitives then
   skip their 8x8 bitmap blit. Returns 0 to fall back to the bitmap font (every
   console target, and SDL3 if the font could not be loaded). fg/shadow are
   palette indices. */
int waifu_platform_glyph(int x, int y, int cell_w, unsigned char ch,
                         unsigned char fg, unsigned char shadow);

/* Preload the story-ending full-screen image so the ending scene does not stall
   mid-typewriter on its first frame. On PC-FX the ending image is a direct
   CD->KRAM DMA performed lazily by the first ending present; calling this while
   the screen is already faded to black (end of the reward->ending transition)
   moves that blocking read behind the black frame. No-op where the ending image
   is preloaded through the normal asset/loading path (CD32X) or is cheap (PC). */
void waifu_platform_prewarm_ending(void);

/* ---- Text: software + hardware --------------------------------------------
 * UI text is rendered two complementary ways, and a single platform may use
 * BOTH at once (PC-FX does):
 *
 *   - SOFTWARE text: glyphs composited straight into the CPU framebuffer
 *     (the draw_text* family in the common code). Available on every platform;
 *     used for in-scene HUD, card stats, etc. It needs no seam — it is just
 *     common code writing pixels, so it stays maximally fast (no indirection).
 *
 *   - HARDWARE text: a dedicated text/overlay layer that sits above the
 *     framebuffer and is untouched by 2D/3D redraws (PC-FX VDC tile overlay).
 *     Whole-screen UI panels that live on such a layer are requested through
 *     the seam below. One call presents an entire panel (no per-glyph
 *     indirection), so it is cheap and platform agnostic.
 *
 * A panel-drawing site asks the platform to present the panel on its hardware
 * text layer; if there is none, the platform returns 0 and the SAME site
 * composites the panel with the software text API instead. This keeps both
 * text paths first-class and composable without #ifdef at the call site.
 */
typedef enum WaifuTextOverlayKind {
    WAIFU_TEXT_OVERLAY_TITLE_PROMPT = 0,
    WAIFU_TEXT_OVERLAY_MENU,
    WAIFU_TEXT_OVERLAY_LOAD_MENU,
    WAIFU_TEXT_OVERLAY_ENDING_STORY,
    WAIFU_TEXT_OVERLAY_ENDING_CREDITS
} WaifuTextOverlayKind;

typedef struct WaifuTextOverlayParams {
    int selected;            /* menu/load cursor index */
    int has_save;            /* title/menu: a save exists */
    int internal_has_save;   /* load menu: internal device has a save */
    int external_has_save;   /* load menu: external device has a save */
    int page;                /* ending story: narration page */
    int prompt_visible;      /* title/ending: blink-visible prompt flag */
    int visible_chars;       /* ending story: typewriter character count */
    const char *name;        /* ending story: protagonist name */
} WaifuTextOverlayParams;

/* Presents a whole-screen UI panel on the platform's hardware text layer.
 * Returns 1 if it was presented in hardware (the caller must NOT also software-
 * draw the panel), or 0 if there is no hardware text layer (the caller renders
 * the panel with the software text API). `params` may be NULL for kinds that
 * take none. */
int waifu_platform_text_overlay(WaifuTextOverlayKind kind, const WaifuTextOverlayParams *params);

/* Clears any hardware text overlay. A no-op where there is no hardware layer. */
void waifu_platform_text_overlay_clear(void);

/* Returns 1 if UI panels are presented on a hardware text layer (so the
 * framebuffer background need only be (re)composed when it actually changes,
 * and panel text is refreshed without touching the framebuffer), or 0 if panels
 * are software-composited into the framebuffer every frame. Lets a draw site
 * pick the right redraw strategy without #ifdef; the value is constant per
 * platform so the branch is trivially predicted. */
int waifu_platform_text_overlay_is_hardware(void);

/* ---- Story portraits ------------------------------------------------------
 * A platform with a hardware sprite plane may take story portraits out of the
 * software framebuffer.  The core brackets every rendered frame, then submits
 * each portrait in draw order.  Returning 1 means the caller must not blit the
 * portrait into the framebuffer. */
void waifu_platform_story_layers_begin(void);
int waifu_platform_story_portrait(int portrait_id, int x, int y);

/* ---- PC frontend seams (SDL3 only) ----------------------------------------
 * Everything below exists only on a build with a hardware 3D/2D frontend
 * (WAIFU_PLATFORM_HW3D). Console targets compile the inert static-inline stubs,
 * which fold away completely, so their behaviour stays byte-identical. */
#if defined(WAIFU_PLATFORM_HW3D)

/* Mouse pointer, reported in the game's widescreen HUD space: x runs
 * 0 .. WAIFU_FM_WIDTH + waifu_platform_ui_extra_w(), y runs 0 .. WAIFU_FM_HEIGHT.
 * A screen drawn in the CENTERED game column subtracts extra/2 from x.
 * `active` is 1 only while the player is actually using the mouse (it moved or
 * was clicked recently), which is what gates the on-screen buttons. */
typedef struct WaifuPointer {
    int active;
    int x, y;
    int left_down;
    int left_pressed;      /* button went down this frame */
    int left_released;     /* button came up this frame */
    int right_pressed;
    int wheel;             /* notches since the last poll: + away, - toward */
    int drag_x, drag_y;    /* where the current/just-ended drag started */
} WaifuPointer;

/* 1 when a pointer exists and `out` was filled. */
int waifu_platform_pointer(WaifuPointer *out);

/* 1 exactly once after the output surface was rebuilt (resolution / window mode
 * change). The persistent canvas is empty afterwards, so the core must drop its
 * retained-screen caches and redraw the whole frame. */
int waifu_platform_display_reset(void);

/* Opens the frontend's own options screen (title-menu OPTIONS row). */
void waifu_platform_open_options(void);

/* Draws the PSX-DOOM style fire across the given screen rect (game HUD space)
 * at the frontend's native resolution. Returns 1 when it was drawn (the caller
 * then skips the low-resolution software flames). */
int waifu_platform_fire(int x, int y, int w, int h);

/* Keyboard text entry. The core turns this on for as long as a text field is
 * open; while it is on the frontend stops routing text-producing keys to the
 * action layer (so typing a name cannot also press buttons) and queues what was
 * typed. waifu_platform_text_poll() takes one edit off that queue: a character
 * code, '\b' for backspace, or 0 when there is nothing left. */
void waifu_platform_text_input(int on);
int waifu_platform_text_poll(void);

/* On-screen prompts name a CONTROL, and on PC the player can rebind it (and may
 * be holding a pad rather than a keyboard), so the prompt has to ask what it is
 * currently called instead of printing the console's fixed button letters.
 * Returns a short upper-case name for the control bound to `action` -- a
 * WaifuPromptAction -- or NULL when the caller should keep its authored text
 * (every console, where the labels are printed on the pad itself). */
typedef enum WaifuPromptAction {
    WAIFU_PROMPT_CONFIRM = 0,
    WAIFU_PROMPT_CANCEL,
    WAIFU_PROMPT_START,
    WAIFU_PROMPT_ASSIST
} WaifuPromptAction;

const char *waifu_platform_prompt_label(int action);

#else

typedef struct WaifuPointer {
    int active;
    int x, y;
    int left_down;
    int left_pressed;
    int left_released;
    int right_pressed;
    int wheel;
    int drag_x, drag_y;
} WaifuPointer;

static inline int waifu_platform_pointer(WaifuPointer *out) { (void)out; return 0; }
static inline int waifu_platform_display_reset(void) { return 0; }
static inline void waifu_platform_open_options(void) {}
static inline int waifu_platform_fire(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; return 0; }
static inline void waifu_platform_text_input(int on) { (void)on; }
static inline int waifu_platform_text_poll(void) { return 0; }
typedef enum WaifuPromptAction {
    WAIFU_PROMPT_CONFIRM = 0,
    WAIFU_PROMPT_CANCEL,
    WAIFU_PROMPT_START,
    WAIFU_PROMPT_ASSIST
} WaifuPromptAction;
static inline const char *waifu_platform_prompt_label(int action) { (void)action; return 0; }

#endif /* WAIFU_PLATFORM_HW3D */

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_PLATFORM_H */
