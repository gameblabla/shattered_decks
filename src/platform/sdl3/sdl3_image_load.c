/* PC-only PNG/WebP -> RGBA8 decoder for the SDL3 frontend (see header). Uses
 * libpng's simplified read API and libwebp; the format is chosen from the file
 * signature so callers need not care about the extension. */

#include "sdl3_image_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <webp/decode.h>

static uint8_t *read_whole_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    long len;
    uint8_t *buf;
    size_t got;
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    len = ftell(fp);
    if (len <= 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { fclose(fp); return NULL; }
    got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (got != (size_t)len) { free(buf); return NULL; }
    *out_len = (size_t)len;
    return buf;
}

static uint8_t *decode_png_rgba(const uint8_t *data, size_t len, int *w, int *h)
{
    png_image img;
    uint8_t *out;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, data, len)) return NULL;
    img.format = PNG_FORMAT_RGBA;
    out = (uint8_t *)malloc(PNG_IMAGE_SIZE(img));
    if (!out) { png_image_free(&img); return NULL; }
    if (!png_image_finish_read(&img, NULL, out, 0, NULL)) {
        free(out);
        png_image_free(&img);
        return NULL;
    }
    *w = (int)img.width;
    *h = (int)img.height;
    return out;
}

static uint8_t *decode_webp_rgba(const uint8_t *data, size_t len, int *w, int *h)
{
    int ww = 0, hh = 0;
    uint8_t *webp;
    uint8_t *out;
    size_t bytes;
    if (!WebPGetInfo(data, len, &ww, &hh) || ww <= 0 || hh <= 0) return NULL;
    webp = WebPDecodeRGBA(data, len, &ww, &hh);
    if (!webp) return NULL;
    /* Copy into a malloc'd buffer so callers can free() uniformly (WebP wants
       WebPFree, but a plain memcpy into malloc keeps one ownership model). */
    bytes = (size_t)ww * (size_t)hh * 4;
    out = (uint8_t *)malloc(bytes);
    if (out) memcpy(out, webp, bytes);
    WebPFree(webp);
    if (!out) return NULL;
    *w = ww;
    *h = hh;
    return out;
}

uint8_t *waifu_sdl3_image_load_rgba(const char *path, int *w, int *h)
{
    size_t len = 0;
    uint8_t *data;
    uint8_t *rgba = NULL;
    if (!path || !*path || !w || !h) return NULL;
    data = read_whole_file(path, &len);
    if (!data) return NULL;
    if (len >= 8 && png_sig_cmp(data, 0, 8) == 0) {
        rgba = decode_png_rgba(data, len, w, h);
    } else if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        rgba = decode_webp_rgba(data, len, w, h);
    }
    free(data);
    return rgba;
}
