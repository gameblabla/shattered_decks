CC ?= gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -DPLATFORM=5 -DBY16=1 -DHARDWARE_DIV=1
INCLUDES = -Isrc/engine -Isrc/generated -Isrc/record -Isrc/game
HEADLESS_SRCS = src/main.c src/engine/renderer3d.c src/engine/common.c src/engine/bmp_writer.c src/record/zmbv_mkv.c
SDL12_SRCS = src/platform/sdl12_main.c src/main.c src/engine/renderer3d.c src/engine/common.c src/engine/bmp_writer.c
TARGET = waifu_fm_headless
SDL12_TARGET = waifu_fm_sdl12
SDL_CONFIG ?= sdl-config
SDL_CFLAGS ?= $(shell $(SDL_CONFIG) --cflags 2>/dev/null)
SDL_LIBS ?= $(shell $(SDL_CONFIG) --libs 2>/dev/null)

all: $(TARGET)

assets:
	python3 tools/gen_assets.py
	python3 tools/gen_title_asset.py

$(TARGET): $(HEADLESS_SRCS) src/generated/waifu_assets.h src/generated/title_asset.h src/game/game_api.h
	$(CC) $(CFLAGS) $(INCLUDES) $(HEADLESS_SRCS) -lm -lz -o $@

sdl12: $(SDL12_TARGET)

$(SDL12_TARGET): $(SDL12_SRCS) src/generated/waifu_assets.h src/generated/title_asset.h src/game/game_api.h
	$(CC) $(CFLAGS) -DWAIFU_FM_NO_HEADLESS_MAIN $(INCLUDES) $(SDL_CFLAGS) $(SDL12_SRCS) $(SDL_LIBS) -lm -lz -o $@

run: $(TARGET)
	./$(TARGET) --frames 900 --commands scripts/battle_mode_demo.txt --out headless_frames --dump-every 10

record-demo: $(TARGET)
	./$(TARGET) --frames 3600 --commands scripts/battle_mode_demo.txt --no-png --record-mkv demo_zmbv.mkv

showcase: $(TARGET)
	./$(TARGET) --showcase --out showcase_frames

clean:
	rm -f $(TARGET) $(SDL12_TARGET)
	rm -rf headless_frames showcase_frames out_frames *.mkv *.mp4

.PHONY: all assets run record-demo showcase sdl12 clean
