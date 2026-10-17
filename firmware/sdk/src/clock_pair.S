    .nodelay
    .section .text.clock_pair,"ax",@progbits
    .global apx_clock_apply_144,apx_clock_apply_96,apx_clock_measure
    .type apx_clock_apply_144,@function
apx_clock_apply_144:
    l.movhi r5,0x112a
    l.ori r5,r5,0x8800
    l.addi r6,r0,0x1123
    /* Match the accepted loader's complete 28-byte aligned block, including
     * coefficient/control order and bounded 500-count settling loop.
     * This delay is not an independently verified PLL-lock measurement. */
    .balignl 32,0x15000000
    .global apx_clock_up_critical
apx_clock_up_critical:
    l.mtspr r0,r5,0x885c
    l.mtspr r0,r6,0x8858
    l.addi r4,r0,500
1:  l.addi r4,r4,-1
    l.sfnei r4,0
    l.bf 1b
    l.nop
    l.jr r9
    l.nop
    .size apx_clock_apply_144,.-apx_clock_apply_144
    .type apx_clock_apply_96,@function
apx_clock_apply_96:
    l.movhi r5,0x0b71
    l.ori r5,r5,0xb000
    l.addi r6,r0,0x1623
    .balignl 32,0x15000000
    .global apx_clock_down_critical
apx_clock_down_critical:
    l.mtspr r0,r5,0x885c
    l.mtspr r0,r6,0x8858
    l.addi r4,r0,500
2:  l.addi r4,r4,-1
    l.sfnei r4,0
    l.bf 2b
    l.nop
    l.jr r9
    l.nop
    .size apx_clock_apply_96,.-apx_clock_apply_96
    .type apx_clock_measure,@function
apx_clock_measure:
    l.ori r3,r0,0x9804
    l.mfspr r5,r3,0
    l.movhi r4,0x000f
    l.ori r4,r4,0x4240
    l.addi r8,r0,0
3:  l.addi r8,r8,1
    l.addi r4,r4,-1
    l.sfnei r4,0
    l.bf 3b
    l.nop
    l.mfspr r11,r3,0
    l.sub r11,r11,r5
    l.andi r11,r11,0x3fff
    l.jr r9
    l.nop
    .size apx_clock_measure,.-apx_clock_measure

    .balignl 32,0x15000000
