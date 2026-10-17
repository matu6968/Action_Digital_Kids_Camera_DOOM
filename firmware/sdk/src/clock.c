#include <appotech/clock.h>
#include <appotech/soc.h>
#include <appotech/watchdog.h>
#include "board.h"
#include <appotech/trace.h>
static struct apx_clock_info info;
static unsigned initialized,prepare_attempts;
extern void apx_clock_apply_144(void),apx_clock_apply_96(void);
extern uint32_t apx_clock_measure(void);
#define measure apx_clock_measure /* (a C loop with a heartbeat was tried; it is slow enough to trip the watchdog) */
static int profile(unsigned mhz) {
 return apx_spr_read(0x8858)==(mhz==144?0x1123u:0x1623u)&&
  apx_spr_read(0x885c)==(mhz==144?0x112a8800u:0x0b71b000u);
}
/* Cold SPI boot: the preserved SRAM bootstrap leaves PLL1 at the 144 MHz pair
 * and the reference counter at 0x701. That already is the SDK's run clock, so
 * nothing is switched here: only the counter is normalized. (An earlier version
 * dropped to the 96 MHz loader pair first, purely so apx_clock_init() could
 * find the state a USB launch leaves; that was two PLL transitions ending where
 * they began, plus a slow measurement loop that overran the ROM's watchdog.)
 * The 96 MHz pair is accepted too, for a non-destructive BootROM-launch rehearsal. */
/* Returns 0, or why the incoming state was refused: 1 PLL pair, 2 timer control, 3 timer period, 4 timer control write never read back. */
int apx_clock_prepare_spi(void) {
 uint32_t timer=apx_spr_read(0x9800);
 if(!profile(144)&&!profile(96))return 1;
 if(timer!=0x701&&timer!=0x721)return 2;
 if(apx_spr_read(0x9808)!=0x3fff)return 3;
 /* The timer control is in the 32 kHz domain and accepts a write only some time after reset, not after SPI entry:
  * a write shows up in the readback ~1 ms later once it is accepted, but earlier ones are dropped. The 664 KB LVGL
  * image reached this point 40 ms after entry and needed 18 tries (~72 ms); the same image repacked at 50 KB
  * (the BootROM loads it sooner, so entry is earlier in absolute time) was still refused after 250 ms. Write,
  * wait, verify, retry for up to 3 s, keeping the watchdog fed. */
 for(prepare_attempts=1;prepare_attempts<=768;++prepare_attempts) {
  apx_spr_write(0x9800,0x721);
  uint32_t start=apx_spr_read(0x9804)&0x3fff;
  do { if(apx_spr_read(0x9800)==0x721)return 0; } while((((apx_spr_read(0x9804)&0x3fff)-start)&0x3fff)<131); /* 4 ms */
  apx_watchdog_feed();
 }
 return 4;
}
unsigned apx_clock_prepare_attempts(void) { return prepare_attempts; }
int apx_clock_init(void) {
 if(initialized)return info.error;
 info.requested_mhz=APX_CPU_MHZ;
 /* Only the known pairs and timer are accepted. USB-owned routing, PLL2,
  * dividers, RAM timing, timer, cache and IRQ fields stay untouched. */
 if((!profile(96)&&!profile(144))||apx_spr_read(0x9800)!=0x721||apx_spr_read(0x9808)!=0x3fff)return info.error=1;
 apx_trace(APX_TRACE_CLOCK_STEP,0x100000u|profile(96)<<4|profile(144)); /* entry: which pair is live */
 if(profile(96)) {
  /* USB launch: the loader pair, measured before and after the switch.
   * The ROM loader's USB link is still live (pull-up on, PHY running) while PLL1 is relocked below. About 9 of 100
   * launches that switched the PLL that way left the USB core unable to send data packets (the host sees
   * a full-speed device and error -71 on every transfer), and restarting the core did not recover it;
   * 0 of 40 failed when the PLL was not switched, 0 of 60 failed with this detach, and a cold SPI boot never
   * switches. So detach first:
   * apx_usb_init() brings the link up again once the clocks are final. */
  apx_spr_write(0xa004,apx_spr_read(0xa004)&~0x40u);
  info.cpu_hz_estimate=96000000;info.baseline_loop_ticks=measure();
  apx_trace(APX_TRACE_CLOCK_STEP+1,info.baseline_loop_ticks); /* the slow 96 MHz measurement finished */
#if APX_CPU_MHZ == 144
  apx_clock_apply_144();
  apx_trace(APX_TRACE_CLOCK_STEP+2,apx_spr_read(0x8858)); /* the PLL switch returned */
  if(!profile(144))return info.error=2;
#endif
 } else {
  /* Cold SPI boot: already at 144 MHz. No baseline exists (it would need a
   * transition just to measure), so it is reported as 0. */
  info.baseline_loop_ticks=0;
#if APX_CPU_MHZ == 96
  apx_clock_apply_96();
  if(!profile(96))return info.error=2;
#endif
 }
 info.cpu_hz_estimate=APX_CPU_HZ_ESTIMATE;info.active_loop_ticks=measure();
 initialized=1;return info.error=0;
}
int apx_clock_restore(void) {
 if(!initialized)return info.error?info.error:1;
 if(!profile(APX_CPU_MHZ))return info.error=1;
#if APX_CPU_MHZ == 144
 apx_clock_apply_96();
#endif
 if(!profile(96))return info.error=2;
 info.cpu_hz_estimate=96000000;initialized=0;return info.error=0;
}
void apx_clock_get_info(struct apx_clock_info *out) {
 if(!out)return;
 *out=info;out->pll1_control=apx_spr_read(0x8858);out->pll1_coefficient=apx_spr_read(0x885c);
}
