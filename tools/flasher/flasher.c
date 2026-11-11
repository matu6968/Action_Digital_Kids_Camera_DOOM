/* SPI NOR flasher / BootROM recovery payload. One position-independent image runs from either pool:
 *   - on-chip SRAM (0x2000), the only memory a forced BootROM has: 512-byte transfers, no cache;
 *   - DDR (0x02180000), alive after a stock-firmware loader entry or an SDK `bootrom` return: 32 KiB transfers.
 * The request block lives wherever the host puts it; the BootROM hands its address to the callback in r3, and
 * flasher.S keeps the stack just below it. PIO only: no DMA, libc, vendor application calls or clock changes.
 *
 * The BootROM only reloads its watchdog (SPR 0x880c) from the USB interrupt handler, so an idle host loses
 * the device after about two seconds. Unless the request sets FLAG_KEEP_WDT every entry disables it first
 * (0x5500 reads back zero, see research/watchdog-power.md). The reload (0x5001d) is only issued while the
 * watchdog still reads non-zero, so it never re-enables a disabled counter. A stock camera in its loader keeps
 * the watchdog so it reboots into the vendor firmware once the host disconnects.
 *
 * Command 0 with address bit 0 set probes the JEDEC ID using the SPI state the BootROM already programmed (same
 * register protocol as BootROM 0x104ac4..0x104d20); without it the full SDK setup is applied first.
 * JEDEC is returned raw. Each flash access reprobes the chip: reads accept capacity codes 0x15/0x16 (2/4 MiB),
 * while erase/program still require exactly 5e4015. No chip geometry is trusted from the host request.
 * Commands 7/8 drive the always-powered PM chip over its serial bus (SPR 0x88e0/0x88ec, same transactions as
 * src/power.c): 8 reads registers 0x40..0x5a, 7 applies the stock start-up sequence so the keep-alive
 * (reg 0x5a = 0x08) is restored without any flash access.
 * Command 6 returns eight diagnostic SPRs (watchdog, PLL1 control/coefficient and the SPI controller registers
 * the BootROM itself reads) without touching the flash bus.
 *
 * DDR requests are cached by the loader's data cache when it is on, while the USB transfers write DDR behind it.
 * The payload therefore drops (after writing back) the lines of the request block on entry and of the response on
 * exit. The SRAM window is uncached and never takes that path.
 *
 * Nothing here may reference its own load address: no data tables, no jump tables, no globals. flasher.ld
 * rejects .rodata/.data/.bss and the Makefile links the image at both bases and requires identical bytes. */
#include <stdint.h>

#define MAGIC 0x52585041u
#define FLAG_KEEP_WDT 1u
#define SRAM_WINDOW_DATA 512u
#define DDR_WINDOW_DATA 0x8000u
#define HEADER_BYTES 28u

#if defined(SILVERGEAR_FULL_RESTORE)&&!defined(SILVERGEAR_BOOT_RESTORE)
#error SILVERGEAR_FULL_RESTORE extends SILVERGEAR_BOOT_RESTORE; build both defines
#endif
#ifdef SILVERGEAR_BOOT_RESTORE
/* Recovery binaries write only a 944016 chip from the SRAM window, only with their own request-flags value, and
 * never touch the PM chip. The incident build allows three sectors; the full build ("SGFR") allows any sector of
 * the 4 MiB chip so a verified stock.bin or a packed SDK image can be written back from a forced BootROM. The two
 * flags values differ on purpose: a host speaking one protocol cannot drive the other binary. */
#ifdef SILVERGEAR_FULL_RESTORE
#define RESTORE_FLAGS 0x53464752u
#define RESTORE_SECTOR_OK(s) ((s)<0x400000u)
#else
#define RESTORE_FLAGS 0x53470000u
#define RESTORE_SECTOR_OK(s) ((s)==0||(s)==0x1000||(s)==0x3ff000)
#endif
#endif

#ifdef FLASHER_HOST /* host model: the harness supplies the registers, the address-space split and the cache */
uint32_t flasher_rd(unsigned r);
void flasher_wr(unsigned r,uint32_t v);
int flasher_is_sram(const volatile void *p);
void flasher_cache_range(const volatile void *start,uint32_t bytes);
#define rd flasher_rd
#define wr flasher_wr
#define IS_SRAM(p) flasher_is_sram(p)
#define cache_range flasher_cache_range
#else
static inline uint32_t rd(unsigned r){uint32_t v;__asm__ volatile("l.mfspr %0,%1,0":"=&r"(v):"r"(r):"memory");return v;}
static inline void wr(unsigned r,uint32_t v){__asm__ volatile("l.mtspr %0,%1,0"::"r"(r),"r"(v):"memory");}
#define IS_SRAM(p) ((uintptr_t)(p)<0x01000000u)
/* Write back, then drop, the data-cache lines of a range (same SPR sequence as apx_cache_flush_range). */
static void cache_range(const volatile void *start,uint32_t bytes){
 uintptr_t a=(uintptr_t)start&~(uintptr_t)15,stop=((uintptr_t)start+bytes+15)&~(uintptr_t)15;
 while(a<stop){
  uintptr_t count=stop-a>4096?4096:stop-a;unsigned lines=(count-1)>>4;
  __asm__ volatile("l.mtspr r0,%0,0x181c\n\tl.mtspr r0,%1,0x180c\n\tl.mtspr r0,%0,0x181c\n\tl.mtspr r0,%1,0x1814"::"r"(lines),"r"(a):"memory");
  a+=count;
 }
}
#endif
/* Only a DDR block can sit in the data cache, and only while the loader has the cache on (SPR 0x1800 bit 0). The
 * SRAM window never reads that register, so a forced BootROM sees exactly the PIO-only behaviour it always had. */
static void sync(const volatile void *start,uint32_t bytes){
 if(IS_SRAM(start)||!(rd(0x1800)&1))return;
 cache_range(start,bytes);
}

static void upd(unsigned r,uint32_t mask,uint32_t value){wr(r,(rd(r)&~mask)|(value&mask));}
static void feed(void){if(rd(0x880c))wr(0x880c,0x5001d);}
static void cs(unsigned high){upd(0x9000,0x4000,high?0x4000:0);}
static int byte(unsigned v,unsigned receive,uint8_t *out){
 upd(0x9000,0x80,receive?0x80:0);wr(0x9250,v);wr(0x9244,1);
 for(unsigned n=0;n<200000;++n){if(rd(0x9248)&1){if(out)*out=rd(0x9250);return 0;}if(!(n&1023))feed();}
 return 2;
}
static int jedec(uint8_t *out){
 cs(1);cs(0);int e=byte(0x9f,0,0);for(unsigned i=0;i<3&&!e;++i)e=byte(255,1,out+i);cs(1);return e;
}
static int status(uint8_t *out){cs(0);int e=byte(5,0,0);if(!e)e=byte(255,1,out);cs(1);return e;}
static int ready(void){uint8_t value;
 for(unsigned n=0;n<1000000;++n){int e=status(&value);if(e||!(value&1))return e;feed();}return 2;
}
static int enable(void){int e=ready();uint8_t value;if(e)return e;cs(0);e=byte(6,0,0);cs(1);if(!e)e=status(&value);return e?e:!(value&2);}
static int address(unsigned command,uint32_t a){int e=byte(command,0,0);for(int shift=16;!e&&shift>=0;shift-=8)e=byte((a>>shift)&255,0,0);return e;}
static int pm_byte(unsigned v,uint8_t *out){
 wr(0x88ec,v);
 for(unsigned n=0;n<200000;++n)if(!(rd(0x88e0)&8)){if(out)*out=rd(0x88ec)&255;return 0;}
 return 2;
}
static int pm_xfer(unsigned reg,unsigned v,uint8_t *out){
 if(rd(0x88e0)&0x100)upd(0x88e0,0x100,0); /* a stale transaction flag would block the bus */
 upd(0x88e0,0x140,0x140);
 int e=pm_byte(reg,0);if(!e)e=pm_byte(v,out);
 upd(0x88e0,0x100,0);
 return e;
}
static int pm_rd(unsigned reg,uint8_t *out){return pm_xfer(reg,0,out);}
static int pm_wr(unsigned reg,unsigned v){return pm_xfer(reg+1,v,0);}
static int pm_enable(void){ /* stock 02026624(1) then 02026818(1) */
 uint8_t v;int e;
 upd(0x88e0,0x40,0x40);
 if((e=pm_wr(0x40,0x06))||(e=pm_wr(0x42,0xe0))||(e=pm_wr(0x44,0x00)))return e;
 if((e=pm_rd(0x58,&v))||(e=pm_wr(0x58,v|0x08)))return e;
 if((e=pm_rd(0x40,&v))||(e=pm_wr(0x40,v|0x10)))return e;
 return pm_wr(0x5a,0x08);
}
static void snap(uint8_t *out,unsigned spr){uint32_t value=rd(spr);for(unsigned j=0;j<4;++j)out[j]=value>>(j*8);}
struct request {uint32_t magic,sequence,command,address,length,status,flags;uint8_t data[];};
/* Every return before the final status store leaves status = 1 (not done), which the host treats as failure. */
static void run(struct request *p){
 p->status=1;unsigned command=p->command;uint32_t a=p->address,n=p->length;int e=0;
#ifdef SILVERGEAR_BOOT_RESTORE
 /* Separate recovery binary only: no PM writes and no general 4 MiB writer.
  * Normal flasher builds retain their original 5e4015-only write policy. */
 if(command==7){p->status=3;return;}
 if(command==9){if(n!=1)return;e=status(p->data);p->status=e;return;}
#endif
 if(command==0){
  if(!(a&1)){
  upd(0x8850,0xf0000000,0x30000000);upd(0x8844,8,0);upd(0x8840,1u<<20,0);
  upd(0x8344,3u<<20,1u<<20);upd(0x8100,0xd00,0xd00);upd(0x8040,0xd00,0);upd(0x8000,0x400,0x400);
  wr(0x9000,0x01115005);wr(0x9004,0);wr(0x9008,0);wr(0x900c,0);
  wr(0x9244,0x03ff0000);wr(0x9248,0);wr(0x924c,5);
  for(unsigned r=0x9250;r<=0x927c;r+=4)wr(r,0);
  wr(0x9260,0xffff);wr(0x9264,0xffffffff);wr(0x926c,0x0fffffff);wr(0x9270,0x0fffffff);
  }
  e=jedec(p->data);
 }else if(command==8){if(n!=14)return;
  for(unsigned i=0;i<14&&!e;++i)e=pm_rd(0x40+2*i,p->data+i);
 }else if(command==7){if(n!=14)return;
  e=pm_enable();
  for(unsigned i=0;i<14&&!e;++i)e=pm_rd(0x40+2*i,p->data+i);
 }else if(command==6){if(n!=32)return;
  snap(p->data,0x880c);snap(p->data+4,0x8858);snap(p->data+8,0x885c);snap(p->data+12,0x8864);
  snap(p->data+16,0x9000);snap(p->data+20,0x9244);snap(p->data+24,0x9248);snap(p->data+28,0x924c);
 }else if(command==4){if(n!=4||a>0xffff)return;snap(p->data,a);}
 else if(command==5){if(a>0x00400000||n>0x00400000-a)return;const volatile uint8_t *src=(const volatile uint8_t *)(uintptr_t)a;
  for(unsigned i=0;i<n;++i)p->data[i]=src[i];}
 else if(command>=1&&command<=3){
  uint8_t id[3];e=jedec(id);if(e){p->status=e;return;}
  if((unsigned)(id[2]-0x15)>1){p->status=3;return;}
  uint32_t capacity=1u<<id[2];if(a>=capacity||n>capacity-a)return;
  if(command==1){cs(0);e=address(3,a);for(unsigned i=0;i<n&&!e;++i)e=byte(255,1,p->data+i);cs(1);}
  else{
#ifdef SILVERGEAR_BOOT_RESTORE
   uint32_t sector=a&~4095u;
   if(!IS_SRAM(p)||p->flags!=RESTORE_FLAGS||id[0]!=0x94||id[1]!=0x40||id[2]!=0x16||
      !RESTORE_SECTOR_OK(sector)){p->status=3;return;}
   uint8_t protection;e=status(&protection);
   if(e||(protection&0xfc)){p->status=e?e:3;return;}
#else
   if(id[0]!=0x5e||id[1]!=0x40||id[2]!=0x15){p->status=3;return;}
#endif
   if(command==2&&(!n||n>256-(a&255)))return;
   if(command==3&&(n||a&4095))return;
   e=enable();if(!e){cs(0);e=address(command==2?2:0x20,a);
    if(command==2)for(unsigned i=0;i<n&&!e;++i)e=byte(p->data[i],0,0);
    cs(1);if(!e)e=ready();}
  }
 }else return;
 p->status=e;
}
void rescue(volatile struct request *v){
 struct request *p=(struct request *)v;
 sync(p,HEADER_BYTES);
 if(!(p->flags&FLAG_KEEP_WDT))wr(0x880c,0x5500);
 if(p->magic!=MAGIC||p->length>(IS_SRAM(p)?SRAM_WINDOW_DATA:DDR_WINDOW_DATA))return;
 uint32_t n=p->length;
 sync(p,HEADER_BYTES+n); /* a program request carries its page after the header */
 run(p);
 sync(p,HEADER_BYTES+(n>3?n:3)); /* covers status and the response, whichever way run() returned */
}
