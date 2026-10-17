/* Throw-away hardware probe for the DOOM port (Action_Digital_Kids_Camera_DOOM).
 * Everything is reported through printf -> `apxtool debug logs`. RAM only, writes nothing to SPI or SD.
 *   A  hardware l.mul / l.muli / l.div / l.divu correctness and speed vs libgcc soft routines
 *   B  is 0x02000000..0x0217ffff usable RAM (saved and restored, 64 KiB stride)
 *   C  raw ADC value stream for the button ladder (press key combinations; logged forever)
 *   D  SD read throughput (first file >= 256 KiB in the root directory)
 *   E  stock apx_audio_play() restart gap for several chunk sizes
 *   F  palette->NV21 conversion and apx_display_blit_nv21 frame time */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/buttons.h>
#include <appotech/storage.h>
#include <appotech/audio.h>
#include <appotech/clock.h>
#include <stdio.h>
#include <string.h>

#define TICKS_PER_MS(x) ((x)*32768u/1000u)

static uint32_t ms_since(uint32_t t0) { return (uint32_t)(apx_ticks()-t0)*1000u/32768u; }
static uint32_t us_since(uint32_t t0) { return (uint32_t)(apx_ticks()-t0)*1000000u/32768u; }

/* ---------------------------------------------------------------- A */
static inline int32_t hw_mul(int32_t a,int32_t b) { int32_t r; __asm__ volatile("l.mul %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
static inline int32_t hw_muli(int32_t a) { int32_t r; __asm__ volatile("l.muli %0,%1,1234":"=r"(r):"r"(a)); return r; }
static inline int32_t hw_div(int32_t a,int32_t b) { int32_t r; __asm__ volatile("l.div %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
static inline uint32_t hw_divu(uint32_t a,uint32_t b) { uint32_t r; __asm__ volatile("l.divu %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
static volatile int32_t sink;
__attribute__((unused)) static void test_muldiv(void) {
    static const int32_t va[]={12345,-12345,0x7fff,-1,0x10000,0x12345678,-0x1234567,7};
    static const int32_t vb[]={6789,6789,0x7fff,-1,0x10000,3,-5,0x7fffffff};
    unsigned bad=0;
    printf("A: testing l.mul (illegal instruction would reset the camera here)\n");
    for(unsigned i=0;i<8;++i) {
        volatile int32_t a=va[i],b=vb[i];
        int32_t soft=a*b,hard=hw_mul(a,b);
        if(soft!=hard) { ++bad;printf("A: l.mul %ld*%ld soft=%ld hard=%ld MISMATCH\n",(long)a,(long)b,(long)soft,(long)hard); }
    }
    printf("A: l.mul done, mismatches=%u\n",bad);
    { volatile int32_t a=-4321; int32_t soft=a*1234,hard=hw_muli(a);
      printf("A: l.muli %s (%ld vs %ld)\n",soft==hard?"ok":"MISMATCH",(long)soft,(long)hard);if(soft!=hard)++bad; }
    printf("A: testing l.div/l.divu\n");
    for(unsigned i=0;i<8;++i) {
        volatile int32_t a=va[i],b=vb[i];if(!b)continue;
        int32_t sd=a/b,hd=hw_div(a,b);
        uint32_t su=(uint32_t)a/(uint32_t)b,hu=hw_divu((uint32_t)a,(uint32_t)b);
        if(sd!=hd||su!=hu) { ++bad;printf("A: div %ld/%ld soft=%ld,%lu hard=%ld,%lu MISMATCH\n",(long)a,(long)b,(long)sd,(unsigned long)su,(long)hd,(unsigned long)hu); }
    }
    printf("A: total mismatches=%u\n",bad);
    const unsigned N=100000;
    uint32_t t=apx_ticks();volatile int32_t x=3,y=0x1357;int32_t acc=1;
    for(unsigned i=0;i<N;++i)acc=acc*y+x;
    uint32_t soft_us=us_since(t);sink=acc;
    t=apx_ticks();acc=1;
    for(unsigned i=0;i<N;++i)acc=hw_mul(acc,y)+x;
    uint32_t hard_us=us_since(t);sink=acc;
    printf("A: %u mul-adds: soft %lu us, hard %lu us\n",N,(unsigned long)soft_us,(unsigned long)hard_us);
    t=apx_ticks();acc=0x7fffffff;volatile int32_t d=7;
    for(unsigned i=0;i<N/10;++i)acc=acc/d+i;
    soft_us=us_since(t);sink=acc;
    t=apx_ticks();acc=0x7fffffff;
    for(unsigned i=0;i<N/10;++i)acc=hw_div(acc,d)+i;
    hard_us=us_since(t);sink=acc;
    printf("A: %u divides: soft %lu us, hard %lu us\n",N/10,(unsigned long)soft_us,(unsigned long)hard_us);
    struct apx_clock_info ci;apx_clock_get_info(&ci);
    printf("A: clock cpu_hz_estimate=%lu\n",(unsigned long)ci.cpu_hz_estimate);
}

/* ---------------------------------------------------------------- B */
__attribute__((unused)) static void test_lower_ram(void) {
    printf("B: lower RAM 0x02000000..0x0217ffff, 64 KiB stride, saved/restored\n");
    enum { N=24 };
    uint32_t saved[N];unsigned bad=0,aliased=0;
    volatile uint32_t *slot[N];
    for(unsigned i=0;i<N;++i) {
        /* offset inside each 64 KiB block varies so a repeating alias shows up */
        slot[i]=(volatile uint32_t*)(0x02000000u+i*0x10000u+0x40u*i);
        saved[i]=*slot[i];
    }
    apx_cache_flush_range((const void*)0x02000000u,(const void*)0x02180000u);
    for(unsigned i=0;i<N;++i)*slot[i]=0xA5000000u|(i<<8)|i;
    apx_cache_flush_range((const void*)0x02000000u,(const void*)0x02180000u);
    for(unsigned i=0;i<N;++i) {
        uint32_t v=*slot[i];
        if(v!=(0xA5000000u|(i<<8)|i)) {
            ++bad;
            for(unsigned j=0;j<N;++j)if(j!=i&&v==(0xA5000000u|(j<<8)|j))++aliased;
            printf("B: %08lx read %08lx (expected %08lx)\n",(unsigned long)(uintptr_t)slot[i],(unsigned long)v,
                   (unsigned long)(0xA5000000u|(i<<8)|i));
        }
    }
    for(unsigned i=0;i<N;++i)*slot[i]=saved[i];
    apx_cache_flush_range((const void*)0x02000000u,(const void*)0x02180000u);
    printf("B: bad=%u aliased=%u of %u probes -> lower RAM %s\n",bad,aliased,(unsigned)N,bad?"NOT usable as tested":"looks usable");
}

/* ---------------------------------------------------------------- D */
static void test_sd(void) {
    if(!apx_storage_volume()) { printf("D: no SD volume mounted, skipping\n");return; }
    DIR dir;FILINFO info;char path[64]="";
    if(f_opendir(&dir,"0:/")!=FR_OK) { printf("D: opendir failed\n");return; }
    while(f_readdir(&dir,&info)==FR_OK&&info.fname[0]) {
        if(!(info.fattrib&AM_DIR)&&info.fsize>=256u*1024u) { snprintf(path,sizeof(path),"0:/%s",info.fname);break; }
    }
    f_closedir(&dir);
    if(!path[0]) { printf("D: no file >= 256 KiB in the SD root; put one (e.g. a WAD) there and relaunch\n");return; }
    FILE *f=apx_fopen(path,"rb");
    if(!f) { printf("D: open %s failed\n",path);return; }
    static uint8_t buf[32768] __attribute__((section(".large_buffer"),aligned(32)));
    uint32_t t=apx_ticks(),total=0;size_t got;
    while(total<2u*1024*1024&&(got=fread(buf,1,sizeof(buf),f))>0) { total+=got;apx_maintenance(); }
    uint32_t ms=ms_since(t);fclose(f);
    printf("D: %s read %lu bytes in %lu ms = %lu KB/s\n",path,(unsigned long)total,(unsigned long)ms,
           (unsigned long)(ms?total/ms*1000u/1024u:0));
}

/* ---------------------------------------------------------------- E */
static int16_t tone[2048*8] __attribute__((section(".large_buffer"),aligned(32)));
__attribute__((unused)) static void test_audio(void) {
    int e=apx_audio_init();printf("E: apx_audio_init -> %d\n",e);if(e)return;
    apx_audio_volume(40);
    for(unsigned i=0;i<sizeof(tone)/sizeof(tone[0]);++i) {
        /* 500 Hz square-ish wave at 16 kHz: 32 samples per period */
        tone[i]=((i/16)&1)?6000:-6000;
    }
    static const unsigned sizes[]={512,1024,2048,4096};
    for(unsigned s=0;s<4;++s) {
        unsigned samples=sizes[s],bytes=samples*2;const unsigned chunks=16;
        uint32_t t=apx_ticks();unsigned played=0;
        while(played<chunks) {
            int r=apx_audio_play(tone,bytes,16000);
            if(r) { printf("E: play failed %d\n",r);return; }
            struct apx_audio_info ai;
            do { apx_debug_poll();apx_audio_poll();apx_audio_get_info(&ai); }
            while(ai.state==APX_AUDIO_PLAYING||ai.state==APX_AUDIO_DRAINING);
            if(ai.state==APX_AUDIO_ERROR) { printf("E: audio error %lu\n",(unsigned long)ai.error);return; }
            ++played;
        }
        uint32_t total_us=us_since(t),ideal_us=chunks*samples*1000000u/16000u;
        printf("E: %u-sample chunks: %u chunks took %lu us, ideal %lu us, gap/chunk %ld us\n",samples,chunks,
               (unsigned long)total_us,(unsigned long)ideal_us,(long)((int32_t)(total_us-ideal_us)/(int32_t)chunks));
    }
    apx_audio_stop();
}

__attribute__((unused)) static void test_audio_rate(void) {
    int e=apx_audio_init();printf("E2: init %d\n",e);if(e)return;
    apx_audio_volume(30);
    const unsigned samples=16384;
    for(unsigned i=0;i<samples;++i)tone[i]=((i/16)&1)?5000:-5000;
    uint32_t t0=apx_ticks();
    int r=apx_audio_play(tone,samples*2,16000);printf("E2: play 16384 samples @16k -> %d (setup %lu us)\n",r,(unsigned long)us_since(t0));
    if(r)return;
    uint32_t last_print=0,start=apx_ticks();struct apx_audio_info ai;
    for(;;) {
        apx_debug_poll();apx_audio_poll();apx_audio_get_info(&ai);
        uint32_t now=ms_since(start);
        if(now-last_print>=100||ai.state!=APX_AUDIO_PLAYING) {
            printf("E2: t=%lu ms rem=%lu status=0x%lx state=%lu\n",(unsigned long)now,(unsigned long)apx_spr_read(0x9158),(unsigned long)apx_spr_read(0x9150),(unsigned long)ai.state);
            last_print=now;
        }
        if(ai.state!=APX_AUDIO_PLAYING&&ai.state!=APX_AUDIO_DRAINING)break;
        if(now>3000)break;
    }
    printf("E2: done after %lu ms (ideal 1024 ms)\n",(unsigned long)ms_since(start));
    apx_audio_stop();
}

/* Does the DAC DMA pick up a new source/length written while a block is still playing? A = 4096 samples (256 ms) of a
 * 500 Hz square wave, B = 4096 samples of 2 kHz. The registers for B are written at +100 ms. Watch 0x9158/0x9150. */
static int16_t tone2[4096] __attribute__((section(".large_buffer"),aligned(32)));
__attribute__((unused)) static void test_audio_chain(unsigned variant) {
    for(unsigned i=0;i<4096;++i){tone[i]=((i/16)&1)?5000:-5000;tone2[i]=((i/4)&1)?5000:-5000;}
    apx_dma_clean(tone2,sizeof(tone2));
    printf("E3: variant %u\n",variant);
    int r=apx_audio_play(tone,4096*2,16000);if(r){printf("E3: play %d\n",r);return;}
    uint32_t start=apx_ticks();unsigned wrote=0;uint32_t lastrem=0xffffffffu,laststat=0xffffffffu;
    while(ms_since(start)<900) {
        apx_debug_poll();apx_audio_poll();
        uint32_t ms=ms_since(start);
        if(!wrote&&ms>=100) {
            wrote=1;
            apx_spr_write(0x9148,(uint32_t)(uintptr_t)tone2);
            apx_spr_write(0x9144,4096*2/4-1);
            if(variant>=1)apx_spr_write(0x9158,4096);
            printf("E3: t=%lu wrote next buffer (variant %u)\n",(unsigned long)ms,variant);
        }
        uint32_t rem=apx_spr_read(0x9158),st=apx_spr_read(0x9150);
        if((rem!=lastrem&&(rem<=16||(rem&0x1ff)==0))||st!=laststat) {
            printf("E3: t=%lu rem=%lu status=0x%lx\n",(unsigned long)ms,(unsigned long)rem,(unsigned long)st);
            lastrem=rem;laststat=st;
        }
    }
    apx_audio_stop();
}

/* ---------------------------------------------------------------- F */
#define FW 224u
#define FH 176u
static uint8_t plane_y[FW*FH] __attribute__((section(".large_buffer"),aligned(32)));
static uint8_t plane_vu[FW*FH/2] __attribute__((section(".large_buffer"),aligned(32)));
static uint8_t src8[240*160] __attribute__((section(".large_buffer"),aligned(32)));
static uint8_t lut_y[256];static uint16_t lut_vu[256];
static void make_luts(unsigned phase) {
    for(unsigned i=0;i<256;++i) {
        int r=(i&0xe0)|((i&0xe0)>>3),g=((i&0x1c)<<3)|((i&0x1c)>>0),b=(i&3)*85;
        r=(r+phase)&255;
        int y=(77*r+150*g+29*b)>>8;
        int v=128+((128*r-107*g-21*b)>>8),u=128+((-43*r-85*g+128*b)>>8);
        v=v<0?0:v>255?255:v;u=u<0?0:u>255?255:u;
        lut_y[i]=(uint8_t)y;lut_vu[i]=(uint16_t)(v|(u<<8));
    }
}
static void convert(void) {
    /* 240x160 8-bit -> centre-crop 220 wide into a 224x176 NV21 frame, 8 black rows top and bottom */
    for(unsigned y=0;y<160;++y) {
        const uint8_t *s=src8+y*240+10;uint8_t *d=plane_y+(y+8)*FW;
        for(unsigned x=0;x<220;++x)d[x]=lut_y[s[x]];
        if(!(y&1)) {
            uint16_t *c=(uint16_t*)(plane_vu+((y+8)/2)*FW);
            for(unsigned x=0;x<220;x+=2)*c++=lut_vu[s[x]];
        }
    }
}
__attribute__((unused)) static void test_frame(void) {
    for(unsigned y=0;y<160;++y)for(unsigned x=0;x<240;++x)src8[y*240+x]=(uint8_t)((x/8+y/8*3)^(x>>5));
    memset(plane_y,0,sizeof(plane_y));memset(plane_vu,128,sizeof(plane_vu));
    make_luts(0);
    uint32_t t=apx_ticks();convert();uint32_t conv=us_since(t);
    printf("F: convert 240x160 -> NV21 %lu us\n",(unsigned long)conv);
    while(apx_display_status()!=APX_DISPLAY_READY) { apx_debug_poll();apx_display_poll(); }
    const unsigned frames=60;t=apx_ticks();uint32_t conv_total=0;
    for(unsigned f=0;f<frames;++f) {
        uint32_t t1=apx_ticks();make_luts(f*4);convert();conv_total+=us_since(t1);
        int r;
        while((r=apx_display_blit_nv21(0,0,FW<220u?FW:220u,FH,plane_y,plane_vu,FW))!=0) {
            apx_debug_poll();apx_display_poll();
            if(apx_display_status()==APX_DISPLAY_ERROR) { printf("F: display error\n");return; }
        }
        while(apx_display_status()!=APX_DISPLAY_READY) { apx_debug_poll();apx_display_poll(); }
    }
    uint32_t ms=ms_since(t);
    printf("F: %u frames in %lu ms (%lu us/frame incl. LUT+convert avg %lu us)\n",frames,(unsigned long)ms,
           (unsigned long)(ms*1000u/frames),(unsigned long)(conv_total/frames));
}

/* ---------------------------------------------------------------- C */
int main(void) {
    apx_usb_init();apx_buttons_init();apx_display_init();
    printf("doom-probe: build %s\n",APX_BUILD_ID);
    /* Let the host attach first so no log is lost. */
    uint32_t t=apx_ticks();while(ms_since(t)<1500) { apx_debug_poll();apx_display_poll();apx_buttons_poll();apx_maintenance(); }
    { FRESULT fr=apx_storage_mount();printf("D0: mount -> %d\n",(int)fr); }
    test_sd();
    printf("C: logging ADC/state; press key combinations\n");
    unsigned last_state=0xffff,last_adc=0xffff,stable=0;
    for(;;) {
        apx_debug_poll();apx_buttons_poll();apx_display_poll();apx_maintenance();
        struct apx_buttons_info bi;apx_buttons_get_info(&bi);
        unsigned d=bi.adc>last_adc?bi.adc-last_adc:last_adc-bi.adc;
        if(bi.state!=last_state||d>12) {
            if(++stable>=6) { printf("C: adc=%u state=0x%02x raw=0x%lx\n",bi.adc,(unsigned)bi.state,(unsigned long)bi.raw_state);
                              last_state=bi.state;last_adc=bi.adc;stable=0; }
        } else stable=0;
    }
}
