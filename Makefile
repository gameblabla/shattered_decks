CC ?= gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -DPLATFORM=5 -DBY16=1 -DHARDWARE_DIV=1
INCLUDES = -Isrc/engine -Isrc/generated -Isrc/record -Isrc/game
HEADLESS_SRCS = src/main.c src/game/ai.c src/game/deck.c src/game/palette.c src/game/sounds.c src/game/assets.c src/engine/renderer3d.c src/engine/common.c src/engine/bmp_writer.c src/platform/host_storage.c src/platform/host_assets.c src/platform/host_video.c src/record/zmbv_mkv.c
SDL12_SRCS = src/platform/sdl12_main.c src/main.c src/game/ai.c src/game/deck.c src/game/palette.c src/game/sounds.c src/game/assets.c src/engine/renderer3d.c src/engine/common.c src/engine/bmp_writer.c src/platform/host_storage.c src/platform/host_assets.c src/platform/host_video.c
TARGET = waifu_fm_headless
SDL12_TARGET = waifu_fm_sdl12
SDL_CONFIG ?= sdl-config
SDL_CFLAGS ?= $(shell $(SDL_CONFIG) --cflags 2>/dev/null)
SDL_LIBS ?= $(shell $(SDL_CONFIG) --libs 2>/dev/null)

all: $(TARGET)

assets:
	python3 tools/gen_assets.py
	python3 tools/gen_title_asset.py
	python3 tools/gen_sound_assets.py
	python3 tools/gen_pcfx_sfx_adpcm.py

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
	rm -f $(TARGET) $(SDL12_TARGET)
	rm -rf headless_frames showcase_frames out_frames *.mkv *.mp4

.PHONY: all assets run record-demo showcase sdl12 headless-cdrom-assets headless-cdrom-assets-2mb headless-cdrom-assets-large headless-cart-assets regression-story regression-card-check regression-result-music clean
