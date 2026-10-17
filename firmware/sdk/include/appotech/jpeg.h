#ifndef APPOTECH_JPEG_H
#define APPOTECH_JPEG_H
#include <stddef.h>
#include <stdint.h>
enum apx_jpeg_state { APX_JPEG_IDLE,APX_JPEG_DECODING,APX_JPEG_ENCODING,APX_JPEG_DONE,APX_JPEG_ERROR };
enum apx_jpeg_error { APX_JPEG_OK,APX_JPEG_INVALID,APX_JPEG_UNSUPPORTED,APX_JPEG_RANGE,APX_JPEG_BUSY,APX_JPEG_HARDWARE,APX_JPEG_TIMEOUT,APX_JPEG_SLOW };
struct apx_jpeg_info { uint32_t state,error,status,width,height,output_width,output_height,pitch,bytes,ticks; };
/* Baseline, eight-bit, three-component interleaved JPEG, 4:2:0 or 4:2:2.
 * Input is aligned DDR with 32 zero guard bytes after the JPEG; output is full-range NV21 with 32-byte row pitch.
 * Buffers remain owned by the engine until DONE/ERROR; poll cooperatively.
 * No vendor firmware calls. */
int apx_jpeg_inspect(const void *jpeg,size_t bytes,struct apx_jpeg_info *info);
/* Decoded output is limited to 224x220; source dimensions up to 2048x1536. */
int apx_jpeg_decode_start(const void *jpeg,size_t bytes,void *output,size_t capacity,unsigned width,unsigned height);
/* Encode an NV21 frame; the engine scales it to the requested dimensions.
 * Input pitch must equal round_up(source_width,32); planes are aligned DDR.
 * Target must be at least as large as source; downscaling is rejected.
 * selector 8 initially selects the stock unity quantizer setting. */
int apx_jpeg_encode_start(const void *luma,const void *chroma,unsigned source_width,unsigned source_height,
                         unsigned pitch,unsigned width,unsigned height,unsigned selector,
                         void *jpeg,size_t capacity);
void apx_jpeg_poll(void);
void apx_jpeg_shutdown(void);
void apx_jpeg_get_info(struct apx_jpeg_info *info);
#endif
