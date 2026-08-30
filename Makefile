CC ?= gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -DPLATFORM=5 -DBY16=1 -DHARDWARE_DIV=1
INCLUDES = -Isrc/engine -Isrc/generated -Isrc/record -Isrc/game
HEADLESS_SRCS = src/main.c src/game/ai.c src/game/deck.c src/game/palette.c src/game/sounds.c src/game/assets.c src/engine/renderer3d.c src/engine/renderer3d_generic.c src/engine/common.c src/engine/bmp_writer.c src/platform/host_storage.c src/platform/host_assets.c src/platform/host_video.c src/record/zmbv_mkv.c
SDL12_SRCS = src/platform/sdl12_main.c src/main.c src/game/ai.c src/game/deck.c src/game/palette.c src/game/sounds.c src/game/assets.c src/engine/renderer3d.c src/engine/renderer3d_generic.c src/engine/common.c src/engine/bmp_writer.c src/platform/host_storage.c src/platform/host_assets.c src/platform/host_video.c
SDL3_SRCS = src/platform/sdl3/sdl3_main.c src/platform/sdl3/sdl3_video.c src/platform/sdl3/sdl3_audio.c src/platform/sdl3/sdl3_scene3d.c src/platform/sdl3/sdl3_image_load.c src/platform/sdl3/sdl3_hires.c src/platform/sdl3/sdl3_text.c src/platform/sdl3/sdl3_settings.c src/platform/sdl3/sdl3_input.c src/platform/sdl3/sdl3_overlay.c src/platform/sdl3/sdl3_menu.c src/main.c src/game/ai.c src/game/deck.c src/game/palette.c src/game/sounds.c src/game/assets.c src/engine/renderer3d.c src/engine/renderer3d_generic.c src/engine/common.c src/engine/bmp_writer.c src/platform/host_storage.c src/platform/host_assets.c
TARGET = waifu_fm_headless
SDL12_TARGET = waifu_fm_sdl12
SDL3_TARGET = waifu_fm_sdl3
SDL_CONFIG ?= sdl-config
SDL_CFLAGS ?= $(shell $(SDL_CONFIG) --cflags 2>/dev/null)
SDL_LIBS ?= $(shell $(SDL_CONFIG) --libs 2>/dev/null)
SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS ?= $(shell pkg-config --libs sdl3 2>/dev/null)
# Full-resolution card / title / ending art on PC decodes source PNG/WebP.
SDL3_IMG_CFLAGS ?= $(shell pkg-config --cflags libpng libwebp 2>/dev/null)
SDL3_IMG_LIBS ?= $(shell pkg-config --libs libpng libwebp 2>/dev/null)
# High-resolution TTF text on PC via FreeType.
SDL3_FT_CFLAGS ?= $(shell pkg-config --cflags freetype2 2>/dev/null)
SDL3_FT_LIBS ?= $(shell pkg-config --libs freetype2 2>/dev/null)

all: $(TARGET)

assets:
	python3 tools/gen_assets.py
	python3 tools/gen_title_asset.py
	python3 tools/gen_sound_assets.py
	python3 tools/gen_pcfx_sfx_adpcm.py
	python3 tools/gen_sdl3_hires_paths.py

$(TARGET): $(HEADLESS_SRCS) src/generated/waifu_assets.h src/generated/deck_pools.h src/generated/title_asset.h src/generated/sound_assets.h src/game/card_ids.h src/game/game_api.h src/game/ai.h src/game/deck.h src/game/palette.h src/game/sounds.h src/game/assets.h
	$(CC) $(CFLAGS) -DWAIFU_FM_HEADLESS_TESTS $(INCLUDES) $(HEADLESS_SRCS) -lm -lz -o $@

headless-cdrom-assets: $(HEADLESS_SRCS) src/generated/waifu_assets.h src/generated/deck_pools.h src/generated/title_asset.h src/generated/sound_assets.h src/game/card_ids.h src/game/assets.h assets/generated/title_screen_img.bin assets/generated/ending_screen_pcfx_yuv422.bin assets/generated/story_portraits.bin assets/generated/story_portrait_mask.bin assets/generated/card_faces.bin assets/generated/card_big_art.bin assets/generated/card_back.bin assets/generated/support_face.bin assets/generated/support_big_art.bin
	$(CC) $(CFLAGS) -DWAIFU_FM_HEADLESS_TESTS -DWAIFU_ASSET_USE_CDROM -DWAIFU_ASSET_EXTERNAL_TITLE_IMAGE -DWAIFU_ASSET_EXTERNAL_STORY_PORTRAITS $(INCLUDES) $(HEADLESS_SRCS) -lm -lz -o waifu_fm_headless_cdrom


headless-cdrom-assets-2mb: $(HEADLESS_SRCS) src/generated/waifu_assets.h src/generated/deck_pools.h src/generated/title_asset.h src/generated/sound_assets.h src/game/card_ids.h src/game/assets.h assets/generated/title_screen_img.bin assets/generated/ending_screen_pcfx_yuv422.bin assets/generated/story_portraits.bin assets/generated/story_portrait_mask.bin assets/generated/card_faces.bin assets/generated/card_big_art.bin assets/generated/card_back.bin assets/generated/support_face.bin assets/generated/support_big_art.bin
	$(CC) $(CFLAGS) -DWAIFU_FM_HEADLESS_TESTS -DWAIFU_ASSET_USE_CDROM -DWAIFU_ASSET_RAM_BUDGET=2097152 $(INCLUDES) $(HEADLESS_SRCS) -lm -lz -o waifu_fm_headless_cdrom_2mb

headless-cdrom-assets-large: $(HEADLESS_SRCS) src/generated/waifu_assets.h src/generated/deck_pools.h src/generated/title_asset.h src/generated/sound_assets.h src/game/card_ids.h src/game/assets.h assets/generated/title_screen_img.bin assets/generated/ending_screen_pcfx_yuv422.bin assets/generated/story_portraits.bin assets/generated/story_portrait_mask.bin assets/generated/card_faces.bin assets/generated/card_big_art.bin assets/generated/card_back.bin assets/generated/support_face.bin assets/generated/support_big_art.bin
	$(CC) $(CFLAGS) -DWAIFU_FM_HEADLESS_TESTS -DWAIFU_ASSET_USE_CDROM -DWAIFU_ASSET_RAM_BUDGET=4194304 $(INCLUDES) $(HEADLESS_SRCS) -lm -lz -o waifu_fm_headless_cdrom_large

headless-cart-assets: $(HEADLESS_SRCS) src/generated/waifu_assets.h src/generated/deck_pools.h src/generated/title_asset.h src/game/card_ids.h src/game/assets.h
	$(CC) $(CFLAGS) -DWAIFU_FM_HEADLESS_TESTS -DWAIFU_ASSET_USE_CART_ROM $(INCLUDES) $(HEADLESS_SRCS) -lm -lz -o waifu_fm_headless_cart

sdl12: $(SDL12_TARGET)

$(SDL12_TARGET): $(SDL12_SRCS) src/generated/waifu_assets.h src/generated/deck_pools.h src/generated/title_asset.h src/generated/sound_assets.h src/game/card_ids.h src/game/game_api.h src/game/ai.h src/game/deck.h src/game/palette.h src/game/sounds.h src/game/assets.h
	$(CC) $(CFLAGS) -DWAIFU_FM_NO_HEADLESS_MAIN $(INCLUDES) $(SDL_CFLAGS) $(SDL12_SRCS) $(SDL_LIBS) -lm -lz -o $@

sdl3: $(SDL3_TARGET)

# The SDL3 build captures every 2D/3D primitive through the hw3d/hw2d seams
# (WAIFU_PLATFORM_HW3D) and renders them with SDL_GPU; the CPU pixel caches
# cache software-rasterized framebuffer content that never exists here, so
# they are compiled out to keep the capture stream complete every frame.
$(SDL3_TARGET): $(SDL3_SRCS) src/platform/sdl3/sdl3_video.h src/platform/sdl3/sdl3_audio.h src/platform/sdl3/sdl3_internal.h src/platform/sdl3/sdl3_hires.h src/platform/sdl3/sdl3_image_load.h src/platform/sdl3/sdl3_text.h src/platform/sdl3/sdl3_settings.h src/platform/sdl3/sdl3_input.h src/platform/sdl3/sdl3_overlay.h src/platform/sdl3/sdl3_menu.h src/engine/hw3d.h src/generated/sdl3_shaders.h src/generated/sdl3_card_paths.h src/generated/waifu_assets.h src/generated/deck_pools.h src/generated/title_asset.h src/generated/sound_assets.h src/game/card_ids.h src/game/game_api.h src/game/ai.h src/game/deck.h src/game/palette.h src/game/sounds.h src/game/assets.h
	$(CC) $(CFLAGS) -DWAIFU_FM_NO_HEADLESS_MAIN -DWAIFU_PLATFORM_HW3D=1 -DWAIFU_BG_CACHE_DISABLE -DWAIFU_BATTLE_BASE_CACHE_DISABLE $(INCLUDES) -Isrc/platform/sdl3 $(SDL3_CFLAGS) $(SDL3_IMG_CFLAGS) $(SDL3_FT_CFLAGS) $(SDL3_SRCS) $(SDL3_LIBS) $(SDL3_IMG_LIBS) $(SDL3_FT_LIBS) -lm -lz -o $@

# Regenerates the PC card/title/ending source-path table (src/generated is
# checked in, so a plain `make sdl3` does not need this).
sdl3-card-paths:
	python3 tools/gen_sdl3_hires_paths.py

# Regenerates the embedded SPIR-V header from src/platform/sdl3/shaders/
# (needs glslc; the generated header is checked in like the other
# src/generated outputs, so plain `make sdl3` does not need shader tools).
sdl3-shaders:
	python3 tools/gen_sdl3_shaders.py

run: $(TARGET)
	./$(TARGET) --frames 900 --commands scripts/battle_mode_demo.txt --out headless_frames --dump-every 10

record-demo: $(TARGET)
	./$(TARGET) --frames 3600 --commands scripts/battle_mode_demo.txt --no-png --record-mkv demo_zmbv.mkv

showcase: $(TARGET)
	./$(TARGET) --showcase --out showcase_frames

regression-story: headless-cdrom-assets-2mb
	./scripts/story_save_duels_regression.sh

regression-card-check: headless-cdrom-assets-2mb
	./waifu_fm_headless_cdrom_2mb --regression-card-check-cache --frames 1 --no-png

regression-result-music: headless-cdrom-assets-2mb
	./waifu_fm_headless_cdrom_2mb --regression-result-music --frames 1 --no-png

clean:
	rm -f $(TARGET) $(SDL12_TARGET) $(SDL3_TARGET) $(DIST_NAME).zip
	rm -rf headless_frames showcase_frames out_frames *.mkv *.mp4

# Self-contained source distribution: everything needed to build from source
# (headless, SDL 1.2, and the PC-FX port, the latter also needing an external
# V810/liberis toolchain). Generated headers/bins under src/generated and
# assets/generated are included so a build needs no Python/PIL; run
# `make assets` to regenerate them. Build artifacts, the local toolchain, ROMs,
# emulators, captures and saves are excluded.
DIST_NAME ?= waifu_card_game_src
dist:
	rm -f $(DIST_NAME).zip
	zip -r -q $(DIST_NAME).zip \
	  src tools assets Music sounds scripts third_party docs \
	  Makefile Makefile.pcfx README.md AGENTS.md INSTRUCTIONS.txt \
	  -x '*/build/*' 'build/*' '*.o' '*.a' \
	  -x 'third_party/*/examples/*' \
	  -x '*.zip' '*.mkv' '*.mp4' \
	  -x '*/.git/*' '*/__pycache__/*' '*/headless_frames/*' '*/showcase_frames/*' '*/out_frames/*'
	@echo "wrote $(DIST_NAME).zip ($$(du -h $(DIST_NAME).zip | cut -f1))"

.PHONY: all assets run record-demo showcase sdl12 sdl3 sdl3-shaders headless-cdrom-assets headless-cdrom-assets-2mb headless-cdrom-assets-large headless-cart-assets regression-story regression-card-check regression-result-music dist clean
