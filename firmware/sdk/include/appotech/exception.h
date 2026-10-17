#ifndef APPOTECH_EXCEPTION_H
#define APPOTECH_EXCEPTION_H
#include <stdint.h>
/* CPU exception capture.
 * This chip's exception vectors are 16-byte slots at SRAM address 0 (bus error 0x20, data/instruction page
 * fault 0x30/0x40, alignment 0x60, illegal instruction 0x70, DTLB/ITLB miss 0x90/0xa0, range 0xb0, syscall 0xc0,
 * FP 0xd0, trap 0xe0; reset 0x00/0x10, tick 0x50 and external interrupt 0x80 are left to the vendor bootstrap).
 * The bootstrap's own handler only arms the watchdog, which made every fault look like an unexplained reset.
 * apx_exception_install() redirects the fault slots to a handler that records the vector, SPR 0x20/0x30/0x40,
 * sp/lr/fp and the top of the stack in SRAM at 0x3400, prints one line on UART0 TX (PA7, polled) and resets
 * the chip. The next boot moves the record aside; apx_exception_previous() returns it (also in `debug info`). */
struct apx_exception {
    uint32_t vector;   /* 0x20.. as above */
    uint32_t spr20;    /* raw SPR 0x20, 0x30 and 0x40 at entry. Standard OR1K names them EPCR0, EEAR0 and ESR0, but the roles */
    uint32_t spr30;    /* are unverified on this chip: SPR 0x40 idles at 0x00100010, and after a fetch from address 0 it held */
    uint32_t spr40;    /* a code address while SPR 0x20 read 0 */
    uint32_t sp, lr, fp;
    uint32_t stack[8]; /* words at [sp] when sp was inside RAM */
};
/* The chip raises vector 0x30 when a software watchdog reset begins (write 0x39 to SPR 0x880c), with SPR 0x40 inside
 * the code that was spinning. A deliberate reset calls this first so the handler lets it finish instead of reporting
 * a crash. A hang that lets the watchdog expire raises no exception at all, so it cannot be reported here. */
void apx_exception_expect_reset(void);
void apx_exception_install(void);   /* runtime start-up; idempotent */
void apx_exception_restore(void);   /* put the vendor slots back (BootROM handoff) */
void apx_exception_boot(void);      /* take the record a crashed run left, if any */
int apx_exception_previous(struct apx_exception *out); /* 1 if the previous run ended in a CPU exception */
const char *apx_exception_name(uint32_t vector);
#endif
