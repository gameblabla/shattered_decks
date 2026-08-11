#include "fmt_layers.h"
#include "common.h"
#include "io.h"
#include "defs.h"
#include "machine.h"

/* Not yet wired into any build (see STATUS.md) -- kept in step with
 * machine.h's runtime VRAM0 base anyway so it is correct the day it is. */
#define VRAM0 g_fmt_vram0_base
#define VRAM1 (VRAM0+PAGE_BYTES)
#define PAGE_BYTES 0x40000u

static fmt_bg_layer_t g_bg[2];

static int configure(fmt_bg_layer_t a, fmt_bg_layer_t b, uint8_t priority)
{
    if (!a.width || !a.height || !b.width || !b.height ||
        a.stride < a.width * 2u || b.stride < b.width * 2u ||
        (uint32_t)a.stride * a.height > PAGE_BYTES ||
        (uint32_t)b.stride * b.height > PAGE_BYTES ||
        (a.stride & 3u) || (b.stride & 3u)) {
        return -1;
    }

    crtc_set_t c = {
        0x0074,0x0530,0,0,0x0617,0x000C,0x0018,0x0030,
        0x049A,0x00E7,(uint16_t)(0x00E7+a.width*2u),
        0x00E7,(uint16_t)(0x00E7+b.width*2u),
        0x0046,(uint16_t)(0x0046+a.height/2u),
        0x0046,(uint16_t)(0x0046+b.height/2u),
        0,0x00E7,1,(uint16_t)(a.stride/4u),
        0,0x00E7,1,(uint16_t)(b.stride/4u),
        0x0056,0x0007,0x0101,0x8005,0x0001,0x0002,0x0188
    };
    video_set_t v = {0x1F, (uint8_t)(0x18u | (priority & 1u))};

    outb(0, IO_FMR_VRAM_OR_MAINRAM);
    stop_display();
    set_crtc(c);
    set_video(v);
    /* FDA0 independently gates layer output.  Single-page setup leaves only
     * layer 0 enabled, so explicitly enable both halves in two-page mode. */
    outb(0x0F,0xFDA0);
    start_display();
    g_bg[0]=a;
    g_bg[1]=b;
    fmt_bg_clear(0,0);
    fmt_bg_clear(1,0x8000u);
    return 0;
}

int fmt_set_two_backgrounds(fmt_bg_layer_t a, fmt_bg_layer_t b, uint8_t front)
{
    return configure(a,b,front);
}

int fmt_set_background_and_sprites(fmt_bg_layer_t bg)
{
    fmt_bg_layer_t sprite={256,256,512};
    int rc=configure(bg,sprite,1);
    if(!rc) {
        volatile uint16_t *v=(volatile uint16_t *)VRAM1;
        for(uint32_t i=0;i<PAGE_BYTES/2u;i++) v[i]=0x8000u;
    }
    return rc;
}

void fmt_bg_put_pixel(uint8_t layer,uint16_t x,uint16_t y,uint16_t color)
{
    if(layer>1 || x>=g_bg[layer].width || y>=g_bg[layer].height) return;
    volatile uint16_t *v=(volatile uint16_t *)(layer?VRAM1:VRAM0);
    v[((uint32_t)y*g_bg[layer].stride)/2u+x]=color;
}

void fmt_bg_clear(uint8_t layer,uint16_t color)
{
    if(layer>1) return;
    volatile uint16_t *v=(volatile uint16_t *)(layer?VRAM1:VRAM0);
    uint32_t n=((uint32_t)g_bg[layer].stride*g_bg[layer].height)/2u;
    while(n--) *v++=color;
}

const fmt_bg_layer_t *fmt_bg_layer(uint8_t layer){return layer<2?&g_bg[layer]:0;}
