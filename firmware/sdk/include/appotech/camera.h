#ifndef APPOTECH_CAMERA_H
#define APPOTECH_CAMERA_H
#include <stdint.h>
#include <stddef.h>
#define APX_CAMERA_WIDTH 640u
#define APX_CAMERA_HEIGHT 480u
#define APX_CAMERA_FRAME_BYTES (640u*480u*3u/2u)
enum apx_camera_id { APX_CAMERA_REAR=0,APX_CAMERA_FRONT=1 };
enum apx_camera_state { APX_CAMERA_OFF,APX_CAMERA_READY,APX_CAMERA_CAPTURING,APX_CAMERA_DONE,APX_CAMERA_ERROR };
enum apx_camera_error { APX_CAMERA_OK,APX_CAMERA_INVALID,APX_CAMERA_RANGE,APX_CAMERA_BUSY,APX_CAMERA_TIMEOUT,APX_CAMERA_HARDWARE,APX_CAMERA_UNSUPPORTED,APX_CAMERA_NACK };
struct apx_camera_info {
 uint32_t state,error,sensor_id,width,height,pitch,buffer,bytes,status,ticks,completed,timeouts,i2c_errors;
 uint32_t scaled,selected,count;
 uint32_t native_width,native_height;
};
/* Board-selected independent I2C, sensor clock/reset and DVP receive DMA.
 * A destination owns complete 32-byte aligned DDR cache lines until DONE/ERROR.
 * Native capture produces 640x480 NV21. Scaled capture uses the camera scaler
 * and one-shot output DMA, retaining the running sensor between frames.
 * Scaled output dimensions must be even, 16..640 by 16..480, with a
 * 32-byte-multiple pitch between round_up(width,32) and 640. Buffer capacity
 * must cover pitch*height*3/2. Do not reuse an LCD-owned destination.
 * No vendor firmware callbacks, heap allocation or interrupts. */
int apx_camera_init(void);
unsigned apx_camera_count(void);
/* Stops the previous idle sensor; rejects selection during capture or printing. */
int apx_camera_select(enum apx_camera_id id);
int apx_camera_capture_start(void *frame,size_t capacity);
int apx_camera_scaled_start(void *frame,size_t capacity,unsigned width,unsigned height,unsigned pitch);
/* Direct access to the selected sensor's registers (ID 0x39: pages 0..2, registers 0..255). The camera must be initialised; works during a scaled capture. Returns 0 or an
 * apx_camera_error (APX_CAMERA_NACK when the sensor does not answer). */
int apx_camera_sensor_read(unsigned page,unsigned reg,unsigned *value);
int apx_camera_sensor_write(unsigned page,unsigned reg,unsigned value);

/* Image controls of the ID 0x39 sensor (the register meanings were found by experiment, research/camera-controls.md). They belong to a camera, survive switching to
 * the other one (the sensor is set up again at every select and the stored values are written back), and are written to the sensor by apx_camera_service(): the setter
 * only stores, so it may be called from anywhere (a USB request handler), while the service call must run in the main loop, where it cannot interrupt a sensor bus
 * transaction. A line of the sensor takes about 66.6 us, a frame at least 500 lines (30 frames per second); with auto exposure the frame grows with the exposure. */
#define APX_CAMERA_LINE_NS 66600u
struct apx_camera_controls {
 uint8_t auto_exposure;       /* 1 (default): the sensor chooses the exposure. 0: exposure_lines is used */
 uint8_t auto_white_balance;  /* 1 (default) or 0 (fixed gains, the colours shift) */
 uint8_t dynamic_frame_rate;  /* 1 (default): in low light the frame may take up to 80 ms (12.4 fps). 0: the exposure is capped so that a frame fits frame_interval_us */
 uint8_t gain;                /* 32..240, default 240: scales the brightness at a given exposure */
 uint8_t contrast;            /* 0..160, default 136: luma gain (saturates above 160) */
 uint8_t brightness;          /* 0..255, default 64: luma offset, HIGHER IS DARKER (about 0.2 luma per step) */
 uint8_t blank_lines;        /* extra dummy lines at the end of every frame (default 0; each takes 66.6 us). The one-shot capture has to be re-armed in the blank after the last line:
                               at 500 lines per frame that is only about 1.3 ms, and a late re-arm costs a whole frame (15 instead of 30 frames per second at 640x480).
                               A few lines are enough; 12 leaves room for a busy main loop */
 uint16_t exposure_lines;     /* manual exposure in sensor lines (1..2047), used when auto_exposure is 0 */
 uint32_t frame_interval_us;  /* wanted frame interval, 0 = none; only used for the exposure cap when dynamic_frame_rate is 0 */
};
void apx_camera_controls_default(struct apx_camera_controls *c);
void apx_camera_controls_get(enum apx_camera_id id,struct apx_camera_controls *c);
int apx_camera_controls_set(enum apx_camera_id id,const struct apx_camera_controls *c);
/* Write changed controls of the selected, initialised camera to the sensor. Cheap when nothing changed. Returns 0 or an apx_camera_error. */
int apx_camera_service(void);
void apx_camera_poll(void);
void apx_camera_stop(void);
void apx_camera_shutdown(void);
void apx_camera_get_info(struct apx_camera_info *info);
int apx_camera_buffer_busy(const void *buffer,size_t bytes);
#endif
