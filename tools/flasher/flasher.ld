/* Linked only to check position independence and to give the disassembly real addresses; the shipped image is
 * the same bytes at any base. FLASHER_BASE comes from the Makefile (--defsym). */
ENTRY(_start)
SECTIONS {
 . = FLASHER_BASE;
 .text : { KEEP(*(.text.entry)) *(.text .text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.* .sdata .sdata.*) }
 .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) }
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
ASSERT(SIZEOF(.rodata) == 0 && SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "flasher must not use data: it has to stay position independent")
/* The SRAM copy is uploaded at 0x2000; the SDK's boot marks start at 0x2f00 and must survive the upload. */
ASSERT(SIZEOF(.text) <= 0xf00, "flasher exceeds the SRAM space below the boot marks")
