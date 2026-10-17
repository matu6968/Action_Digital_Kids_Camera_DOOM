/* Cold boot after the original, board-specific 8 KiB SRAM bootstrap.
 * No vendor application functions or vendor USB driver calls. */
#include <appotech/soc.h>
#include <appotech/clock.h>
#include <appotech/watchdog.h>
#include <stdint.h>
#include <appotech/buttons.h>
#include <appotech/trace.h>
#include <appotech/exception.h>
extern uint32_t apx_boot_context[6];
extern _Noreturn void apx_jump_to_rom_from_boot(uint32_t stack);
extern void apx_cache_flush_range(const void *start,const void *end);
extern unsigned char __large_buffer_start[],__large_buffer_end[];
extern unsigned char __display_dma_start[],__display_dma_end[];
extern unsigned char __jpeg_buffer_start[],__jpeg_buffer_end[];
extern void apx_clock_apply_96(void);
/* A cold boot arrives with the BootROM's watchdog still armed (about 1.8 s at
 * selector 5) and nobody feeding it: the ROM only reloads it from its USB
 * interrupt. Zeroing 1.27 MiB of uncached DDR byte by byte outlasts that, which
 * made the first SPI image reset forever right after the stack-page check.
 * Clear with word stores and reload the watchdog every 64 KiB. */
static void clear(unsigned char *start,unsigned char *end){
 while(start<end&&((uintptr_t)start&3))*start++=0;
 volatile uint32_t *word=(volatile uint32_t *)start;
 for(unsigned n=0;(unsigned char *)(word+1)<=end;++n){
  *word++=0;
  if(!(n&0x3fff))apx_watchdog_feed();
 }
 for(start=(unsigned char *)word;start<end;)*start++=0;
}
/* A cold-boot failure must not be a silent reset loop: that is a dead-looking
 * camera with no evidence. Record the cause, then fall into the BootROM USB
 * loader (the same state a forced recovery reaches), where the trace sector and
 * registers can be read. Only when the saved ROM stack is unusable is there
 * nothing safe to return to, and the fast watchdog reset remains the fallback. */
_Noreturn void apx_spi_boot_fail(unsigned id,uint32_t arg){
 apx_mark(id,arg);
 apx_exception_restore();
 apx_trace_regs(3);apx_trace(id,arg);
 uint32_t stack=apx_boot_context[0];
 if(stack<0x4000||stack>=0x4f74||(stack&3))apx_watchdog_reset();
 apx_trace(APX_TRACE_BOOTROM_HANDOFF,id);
 /* The loader expects its own 96 MHz PLL1 pair; the bootstrap leaves 144. */
 if(apx_spr_read(0x8858)==0x1123u&&apx_spr_read(0x885c)==0x112a8800u)apx_clock_apply_96();
 apx_spr_write(0x880c,0x5500); /* the loader installs its own watchdog policy */
 apx_spr_write(0x18,apx_spr_read(0x18)&~0x4006u); /* as the stock 0xda handler does; bit 14 is left set by the bootstrap */
 apx_jump_to_rom_from_boot(stack);
 __builtin_unreachable();
}
void apx_spi_boot_prepare(void){
 /* SRAM 0x1e6c is the bootstrap's saved ROM stack page (SPI 0540..0580).
  * Restore the page just as SPI 0584..05c8 does, without returning to boot.
  * The saved r1 points at the pushed return PC, so recovery uses r1+4. */
 const volatile uint32_t *saved=(const volatile uint32_t *)0x1e6c;
 uint32_t stack=saved[0],base=stack&~255u;
 if(stack<0x4000||stack>=0x4f74||(stack&3)){apx_trace(APX_TRACE_FAIL_STACK,stack);apx_watchdog_reset();}
 volatile uint32_t *target=(volatile uint32_t *)(uintptr_t)base;
 for(unsigned i=0;i<64;++i)target[i]=saved[i];
 apx_boot_context[0]=stack+4;
 apx_mark(APX_TRACE_STACK_OK,stack);apx_trace(APX_TRACE_STACK_OK,stack);
 /* These sections are NOLOAD and are normally cleared by the USB launcher. */
 clear(__large_buffer_start,__large_buffer_end);
 clear(__display_dma_start,__display_dma_end);
 clear(__jpeg_buffer_start,__jpeg_buffer_end);
 /* With the data cache on, those zeros sit dirty in cache lines; the drivers' DMA expects them in DDR. */
 apx_cache_flush_range(__large_buffer_start,__large_buffer_end);
 apx_cache_flush_range(__display_dma_start,__display_dma_end);
 apx_cache_flush_range(__jpeg_buffer_start,__jpeg_buffer_end);
 apx_trace(APX_TRACE_BUFFERS_CLEARED,0);
 {int refused=apx_clock_prepare_spi();if(refused)apx_spi_boot_fail(APX_TRACE_FAIL_CLOCK_PREPARE,(uint32_t)refused<<16|(apx_spr_read(0x9800)&0xffffu));}
 apx_trace(APX_TRACE_CLOCK_PREPARED,0);
 apx_trace(APX_TRACE_TIMER_CTL,apx_spr_read(0x9800));
}

/* Hold SHUTTER during power-on for the ROM USB loader. This early path uses
 * only GPIO/ADC and runs before the display, sensor, SD and audio drivers. */
void apx_spi_boot_recovery(void){
 apx_buttons_init();apx_trace(APX_TRACE_RECOVERY_BUTTONS,0);
 uint32_t deadline=apx_ticks()+8192;unsigned polls=0;
 while(!apx_deadline_expired(deadline)){
  apx_buttons_poll();apx_maintenance();
  struct apx_buttons_info buttons;apx_buttons_get_info(&buttons);
  if(++polls==1)apx_trace(APX_TRACE_RECOVERY_POLL,buttons.adc);
  if(polls==4096)apx_trace(APX_TRACE_RECOVERY_TICKS,apx_ticks());
  if(buttons.state&APX_BUTTON_SHUTTER){apx_trace(APX_TRACE_RECOVERY_SHUTTER,buttons.state);apx_bootrom();}
 }
 apx_buttons_shutdown();
}
