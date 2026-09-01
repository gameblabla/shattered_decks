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
           + ` --opt-code-size`;

ProjName = "waifu_msx2";

//-- Every module compiled into the ROM.  src/main.c is deliberately absent:
//   this port never compiles it (MSX2_PORT_PLAN.md §1.1).
ProjModules = [
	"msx2_main",
	"msx2_duel",
	"msx2_cards",
	"msx2_probe",
	"msx2_audio",
];

//-- Shared, platform-neutral game logic, compiled unmodified.
AddSources = [
	"../game/deck.c",
	"../game/ai.c",
	"msx2_libc.c",
];

LibModules = [ "system", "bios", "vdp", "print", "input", "memory" ];

Machine = "2";

//-- NEO-16: 16 KB segments, up to 64 MB.  The shipping cartridge is 16 MB; the
//   bring-up ROM stays at 1 MB so a build-and-verify cycle is seconds, not
//   minutes.  MSX2_ROM_SIZE_KB overrides it from Makefile.msx2.
Target = "ROM_NEO16";
ROMSize = Number(process.env.MSX2_ROM_SIZE_KB || 1024);

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
