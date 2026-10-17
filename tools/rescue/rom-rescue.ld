ENTRY(_start)
MEMORY { SRAM (rwx) : ORIGIN = 0x2000, LENGTH = 0x1000 }
SECTIONS {
 .text : { KEEP(*(.text.entry)) *(.text .text.*) *(.rodata .rodata.*) } > SRAM
 .data : { *(.data .data.*) } > SRAM
 .bss (NOLOAD) : { *(.bss .bss.* COMMON) } > SRAM
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
