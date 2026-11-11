#ifndef APPOTECH_PRINTER_RENDER_INTERNAL_H
#define APPOTECH_PRINTER_RENDER_INTERNAL_H
#include <appotech/printer.h>
/* Streaming stock photo rendering: no heap or second full image. */
struct apx_printer_render {
 const uint8_t *source;
 unsigned source_width,source_height,pitch,width,height,clockwise,mode,photo_curve,next_row;
 uint32_t offset[APX_PRINTER_WIDTH];
 uint8_t row[APX_PRINTER_WIDTH],tone[256];
 int32_t errors[3][APX_PRINTER_WIDTH+4];
};
int apx_printer_render_init(struct apx_printer_render *r,const uint8_t *source,size_t capacity,
 unsigned width,unsigned height,unsigned pitch,unsigned output_width,unsigned output_height,
 unsigned clockwise,unsigned mode,unsigned photo_curve);
int apx_printer_render_row(struct apx_printer_render *r,unsigned y);
unsigned apx_printer_gray_threshold(unsigned plane);
#endif
