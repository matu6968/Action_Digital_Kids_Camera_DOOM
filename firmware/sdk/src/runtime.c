#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/watchdog.h>
#include <appotech/clock.h>
#include <appotech/trace.h>
#include <appotech/exception.h>
#include <appotech/power.h>
#include <appotech/battery.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint32_t apx_boot_context[6] __attribute__((section(".noinit")));
extern unsigned char __bss_start[],__bss_end[],__heap_start[],__heap_end[];
extern unsigned char __data_source[],__data_start[],__data_end[];
extern void apx_spi_boot_prepare(void),apx_spi_boot_recovery(void);
extern _Noreturn void apx_spi_boot_fail(unsigned id,uint32_t arg);
unsigned apx_boot_from_spi(void){return apx_boot_context[4]==0x53504942u;}
static unsigned char *heap_break;
static uint32_t accumulated_ticks,previous_counter;
static unsigned char log_ring[4096];
static uint32_t log_in,log_out,log_lost;

__attribute__((weak,noinline)) void apx_uart_console_put(char value){(void)value;}
static int log_put(char c,FILE *stream) {
    (void)stream;apx_uart_console_put(c);
    if(log_in-log_out==sizeof(log_ring)) { ++log_out; ++log_lost; }
    log_ring[log_in++ & (sizeof(log_ring)-1)]=(unsigned char)c;
    return 0;
}
static int log_get(FILE *stream) { (void)stream; return _FDEV_EOF; }
static FILE console=FDEV_SETUP_STREAM(log_put,log_get,NULL,_FDEV_SETUP_RW);
FILE *const stdin=&console;
FILE *const stdout=&console;
FILE *const stderr=&console;
/* Optional display hook. A linked display driver replaces this weak stub.
 * Keep a real SDK target in USB-only builds, rather than a call to address 0. */
__attribute__((weak,noinline)) void apx_display_shutdown(void) {}
__attribute__((weak,noinline)) void apx_sd_shutdown(void) {}
__attribute__((weak,noinline)) void apx_buttons_shutdown(void) {}
__attribute__((weak,noinline)) void apx_jpeg_shutdown(void) {}
__attribute__((weak,noinline)) void apx_audio_shutdown(void) {}
__attribute__((weak,noinline)) void apx_mic_shutdown(void) {}
__attribute__((weak,noinline)) void apx_camera_shutdown(void) {}
__attribute__((weak,noinline)) void apx_spi_shutdown(void) {}
__attribute__((weak,noinline)) void apx_battery_shutdown(void) {}
__attribute__((weak,noinline)) void apx_uart_shutdown(void) {}

size_t apx_log_read(void *buffer,size_t capacity) {
    unsigned char *out=buffer; size_t count=0;
    while(log_out!=log_in && count<capacity) out[count++]=log_ring[log_out++ & (sizeof(log_ring)-1)];
    return count;
}
uint32_t apx_log_dropped(void) { return log_lost; }
void *sbrk(intptr_t amount) {
    unsigned char *old=heap_break;
    uintptr_t available=(uintptr_t)(__heap_end-heap_break);
    if(amount<0 || (uintptr_t)amount>available) { errno=ENOMEM; return (void *)-1; }
    heap_break+=amount; return old;
}
uint32_t apx_ticks(void) {
    uint32_t current=apx_spr_read(0x9804)&0x3fff;
    accumulated_ticks+=(current-previous_counter)&0x3fff;
    previous_counter=current; return accumulated_ticks;
}
int apx_dma_clean(const void *buffer,size_t length) {
    uintptr_t address=(uintptr_t)buffer;
    if(address<APX_RAM_BASE || address>APX_RAM_END || length>APX_RAM_END-address)return -1;
    while(length) {
        size_t count=length>4096?4096:length;
        unsigned lines=(address+count-(address&~(uintptr_t)15)-1)>>4;
        /* Match the vendor's immediate-SPR maintenance instructions exactly. */
        __asm__ volatile("l.mtspr r0,%0,0x181c\n\tl.mtspr r0,%1,0x180c"
                         :: "r"(lines),"r"(address):"memory");
        address+=count;length-=count;
    }
    return 0;
}
void apx_delay_ms(uint32_t ms) {
    uint32_t start=apx_ticks(),duration=(ms*32768u+999u)/1000u;
    while(apx_ticks()-start<duration) apx_maintenance();
}
int apx_dma_invalidate(void *buffer,size_t length) {
    uintptr_t address=(uintptr_t)buffer;
    if((address&15)||(length&15)||address<APX_RAM_BASE||address>APX_RAM_END||length>APX_RAM_END-address)return -1;
    while(length) {
        size_t count=length>4096?4096:length;
        unsigned lines=(count-1)>>4;
        __asm__ volatile("l.mtspr r0,%0,0x181c\n\tl.mtspr r0,%1,0x1814"
                         :: "r"(lines),"r"(address):"memory");
        address+=count;length-=count;
    }
    return 0;
}
/* Write back, then drop, every cache line overlapping [start,end). */
void apx_cache_flush_range(const void *start,const void *end) {
    uintptr_t address=(uintptr_t)start&~(uintptr_t)15,stop=((uintptr_t)end+15)&~(uintptr_t)15;
    while(address<stop) {
        uintptr_t count=stop-address>4096?4096:stop-address;
        unsigned lines=(count-1)>>4;
        __asm__ volatile("l.mtspr r0,%0,0x181c\n\tl.mtspr r0,%1,0x180c\n\tl.mtspr r0,%0,0x181c\n\tl.mtspr r0,%1,0x1814"
                         :: "r"(lines),"r"(address):"memory");
        address+=count;
    }
}
/* The stock application enables both CPU caches first thing (02022c10: data cache window at DDR,
 * 02022c54: instruction cache); everything it launches or hands over inherits that. An SPI boot gets
 * neither from the vendor bootstrap, so the SDK ran from DDR uncached at about 5% speed, and the PLL
 * routine, which relies on executing from one instruction-cache line while memory is unavailable, had
 * nothing to run from. Only enable a disabled cache: toggling a live one could discard dirty lines. */
static void apx_caches_enable(void) {
    if(!(apx_spr_read(0x1800)&1)) { apx_spr_write(0x1808,APX_RAM_BASE);apx_spr_write(0x1800,apx_spr_read(0x1800)|1u); }
    if(!(apx_spr_read(0x2000)&1)) { apx_spr_write(0x2008,0);apx_spr_write(0x2000,apx_spr_read(0x2000)|1u); }
}
extern unsigned char __stack_bottom[];
void apx_runtime_init(void) {
    /* Paint the unused stack so a post-mortem can tell how deep it ever went (see lvgl_port.c). */
    for(volatile uint32_t *p=(volatile uint32_t *)__stack_bottom;(uintptr_t)p+512<(uintptr_t)__builtin_frame_address(0);++p)*p=0xa5a5a5a5u;
#if !APX_NO_CACHE_ENABLE
    apx_caches_enable();
#endif
#if APX_LAUNCH_SETTLE_TICKS
    if(apx_boot_context[4]!=0x53504942u) {
        /* Experiment: let the ROM loader's USB activity drain before the SDK reconfigures anything. */
        uint32_t start=apx_spr_read(0x9804)&0x3fff;
        while((((apx_spr_read(0x9804)&0x3fff)-start)&0x3fff)<APX_LAUNCH_SETTLE_TICKS);
    }
#endif
#if APX_LAUNCH_READS
    if(apx_boot_context[4]!=0x53504942u) {
        /* Experiment: dummy reads of the registers the wider trace snapshot touches at entry. */
        static const uint16_t probe[]={APX_LAUNCH_READ_LIST};
        for(unsigned i=0;i<sizeof(probe)/sizeof(probe[0]);++i)(void)apx_spr_read(probe[i]);
    }
#endif
    /* First thing on a cold SPI boot, before any state is touched. */
    if(apx_boot_context[4]==0x53504942u){apx_mark_reset();apx_mark(APX_TRACE_ENTRY,apx_boot_context[0]);apx_watchdog_feed();apx_trace_begin();apx_trace(APX_TRACE_ENTRY,apx_boot_context[0]);apx_trace_import_ram();
#if APX_SPI_TRACE_RING_ONLY
        /* Production timing: the previous run's ring is in the log now; this run records into SRAM only. */
        apx_trace_set_mode(4);
#endif
    }
#if APX_LAUNCH_TRACE_MODE
    else {apx_trace_begin_mode(APX_TRACE_LAUNCH_SECTOR,APX_LAUNCH_TRACE_MODE);apx_trace(APX_TRACE_ENTRY,0xa00000u);} /* trace builds only: a USB launch from the ROM loader */
#else
    else apx_trace_off(); /* the launched image inherits the SPI boot's "tracing on" state from RAM; stop it */
#endif
    /* RAM uploads use equal LMA/VMA; copying also supports future loaders. */
    if(&__data_source[0]!=&__data_start[0])
        for(unsigned char *p=__data_start,*s=__data_source;p<__data_end;++p,++s) *p=*s;
    for(unsigned char *p=__bss_start;p<__bss_end;++p) *p=0;
    apx_exception_boot();    /* before installing: the record a crashed run left becomes the "previous" one */
    apx_exception_install();
    if(apx_boot_context[4]==0x53504942u) {
        /* Before anything slow: if the camera was started by a Power press, the
         * keep-alive in the always-powered PM chip (see power.h) only exists once
         * this has run. */
        apx_watchdog_feed();
        uint8_t a=0,b=0,c=0,d=0;
        (void)apx_power_read(0x40,&a);(void)apx_power_read(0x44,&b);(void)apx_power_read(0x58,&c);(void)apx_power_read(0x5a,&d);
        int failed=apx_power_domain_enable();
        uint8_t a2=0,b2=0,c2=0,d2=0;
        (void)apx_power_read(0x40,&a2);(void)apx_power_read(0x44,&b2);(void)apx_power_read(0x58,&c2);(void)apx_power_read(0x5a,&d2);
        apx_trace(APX_TRACE_PM_BEFORE,(uint32_t)a<<16|(uint32_t)b<<8|c);
        apx_trace(APX_TRACE_PM_AFTER,(failed?0x800000u:0)|(uint32_t)(a2&0x7f)<<16|(uint32_t)b2<<8|c2);
        apx_trace(APX_TRACE_PM_5A,(uint32_t)d<<8|d2);
        apx_battery_probe(0);
        apx_mark(APX_TRACE_PM_AFTER,(uint32_t)a2<<16|(uint32_t)b2<<8|c2);
        apx_spi_boot_prepare();
        apx_mark(APX_TRACE_CLOCK_PREPARED,apx_clock_prepare_attempts()<<16|(apx_spr_read(0x9800)&0xffffu));
    }
    apx_trace(APX_TRACE_CACHES,(apx_spr_read(0x1800)&0xfff)<<12|(apx_spr_read(0x2000)&0xfff));
    heap_break=__heap_start;
    previous_counter=apx_spr_read(0x9804)&0x3fff;
    apx_watchdog_init();
    apx_trace(APX_TRACE_RUNTIME_READY,apx_spr_read(0x880c));
    apx_mark(APX_TRACE_RUNTIME_READY,apx_spr_read(0x880c));
#if APX_LAUNCH_WDT_STOP
    if(!apx_boot_from_spi())apx_watchdog_stop(); /* experiment: run a USB launch with the watchdog off */
#endif
#if APX_SPI_TRACE && !APX_TRACE_KEEP_WDT
    /* A diagnostic image must hang visibly rather than hide a stall in a reset. */
    if(apx_boot_from_spi())apx_watchdog_stop();
#endif
    apx_trace(APX_TRACE_TIMER_CTL+1,apx_spr_read(0x9800));
    int clock_error=apx_clock_init();
    apx_mark(APX_TRACE_CLOCK_STEP,(uint32_t)clock_error);
    {struct apx_clock_info clock_info;apx_clock_get_info(&clock_info);
     apx_trace(APX_TRACE_LOOP_BASELINE,clock_info.baseline_loop_ticks);
     apx_trace(APX_TRACE_LOOP_ACTIVE,clock_info.active_loop_ticks);}
    if(clock_error){
        if(apx_boot_from_spi())apx_spi_boot_fail(APX_TRACE_FAIL_CLOCK_INIT,(uint32_t)clock_error);
        apx_trace_regs(3);apx_trace(APX_TRACE_FAIL_CLOCK_INIT,(uint32_t)clock_error);
        apx_watchdog_reset();
    }
    apx_trace(APX_TRACE_CLOCK_READY,0);
    apx_mark(APX_TRACE_CLOCK_READY,0);
    {
        struct apx_exception crash;
        if(apx_exception_previous(&crash))
            printf("previous run died in CPU exception %#lx (%s): S20=%#lx S30=%#lx S40=%#lx sp=%#lx lr=%#lx\n",
                   (unsigned long)crash.vector,apx_exception_name(crash.vector),(unsigned long)crash.spr20,
                   (unsigned long)crash.spr30,(unsigned long)crash.spr40,(unsigned long)crash.sp,(unsigned long)crash.lr);
    }
    if(apx_boot_from_spi()){apx_spi_boot_recovery();apx_trace(APX_TRACE_RECOVERY_DONE,0);apx_mark(APX_TRACE_RECOVERY_DONE,0);}
}
_Noreturn void _exit(int status) {
    printf("exit(%d)\n",status);
    for(;;) apx_debug_poll();
}
/* Leave the caches coherent with DDR, then enter the ROM loader (0x10385c) on the saved ROM stack.
 * The loader goes on to receive a new image over the same addresses this SDK ran from. Without
 * this, stale instruction lines of the old image keep executing in place of the new one (a launch
 * works only if the new image is byte-identical to the old), and dirty data lines can be written
 * back over the new image later. One block of asm, no stack traffic after the flush starts:
 * per 4 KiB block write back, then drop; then switch both caches off (the loader runs uncached; whatever
 * it launches enables its own caches from a clean state, as the stock start-up does) and jump. */
static _Noreturn void apx_jump_to_rom(uint32_t stack) {
    __asm__ volatile(
        "l.movhi r3,0x0200\n\t"          /* DDR start 0x02000000 */
        "l.movhi r4,0x0240\n\t"          /* DDR end   0x02400000 */
        "l.addi r5,r0,255\n\t"           /* 256 lines of 16 bytes = 4096 bytes */
        "1:\n\t"
        "l.mtspr r0,r5,0x181c\n\t"
        "l.mtspr r0,r3,0x180c\n\t"       /* write back this block */
        "l.mtspr r0,r5,0x181c\n\t"
        "l.mtspr r0,r3,0x1814\n\t"       /* then drop it */
        "l.addi r3,r3,2048\n\t"
        "l.addi r3,r3,2048\n\t"
        "l.sfne r3,r4\n\t"
        "l.bf 1b\n\t"
        "l.nop\n\t"
        "l.addi r6,r0,6144\n\t"          /* data-cache control, SPR 0x1800: everything above is written back */
        "l.mfspr r7,r6,0\n\t"
        "l.andi r8,r7,0xfffe\n\t"
        "l.mtspr r6,r8,0\n\t"            /* off */
        "l.addi r6,r0,8192\n\t"          /* instruction-cache control, SPR 0x2000 */
        "l.mfspr r7,r6,0\n\t"
        "l.andi r8,r7,0xfffe\n\t"
        "l.mtspr r6,r8,0\n\t"            /* off: discards every line */
        "l.ori r1,%0,0\n\t"
        "l.movhi r9,0x0010\n\t"
        "l.ori r9,r9,0x385c\n\t"
        "l.jr r9\n\t"
        "l.nop"
        :: "r"(stack) : "r3","r4","r5","r6","r7","r8","r9","memory");
    __builtin_unreachable();
}
void apx_jump_to_rom_from_boot(uint32_t stack) { apx_jump_to_rom(stack); }
_Noreturn void apx_bootrom(void) {
    apx_trace(APX_TRACE_BOOTROM_CALL,0);
    /* The ROM installs its own watchdog policy; prevent a short application
     * timeout from expiring during this acknowledged handoff. */
    apx_watchdog_stop();
    apx_exception_restore(); /* the loader and the next image bring their own handlers */
    /* Let the host complete its final bulk URB before removing the pull-up. */
    apx_delay_ms(100);
    apx_camera_shutdown();apx_mic_shutdown();apx_audio_shutdown();apx_jpeg_shutdown();
    apx_uart_shutdown();apx_spi_shutdown();apx_battery_shutdown();apx_buttons_shutdown();
    apx_display_shutdown();
    apx_sd_shutdown();
    apx_spr_write(0xa004,apx_spr_read(0xa004)&~0x40u);
    apx_delay_ms(100);
    /* Stock 0203c1cc (its 0xda handler) clears SPR 0x18 bits 1, 2 and finally 14 before it hands
     * control back to the ROM. Bit 14 is already clear when the SDK was launched from the loader,
     * but the vendor bootstrap leaves it set on an SPI boot, and the ROM loader then never comes
     * up (the camera vanishes from USB). */
    apx_spr_write(0x18,apx_spr_read(0x18)&~0x4006u);
    apx_spr_write(0x4800,apx_spr_read(0x4800)&~0x8000u);
    apx_spr_write(0x9840,apx_spr_read(0x9840)&0xfff3fffau);
    apx_spr_write(0xd804,apx_spr_read(0xd804)|8u);
    if(apx_clock_restore())apx_watchdog_reset();
    /* Deliberate ROM use is confined to this recovery handoff. */
    apx_jump_to_rom(apx_boot_context[0]);
    __builtin_unreachable();
}
