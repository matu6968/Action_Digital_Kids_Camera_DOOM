#ifndef APPOTECH_PRINTER_IMAGE_INTERNAL_H
#define APPOTECH_PRINTER_IMAGE_INTERNAL_H
#include <appotech/printer.h>
/* Private camera-demo conversion: rotate clockwise before scaling/diffusion.
 * Input remains immutable; scratch requirements match apx_printer_dither(). */
int apx_printer_dither_clockwise(const uint8_t *gray,unsigned width,unsigned height,unsigned pitch,
 uint8_t *raster,size_t capacity,unsigned output_width,unsigned output_height,int16_t *errors,size_t error_cells);
/* Camera photo job, including the board's forward tear-off feed.
 * DONE is reported only after finishing; normal raster/feed APIs are unchanged. */
int apx_printer_submit_photo(const uint8_t *raster,size_t capacity,unsigned width,unsigned height,unsigned stride);
/* Borrow a camera luma frame, rotate clockwise and render the selected mode.
 * The source must stay immutable through printing and finishing. */
int apx_printer_submit_photo_luma(const uint8_t *gray,size_t capacity,unsigned width,unsigned height,unsigned pitch,
 unsigned output_width,unsigned output_height);
/* Diagnostic comparison: curve1 is stock final mapping;2 first squares luma.
 * Both retain the same orientation, pulse weights and finishing feed. */
int apx_printer_submit_photo_luma_curve(const uint8_t *gray,size_t capacity,unsigned width,unsigned height,unsigned pitch,
 unsigned output_width,unsigned output_height,unsigned curve);
#endif
