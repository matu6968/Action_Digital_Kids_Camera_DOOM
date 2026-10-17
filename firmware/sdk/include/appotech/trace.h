#ifndef APPOTECH_TRACE_H
#define APPOTECH_TRACE_H
#include <stdint.h>
/* Cold-boot flight recorder (build with APX_SPI_TRACE=1).
 *
 * A silent early failure on a real power-up leaves no evidence: no USB, no
 * display, no RAM that survives. A trace build therefore records progress in
 * the otherwise unused last SPI sector (0x1ff000) and on the UART TX pad:
 *
 *   +0x000  header: 'APXT', version, 16-byte ASCII build id
 *   +0x100  register snapshots n=0..3, one 256-byte page each, 32 x {spr,value}
 *   +0x800  breadcrumbs: slot id at +4*id = {id, arg[7:0], arg[15:8], arg[23:16]}
 *
 * Every write is a page program of previously erased bytes, so a breadcrumb
 * survives a hang or reset the instant it is made. Only that one sector is ever
 * touched, and tracing silently disables itself unless the JEDEC ID matches.
 * Decode with `apxtool trace decode` or `apxtool rom-rescue trace`. */
#define APX_TRACE_SECTOR 0x001ff000u
#define APX_TRACE_LAUNCH_SECTOR 0x001fe000u /* log of a USB-launched run; the SPI boot that follows a crash must not erase it */
#define APX_TRACE_REGS_OFFSET 0x100u
#define APX_TRACE_CRUMBS_OFFSET 0x800u
#define APX_TRACE_SPR_COUNT 32u
enum apx_trace_id {
    APX_TRACE_ENTRY = 0x01,        /* arg: stack pointer at SPI entry */
    APX_TRACE_STACK_OK = 0x02,     /* arg: saved ROM stack pointer */
    APX_TRACE_BUFFERS_CLEARED = 0x03,
    APX_TRACE_CLOCK_PREPARED = 0x04,
    APX_TRACE_RUNTIME_READY = 0x05,
    APX_TRACE_CLOCK_READY = 0x06,
    APX_TRACE_RECOVERY_DONE = 0x07,
    APX_TRACE_MAIN = 0x08,
    APX_TRACE_RECOVERY_BUTTONS = 0x09, /* buttons/ADC initialised */
    APX_TRACE_RECOVERY_POLL = 0x0a,    /* first poll done; arg: ADC reading */
    APX_TRACE_RECOVERY_TICKS = 0x0b,   /* arg: tick counter after 4096 polls (must advance) */
    APX_TRACE_RECOVERY_SHUTTER = 0x0c, /* SHUTTER seen; arg: button state */
    APX_TRACE_BOOTROM_CALL = 0x0d,     /* apx_bootrom() entered while SPI-booted */
    APX_TRACE_USB_FIRST_POLL = 0x18,   /* arg: tick counter (32.768 kHz) at the first apx_usb_poll() */
    APX_TRACE_USB_FIRST_SETUP = 0x19,  /* arg: ticks when the first control request from the host was handled */
    APX_TRACE_USB_CONFIGURED = 0x1d,   /* arg: ticks when the host set the configuration (enumeration complete) */
    APX_TRACE_CACHES = 0x1e,           /* arg: (SPR 0x1800 data-cache ctl << 12) | SPR 0x2000 instruction-cache ctl */
    APX_TRACE_PM_BEFORE = 0x1a,        /* arg: PM reg 0x40 << 16 | 0x44 << 8 | 0x58 before the power-domain init */
    APX_TRACE_PM_AFTER = 0x1b,         /* same registers after; arg bit 23 set if the init failed */
    APX_TRACE_PM_5A = 0x1c,            /* arg: PM reg 0x5a before << 8 | after */
    APX_TRACE_LVGL = 0x40,             /* lvgl-demo main: 0x40 entered, 0x41 USB up, 0x42 overlay load result, 0x43 port init result, 0x44 demo created, 0x45 first poll */
    APX_TRACE_TIMER_CTL = 0x74,        /* arg: SPR 0x9800 (0x0b/0x74 right after the SPI-boot normalization, 0x75 just before clock init) */
    APX_TRACE_CLOCK_STEP = 0x70,       /* 0x70 entry, 0x71 baseline measured, 0x72 PLL switch returned */
    APX_TRACE_LOOP_BASELINE = 0x0e,    /* arg: 1M-iteration loop time at 96 MHz, 32.768 kHz ticks (RAM launch: ~2389) */
    APX_TRACE_LOOP_ACTIVE = 0x0f,      /* arg: the same loop at the SDK clock (RAM launch: ~1593) */
    APX_TRACE_APP_BASE = 0x10,     /* applications: 0x10..0x7f */
    APX_TRACE_BATTERY_ADC = 0x2f,      /* arg: ADC control (0x9170) << 12 | 0x8840 >> 20 at battery init */
    APX_TRACE_BATTERY_PAIR = 0x30,     /* 0x30..0x3f: first 16 pairs, arg: channel9 << 12 | channel11 (raw 10-bit) */
    APX_TRACE_FAIL_STACK = 0x80,   /* arg: bad saved stack pointer */
    APX_TRACE_FAIL_CLOCK_PREPARE = 0x81,
    APX_TRACE_FAIL_CLOCK_INIT = 0x82, /* arg: apx_clock_init error */
    APX_TRACE_BOOTROM_HANDOFF = 0x83  /* arg: failure id that caused it */
};
/* Always compiled: tiny SRAM breadcrumbs for production SPI boots (src/boot_marks.c). */
void apx_mark_reset(void);                     /* start a fresh list (SPI boot entry) */
void apx_mark(unsigned id,uint32_t arg);       /* append (id, arg[23:0]) and the raw 32 kHz timer; 31 entries */
#if defined(APX_SPI_TRACE) && APX_SPI_TRACE
void apx_trace_begin(void);                    /* erase the sector, write the header, snapshot 0 */
void apx_trace_begin_at(uint32_t sector);
void apx_trace_begin_mode(uint32_t sector,unsigned how); /* how: bit 0 flash log, bit 1 UART echo, bit 2 SRAM ring (4 = ring only: no flash, no UART) */
void apx_trace_import_ram(void);               /* copy crumbs a crashed run left in SRAM into the log (slots 0xc0..0xdf) */
void apx_trace_heartbeat(uint32_t progress);   /* ring mode: remember how far a long loop got, and when (raw 32 kHz timer) */
void apx_trace_set_mode(unsigned how); /* change what an active trace records (bit 0 flash, bit 1 UART, bit 2 SRAM ring) */
void apx_trace_usb(unsigned code,uint32_t a,uint32_t b); /* UART only: one 'U<code>:<a> <b>' line per USB event */
void apx_trace_spr(unsigned spr); /* ring mode: record the full 32-bit value of one SPR */
void apx_trace_off(void);                      /* stop recording (a USB launch inherits the SPI boot's state in RAM) */
int apx_trace_active(void);      /* same, in another 4 KiB sector (e.g. 0x1fe000 for USB-launched runs) */
void apx_trace(unsigned id,uint32_t arg);      /* arg is truncated to 24 bits */
void apx_trace_regs(unsigned snapshot);        /* snapshot 0..3 */
#else
static inline void apx_trace_begin(void) {}
static inline void apx_trace_begin_at(uint32_t sector) { (void)sector; }
static inline void apx_trace_begin_mode(uint32_t sector,unsigned how) { (void)sector;(void)how; }
static inline void apx_trace_off(void) {}
static inline void apx_trace_usb(unsigned code,uint32_t a,uint32_t b) { (void)code;(void)a;(void)b; }
static inline void apx_trace_spr(unsigned spr) { (void)spr; }
static inline void apx_trace_set_mode(unsigned how) { (void)how; }
static inline void apx_trace_heartbeat(uint32_t progress) { (void)progress; }
static inline void apx_trace_import_ram(void) {}
static inline int apx_trace_active(void) { return 0; }
static inline void apx_trace(unsigned id,uint32_t arg) { (void)id;(void)arg; }
static inline void apx_trace_regs(unsigned snapshot) { (void)snapshot; }
#endif
#endif
