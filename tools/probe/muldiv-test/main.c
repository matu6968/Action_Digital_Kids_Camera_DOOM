/* Known-answer test for the multiply/divide routines behind libgcc's entry points (src/hwmuldiv.S when HW_MULDIV=1, libgcc's software
 * routines otherwise). Vectors are generated from Python integers. Runs once, prints the result to the log ring and the USB debug
 * link keeps running. The operands are volatile so the compiler cannot fold the operations away. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <stdint.h>
#include <stdio.h>
#include "vectors.inc"
int main(void) {
    apx_usb_init();
    uint32_t t=apx_ticks();while((uint32_t)(apx_ticks()-t)<49152){apx_debug_poll();apx_maintenance();}
    unsigned failures=0,n=sizeof(vectors)/sizeof(vectors[0]);
    for(unsigned i=0;i<n;++i) {
        volatile int32_t a=vectors[i].a,b=vectors[i].b;
        volatile uint32_t ua=(uint32_t)vectors[i].a,ub=(uint32_t)vectors[i].b;
        if(a*b!=vectors[i].mul) { ++failures;printf("mul %ld*%ld got %ld want %ld\n",(long)a,(long)b,(long)(a*b),(long)vectors[i].mul); }
        if(vectors[i].sok) {
            if(a/b!=vectors[i].sd||a%b!=vectors[i].sm) { ++failures;printf("sdiv %ld/%ld got %ld,%ld want %ld,%ld\n",(long)a,(long)b,(long)(a/b),(long)(a%b),(long)vectors[i].sd,(long)vectors[i].sm); }
        }
        if(vectors[i].uok) {
            if(ua/ub!=vectors[i].ud||ua%ub!=vectors[i].um) { ++failures;printf("udiv %lu/%lu got %lu,%lu want %lu,%lu\n",(unsigned long)ua,(unsigned long)ub,(unsigned long)(ua/ub),(unsigned long)(ua%ub),(unsigned long)vectors[i].ud,(unsigned long)vectors[i].um); }
        }
        apx_debug_poll();
    }
    /* timing, 20000 operations each (short enough that the watchdog and the 14-bit tick counter need no service). Operands depend on the loop counter so nothing is folded, and the cost of a software multiply
     * depends on the multiplier (shift-and-add loop), so small and dense multipliers are timed separately. */
    uint32_t t0;static volatile int32_t sink;
    volatile int32_t small=0x1357,dense=0x5a5a5a5b,div7=7,div1000003=1000003;volatile uint32_t base=0x7ffffff0u;
    int32_t acc=0;
    t0=apx_ticks();for(unsigned i=0;i<20000;++i)acc+=(int32_t)i*small;sink=acc;uint32_t t_small=apx_ticks()-t0;
    t0=apx_ticks();for(unsigned i=0;i<20000;++i)acc+=(int32_t)i*dense;sink=acc;uint32_t t_dense=apx_ticks()-t0;
    t0=apx_ticks();for(unsigned i=0;i<20000;++i)acc+=(int32_t)(base+i)/div7;sink=acc;uint32_t t_div7=apx_ticks()-t0;
    t0=apx_ticks();for(unsigned i=0;i<20000;++i)acc+=(int32_t)(base+i)/div1000003;sink=acc;uint32_t t_divbig=apx_ticks()-t0;
    t0=apx_ticks();for(unsigned i=0;i<20000;++i)acc+=(int32_t)(base+i)%div7;sink=acc;uint32_t t_mod=apx_ticks()-t0;
    (void)sink;
    printf("muldiv-test: %u vectors, failures=%u; ns per op (loop overhead included): mul small %lu, mul dense %lu, div by 7 %lu, div by 1000003 %lu, mod 7 %lu\n",
           n,failures,(unsigned long)(t_small*15259u/10000u),(unsigned long)(t_dense*15259u/10000u),(unsigned long)(t_div7*15259u/10000u),
           (unsigned long)(t_divbig*15259u/10000u),(unsigned long)(t_mod*15259u/10000u));
    for(;;){apx_debug_poll();apx_maintenance();}
}
