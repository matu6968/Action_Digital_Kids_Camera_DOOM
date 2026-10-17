    .nodelay
    .section .text.entry,"ax",@progbits
    .global _start,apx_spi_entry
    .type _start,@function
_start:
    l.addi r7,r0,0
    l.j 2f
    l.nop
    .type apx_spi_entry,@function
apx_spi_entry:
    l.movhi r7,0x5350
    l.ori r7,r7,0x4942
2:
    l.movhi r10,hi(apx_boot_context)
    l.ori r10,r10,lo(apx_boot_context)
    l.sw 0(r10),r1
    l.sw 4(r10),r2
    l.sw 8(r10),r9
    l.ori r4,r0,0x18
    l.mfspr r5,r4,0
    l.sw 12(r10),r5
    l.sw 16(r10),r7
    /* Preserve all upper status bits; only clear vendor external IRQ bit 1. */
    l.addi r6,r0,-3
    l.and r5,r5,r6
    l.mtspr r4,r5,0
    l.movhi r1,hi(__stack_top)
    l.ori r1,r1,lo(__stack_top)
    l.jal apx_runtime_init
    l.nop
    l.jal main
    l.nop
1:  l.jal apx_debug_poll
    l.nop
    l.j 1b
    l.nop
    .size _start,.-_start
