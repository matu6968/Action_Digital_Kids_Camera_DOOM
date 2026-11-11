/* Native leaf instruction sequences from the verified Silvergear application.
 * Keep prologues/epilogues and separate calls: inlining shortens pad pulses.
 * JT5295: little endian, PC+4 links, no branch delay slots.
 * the leaf bodies were binary-checked against the stock application (the check script was removed with the bring-up tools). */
.nodelay
.text
.balign 4
.macro set_pin name, spr, mask
.global \name
.type \name, @function
\name:
 l.ori r4,r0,\spr
 l.sw -4(r1),r1
 l.addi r1,r1,-4
 l.mfspr r3,r4,0
 l.ori r3,r3,\mask
 l.mtspr r4,r3,0
 l.addi r1,r1,4
 l.lwz r1,-4(r1)
 l.jr r9
.size \name,.-\name
.endm
.macro clear_pin name, spr, inverse
.global \name
.type \name, @function
\name:
 l.ori r4,r0,\spr
 l.sw -4(r1),r2
 l.sw -8(r1),r1
 l.addi r2,r0,\inverse
 l.mfspr r3,r4,0
 l.addi r1,r1,-8
 l.and r3,r3,r2
 l.mtspr r4,r3,0
 l.addi r1,r1,8
 l.lwz r1,-8(r1)
 l.lwz r2,-4(r1)
 l.jr r9
.size \name,.-\name
.endm
set_pin apx_printer_reference_strobe_high,0x8000,0x200
clear_pin apx_printer_reference_strobe_low,0x8000,-513
set_pin apx_printer_reference_latch_high,0x8004,0x20
clear_pin apx_printer_reference_latch_low,0x8004,-33
