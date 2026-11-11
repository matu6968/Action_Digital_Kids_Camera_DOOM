#ifndef APPOTECH_PHOTO_PREPARE_INTERNAL_H
#define APPOTECH_PHOTO_PREPARE_INTERNAL_H
#include <appotech/printer.h>
#include <stddef.h>
#include <stdint.h>
/* Stock photo preparation stages, ported from the stock Silvergear application (SHA256 9f9b9e62...).
 * Each is compared bit for bit with the real stock instructions on the host
 * (tools/stock_photo_reference.py, tools/test_photo_prepare.py).
 * Buffers are plain 8-bit luma with pitch == width, 0 black, 255 white. No heap. */

/* Called between image rows / tiles by every stage below (weak, default does nothing). The stages take
 * seconds on the 144 MHz core: an application overrides it to service USB, buttons and the reference-tick
 * accounting (apx_ticks() only stays correct if it is called at least every 0.5 s). */
void apx_photo_yield(void);

/* Stock 0206c6a4. Edge-aware sharpen. strength 1..5 (stock Gray uses 4), offset 0..50 added to every
 * pixel (stock Gray uses 0). width and height must be even. Border pixels are scaled by 1.5, as stock does.
 * scratch must hold width*height bytes. */
int apx_photo_sharpen(uint8_t *img,unsigned width,unsigned height,int strength,int offset,uint8_t *scratch,size_t scratch_bytes);

/* Stock 020680c8: contrast-limited adaptive histogram equalisation (tile count nx*ny, <=64 each),
 * including its size checks (return negative, image untouched) and its clip-limit float arithmetic.
 * hist must hold nx*ny*bins uint32 values (bins 0 means 128). */
#define APX_PHOTO_CLAHE_MAX_HIST_BYTES (16u*16u*64u*4u)
int apx_photo_clahe(uint8_t *img,unsigned width,unsigned height,unsigned low,unsigned high,
 unsigned nx,unsigned ny,unsigned bins,float clip,uint32_t *hist,size_t hist_bytes);

/* Stock 020688a8: quantise to `levels` grey levels (stock Gray uses 16) with error diffusion.
 * Error is also spilled below the last rows in the stock code; writes outside the image are dropped. */
void apx_photo_quantise(uint8_t *img,unsigned width,unsigned height,unsigned levels);

/* The stock photo pipelines (0205436c Gray, 02053e0c Dot) for a width x height luma image:
 *  Gray: sharpen(4,0), clahe(12x12 tiles, 64 bins, clip 1.5) when the size allows it, quantise(16).
 *  Dot : clahe(8x8 tiles, 64 bins, clip 2.0) for the sizes stock accepts, otherwise untouched.
 * scratch must hold width*height bytes (it also holds the CLAHE tables). Returns 0, or a negative
 * error for invalid arguments or too small scratch. */
int apx_photo_prepare(enum apx_printer_mode mode,uint8_t *img,unsigned width,unsigned height,void *scratch,size_t scratch_bytes);
/* Bilinear luma scaler (16.16 fixed point, no float) used to bring a camera frame to the stock photo
 * geometry (512x384) before apx_photo_prepare(). Source and destination must not overlap. */
int apx_photo_scale(const uint8_t *src,unsigned sw,unsigned sh,unsigned spitch,uint8_t *dst,unsigned dw,unsigned dh);
#endif
