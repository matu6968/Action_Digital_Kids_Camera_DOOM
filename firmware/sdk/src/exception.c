#include <appotech/exception.h>
#include <appotech/soc.h>
#include <appotech/watchdog.h>
#include <string.h>
#ifndef APX_EXC_VECTORS /* host model tests */
#define APX_EXC_VECTORS ((volatile uint32_t *)0u)
#endif
#ifndef APX_EXC_RECORD
#define APX_EXC_RECORD ((volatile uint32_t *)0x3400u)
#endif
#ifndef APX_EXC_PREVIOUS
#define APX_EXC_PREVIOUS ((volatile uint32_t *)0x3480u)
#endif
#ifndef APX_EXC_FLAG
#define APX_EXC_FLAG ((volatile uint32_t *)0x3470u)
#endif
#define MAGIC 0x48535243u /* 'CRSH' */
#define EXPECT_RESET 0x52535421u
/* word layout of a record */
enum { R_MAGIC, R_BUSY, R_VECTOR, R_S20, R_S30, R_S40, R_SP, R_LR, R_FP, R_STACK, R_WORDS = R_STACK + 8 };
static const uint8_t slots[] = {0x20,0x30,0x40,0x60,0x70,0x90,0xa0,0xb0,0xc0,0xd0,0xe0};
static uint32_t saved[sizeof(slots)][4];
static unsigned installed;
extern void apx_exception_common(void);
const char *apx_exception_name(uint32_t vector) {
    switch(vector) {
    case 0x20:return "bus error";case 0x30:return "data page fault";case 0x40:return "instruction page fault";
    case 0x60:return "alignment";case 0x70:return "illegal instruction";case 0x90:return "data TLB miss";
    case 0xa0:return "instruction TLB miss";case 0xb0:return "range";case 0xc0:return "syscall";
    case 0xd0:return "floating point";case 0xe0:return "trap";default:return "unknown";
    }
}
/* Slot contents: l.ori r3,r0,vector ; l.movhi r4,hi(handler) ; l.ori r4,r4,lo(handler) ; l.jr r4 */
static void slot_words(unsigned vector, uint32_t handler, uint32_t out[4]) {
    out[0]=0xa8600000u|vector;
    out[1]=0x18800000u|(handler>>16);
    out[2]=0xa8840000u|(handler&0xffffu);
    out[3]=0x44002000u;
}
void apx_exception_install(void) {
    if(installed)return;
    volatile uint32_t *table=APX_EXC_VECTORS;
    for(unsigned i=0;i<sizeof(slots);++i) {
        uint32_t words[4];
#ifdef APX_EXC_HANDLER_ADDRESS
        slot_words(slots[i],APX_EXC_HANDLER_ADDRESS,words);
#else
        slot_words(slots[i],(uint32_t)(uintptr_t)apx_exception_common,words);
#endif
        for(unsigned k=0;k<4;++k)saved[i][k]=table[slots[i]/4+k];
        for(unsigned k=3;k>0;--k)table[slots[i]/4+k]=words[k];
        table[slots[i]/4]=words[0];
    }
    installed=1;
}
void apx_exception_restore(void) {
    if(!installed)return;
    volatile uint32_t *table=APX_EXC_VECTORS;
    for(unsigned i=0;i<sizeof(slots);++i)for(unsigned k=0;k<4;++k)table[slots[i]/4+k]=saved[i][k];
    installed=0;
}
void apx_exception_expect_reset(void) { APX_EXC_FLAG[0]=EXPECT_RESET; }
void apx_exception_boot(void) {
    volatile uint32_t *record=APX_EXC_RECORD,*previous=APX_EXC_PREVIOUS;
    APX_EXC_FLAG[0]=0;
    if(record[R_MAGIC]==MAGIC) {
        for(unsigned i=0;i<R_WORDS;++i)previous[i]=record[i];
        previous[R_BUSY]=0;
        record[R_MAGIC]=0;
    } else previous[R_MAGIC]=0;
    record[R_BUSY]=0; /* also after a fault inside the report itself, which leaves the flag set without a record */
}
int apx_exception_previous(struct apx_exception *out) {
    volatile uint32_t *previous=APX_EXC_PREVIOUS;
    if(previous[R_MAGIC]!=MAGIC)return 0;
    if(out) {
        out->vector=previous[R_VECTOR];out->spr20=previous[R_S20];out->spr30=previous[R_S30];out->spr40=previous[R_S40];
        out->sp=previous[R_SP];out->lr=previous[R_LR];out->fp=previous[R_FP];
        for(unsigned i=0;i<8;++i)out->stack[i]=previous[R_STACK+i];
    }
    return 1;
}
static void put(char c) {
    apx_spr_write(0x9088,(uint8_t)c);
    for(unsigned n=0;n<20000&&!(apx_spr_read(0x908c)&2);++n);
}
static void text(const char *s) { while(*s)put(*s++); }
static void hex(uint32_t value) { for(int shift=28;shift>=0;shift-=4)put("0123456789abcdef"[(value>>shift)&15]); }
/* Called by apx_exception_common on a fresh stack, after it stored the registers in the record. */
__attribute__((used,externally_visible,noinline)) _Noreturn void apx_exception_report(void) {
    volatile uint32_t *record=APX_EXC_RECORD;
    uint32_t sp=record[R_SP];
    if(sp>=APX_RAM_BASE&&sp<=APX_RAM_END-32&&!(sp&3))for(unsigned i=0;i<8;++i)record[R_STACK+i]=((volatile uint32_t *)(uintptr_t)sp)[i];
    record[R_MAGIC]=MAGIC;
    text("\r\nEXC ");hex(record[R_VECTOR]);put(' ');text(apx_exception_name(record[R_VECTOR]));
    text(" S20=");hex(record[R_S20]);text(" S30=");hex(record[R_S30]);text(" S40=");hex(record[R_S40]);
    text(" SP=");hex(sp);text(" LR=");hex(record[R_LR]);text("\r\n");
    apx_watchdog_reset();
}
#ifndef APX_EXC_NO_ASM
/* r3 = vector number from the slot. Uses r3-r5 only until the registers are saved; no stack is touched. A second
 * exception while the first is being reported resets at once instead of recursing. */
__asm__(".nodelay\n.text\n.align 4\n.global apx_exception_common\n.type apx_exception_common,@function\napx_exception_common:\n"
"l.movhi r4,0x0\n" "l.ori r4,r4,0x3400\n"
    "l.lwz r5,0x70(r4)\n" "l.movhi r6,0x5253\n" "l.ori r6,r6,0x5421\n" "l.sfeq r5,r6\n" "l.bf 4f\n" "l.nop\n"
    "l.lwz r5,4(r4)\n" "l.sfeqi r5,0\n" "l.bf 1f\n" "l.nop\n"
    "l.ori r5,r0,0x39\n" "l.mtspr r0,r5,0x880c\n" "2: l.j 2b\n" "l.nop\n"
    "1: l.addi r5,r0,1\n" "l.sw 4(r4),r5\n"
    "l.sw 8(r4),r3\n"
    "l.mfspr r5,r0,0x20\n" "l.sw 12(r4),r5\n"
    "l.mfspr r5,r0,0x30\n" "l.sw 16(r4),r5\n"
    "l.mfspr r5,r0,0x40\n" "l.sw 20(r4),r5\n"
    "l.sw 24(r4),r1\n" "l.sw 28(r4),r9\n" "l.sw 32(r4),r2\n"
    "l.movhi r1,hi(__stack_top)\n" "l.ori r1,r1,lo(__stack_top)\n"
    "l.jal apx_exception_report\n" "l.nop\n"
    "3: l.j 3b\n" "l.nop\n"
    "4: l.j 4b\n" "l.nop\n"); /* an expected reset is under way: keep still and let it finish */
#endif
