#include <appotech/display.h>
#include <appotech/soc.h>
#include <appotech/gpio.h>
#include "board.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Evidence: research/display.md. These are custom SPR numbers, not MMIO. */
#define LCD_CONTROL 0xa800u
#define LCD_STATUS 0xa804u
#define LCD_PIO 0xa858u
struct panel_step { uint16_t index,value,delay_ms; };
/* Condensed, byte-for-byte equivalent register values from flash.bin
 * 0x96284..0x96703. 0xfffe means command only. */
static const struct panel_step panel_init[] = {
    {0x01,0x011c,0},{0x02,0x0000,0},{0x03,0x1030,0},
    {0x08,0x0202,0},{0x0c,0x0000,0},{0x0f,0x0d01,0},
    {0x20,0x0000,0},{0x21,0x0000,50},
    {0x10,0x0a00,0},{0x11,0x1038,50},{0x12,0x1121,0},
    {0x13,0x0066,0},{0x14,0x5f45,0},
    {0x30,0x0000,0},{0x31,0x00db,0},{0x32,0x0000,0},
    {0x33,0x0000,0},{0x34,0x00db,0},{0x35,0x0000,0},
    {0x36,0x00af,0},{0x37,0x0000,0},{0x38,0x00db,0},{0x39,0x0000,0},
    {0x50,0x0400,0},{0x51,0x060b,0},{0x52,0x0c0a,0},
    {0x53,0x0105,0},{0x54,0x0a0c,0},{0x55,0x0b06,0},
    {0x56,0x0004,0},{0x57,0x0501,0},{0x58,0x0e00,0},{0x59,0x000e,50},
    {0x07,0x1017,10},{0xfffe,0x0022,100}
};
static enum apx_display_state state;
static unsigned init_phase,init_index,window_index;
static uint32_t deadline,pixel_count,timeout_count;
static const uint16_t *source;
static unsigned rect_w,rect_h,row,column,stride;
static uint16_t fill_color;
static int filling;
static enum apx_display_rotation rotation=APX_LCD_DEFAULT_ROTATION;
static uint16_t window_values[7];
static unsigned dma,dma_active;
static uint32_t dma_count,transfer_start,last_transfer_ticks;
static const uint8_t *luma,*chroma;
static unsigned dma_pitch,dma_rows,conversion_row;
/* Largest frame is portrait: 192-byte pitch * 220 rows * 3/2.
 * The linker/manifest reserve a separate 64 KiB area in display builds. */
static uint8_t dma_frame[192u*220u*3u/2u]
    __attribute__((section(".display_dma"),aligned(32)));
#include "display_rgb_table.inc"

static void update(uint32_t spr,uint32_t mask,uint32_t bits) {
    apx_spr_write(spr,(apx_spr_read(spr)&~mask)|bits);
}
static void gpio_write(unsigned group,unsigned mask,int high) {
    apx_gpio_write(group,mask,high?mask:0);
}
static void gpio_output(unsigned group,unsigned mask) {
    apx_gpio_configure(group,mask,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,
                       apx_spr_read(0x8000u+4u*group)&mask);
}
static void bus_pin_group(unsigned group,unsigned mask) {
    update(0x8380u+4u*group,mask,mask);
    update(0x8040u+4u*group,mask,0);
    update(0x8100u+4u*group,mask,mask);
}
static void settle(void) {
    /* Match stock command helper's 200-iteration settling loop.
     * Timing is conservative and depends on the inherited CPU clock. */
    for(unsigned i=0;i<200;++i)apx_cpu_relax();
}
void apx_display_backlight(int enabled) {
    if(state!=APX_DISPLAY_OFF)
        gpio_write(APX_LCD_BACKLIGHT_GROUP,APX_LCD_BACKLIGHT_MASK,enabled!=0);
}
void apx_display_shutdown(void) {
    if(state==APX_DISPLAY_OFF)return;
    gpio_write(APX_LCD_BACKLIGHT_GROUP,APX_LCD_BACKLIGHT_MASK,0);
    apx_spr_write(LCD_CONTROL,0);
    apx_spr_write(0xc800,0);
    apx_spr_write(0xc818,0);
    apx_spr_write(0xc828,0);apx_spr_write(0xc830,0);
    update(0x4800,(1u<<4)|(1u<<21),0);
    update(LCD_STATUS,0x001f0000u,0);
    apx_spr_write(0xa85c,0);apx_spr_write(0xa86c,0);
    source=NULL;dma_active=0;state=APX_DISPLAY_OFF;
}
static int byte_out(unsigned value) {
    uint32_t until=apx_ticks()+APX_LCD_PIO_TIMEOUT_TICKS;
    apx_spr_write(LCD_PIO,value&255u);
    while(!(apx_spr_read(LCD_STATUS)&0x80000000u)) {
        if(apx_deadline_expired(until)) {
            ++timeout_count;apx_display_shutdown();state=APX_DISPLAY_ERROR;
            return -1;
        }
        apx_maintenance();
    }
    return 0;
}
static int word_out(unsigned value,int data) {
    gpio_write(APX_LCD_RS_GROUP,APX_LCD_RS_MASK,data);
    settle();
    if(byte_out(value>>8)<0)return -1;
    settle();
    return byte_out(value);
}
static int register_out(unsigned index,unsigned value) {
    if(word_out(index,0)<0)return -1;
    return word_out(value,1);
}
static void wait_ms(unsigned ms) { deadline=apx_ticks()+(ms*32768u+999u)/1000u; }
void apx_display_init(void) {
    /* Stop inherited display DMA before configuring pins or touching GRAM.
     * Keep CPU display interrupts masked: this driver polls exclusively. */
    apx_spr_write(LCD_CONTROL,0);
    apx_spr_write(0xc800,0);
    apx_spr_write(0xc818,0);
    apx_spr_write(0xc828,0);apx_spr_write(0xc830,0);
    update(0x4800,(1u<<4)|(1u<<21),0);
    update(0x8840,1u<<25,0); /* active-low LCD clock gate */
    update(0x8840,1u<<7,0); /* compositor clock, vendor 0x02029e18 */
    state=APX_DISPLAY_INITIALIZING;source=NULL;
    init_phase=init_index=0;pixel_count=timeout_count=dma_count=last_transfer_ticks=0;
    dma=dma_active=0;
    gpio_output(APX_LCD_BACKLIGHT_GROUP,APX_LCD_BACKLIGHT_MASK);
    gpio_write(APX_LCD_BACKLIGHT_GROUP,APX_LCD_BACKLIGHT_MASK,0);
    gpio_output(APX_LCD_RESET_GROUP,APX_LCD_RESET_MASK);
    gpio_write(APX_LCD_RESET_GROUP,APX_LCD_RESET_MASK,1);
    wait_ms(50);
}
static void bus_init(void) {
    /* 0x0202c4dc: pinmux selector 16 = LCD (13). Preserve unrelated pins. */
    update(0x8348,0x3c0,13u<<6);
    bus_pin_group(2,0x1fe0);bus_pin_group(4,0x16);
    update(0x8380+4*4,8,8);update(0x8040+4*4,8,8);update(0x8100+4*4,8,8);
    apx_spr_write(LCD_STATUS,0x00809f00); /* stock status clear/config; IRQ sources off */
    apx_spr_write(0xa808,6);
    apx_spr_write(0xa80c,0x44);
    apx_spr_write(0xa810,0x090000ff);
    apx_spr_write(0x887c,(APX_LCD_CLOCK_DIVIDER-1u)<<2);
    for(unsigned spr=0xa814;spr<=0xa838;spr+=4)apx_spr_write(spr,0);
    apx_spr_write(0xa840,APX_DISPLAY_NATIVE_WIDTH);apx_spr_write(0xa844,APX_DISPLAY_NATIVE_HEIGHT);
    apx_spr_write(0xa848,APX_DISPLAY_NATIVE_WIDTH-1);apx_spr_write(0xa84c,APX_DISPLAY_NATIVE_HEIGHT-1);
    apx_spr_write(0xa854,0);
    for(unsigned spr=0xa85c;spr<=0xa86c;spr+=4)apx_spr_write(spr,0);
    /* Own the full compositor path: no inherited frame, IRQ, overlay or LUT.
     * Identity scaler coefficients from stock 0x0205dc4c (40 words). */
    static const uint32_t coefficients[40]={
        0x40000000,0,0x40000000,0,0x40000000,0,0x40000000,0,
        0x40000000,0,0x40000000,0,0x40000000,0,0x40000000,0,
        0x4000,0xff083efb,0xfc123af8,0xfa1e31f7,0xf82729f8,
        0xf7311efa,0xf83a12fc,0xfb3e08ff,
        0x40000000,0,0x40000000,0,0x40000000,0,0x40000000,0,
        0x40000000,0,0x40000000,0,0x40000000,0,0x40000000,0};
    apx_spr_write(0xc804,15);apx_spr_write(0xc808,0);apx_spr_write(0xc80c,0);
    apx_spr_write(0xc814,0);apx_spr_write(0xc81c,0x707);
    apx_spr_write(0xc824,0);apx_spr_write(0xc838,0x4000);
    for(unsigned spr=0xc83c;spr<=0xc878;spr+=4)apx_spr_write(spr,0);
    for(unsigned i=0;i<40;++i)apx_spr_write(0xc880+4*i,coefficients[i]);
    apx_spr_write(LCD_CONTROL,0x11); /* MCU bus, enabled; no DMA frame trigger */
}
enum apx_display_state apx_display_status(void) { return state; }
uint32_t apx_display_pixels_sent(void) { return pixel_count; }
uint32_t apx_display_timeouts(void) { return timeout_count; }
uint32_t apx_display_dma_transfers(void) { return dma_count; }
uint32_t apx_display_last_transfer_ticks(void) { return last_transfer_ticks; }
int apx_display_set_rotation(enum apx_display_rotation value) {
    if((unsigned)value>APX_DISPLAY_ROTATE_270 || state==APX_DISPLAY_BUSY || state==APX_DISPLAY_INITIALIZING)return -1;
    rotation=value;return 0;
}
enum apx_display_rotation apx_display_get_rotation(void) { return rotation; }
unsigned apx_display_width(void) { return rotation&1?APX_DISPLAY_NATIVE_HEIGHT:APX_DISPLAY_NATIVE_WIDTH; }
unsigned apx_display_height(void) { return rotation&1?APX_DISPLAY_NATIVE_WIDTH:APX_DISPLAY_NATIVE_HEIGHT; }
static int queue(unsigned x,unsigned y,unsigned w,unsigned h) {
    unsigned screen_w=apx_display_width(),screen_h=apx_display_height();
    if(state!=APX_DISPLAY_READY || !w || !h || x>=screen_w || y>=screen_h ||
       w>screen_w-x || h>screen_h-y)return -1;
    rect_w=w;rect_h=h;row=column=window_index=0;
    unsigned gx,gy;
    switch(rotation) {
    case APX_DISPLAY_ROTATE_0:
        gx=x;gy=y;window_values[0]=0x1030;
        window_values[1]=x+w-1;window_values[2]=x;
        window_values[3]=y+h-1;window_values[4]=y;break;
    case APX_DISPLAY_ROTATE_90:
        gx=y;gy=APX_DISPLAY_NATIVE_HEIGHT-1-x;window_values[0]=0x1018;
        window_values[1]=y+h-1;window_values[2]=y;
        window_values[3]=gy;window_values[4]=gy-w+1;break;
    case APX_DISPLAY_ROTATE_180:
        gx=APX_DISPLAY_NATIVE_WIDTH-1-x;gy=APX_DISPLAY_NATIVE_HEIGHT-1-y;window_values[0]=0x1000;
        window_values[1]=gx;window_values[2]=gx-w+1;
        window_values[3]=gy;window_values[4]=gy-h+1;break;
    default:
        gx=APX_DISPLAY_NATIVE_WIDTH-1-y;gy=x;window_values[0]=0x1028;
        window_values[1]=gx;window_values[2]=gx-h+1;
        window_values[3]=x+w-1;window_values[4]=x;break;
    }
    window_values[5]=gx;window_values[6]=gy;
    dma=dma_active=conversion_row=0;transfer_start=apx_ticks();state=APX_DISPLAY_BUSY;return 0;
}
int apx_display_blit(unsigned x,unsigned y,unsigned w,unsigned h,const uint16_t *pixels,unsigned pitch) {
    uint64_t words=h?(uint64_t)(h-1)*pitch+w:0;
    if(!pixels || ((uintptr_t)pixels&1) || pitch<w || words>SIZE_MAX/sizeof(*pixels) ||
       words*sizeof(*pixels)>UINTPTR_MAX-(uintptr_t)pixels)return -1;
    if(queue(x,y,w,h)<0)return -1;
    source=pixels;stride=pitch;filling=0;return 0;
}
int apx_display_blit_dma(unsigned x,unsigned y,unsigned w,unsigned h,const uint16_t *pixels) {
    uintptr_t address=(uintptr_t)pixels;
    uint64_t bytes=(uint64_t)w*h*2;
    if(address&1 || !bytes || (w|h)&1 || address<APX_RAM_BASE || address>=APX_RAM_END ||
       bytes>APX_RAM_END-address)return -1;
    if(address<0x02220000u && address+bytes>0x02210000u)return -1;
    if(queue(x,y,w,h)<0)return -1;
    source=pixels;stride=w;filling=0;dma=1;return 0;
}
static int ddr_range(const void *p,uint64_t bytes) {
    uintptr_t a=(uintptr_t)p;
    return !(a&31) && a>=APX_RAM_BASE && a<APX_RAM_END && bytes<=APX_RAM_END-a;
}
int apx_display_blit_nv21(unsigned x,unsigned y,unsigned w,unsigned h,
                         const uint8_t *y_plane,const uint8_t *vu_plane,unsigned pitch) {
    if(!w || !h || (w|h)&1 || pitch<w || pitch&31 ||
       !ddr_range(y_plane,(uint64_t)pitch*h) ||
       !ddr_range(vu_plane,(uint64_t)pitch*(h/2)))return -1;
    if(queue(x,y,w,h)<0)return -1;
    luma=y_plane;chroma=vu_plane;dma_pitch=pitch;dma_rows=h;dma=2;return 0;
}
static uint8_t clamp_chroma(int value) {
    return value<0?0:value>255?255:value;
}
static void convert_strip(void) {
    dma_pitch=(rect_w+31)&~31u;dma_rows=rect_h;
    const uint16_t *first=source+conversion_row*stride,*second=first+stride;
    uint8_t *y_out=dma_frame+conversion_row*dma_pitch;
    uint8_t *vu_out=dma_frame+rect_h*dma_pitch+(conversion_row/2)*dma_pitch;
    memset(y_out,0,dma_pitch*2);memset(vu_out,128,dma_pitch);
    for(unsigned x=0;x<rect_w;x+=2) {
        int u=0,v=0;
        for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx) {
            unsigned pixel=(dy?second:first)[x+dx];
            const struct rgb_component *r=&rgb_red[pixel>>11];
            const struct rgb_component *g=&rgb_green[(pixel>>5)&63];
            const struct rgb_component *b=&rgb_blue[pixel&31];
            y_out[dy*dma_pitch+x+dx]=(r->y+g->y+b->y)>>8;
            u+=r->u+g->u+b->u;v+=r->v+g->v+b->v;
        }
        /* Full-range BT.601; this board consumes V,U, not U,V. */
        vu_out[x]=clamp_chroma(128+(v>>10));
        vu_out[x+1]=clamp_chroma(128+(u>>10));
    }
    conversion_row+=2;luma=dma_frame;chroma=dma_frame+dma_pitch*rect_h;
}
static void dma_start(void) {
    unsigned geometry=((dma_rows-1)<<16)|(rect_w-1);
    apx_spr_write(LCD_CONTROL,0);apx_spr_write(0xc800,0);
    apx_spr_write(LCD_STATUS,0x00809f00);apx_spr_write(0xc80c,0);
    apx_spr_write(0xc810,geometry);apx_spr_write(0xc820,geometry);
    apx_spr_write(0xc828,(uintptr_t)luma);apx_spr_write(0xc830,(uintptr_t)chroma);
    apx_spr_write(0xc82c,dma_pitch/4);apx_spr_write(0xc834,dma_pitch/4);
    apx_spr_write(0xc920,65536);apx_spr_write(0xc924,65536);
    apx_spr_write(0xc928,geometry);apx_spr_write(0xc92c,geometry);
    /* RGB565 output, YUV conversion, identity scaler, main plane only. */
    apx_spr_write(0xc818,0x52081);
    apx_spr_write(0xa840,rect_w);apx_spr_write(0xa844,dma_rows);
    apx_spr_write(0xa848,rect_w-1);apx_spr_write(0xa84c,dma_rows-1);
    apx_spr_write(0xa854,rect_w*dma_rows*2-1);
    /* LCD packing words from panel config; these are not RAM pointers. */
    apx_spr_write(0xa85c,0x02100008);apx_spr_write(0xa868,0);apx_spr_write(0xa86c,0);
    apx_spr_write(LCD_CONTROL,0x11);apx_spr_write(0xc800,3);
    apx_spr_write(LCD_CONTROL,0x13);
    deadline=apx_ticks()+32768;dma_active=1;++dma_count;
}
int apx_display_fill(unsigned x,unsigned y,unsigned w,unsigned h,uint16_t color) {
    if(queue(x,y,w,h)<0)return -1;
    fill_color=color;filling=1;source=NULL;return 0;
}
void apx_display_poll(void) {
    if(state==APX_DISPLAY_INITIALIZING) {
        if(!apx_deadline_expired(deadline))return;
        if(init_phase==0) { gpio_write(APX_LCD_RESET_GROUP,APX_LCD_RESET_MASK,0);wait_ms(50);++init_phase; }
        else if(init_phase==1) { gpio_write(APX_LCD_RESET_GROUP,APX_LCD_RESET_MASK,1);wait_ms(50);++init_phase; }
        else if(init_phase==2) { bus_init();wait_ms(20);++init_phase; }
        else if(init_index<sizeof(panel_init)/sizeof(panel_init[0])) {
            const struct panel_step *s=&panel_init[init_index++];
            if((s->index==0xfffe?word_out(s->value,0):register_out(s->index,s->value))<0)return;
            wait_ms(s->delay_ms);
        } else { state=APX_DISPLAY_READY;apx_display_backlight(1); }
        return;
    }
    if(state!=APX_DISPLAY_BUSY)return;
    if(window_index<8) {
        static const uint8_t indices[]={0x03,0x36,0x37,0x38,0x39,0x20,0x21};
        if((window_index==7?word_out(0x22,0):register_out(indices[window_index],window_values[window_index]))<0)return;
        if(++window_index==8) { gpio_write(APX_LCD_RS_GROUP,APX_LCD_RS_MASK,1);settle(); }
        return;
    }
    if(dma) {
        if(!dma_active) {
            if(dma==1 && conversion_row<rect_h) { convert_strip();return; }
            if(apx_dma_clean(luma,dma_pitch*dma_rows)<0 ||
               apx_dma_clean(chroma,dma_pitch*(dma_rows/2))<0) {
                apx_display_shutdown();state=APX_DISPLAY_ERROR;return;
            }
            dma_start();
        } else {
            uint32_t status=apx_spr_read(LCD_STATUS);
            if((status&0x80000008u)==0x80000008u && (apx_spr_read(0xc80c)&1)) {
                apx_spr_write(0xc800,0);apx_spr_write(0xc818,0);
                apx_spr_write(0xc828,0);apx_spr_write(0xc830,0);
                apx_spr_write(0xc80c,0);
                apx_spr_write(LCD_STATUS,0x00800800);
                pixel_count+=rect_w*dma_rows;row+=dma_rows;dma_active=0;
                if(row==rect_h) {
                    source=NULL;state=APX_DISPLAY_READY;
                    last_transfer_ticks=apx_ticks()-transfer_start;
                }
            } else if(apx_deadline_expired(deadline)) {
                ++timeout_count;apx_display_shutdown();state=APX_DISPLAY_ERROR;
            }
        }
        return;
    }
    /* Bounded bursts keep EP0, bulk debugging, ticks and recovery responsive. */
    for(unsigned budget=0;budget<16;++budget) {
        uint16_t pixel=filling?fill_color:source[row*stride+column];
        if(byte_out(pixel>>8)<0 || byte_out(pixel)<0)return;
        ++pixel_count;
        if(++column==rect_w) {
            column=0;
            if(++row==rect_h) { source=NULL;state=APX_DISPLAY_READY;last_transfer_ticks=apx_ticks()-transfer_start;return; }
        }
    }
}
