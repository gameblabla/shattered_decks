#!/usr/bin/env python3
"""Host test for the exact SNES story pagination routine."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
scene = (ROOT / "src/snes/snes_scene.c").read_text()
start = scene.index("static u8 story_page_extract")
end = scene.index("\n}\n\nstatic void clear_text_rows", start) + 2
routine = scene[start:end]
harness = r'''
#include <stdio.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned short u16;
#define SCENE_PANEL_TEXT_W 30
static char story_page_text[96];
''' + routine + r'''
#include "snes_dialogue_data.h"
int main(void) {
  int d, l, p, n, total;
  char rebuilt[512];
  for (d = 0; d < 5; ++d) for (l = 0; l < snes_story_line_count[d]; ++l) {
    total = 0;
    for (p = 0; ; ++p) {
      n = story_page_extract(snes_story_lines[d][l], (u8)p);
      if (!n) break;
      /* A page contains three independent 30-column visual rows.  Restore
         their implicit separator before comparing prose tokens. */
      { int off;
        for (off = 0; off < n; off += 30) {
          int end = off + 30 < n ? off + 30 : n;
          while (end > off && story_page_text[end - 1] == ' ') --end;
          memcpy(rebuilt + total, story_page_text + off, end - off);
          total += end - off;
          if (end > off && total < 512) rebuilt[total++] = ' ';
        }
      }
    }
    while (total && rebuilt[total - 1] == ' ') --total;
    rebuilt[total] = 0;
    /* The routine pads each short row to width 30 before the next row; ignore
       that presentational padding while requiring every prose word. */
    {
      char normalized[512], expected[512];
      int i, j;
      for (i = j = 0; rebuilt[i]; ++i) if (rebuilt[i] != ' ' || (i && rebuilt[i-1] != ' ')) normalized[j++] = rebuilt[i];
      normalized[j] = 0;
      for (i = j = 0; snes_story_lines[d][l][i]; ++i) if (snes_story_lines[d][l][i] != ' ' || (i && snes_story_lines[d][l][i-1] != ' ')) expected[j++] = snes_story_lines[d][l][i];
      expected[j] = 0;
    if (strcmp(normalized, expected)) {
      fprintf(stderr, "reconstruction failed duel %d line %d: %s\n", d, l, rebuilt);
      return 1;
    }
    }
    if (story_page_extract(snes_story_lines[d][l], (u8)p) != 0) return 2;
  }
  if (story_page_extract("", 0) != 0) return 3;
  puts("dialogue pagination OK");
  return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
    c = Path(td) / "test.c"
    exe = Path(td) / "test"
    (Path(td) / "snes.h").write_text("typedef signed char s8; typedef short s16; typedef long s32; typedef unsigned char u8; typedef unsigned short u16; typedef unsigned long u32;\n")
    c.write_text(harness)
    subprocess.run(["cc", "-std=c99", "-Wall", "-Werror", "-I", td, "-I", str(ROOT / "src/snes"), str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
