/* Cold-boot flight recorder; see include/appotech/trace.h.
 * PIO-only copy of the SPI sequence in boot/rom-rescue.c, which was verified on
 * hardware against the BootROM's own driver (BootROM 0x104ac4..0x104d20). It
 * uses the SPI state the BootROM left after loading this image (or the SDK
 * setup if that fails), needs no
 * stack beyond one 256-byte page, no .data/.bss, and never touches any flash
 * address outside APX_TRACE_SECTOR. */
#include <appotech/trace.h>
#if defined(APX_SPI_TRACE) && APX_SPI_TRACE
#include <appotech/soc.h>
#ifndef APX_BUILD_ID
#define APX_BUILD_ID "unknown"
#endif
#define MAGIC 0x54524331u
#define SECTOR_SIZE 4096u
#define PAGE_SIZE 256u
#define CRUMB_COUNT 256u
/* .noinit survives apx_runtime_init, which clears .bss. */
static uint32_t state __attribute__((section(".noinit")));
static uint32_t base __attribute__((section(".noinit"))); /* sector in use */
static uint32_t mode __attribute__((section(".noinit"))); /* bit 0: log to flash, bit 1: echo on the UART, bit 2: record in an SRAM ring */
/* A crash that resets the chip loses RAM-only state except on-chip SRAM, which the vendor bootstrap
 * and the ROM leave alone at 0x2800 (the rescue payload uses 0x2000-0x3c00 the same way). The SPI
 * boot that follows a crash imports the ring into its flash log (apx_trace_import_ram). */
#ifdef APX_TRACE_RING_PTR /* host model tests */
extern uint32_t trace_test_ring[];
#define RING APX_TRACE_RING_PTR
#else
#define RING ((volatile uint32_t *)0x2800u)
#endif
#define RING_MAGIC 0x474e5252u
#define RING_CAPACITY 200u
#define RING_HEARTBEAT (2u+2u*RING_CAPACITY) /* words: progress counter, raw timer at that point */

static const uint16_t sprs[] = {
    0x0018,0x880c,0x8848,0x884c,0x8850,0x8858,0x885c,0x8860,0x8864,0x8868,0x8340,0x8100,0x8040,
    0x88e0,0x9800,0x9804,0x9808,0x9840,0x9870,0x9080,0x908c,0x9000,0x924c,0xa004,0x4800,0xd804,
    0xd80c,0xd800,0x8840,0x8844,0x984c,0x9850,
};

static void upd(uint32_t r,uint32_t mask,uint32_t value){apx_spr_write(r,(apx_spr_read(r)&~mask)|(value&mask));}
static void feed(void){if(apx_spr_read(0x880c))apx_spr_write(0x880c,0x5001d);}
static void cs(unsigned high){upd(0x9000,0x4000,high?0x4000:0);}
static int xfer(unsigned value,unsigned receive,uint8_t *out){
    upd(0x9000,0x80,receive?0x80:0);apx_spr_write(0x9250,value);apx_spr_write(0x9244,1);
    for(unsigned n=0;n<200000;++n){
        if(apx_spr_read(0x9248)&1){if(out)*out=apx_spr_read(0x9250)&255;return 0;}
        if(!(n&1023))feed();
    }
    return 2;
}
static int status(uint8_t *out){cs(0);int e=xfer(5,0,0);if(!e)e=xfer(255,1,out);cs(1);return e;}
static int ready(void){
    uint8_t value;
    for(unsigned n=0;n<1000000;++n){int e=status(&value);if(e||!(value&1))return e;feed();}
    return 2;
}
static int enable(void){
    uint8_t value;int e=ready();
    if(e)return e;
    cs(0);e=xfer(6,0,0);cs(1);
    if(!e)e=status(&value);
    return e?e:!(value&2);
}
static int address(unsigned command,uint32_t a){
    int e=xfer(command,0,0);
    for(int shift=16;!e&&shift>=0;shift-=8)e=xfer((a>>shift)&255,0,0);
    return e;
}
/* Program bytes that were erased (0xff); both bounds keep it inside the sector. */
static int program(uint32_t offset,const uint8_t *data,unsigned length){
    if(!length||offset>=SECTOR_SIZE||length>PAGE_SIZE-(offset&(PAGE_SIZE-1))||length>SECTOR_SIZE-offset)return 3;
    int e=enable();
    if(e)return e;
    cs(0);e=address(0x02,base+offset);
    for(unsigned i=0;i<length&&!e;++i)e=xfer(data[i],0,0);
    cs(1);
    return e?e:ready();
}
static int erase(void){
    int e=enable();
    if(e)return e;
    cs(0);e=address(0x20,base);cs(1);
    return e?e:ready();
}
static int jedec_ok(void){
    uint8_t id[3]={0,0,0};
    cs(1);cs(0);
    int e=xfer(0x9f,0,0);
    for(unsigned i=0;i<3&&!e;++i)e=xfer(255,1,id+i);
    cs(1);
    return !e&&id[0]==0x5e&&id[1]==0x40&&id[2]==0x15;
}
/* Same controller bring-up as apx_spi_init() and boot/rom-rescue.c command 0. */
static void spi_setup(void){
    upd(0x8850,0xf0000000,0x30000000);upd(0x8844,8,0);upd(0x8840,1u<<20,0);
    upd(0x8344,3u<<20,1u<<20);upd(0x8100,0xd00,0xd00);upd(0x8040,0xd00,0);upd(0x8000,0x400,0x400);
    apx_spr_write(0x9000,0x01115005);apx_spr_write(0x9004,0);apx_spr_write(0x9008,0);apx_spr_write(0x900c,0);
    apx_spr_write(0x9244,0x03ff0000);apx_spr_write(0x9248,0);apx_spr_write(0x924c,5);
    for(unsigned r=0x9250;r<=0x927c;r+=4)apx_spr_write(r,0);
    apx_spr_write(0x9260,0xffff);apx_spr_write(0x9264,0xffffffff);
    apx_spr_write(0x926c,0x0fffffff);apx_spr_write(0x9270,0x0fffffff);
}
static void uart_put(char c){
    apx_spr_write(0x9088,(uint8_t)c);
    for(unsigned n=0;n<20000&&!(apx_spr_read(0x908c)&2);++n);
}
static void uart_hex(uint32_t value,unsigned digits){
    while(digits--)uart_put("0123456789abcdef"[(value>>(digits*4))&15]);
}

void apx_trace_begin(void){apx_trace_begin_mode(APX_TRACE_SECTOR,3);}
void apx_trace_begin_at(uint32_t sector){apx_trace_begin_mode(sector,3);}
void apx_trace_off(void){state=0;}
void apx_trace_set_mode(unsigned how){
    if(state!=MAGIC)return;
    mode=how&7;
    if(mode&4){RING[1]=0;RING[RING_HEARTBEAT]=0;RING[RING_HEARTBEAT+1]=0;RING[0]=RING_MAGIC;} /* a fresh ring for this run */
}
int apx_trace_active(void){return state==MAGIC;}
void apx_trace_begin_mode(uint32_t sector,unsigned how){
    state=0;base=sector;mode=how&7;
    if(mode&4){RING[1]=0;RING[RING_HEARTBEAT]=0;RING[RING_HEARTBEAT+1]=0;RING[0]=RING_MAGIC;}
    if(!(mode&1)){state=MAGIC;return;} /* UART-only: nothing touches the flash bus */
    /* Prefer the state the BootROM left (as the forced-ROM rescue does); if the
     * controller was gated off after the image load, bring it up as the SDK does. */
    if(!jedec_ok()){spi_setup();if(!jedec_ok())return;}
    if(erase())return;
    uint8_t header[32]={'A','P','X','T',1,0,0,0};
    for(unsigned i=0;i<16&&APX_BUILD_ID[i];++i)header[8+i]=(uint8_t)APX_BUILD_ID[i];
    if(program(0,header,sizeof(header)))return;
    state=MAGIC;
    apx_trace_regs(0);
}
void apx_trace(unsigned id,uint32_t arg){
    if(state!=MAGIC||!id||id>=CRUMB_COUNT-1)return;
    const uint8_t slot[4]={(uint8_t)id,(uint8_t)arg,(uint8_t)(arg>>8),(uint8_t)(arg>>16)};
    if(mode&1)(void)program(APX_TRACE_CRUMBS_OFFSET+4*id,slot,sizeof(slot));
    if(mode&2){uart_put('T');uart_hex(id,2);uart_put(':');uart_hex(arg&0xffffff,6);uart_put('\n');}
    if(mode&4){uint32_t n=RING[1];if(n<RING_CAPACITY){RING[2+2*n]=(uint32_t)id<<24|(arg&0xffffffu);RING[3+2*n]=apx_spr_read(0x9804)&0x3fff;RING[1]=n+1;}}
}
void apx_trace_heartbeat(uint32_t progress){
    if(state!=MAGIC||!(mode&4))return;
    RING[RING_HEARTBEAT]=progress;RING[RING_HEARTBEAT+1]=apx_spr_read(0x9804)&0x3fff;
}
void apx_trace_usb(unsigned code,uint32_t a,uint32_t b){
    if(state!=MAGIC)return;
    if(mode&4) { /* ring: crumb 0x90+code, arg = low 24 bits that identify the event */
        uint32_t arg=code==2?((a>>8)&0x800000u)|((a>>16)&0x7fu)<<16|(a&0xffffu)   /* direction | request | wValue */
                    :code==1?((a&0xffff)<<8)|(b&0xff):(a&0xffffffu);
        uint32_t n=RING[1];
        if(n<RING_CAPACITY){RING[2+2*n]=(0x90u+code)<<24|arg;RING[3+2*n]=apx_spr_read(0x9804)&0x3fff;RING[1]=n+1;}
    }
    if(!(mode&2))return;
    uart_put('U');uart_hex(code,1);uart_put(':');uart_hex(a,8);uart_put(' ');uart_hex(b,8);uart_put('\n');
}
void apx_trace_spr(unsigned spr){
    if(state!=MAGIC||!(mode&4))return;
    uint32_t n=RING[1];
    if(n<RING_CAPACITY){RING[2+2*n]=0x9au<<24|(spr&0xffffu);RING[3+2*n]=apx_spr_read(spr);RING[1]=n+1;}
}
void apx_trace_import_ram(void){
    if(state!=MAGIC||RING[0]!=RING_MAGIC)return;
    uint32_t n=RING[1];RING[0]=0;
    if(n>RING_CAPACITY)n=RING_CAPACITY;
    /* slot 0xC0+i holds crumb i as (id << 16 | low 16 bits of its argument) */
    /* only 32 slots exist: keep the newest 32 events, which are the ones next to a hang */
    uint32_t first=n>32?n-32:0;
    for(uint32_t i=first;i<n;++i){uint32_t v=RING[2+2*i];apx_trace(0xc0+i-first,(v>>24)<<16|(v&0xffffu));apx_trace(0xa0+i-first,RING[3+2*i]);}
    apx_trace(0xe0,RING[RING_HEARTBEAT]&0xffffff);apx_trace(0xe1,RING[RING_HEARTBEAT+1]);
}
void apx_trace_regs(unsigned snapshot){
    if(state!=MAGIC||!(mode&1)||snapshot>3)return;
    uint32_t page[PAGE_SIZE/4];
    for(unsigned i=0;i<PAGE_SIZE/4;++i)page[i]=0xffffffffu;
    for(unsigned i=0;i<sizeof(sprs)/sizeof(sprs[0]);++i){page[2*i]=sprs[i];page[2*i+1]=apx_spr_read(sprs[i]);}
    uint8_t bytes[PAGE_SIZE];
    for(unsigned i=0;i<PAGE_SIZE;++i)bytes[i]=(uint8_t)(page[i/4]>>((i&3)*8));
    (void)program(APX_TRACE_REGS_OFFSET*(snapshot+1),bytes,sizeof(bytes));
}
#endif
