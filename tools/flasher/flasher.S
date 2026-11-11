/* Callback entry. The BootROM calls this with the request block's address in r3. The register save area and the
 * stack sit just below that block (save at r3-0x100, stack top at r3-0x110), so the image has no fixed address and
 * the same bytes run from SRAM or DDR. The loader's own stack is never used. */
    .nodelay
    .section .text.entry,"ax",@progbits
    .global _start
_start:
    l.addi r11,r3,-0x100
    l.sw 0(r11),r1
    l.sw 4(r11),r2
    l.sw 8(r11),r9
    l.sw 12(r11),r10
    l.addi r1,r3,-0x110
    l.jal rescue
    l.nop
    l.addi r11,r1,0x10
    l.lwz r1,0(r11)
    l.lwz r2,4(r11)
    l.lwz r9,8(r11)
    l.lwz r10,12(r11)
    l.jr r9
    l.nop
