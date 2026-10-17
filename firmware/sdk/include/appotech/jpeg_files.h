#ifndef APPOTECH_JPEG_FILES_H
#define APPOTECH_JPEG_FILES_H
#include <stddef.h>
#include <stdint.h>
/* Optional SD/display integration, with a linker-owned 1152 KiB JPEG arena.
 * Call init once, then poll with debug/display/buttons. Commands start jobs;
 * query their result after completion. Originals are never overwritten.
 * Hardware baseline or stb software decoding fits the image without changing
 * aspect. Progressive JPEG paging creates private temporary SD files; generic
 * image formats require enough unused arena RAM for their decoded pixels.
 * Sample encode scales a deterministic 220x176 NV21 pattern in hardware. */
void apx_jpeg_files_init(void);
void apx_jpeg_files_poll(void);
uint32_t apx_jpeg_preview_file(const char *path);
/* Hardware decoding retains its full size range. Slow software images require
 * APX_IMAGE_ALLOW_SLOW from <appotech/image.h>, explicitly for this job. */
uint32_t apx_jpeg_preview_file_ex(const char *path,unsigned flags);
uint32_t apx_jpeg_encode_sample_file(const char *path,unsigned width,unsigned height,unsigned selector);
uint32_t apx_jpeg_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length);
#endif
