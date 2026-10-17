#ifndef APPOTECH_IMAGE_H
#define APPOTECH_IMAGE_H
#include <appotech/jpeg.h>
struct apx_image_info {
 struct apx_jpeg_info image;
 uint32_t progressive,scans,backing_files,backing_reads,backing_writes,workspace_used,backing_retries;
 uint32_t slow_reasons;
};
#define APX_IMAGE_ALLOW_SLOW 1u
#define APX_IMAGE_SLOW_DIMENSIONS 1u
#define APX_IMAGE_SLOW_INPUT 2u
#define APX_IMAGE_SLOW_QUANTIZATION 4u
#define APX_IMAGE_DEFAULT_MAX_PIXELS (220u*176u)
#define APX_IMAGE_DEFAULT_MAX_DIMENSION 220u
#define APX_IMAGE_DEFAULT_MAX_BYTES (128u*1024u)
/* Software stb fallback: progressive/baseline eight-bit JPEG, PNG, BMP, TGA,
 * GIF first frame, PNM. HDR/12-bit/arithmetic JPEG are unsupported.
 * Output: NV21, even dimensions <=224x220. Buffers and explicit workspace
 * remain owned until DONE/ERROR or shutdown. Workspace is a bounded arena,
 * separate from the libc heap. Large JPEG coefficient arrays use temporary
 * create-new SD files, removed on completion/error/cancel. No SPI writes.
 * Poll between scans/output rows. One scan may perform cooperative SD I/O;
 * maintenance runs each MCU row. This is not hardware progressive decode. */
int apx_image_inspect(const void *input,size_t bytes,struct apx_image_info *info);
int apx_image_decode_start(const void *input,size_t bytes,void *output,size_t capacity,
 unsigned width,unsigned height,void *workspace,size_t workspace_bytes);
/* Default limits apply to source dimensions, input size and high-precision
 * JPEG quantization. APX_JPEG_SLOW requires explicit per-job ALLOW_SLOW.
 * Opt-in retains all hard bounds. Rendering can take minutes and page to SD. */
int apx_image_decode_start_ex(const void *input,size_t bytes,void *output,size_t capacity,
 unsigned width,unsigned height,void *workspace,size_t workspace_bytes,unsigned flags);
void apx_image_poll(void);
void apx_image_get_info(struct apx_image_info *info);
void apx_image_shutdown(void);
/* Close temporary backing files before storage unmount/BootROM. */
uint32_t apx_image_prepare_stop(void);
#endif
