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
           // The blind soak needs the player's turn played by the AI; see
           // MSX2_DEBUG_AUTOPLAY in msx2_board.c.  Driven from the environment
           // so `make -f Makefile.msx2 soak` is the only thing that knows.
           + (process.env.MSX2_AUTOPLAY ? ` -DMSX2_DEBUG_AUTOPLAY` : ``)
           + (process.env.MSX2_STORY_AUTOPLAY ? ` -DMSX2_DEBUG_STORY_AUTOPLAY` : ``)
           + (process.env.MSX2_REGRESSION ? ` -DMSX2_DEBUG_REGRESSION` : ``)
           + (process.env.MSX2_TEST_SEED ? ` -DMSX2_TEST_SEED=${process.env.MSX2_TEST_SEED}u` : ``)
           + (process.env.MSX2_TEST_FIXTURE ? ` -DMSX2_TEST_FIXTURE=${process.env.MSX2_TEST_FIXTURE}` : ``);

ProjName = "waifu_msx2";

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
	"msx2_main",
	"msx2_title",
	"msx2_video",
	"msx2_sprite",
	"msx2_bank",
	"msx2_disk",
	"msx2_input",
	"msx2_battle_fx",
	"msx2_raster",
	"msx2_duel",
	"msx2_cards",
	"msx2_probe",
	"msx2_regression",
];

//-- Shared, platform-neutral game logic, compiled unmodified.
AddSources = [
	"../game/deck.c",
	"../game/ai.c",
	"msx2_libc.c",
];

LibModules = [ "system", "bios", "vdp", "input", "memory" ];

Machine = "2";

//-- The interrupt handler goes into RAM page 3 rather than into cartridge
//   segment 2 at 0x0038.  That is what makes page 0 a SWITCHABLE code window:
//   with the ISR in ROM, mapping any other segment at 0x0000 would take the
//   handler away with it and the next interrupt would run whatever byte landed
//   there.  It costs 452 bytes of page-3 RAM and puts the Z80 in IM 2.
//   See waifu_msx2_s3_b0.c and msx2_bank.c for what the window is used for.
InstallRAMISR = "RAMISR_PAGE3";

//-- NEO-16: 16 KB segments, up to 64 MB.  The shipping cartridge is 16 MB; the
//   bring-up ROM is the smallest size the baked assets fit in, so a
//   build-and-verify cycle is seconds, not minutes.  MSX2_ROM_SIZE_KB
//   overrides it from Makefile.msx2.
Target = "ROM_NEO16";
ROMSize = Number(process.env.MSX2_ROM_SIZE_KB || 2048);

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
