/* Forced BootROM recovery. All code/stack/requests are on-chip SRAM.
 * PIO-only copy of the independently verified SDK SPI sequence. No DDR,
 * DMA, libc, vendor application calls, or clock-profile changes.
 *
 * The BootROM only reloads its watchdog (SPR 0x880c) from the USB interrupt
 * handler, so an idle host loses the device after about two seconds. Every
 * entry therefore disables it first (0x5500 reads back zero, see
 * research/watchdog-power.md). The reload (0x5001d) is only issued while the
 * watchdog still reads non-zero, so it never re-enables a disabled counter.
 *
 * Command 0 with address bit 0 set probes the JEDEC ID using the SPI state
 * the BootROM already programmed (same register protocol as BootROM
 * 0x104ac4..0x104d20); without it the full SDK setup is applied first.
 * Commands 7/8 drive the always-powered PM chip over its serial bus (SPR 0x88e0/0x88ec,
 * same transactions as src/power.c): 8 reads registers 0x40..0x5a, 7 applies the stock
 * start-up sequence so the keep-alive (reg 0x5a = 0x08) is restored without any flash access.
 * Command 6 returns eight diagnostic SPRs (watchdog, PLL1 control/coefficient
 * and the SPI controller registers the BootROM itself reads) without touching
 * the flash bus. */
#include <stdint.h>
static inline uint32_t rd(unsigned r){uint32_t v;__asm__ volatile("l.mfspr %0,%1,0":"=&r"(v):"r"(r):"memory");return v;}
static inline void wr(unsigned r,uint32_t v){__asm__ volatile("l.mtspr %0,%1,0"::"r"(r),"r"(v):"memory");}
static void upd(unsigned r,uint32_t mask,uint32_t value){wr(r,(rd(r)&~mask)|(value&mask));}
static void feed(void){if(rd(0x880c))wr(0x880c,0x5001d);}
static void cs(unsigned high){upd(0x9000,0x4000,high?0x4000:0);}
static int byte(unsigned v,unsigned receive,uint8_t *out){
 upd(0x9000,0x80,receive?0x80:0);wr(0x9250,v);wr(0x9244,1);
 for(unsigned n=0;n<200000;++n){if(rd(0x9248)&1){if(out)*out=rd(0x9250);return 0;}if(!(n&1023))feed();}
 return 2;
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
struct request {uint32_t magic,sequence,command,address,length,status;uint8_t data[512];};
void rescue(volatile struct request *v){
 struct request *p=(struct request *)v;
 wr(0x880c,0x5500);
 if(p->magic!=0x52585041u||p->length>512)return;
 p->status=1;unsigned command=p->command;uint32_t a=p->address,n=p->length;int e=0;
 if(command==0){
  if(!(a&1)){
  upd(0x8850,0xf0000000,0x30000000);upd(0x8844,8,0);upd(0x8840,1u<<20,0);
  upd(0x8344,3u<<20,1u<<20);upd(0x8100,0xd00,0xd00);upd(0x8040,0xd00,0);upd(0x8000,0x400,0x400);
  wr(0x9000,0x01115005);wr(0x9004,0);wr(0x9008,0);wr(0x900c,0);
  wr(0x9244,0x03ff0000);wr(0x9248,0);wr(0x924c,5);
  for(unsigned r=0x9250;r<=0x927c;r+=4)wr(r,0);
  wr(0x9260,0xffff);wr(0x9264,0xffffffff);wr(0x926c,0x0fffffff);wr(0x9270,0x0fffffff);
  }
  cs(1);cs(0);e=byte(0x9f,0,0);for(unsigned i=0;i<3&&!e;++i)e=byte(255,1,p->data+i);cs(1);
  if(!e&&(p->data[0]!=0x5e||p->data[1]!=0x40||p->data[2]!=0x15))e=3;
 }else if(command==8){if(n!=14)return;
  for(unsigned i=0;i<14&&!e;++i)e=pm_rd(0x40+2*i,p->data+i);
 }else if(command==7){if(n!=14)return;
  e=pm_enable();
  for(unsigned i=0;i<14&&!e;++i)e=pm_rd(0x40+2*i,p->data+i);
 }else if(command==6){if(n!=32)return;
  static const uint16_t sprs[8]={0x880c,0x8858,0x885c,0x8864,0x9000,0x9244,0x9248,0x924c};
  for(unsigned i=0;i<8;++i){uint32_t value=rd(sprs[i]);for(unsigned j=0;j<4;++j)p->data[i*4+j]=value>>(j*8);}
 }else if(command==4){if(n!=4||a>0xffff)return;uint32_t value=rd(a);for(unsigned i=0;i<4;++i)p->data[i]=value>>(i*8);}
 else if(command==5){if(a>0x00400000||n>0x00400000-a)return;const volatile uint8_t *src=(const volatile uint8_t *)(uintptr_t)a;
  for(unsigned i=0;i<n;++i)p->data[i]=src[i];}
 else if(a>=0x200000||n>0x200000-a)return;
 else if(command==1){cs(0);e=address(3,a);for(unsigned i=0;i<n&&!e;++i)e=byte(255,1,p->data+i);cs(1);}
 else if(command==2||command==3){
  if(command==2&&(!n||n>256-(a&255)))return;
  if(command==3&&(n||a&4095))return;
  e=enable();if(!e){cs(0);e=address(command==2?2:0x20,a);
   if(command==2)for(unsigned i=0;i<n&&!e;++i)e=byte(p->data[i],0,0);
   cs(1);if(!e)e=ready();}
 }else return;
 p->status=e;
}
