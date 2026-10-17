/* A few bytes of always-on breadcrumbs for SPI boots in production builds.
 * The flight recorder (trace.c) changes timing and needs APX_SPI_TRACE; this does not: 31 (id, timer) pairs in
 * on-chip SRAM at 0x2f00, which a reset leaves alone (like the trace ring at 0x2800). After a failed boot the
 * camera lands in the BootROM loader, where `apxtool rom-rescue marks` reads them. */
#include <appotech/trace.h>
#include <appotech/soc.h>
#ifndef APX_MARK_PTR /* host model tests */
#define APX_MARK_PTR ((volatile uint32_t *)0x2f00u)
#endif
#define MAGIC 0x4b52414du /* 'MARK' */
#define CAPACITY 31u
void apx_mark_reset(void) {
    volatile uint32_t *m=APX_MARK_PTR;
    m[1]=0;m[0]=MAGIC;
}
void apx_mark(unsigned id,uint32_t arg) {
    volatile uint32_t *m=APX_MARK_PTR;
    if(m[0]!=MAGIC)return;
    uint32_t n=m[1];
    if(n>=CAPACITY)return;
    m[2+2*n]=(uint32_t)id<<24|(arg&0xffffffu);
    m[3+2*n]=apx_spr_read(0x9804)&0x3fff;
    m[1]=n+1;
}
