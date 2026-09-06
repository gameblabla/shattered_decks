WebMSX MoonSound / OPL4 NEW2 detection fix

Replace your existing wmsx.js with the included wmsx.js.

Keep your page configuration as:
  WMSX.MACHINE = "MSX2PE";
  WMSX.PRESETS = "OPL4";

This patch adds the YMF278B NEW2 status-bit behavior needed by the game\'s MoonSound detection routine.
