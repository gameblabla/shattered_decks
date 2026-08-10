#include "fmt_sprite.h"
#include "io.h"

#define SPRRAM ((volatile uint16_t *)0xC00000u)
#define SPR_ADDR 0x450
#define SPR_DATA 0x452

static void reg(uint8_t r,uint8_t v){outb(r,SPR_ADDR);outb(v,SPR_DATA);}

void fmt_sprite_init(void)
{
    fmt_sprite_disable();
    for(uint16_t i=0;i<1024;i++) fmt_sprite_hide(i);
    reg(2,0);reg(3,0);reg(4,0);reg(5,0);reg(6,0);
}

void fmt_sprite_load_15bpp(uint16_t p,const void *tiles,uint16_t count)
{
    volatile uint16_t *d=SPRRAM+(uint32_t)p*64u;
    const uint16_t *s=(const uint16_t *)tiles;
    uint32_t n=(uint32_t)count*256u;
    while(n--) *d++=*s++;
}

void fmt_sprite_set(uint16_t i,uint16_t x,uint16_t y,uint16_t p,
                    uint8_t t,uint8_t sx,uint8_t sy)
{
    if(i>1023) return;
    volatile uint16_t *d=SPRRAM+(uint32_t)i*4u;
    d[0]=x&511; d[1]=y&511;
    d[2]=(p&0x3ff)|((uint16_t)(t&7)<<12)|(sx?0x400:0)|(sy?0x800:0);
    d[3]=0;
}
void fmt_sprite_hide(uint16_t i){if(i<1024) SPRRAM[(uint32_t)i*4u+3]=0x2000;}
void fmt_sprite_enable(uint16_t first){reg(0,(uint8_t)first);reg(1,0x80|((first>>8)&3));}
void fmt_sprite_disable(void){reg(1,0);}
int fmt_sprite_busy(void){return (inb(0x44C)&2)!=0;}
void fmt_sprite_freeze(void)
{
    while(fmt_sprite_busy()){}
    uint8_t display=(uint8_t)(1u-(inb(0x44C)&1u));
    fmt_sprite_disable();
    reg(6,display?0x80:0);
}
