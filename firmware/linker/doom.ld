/* Memory map for the DOOM image.
 *
 * Derived from the SDK's ram.ld (sdk-ram.ld next to this file). The SDK's USB launcher and SPI packer only accept the
 * fixed reservations below, so the layout keeps to them:
 *
 *   0x02180000  RUNTIME    128K  SDK code, libc, libgcc, port/main.c and the SDK's data: everything that runs before the
 *                                overlay is in place. This is all the BootROM loads on an SPI boot.
 *   0x02200000  STACK       16K
 *   0x02220000  EXTRA_RAM  768K  DOOM engine zeroed state: zone, frame buffers, audio blocks (not stored in the image)
 *   0x022e0000  CODEC_RAM 1152K  DOOM engine + port: text, rodata, data. For SPI boot this is an "overlay": the packer
 *                                stores it densely in free flash and apx_spi_overlay_load() (first thing in main) copies
 *                                it here. A USB launch loads it directly. It is one fully initialised segment.
 *   0x02000000  WAD_RAM   1536K  the WAD, read from the SD card (or sent over USB) at start-up. NOT an SDK reservation:
 *                                on a USB launch the old firmware's data is still there and the SDK never looks at it.
 *                                Measured usable (doom-probe).
 *
 * Keeping the code in the overlay bank instead of the extra bank matters for the SPI image: the BootROM loads one
 * contiguous block from 0x02180000, and the 512 KiB hole between the runtime block and the extra bank would be stored as
 * zeros in flash. This way the load block is the 128 KiB runtime window and the 700 KiB of game code costs 700 KiB.
 */
ENTRY(_start)
MEMORY { RUNTIME (rwx) : ORIGIN = 0x02180000, LENGTH = 128K
         STACK (rw) : ORIGIN = 0x02200000, LENGTH = 16K
         LCD_DMA (rw) : ORIGIN = 0x02210000, LENGTH = 64K
         EXTRA_RAM (rwx) : ORIGIN = 0x02220000, LENGTH = 768K
         CODEC_RAM (rw) : ORIGIN = 0x022e0000, LENGTH = 1152K
         WAD_RAM (rw) : ORIGIN = 0x02000000, LENGTH = 1536K }
__heap_size = DEFINED(APX_HEAP_SIZE) ? APX_HEAP_SIZE : 12K;
SECTIONS {
 /* Engine and port: every object built into build/doom/ except main.c, which runs before the overlay is in place. (hwmuldiv.S, which SDK code calls before that,
  * is an SDK object in sdk/src and so already in the runtime window.)
  * One fully initialised segment: the SPI packer stores segments above 0x022e0000 as overlays and requires exactly that. */
 .doom_image : ALIGN(16) {
    __doom_image_start = .;
    EXCLUDE_FILE(*/doom/main.c.o) */doom/*.o(.text .text.* .rodata .rodata.* .data .data.* .sdata .sdata.*)
    /* libgcc (soft float, 64-bit helpers) is only needed once the engine, SD card and display are running: nothing that runs
     * before apx_spi_overlay_load() calls it. libc stays in the runtime window, the USB debug link may call printf during
     * the overlay copy. */
    *libgcc.a:*(.text .text.* .rodata .rodata.*)
    . = ALIGN(16);
    __doom_image_end = .;
 } > CODEC_RAM
 /* Engine zeroed state (zone, frame planes, mixer blocks, engine .bss). NOT cleared by the SDK on an SPI boot:
  * port/main.c clears [__doom_bss_start,__doom_bss_end). Placed so that the SDK's ownership bookkeeping (src/debug.c) and
  * the launcher's zero preflight see it as the extra bank. */
 .doom_bss (NOLOAD) : ALIGN(32) {
    __extra_ram_start = .; __doom_bss_start = .;
    */doom/*.o(.bss .bss.* .sbss .sbss.* COMMON)
    *(.doom_buffer .doom_buffer.*)
    . = ALIGN(32);
    __doom_bss_end = .; __extra_ram_end = .;
 } > EXTRA_RAM
 /* SDK bookkeeping names: the "JPEG buffer" bounds are the code bank (APXD protects it), no large buffer. */
 __jpeg_buffer_start = __doom_image_start;
 __jpeg_buffer_end = __doom_image_end;
 .text : ALIGN(4) { FILL(0x00000015) KEEP(*(.text.entry)) *(.text .text.*) } > RUNTIME
 .rodata : ALIGN(4) { *(.rodata .rodata.*) } > RUNTIME
 .data : ALIGN(4) { __data_start = .; *(.data .data.* .sdata .sdata.*) __data_end = .; } > RUNTIME
 __data_source = LOADADDR(.data);
 .bss (NOLOAD) : ALIGN(8) { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) __bss_end = .; } > RUNTIME
 /* the SPI driver's 4 KiB DMA bounce buffer (section name inherited from the SDK) */
 .large_buffer (NOLOAD) : ALIGN(32) { __large_buffer_start = .; *(.large_buffer) __large_buffer_end = .; } > RUNTIME
 .noinit (NOLOAD) : ALIGN(8) { *(.noinit .noinit.*) } > RUNTIME
 .heap (NOLOAD) : ALIGN(8) { __heap_start = .; . += __heap_size; __heap_end = .; } > RUNTIME
 __runtime_used_end = .;
 .display_dma (NOLOAD) : ALIGN(32) {
    __display_dma_start = .; *(.display_dma) __display_dma_end = .;
 } > LCD_DMA
 /* Symbols only: an output section here would become a PT_LOAD outside the SDK reservations and the launcher would refuse it. */
 __wad_buffer_start = ORIGIN(WAD_RAM);
 __wad_buffer_end = ORIGIN(WAD_RAM) + LENGTH(WAD_RAM);
 .stack (NOLOAD) : { __stack_bottom = .; . += LENGTH(STACK); __stack_top = .; } > STACK
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
ASSERT(__runtime_used_end <= ORIGIN(RUNTIME)+LENGTH(RUNTIME), "SDK exceeds runtime reservation")
ASSERT(__doom_image_end <= ORIGIN(CODEC_RAM)+LENGTH(CODEC_RAM), "DOOM image exceeds the 1152 KiB code bank")
ASSERT(__doom_bss_end <= ORIGIN(EXTRA_RAM)+LENGTH(EXTRA_RAM), "DOOM buffers exceed the 768 KiB extra bank")
