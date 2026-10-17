#ifndef APPOTECH_DISPLAY_H
#define APPOTECH_DISPLAY_H
#include <stdint.h>
#define APX_DISPLAY_NATIVE_WIDTH 176u
#define APX_DISPLAY_NATIVE_HEIGHT 220u
/* Default logical dimensions for this board's landscape mounting. */
#define APX_DISPLAY_WIDTH 220u
#define APX_DISPLAY_HEIGHT 176u
#define APX_RGB565(r,g,b) ((uint16_t)((((r)&255u)>>3)<<11 | (((g)&255u)>>2)<<5 | (((b)&255u)>>3)))
enum apx_display_state {
    APX_DISPLAY_OFF, APX_DISPLAY_INITIALIZING, APX_DISPLAY_READY,
    APX_DISPLAY_BUSY, APX_DISPLAY_ERROR
};
enum apx_display_rotation {
    APX_DISPLAY_ROTATE_0, APX_DISPLAY_ROTATE_90,
    APX_DISPLAY_ROTATE_180, APX_DISPLAY_ROTATE_270
};
/* Logical coordinates after rotation. Poll alongside apx_debug_poll().
 * Initialization resets the panel and takes approximately 0.5 seconds.
 * No vendor calls, interrupts, heap or framebuffer allocation. */
void apx_display_init(void);
void apx_display_poll(void);
enum apx_display_state apx_display_status(void);
void apx_display_shutdown(void);
void apx_display_backlight(int enabled);
/* Reject while initializing/busy; takes effect on the next fill/blit.
 * Board default is ROTATE_90: logical (x,y) maps to GRAM (y,219-x).
 * Rotation uses panel address traversal, with no framebuffer conversion. */
int apx_display_set_rotation(enum apx_display_rotation rotation);
enum apx_display_rotation apx_display_get_rotation(void);
unsigned apx_display_width(void);
unsigned apx_display_height(void);
/* Return 0 when queued, -1 on invalid bounds/stride/busy/not ready.
 * RGB565 words are CPU-native endian; the bus sends the high byte first.
 * The source must remain valid and unchanged until READY or ERROR.
 * stride_pixels is the distance between row starts, including padding. */
int apx_display_blit(unsigned x,unsigned y,unsigned width,unsigned height,
                     const uint16_t *pixels,unsigned stride_pixels);
/* RGB565 in DDR, even width/height. Converts two rows per poll into a complete
 * NV21 frame, then uses one compositor/LCD DMA transfer. The linker reserves
 * 64 KiB at 0x02210000 for this static workspace; no heap allocation.
 * Conversion subsamples chroma in 2x2 blocks. Source lifetime matches blit. */
int apx_display_blit_dma(unsigned x,unsigned y,unsigned width,unsigned height,
                        const uint16_t *pixels);
/* One DMA frame from full-range BT.601 NV21: Y and interleaved V,U planes.
 * Even width/height, 32-byte aligned DDR planes and byte pitch divisible by 32.
 * Each plane must span pitch*height and pitch*height/2 respectively.
 * Both buffers must stay unchanged until READY or ERROR. */
int apx_display_blit_nv21(unsigned x,unsigned y,unsigned width,unsigned height,
                         const uint8_t *luma,const uint8_t *chroma,unsigned pitch_bytes);
int apx_display_fill(unsigned x,unsigned y,unsigned width,unsigned height,uint16_t color);
uint32_t apx_display_pixels_sent(void);
uint32_t apx_display_timeouts(void);
uint32_t apx_display_dma_transfers(void);
uint32_t apx_display_last_transfer_ticks(void);
#endif
