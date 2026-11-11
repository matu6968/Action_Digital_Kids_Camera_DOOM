#ifndef APPOTECH_USB_UVC_H
#define APPOTECH_USB_UVC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "board.h"

/* UVC video in APX_USB_MODE_CAMERA (USB_STACK=tinyusb, APX_USB_UVC=1): one video function per sensor of the board (APX_CAMERA_COUNT: two on the Silvergear, one on the Action, like
 * most webcams), so with two sensors the host sees two video devices and chooses the camera by opening one of them. Each is one MJPEG stream with bulk transfers (stream 0 on EP2,
 * stream 1 on EP3), next to a mass-storage interface on EP1. Stream n belongs to camera n: stream 0 is the rear sensor, stream 1 the front one (examples/uvc-camera). The application produces JPEG frames and hands them over; they are split into UVC payloads and sent straight
 * from the application's buffer (no copy), the payload headers being written in place.
 *
 * Frame sizes offered on each: index 1 = 640x480 (the default), index 2 = 320x240, any frame interval from 33 ms (30 fps) to 1 s, default 15 fps.
 * Camera controls: each function's camera terminal and processing unit offer auto-exposure mode, auto-exposure priority, exposure time, brightness, contrast, gain and
 * white balance auto. They are stored per camera (apx_camera_controls_*, include/appotech/camera.h); the application must call apx_camera_service() from its main loop.
 * Everything runs inside apx_usb_poll(); apx_usb_uvc_send_frame() only queues. */
#define APX_UVC_STREAMS APX_CAMERA_COUNT
#define APX_UVC_FRAME_640x480 1
#define APX_UVC_FRAME_320x240 2

/* The host has opened the stream (VS_COMMIT) and wants frames. */
bool apx_usb_uvc_streaming(unsigned stream);
/* A frame is still being sent on this stream: its buffer is in use until this turns false. */
bool apx_usb_uvc_busy(unsigned stream);
/* Queue one complete JPEG (SOI..EOI). The frame is sent straight from its buffer, without a copy, so the buffer must stay valid and unchanged until apx_usb_uvc_busy() is false and
 * must have TWO WRITABLE BYTES BEFORE the JPEG (the payload header is written there, and the same two bytes of every later payload's start inside the frame are overwritten once
 * the data before them has gone out) and one spare byte after it (padding when the last payload would otherwise end exactly on a packet boundary). A frame that is not 32-byte
 * aligned is fine; start the JPEG at a 32-byte offset into its buffer and the encoder can write there directly. 0 = queued, -1 = not streaming, busy, or a bad size. */
int apx_usb_uvc_send_frame(unsigned stream, const void *jpeg, size_t bytes);
/* The frame interval the host committed, in microseconds (default 66666 = 15 fps). The application sends frames no faster than this. */
uint32_t apx_usb_uvc_frame_interval_us(unsigned stream);
/* The frame size the host committed: APX_UVC_FRAME_*. */
unsigned apx_usb_uvc_frame_index(unsigned stream);
/* Counts the host's VS_COMMIT requests: it changes each time a client starts (or restarts) streaming. Bulk streaming has no "stop" signal, so an application
 * that wants to idle when nobody reads sees busy stay true for a long time, then a new session value when someone comes back. */
uint32_t apx_usb_uvc_session(unsigned stream);
/* Frames completely sent since the start. */
uint32_t apx_usb_uvc_frames_sent(unsigned stream);
/* Frames dropped by the host's restart or a halt. */
uint32_t apx_usb_uvc_frames_aborted(unsigned stream);
#endif
