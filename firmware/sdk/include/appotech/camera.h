#ifndef APPOTECH_CAMERA_H
#define APPOTECH_CAMERA_H
#include <stdint.h>
#include <stddef.h>
#define APX_CAMERA_WIDTH 640u
#define APX_CAMERA_HEIGHT 480u
#define APX_CAMERA_FRAME_BYTES (640u*480u*3u/2u)
enum apx_camera_state { APX_CAMERA_OFF,APX_CAMERA_READY,APX_CAMERA_CAPTURING,APX_CAMERA_DONE,APX_CAMERA_ERROR };
enum apx_camera_error { APX_CAMERA_OK,APX_CAMERA_INVALID,APX_CAMERA_RANGE,APX_CAMERA_BUSY,APX_CAMERA_TIMEOUT,APX_CAMERA_HARDWARE,APX_CAMERA_UNSUPPORTED,APX_CAMERA_NACK };
struct apx_camera_info {
 uint32_t state,error,sensor_id,width,height,pitch,buffer,bytes,status,ticks,completed,timeouts,i2c_errors;
 uint32_t scaled;
};
/* Action Vision board: independent I2C, sensor clock/reset and DVP receive DMA.
 * A destination owns complete 32-byte aligned DDR cache lines until DONE/ERROR.
 * Native capture produces 640x480 NV21. Scaled capture uses the camera scaler
 * and one-shot output DMA, retaining the running sensor between frames.
 * Scaled output dimensions must be even, 16..640 by 16..480, with a
 * 32-byte-multiple pitch between round_up(width,32) and 640. Buffer capacity
 * must cover pitch*height*3/2. Do not reuse an LCD-owned destination.
 * No vendor firmware callbacks, heap allocation or interrupts. */
int apx_camera_init(void);
int apx_camera_capture_start(void *frame,size_t capacity);
int apx_camera_scaled_start(void *frame,size_t capacity,unsigned width,unsigned height,unsigned pitch);
void apx_camera_poll(void);
void apx_camera_stop(void);
void apx_camera_shutdown(void);
void apx_camera_get_info(struct apx_camera_info *info);
int apx_camera_buffer_busy(const void *buffer,size_t bytes);
#endif
