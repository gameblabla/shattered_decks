//──────────────────────────────────────────────────────────────────────────────
//  Shattered Decks — MSX2 target (MSXgl build configuration)
//
//  Driven by Makefile.msx2, which runs MSXgl's node build with this directory
//  as the working directory.  MSXGL_PATH selects the engine tree; nothing here
//  hardcodes a path outside it.
//
//  See MSX2_PORT_PLAN.md for what this target is and why it is shaped this way.
//──────────────────────────────────────────────────────────────────────────────

//-- The bundled SDCC is relocated, so it cannot find its own standard headers
//   without being told where they are.  Our own include roots follow: the MSX2
//   fork sources, then the shim that stands in for the 5.3 MB generated asset
//   header (must precede src/generated), then the shared game logic.
CompileOpt = `-I${ToolsDir}sdcc/include`
           + ` -I./compat -I../game -I../generated`
           + ` --opt-code-size`
           + (process.env.MSX2_MAPPER === "ascii16x" ? ` -DMSX2_ASCII16X` : ``)
           // The blind soak needs the player's turn played by the AI; see
           // MSX2_DEBUG_AUTOPLAY in msx2_board.c.  Driven from the environment
           // so `make -f Makefile.msx2 soak` is the only thing that knows.
           + (process.env.MSX2_AUTOPLAY ? ` -DMSX2_DEBUG_AUTOPLAY` : ``)
           + (process.env.MSX2_STORY_AUTOPLAY ? ` -DMSX2_DEBUG_STORY_AUTOPLAY` : ``)
           + (process.env.MSX2_REGRESSION ? ` -DMSX2_DEBUG_REGRESSION` : ``)
           // The MSX2+ cartridge: the 2-D screens in SCREEN 10 (YJK+YAE)
           // instead of GRAPHIC 7.  Same code, same segments, different
           // pictures -- see tools/msx2/gen_msx_plus.py.
           + (process.env.MSX2_PLUS ? ` -DMSX2_PLUS` : ``)
           + (process.env.MSX2_TEST_SEED ? ` -DMSX2_TEST_SEED=${process.env.MSX2_TEST_SEED}u` : ``)
           + (process.env.MSX2_TEST_FIXTURE ? ` -DMSX2_TEST_FIXTURE=${process.env.MSX2_TEST_FIXTURE}` : ``);

const MSX2_ASCII16X = process.env.MSX2_MAPPER === "ascii16x";

// Keep the segment wrapper stem stable while giving each mapper its own
// incremental object tree and ROM name.
ProjName = process.env.MSX2_OUTPUT_NAME || "waifu_msx2";
ProjSegments = MSX2_ASCII16X ? "waifu_msx2_ascii" : "waifu_msx2";
if (process.env.MSX2_OUT_DIR)
	OutDir = process.env.MSX2_OUT_DIR;
if (!OutDir.endsWith("/"))
	OutDir += "/";

//-- Every module compiled into the ROM.  src/main.c is deliberately absent:
//   this port never compiles it (MSX2_PORT_PLAN.md §1.1).
ProjModules = [
	//-- FIRST, and that placement is load-bearing.  The streamer switches the
	//   0x8000 window to cartridge data, so none of the code up there exists
	//   while it runs -- itself included.  Linking it ahead of everything else
	//   puts it just past crt0, far below the window; pack_msx_rom.py fails the
	//   build if that ever stops being true.
	"msx2_stream",
	// These MSXgl modules are explicit project objects so they are placed
	// beside the streamer, below the 0x8000 window.  Keeping them in LibModules
	// would make the linker pull them after every project object.
	"msx2_psg",
	"msx2_lvgm",
	"msx2_audio",
	"msx2_entropy",
	"msx2_main",
	// Title presentation lives in the modal bank; public calls use msx2_bank.
	"msx2_video",
	"msx2_sprite",
	"msx2_bank",
	"msx2_disk",
	"msx2_input",
	// msx2_battle_fx is NOT here: it is #included into waifu_msx2_s2_b0.c,
	// because the duel screen is its only caller and _CODE had run out.
	"msx2_raster",
	"msx2_probe",
	"msx2_regression",
];

if (MSX2_ASCII16X)
	AddSources = [ "msx2_libc.c" ];
else
	AddSources = [ "../game/deck.c", "../game/ai.c", "msx2_libc.c" ];

if (MSX2_ASCII16X)
	ProjModules.splice(1, 0, "msx2_mapper");
else
	ProjModules.splice(12, 0, "msx2_duel", "msx2_cards");

// ASCII16-X uses the explicit page-3 IM 2 path.  Page 0 remains the machine's
// BIOS/RAM page; all cartridge scene code uses the 0x8000 window and the fixed
// trampolines in msx2_bank.c.  This keeps the SDK's page-0 copy source from
// colliding with the initialized-data payload in the fixed cartridge image.

LibModules = [ "system", "bios", "vdp", "input", "memory" ];

//-- The NEO-16 plus build uses MSXgl's normal MSX2+ surface.  ASCII16-X has
//   its own small R#25/YJK path, so it is compiled as an MSX2 application and
//   avoids pulling unused MSX2+ VDP helpers into its 32 KB resident image.
Machine = process.env.MSX2_PLUS
        ? (MSX2_ASCII16X ? "2" : "2P")
        : "2";

//-- The interrupt handler goes into RAM page 3 rather than into cartridge
//   segment 2 at 0x0038.  That is what makes page 0 a SWITCHABLE code window:
//   with the ISR in ROM, mapping any other segment at 0x0000 would take the
//   handler away with it and the next interrupt would run whatever byte landed
//   there.  It costs 452 bytes of page-3 RAM and puts the Z80 in IM 2.
//   See waifu_msx2_s3_b0.c and msx2_bank.c for what the window is used for.
InstallRAMISR = "RAMISR_PAGE3";

//-- NEO-16: 16 KB segments, 1 MB to 64 MB in powers of two.  The cartridge is
//   the smallest of those the baked assets fit in -- 8 MB, for a last asset
//   segment of 256 -- and that is both the bring-up and the shipping size.
//   MSX2_ROM_SIZE_KB overrides it from Makefile.msx2, which carries the note on
//   what pushed the image past 4 MB and why 4 MB is out of reach without a
//   smaller FM encoding or fewer assets.
Target = MSX2_ASCII16X ? "ROM_ASCII16X" : "ROM_NEO16";
ROMSize = Number(process.env.MSX2_ROM_SIZE_KB || 8192);

CheckVersion = true;
AddROMSignature = true;

AppSignature = true;
AppCompany = "GB";
AppID = "SD";

Optim = "Size";
CompileComplexity = "Default";

//-- Report the RAM and code footprint every build: the port lives inside a
//   32 KB resident-code and ~12 KB RAM budget, and the map file is the only
//   honest measure of both.
Analyzer = true;
AnalyzerOutput = "Both";
AnalyzerReport = "ASM";
AnalyzerSort = "Size";

Verbose = true;
DoRun = false;
