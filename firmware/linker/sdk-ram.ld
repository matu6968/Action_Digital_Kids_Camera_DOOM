ENTRY(_start)
MEMORY { RUNTIME (rwx) : ORIGIN = 0x02180000, LENGTH = 128K
         STACK (rw) : ORIGIN = 0x02200000, LENGTH = 16K
         LCD_DMA (rw) : ORIGIN = 0x02210000, LENGTH = 64K
         EXTRA_RAM (rwx) : ORIGIN = 0x02220000, LENGTH = 768K
         CODEC_RAM (rw) : ORIGIN = 0x022e0000, LENGTH = 1152K }
__heap_size = DEFINED(APX_HEAP_SIZE) ? APX_HEAP_SIZE : 16K;
SECTIONS {
 .lvgl_code : ALIGN(4) { *lv_*.o(.text .text.*) } > CODEC_RAM
 .lvgl_tables : ALIGN(4) { *lv_*.o(.rodata .rodata.*) *img_*.o(.rodata .rodata.*) } > CODEC_RAM
 .peripheral_code : ALIGN(4) { *uart.o(.text .text.*) *adc.o(.text .text.*) *battery.o(.text .text.*) *spi.o(.text .text.*) } > EXTRA_RAM
 .peripheral_tables : ALIGN(4) { *uart.o(.rodata .rodata.*) *adc.o(.rodata .rodata.*) *battery.o(.rodata .rodata.*) *spi.o(.rodata .rodata.*) __peripheral_code_end = .; } > EXTRA_RAM
 .camera_code : ALIGN(4) { *camera.o(.text .text.*) *camera_files.o(.text .text.*) } > EXTRA_RAM
 .camera_tables : ALIGN(4) { *camera.o(.rodata .rodata.*) *camera_files.o(.rodata .rodata.*) __camera_code_end = .; } > EXTRA_RAM
 .audio_code : ALIGN(4) {
    *audio.o(.text .text.*) *audio_files.o(.text .text.*) *microphone.o(.text .text.*) *microphone_files.o(.text .text.*)
 } > EXTRA_RAM
 .audio_tables : ALIGN(4) {
    *audio.o(.rodata .rodata.*) *audio_files.o(.rodata .rodata.*) *microphone.o(.rodata .rodata.*) *microphone_files.o(.rodata .rodata.*) __audio_code_end = .;
 } > EXTRA_RAM
 .jpeg_code : ALIGN(4) {
    *image.o(.text .text.*) *jpeg.o(.text .text.*) *jpeg_files.o(.text .text.*)
 } > EXTRA_RAM
 .jpeg_tables : ALIGN(4) {
    *image.o(.rodata .rodata.*) *jpeg.o(.rodata .rodata.*) *jpeg_files.o(.rodata .rodata.*) __jpeg_code_end = .;
 } > EXTRA_RAM
 .text : ALIGN(4) { FILL(0x00000015) KEEP(*(.text.entry)) *(.text .text.*) } > RUNTIME
 .fatfs_tables : ALIGN(4) {
    __fatfs_tables_start = .; *ffunicode.o(.rodata .rodata.*) __fatfs_tables_end = .;
 } > EXTRA_RAM
 .large_buffer (NOLOAD) : ALIGN(32) {
    __large_buffer_start = .; *(.large_buffer) __large_buffer_end = .;
 } > EXTRA_RAM
 __extra_ram_start = ORIGIN(EXTRA_RAM);
 __extra_ram_end = MAX(__peripheral_code_end,MAX(MAX(MAX(__fatfs_tables_end,__large_buffer_end),__jpeg_code_end),MAX(__audio_code_end,__camera_code_end)));
 .rodata : ALIGN(4) { *(.rodata .rodata.*) } > RUNTIME
 .data : ALIGN(4) { __data_start = .; *(.data .data.* .sdata .sdata.*) __data_end = .; } > RUNTIME
 __data_source = LOADADDR(.data);
 .bss (NOLOAD) : ALIGN(8) { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) __bss_end = .; } > RUNTIME
 .noinit (NOLOAD) : ALIGN(8) { *(.noinit .noinit.*) } > RUNTIME
 .heap (NOLOAD) : ALIGN(8) { __heap_start = .; . += __heap_size; __heap_end = .; } > RUNTIME
 __runtime_used_end = .;
 .display_dma (NOLOAD) : ALIGN(32) {
    __display_dma_start = .; *(.display_dma) __display_dma_end = .;
 } > LCD_DMA
 .jpeg_buffer (NOLOAD) : ALIGN(32) {
    __jpeg_buffer_start = .; *(.jpeg_buffer) __jpeg_buffer_end = .;
 } > CODEC_RAM
 .stack (NOLOAD) : { __stack_bottom = .; . += LENGTH(STACK); __stack_top = .; } > STACK
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
ASSERT(__runtime_used_end <= ORIGIN(RUNTIME)+LENGTH(RUNTIME), "SDK exceeds runtime reservation")
