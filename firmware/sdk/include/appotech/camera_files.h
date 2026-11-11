#ifndef APPOTECH_CAMERA_FILES_H
#define APPOTECH_CAMERA_FILES_H
#include <stdint.h>
#include <stddef.h>
/* Optional guarded capture, live display preview and JPEG/SD service.
 * Link separately from jpeg_files: both services use the codec RAM arena. */
int apx_camera_files_init(void);
/* Initialize guarded service buffers only; leave sensors and preview off.
 * Useful for a host-controlled bring-up after the APXD build handshake. */
void apx_camera_files_prepare(void);
void apx_camera_files_poll(void);
/* Stop capture and drain LCD ownership before releasing shared printer pins. */
int apx_camera_files_suspend(void);
int apx_camera_files_raw_frame(const uint8_t **frame,size_t *bytes);
/* Repaint the application overlay on a frozen frame once LCD DMA is idle.
 * The original camera luma/chroma remains unchanged. Live preview repaints itself. */
void apx_camera_files_redraw(void);
uint32_t apx_camera_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length);
/* Optional hook before preview is enabled; default does nothing. */
void apx_camera_preview_start(void);
/* Optional application hook: completed board-sized NV21 frame, before LCD DMA.
 * Do not retain the pointers or start capture/display from this hook. */
void apx_camera_preview_overlay(uint8_t *y,uint8_t *vu,unsigned pitch);
#endif
