    .nodelay
    .section .text.entry,"ax",@progbits
    .global _start
_start:
    l.ori r11,r0,0x3800
    l.sw 0(r11),r1
    l.sw 4(r11),r2
    l.sw 8(r11),r9
    l.sw 12(r11),r10
    l.ori r1,r0,0x37f0
    l.jal rescue
    l.nop
    l.ori r11,r0,0x3800
    l.lwz r1,0(r11)
    l.lwz r2,4(r11)
    l.lwz r9,8(r11)
    l.lwz r10,12(r11)
    l.jr r9
    l.nop
