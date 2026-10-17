#ifndef APPOTECH_CAMERA_FILES_H
#define APPOTECH_CAMERA_FILES_H
#include <stdint.h>
#include <stddef.h>
/* Optional guarded capture, live display preview and JPEG/SD service.
 * Link separately from jpeg_files: both services use the codec RAM arena. */
int apx_camera_files_init(void);
void apx_camera_files_poll(void);
uint32_t apx_camera_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length);
/* Optional application hook: completed 220x176 NV21 frame, before LCD DMA.
 * Do not retain the pointers or start capture/display from this hook. */
void apx_camera_preview_overlay(uint8_t *y,uint8_t *vu,unsigned pitch);
#endif
