/* UVC video for APX_USB_MODE_CAMERA: the application API (include/appotech/usb_uvc.h) on top of TinyUSB's video class.
 * One video function per sensor of the board (APX_UVC_STREAMS: streams 0 and 1 on the Silvergear, stream 0 on the Action), each with one streaming interface. The descriptors are in usb_tusb.c.
 *
 * TinyUSB's video class does the negotiation (probe/commit), the interfaces and the controls' plumbing. The frame data does NOT go through its streaming code: that copies
 * every payload (UVC header plus data) into a payload buffer first, and on this CPU such a copy of 2 KB costs 230 us (a cold read from DDR takes about 0.55 us per 16-byte line,
 * and the copy also fills and writes back the destination's cache lines), which capped video at 4.5 MB/s. This file sends payloads straight from the application's buffer:
 * the 2-byte payload header is written in place in the two bytes before the data, and the DCD reads the packets from there (it reads a payload once, into the core's packet RAM). */
#include "tusb.h"
#if CFG_TUD_VIDEO
#include "device/usbd_pvt.h"
#include <appotech/usb_uvc.h>
#include <appotech/camera.h>

static struct {
  unsigned frame_index;
  uint32_t interval_us; /* the committed frame interval */
  uint32_t max_payload; /* the negotiated payload size: UVC header (2 bytes) + data */
  volatile bool busy;
  uint32_t sent, aborted, session;
} stream[APX_UVC_STREAMS] = { [0 ... APX_UVC_STREAMS - 1] = { 1, 66666, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE, false, 0, 0, 0 } };

/* The frame being sent on a stream, payload by payload. */
static struct {
  uint8_t *frame;                 /* first byte of the JPEG; two writable bytes before it carry the header of the first payload */
  uint32_t total, offset, chunk;  /* bytes of the frame (with a pad byte if any), bytes sent, bytes in the payload in flight */
  uint8_t fid;                    /* frame ID bit of the payload header */
  bool inflight;                  /* a payload transfer is queued */
  bool abort_pending;             /* a new COMMIT arrived: give the frame up when the payload in flight is over */
} tx[APX_UVC_STREAMS];

static uint8_t stream_ep(unsigned s) { return (uint8_t)(0x82u + s); } /* video function 0 uses EP2, function 1 EP3 */

static void frame_done(unsigned s, bool aborted) {
  tx[s].inflight = false;
  tx[s].abort_pending = false;
  tx[s].frame = NULL;
  if (aborted) ++stream[s].aborted; else ++stream[s].sent;
  stream[s].busy = false;
}

/* Queue the next payload of the frame: a header in the two bytes before this chunk (for the first chunk the caller's reserved bytes, for later ones the last two bytes of the
 * chunk before, which have already been copied to the core), then the chunk. */
static bool start_payload(unsigned s) {
  uint32_t chunk = stream[s].max_payload - 2u;
  if (chunk > tx[s].total - tx[s].offset) chunk = tx[s].total - tx[s].offset;
  uint8_t *p = tx[s].frame + tx[s].offset - 2u;
  p[0] = 2;                                                                        /* header length */
  p[1] = (uint8_t)(0x80u | tx[s].fid | (tx[s].offset + chunk >= tx[s].total ? 0x02u : 0u)); /* end of header, frame ID, end of frame */
  tx[s].chunk = chunk;
  if (!usbd_edpt_claim(0, stream_ep(s))) return false;
  if (!usbd_edpt_xfer(0, stream_ep(s), p, (uint16_t)(chunk + 2u), false)) { usbd_edpt_release(0, stream_ep(s)); return false; }
  tx[s].inflight = true;
  return true;
}

bool apx_usb_uvc_streaming(unsigned s) { return s < APX_UVC_STREAMS && tud_video_n_streaming(s, 0); }
bool apx_usb_uvc_busy(unsigned s) { return s < APX_UVC_STREAMS && stream[s].busy; }
unsigned apx_usb_uvc_frame_index(unsigned s) { return s < APX_UVC_STREAMS ? stream[s].frame_index : 1; }
uint32_t apx_usb_uvc_frames_sent(unsigned s) { return s < APX_UVC_STREAMS ? stream[s].sent : 0; }
uint32_t apx_usb_uvc_frame_interval_us(unsigned s) { return s < APX_UVC_STREAMS ? stream[s].interval_us : 66666; }
uint32_t apx_usb_uvc_session(unsigned s) { return s < APX_UVC_STREAMS ? stream[s].session : 0; }
uint32_t apx_usb_uvc_frames_aborted(unsigned s) { return s < APX_UVC_STREAMS ? stream[s].aborted : 0; }

int apx_usb_uvc_send_frame(unsigned s, const void *jpeg, size_t bytes) {
  if (s >= APX_UVC_STREAMS || stream[s].busy || !jpeg || bytes < 4 || bytes > 0xfffffu || !tud_video_n_streaming(s, 0)) return -1;
  /* Bulk streaming ends a payload with a short packet; the host (Linux uvcvideo) also ends one that reaches the negotiated size. The frame goes out as payloads of
   * max_payload bytes, the last one shorter, and no zero-length packet: if that last payload is exactly 512, 1024, ... bytes long the host cannot see where it ends and
   * glues the next frame's header onto this frame (seen about once in 300 frames). Pad the frame with one zero byte then; JPEG decoders ignore bytes after EOI. */
  {
    const uint32_t data = stream[s].max_payload - 2u;
    uint32_t last = (uint32_t)bytes % data;
    if (last == 0) last = data;
    if ((last + 2u) % 512u == 0 && last + 2u < stream[s].max_payload) { ((uint8_t *)(uintptr_t)jpeg)[bytes] = 0; ++bytes; }
  }
  tx[s].frame = (uint8_t *)(uintptr_t)jpeg;
  tx[s].total = (uint32_t)bytes;
  tx[s].offset = 0;
  tx[s].fid ^= 1u;
  tx[s].abort_pending = false;
  stream[s].busy = true;
  if (!start_payload(s)) { tx[s].fid ^= 1u; tx[s].frame = NULL; stream[s].busy = false; return -1; }
  return 0;
}

/* A payload finished (or failed). Returns true if the transfer was ours. */
static bool payload_done(unsigned s, xfer_result_t result) {
  if (!tx[s].inflight) return false;
  tx[s].inflight = false;
  if (result != XFER_RESULT_SUCCESS || tx[s].abort_pending) { frame_done(s, true); return true; }
  tx[s].offset += tx[s].chunk;
  if (tx[s].offset >= tx[s].total) { frame_done(s, false); return true; }
  if (!start_payload(s)) frame_done(s, true);
  return true;
}

/* From apx_usb_poll(): a queued payload that no longer exists (the endpoint was halted or reset, which drops it without a completion) gives the frame up. */
void apx_uvc_poll(void) {
  for (unsigned s = 0; s < APX_UVC_STREAMS; ++s)
    if (tx[s].inflight && !usbd_edpt_busy(0, stream_ep(s))) frame_done(s, true);
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, video_probe_and_commit_control_t const *parameters) {
  (void)stm_idx;
  if (ctl_idx >= APX_UVC_STREAMS) return VIDEO_ERROR_NONE;
  stream[ctl_idx].frame_index = parameters->bFrameIndex;
  if (parameters->dwFrameInterval >= 10000u) { /* 100 ns units */
    stream[ctl_idx].interval_us = parameters->dwFrameInterval / 10u;
    struct apx_camera_controls c; /* the exposure cap for "constant frame rate" follows the interval */
    apx_camera_controls_get((enum apx_camera_id)ctl_idx, &c);
    c.frame_interval_us = stream[ctl_idx].interval_us;
    c.blank_lines = 12; /* leave the capture a little time to be re-armed (see apx_camera_controls) */
    apx_camera_controls_set((enum apx_camera_id)ctl_idx, &c);
  }
  if (parameters->dwMaxPayloadTransferSize >= 4 && parameters->dwMaxPayloadTransferSize <= CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE)
    stream[ctl_idx].max_payload = parameters->dwMaxPayloadTransferSize;
  ++stream[ctl_idx].session;
  if (stream[ctl_idx].busy) { /* a new client: the frame of the old one is given up once the payload in flight is over (it cannot be recalled) */
    if (tx[ctl_idx].inflight) tx[ctl_idx].abort_pending = true; else frame_done(ctl_idx, true);
  }
  return VIDEO_ERROR_NONE;
}

/*--------------------------------------------------------------------+
 * Camera terminal and processing unit controls
 *
 * TinyUSB's video class does not answer requests to these entities, so this driver sits in front of it: a class request to the video control interface of a function
 * with an entity ID of 1 (camera terminal) or 3 (processing unit) is handled here, everything else goes to TinyUSB. Function n controls camera n (0 rear, 1 front; a single-sensor board has only camera 0). The
 * values are stored in the camera driver (apx_camera_controls_set) and written to the sensor from the application's main loop (apx_camera_service).
 *--------------------------------------------------------------------*/

enum { ENT_CT = 1, ENT_PU = 3 };
enum { REQ_SET_CUR = 0x01, REQ_GET_CUR = 0x81, REQ_GET_MIN = 0x82, REQ_GET_MAX = 0x83, REQ_GET_RES = 0x84, REQ_GET_LEN = 0x85, REQ_GET_INFO = 0x86, REQ_GET_DEF = 0x87 };
enum { CT_AE_MODE = 0x02, CT_AE_PRIORITY = 0x03, CT_EXPOSURE_ABS = 0x04 };
enum { PU_BRIGHTNESS = 0x02, PU_CONTRAST = 0x03, PU_GAIN = 0x04, PU_WB_TEMP_AUTO = 0x0b };
#define AE_MANUAL 0x01u
#define AE_APERTURE_PRIORITY 0x08u /* what hosts call "auto mode" */

static uint8_t ctl_buf[8] TU_ATTR_ALIGNED(4);
static int ui_brightness[APX_UVC_STREAMS]; /* the brightness the host last set (-100..100); the sensor register is not exactly invertible */

/* Brightness -100..+100 <-> the sensor's luma offset register (0..255, higher is darker; 64 is neutral). */
static uint8_t brightness_to_reg(int b) { return (uint8_t)(b >= 0 ? 64 - b * 64 / 100 : 64 + (-b) * 191 / 100); }
/* Exposure time in the unit of the control (100 us) <-> sensor lines. */
static unsigned exposure_to_lines(uint32_t u) { return (unsigned)((uint64_t)u * 100000u / APX_CAMERA_LINE_NS); }
static uint32_t lines_to_exposure(unsigned lines) { return (uint32_t)((uint64_t)lines * APX_CAMERA_LINE_NS / 100000u); }

struct limits { int32_t min, max, res, def; uint8_t length; };
static bool control_limits(uint8_t entity, uint8_t selector, struct limits *l) {
  if (entity == ENT_CT) {
    switch (selector) {
      case CT_AE_MODE: *l = (struct limits){ 0, 0, AE_MANUAL | AE_APERTURE_PRIORITY, AE_APERTURE_PRIORITY, 1 }; return true; /* RES = the modes supported */
      case CT_AE_PRIORITY: *l = (struct limits){ 0, 1, 1, 1, 1 }; return true;
      case CT_EXPOSURE_ABS: *l = (struct limits){ 7, 1200, 1, 333, 4 }; return true;
    }
  } else if (entity == ENT_PU) {
    switch (selector) {
      case PU_BRIGHTNESS: *l = (struct limits){ -100, 100, 1, 0, 2 }; return true;
      case PU_CONTRAST: *l = (struct limits){ 0, 160, 1, 136, 2 }; return true;
      case PU_GAIN: *l = (struct limits){ 32, 240, 1, 240, 2 }; return true;
      case PU_WB_TEMP_AUTO: *l = (struct limits){ 0, 1, 1, 1, 1 }; return true;
    }
  }
  return false;
}

static int32_t control_get(unsigned cam, uint8_t entity, uint8_t selector) {
  struct apx_camera_controls c;
  apx_camera_controls_get((enum apx_camera_id)cam, &c);
  if (entity == ENT_CT) return selector == CT_AE_MODE ? (c.auto_exposure ? (int32_t)AE_APERTURE_PRIORITY : (int32_t)AE_MANUAL) : selector == CT_AE_PRIORITY ? c.dynamic_frame_rate : (int32_t)lines_to_exposure(c.exposure_lines);
  switch (selector) {
    case PU_BRIGHTNESS: return ui_brightness[cam];
    case PU_CONTRAST: return c.contrast;
    case PU_GAIN: return c.gain;
    default: return c.auto_white_balance;
  }
}

static bool control_set(unsigned cam, uint8_t entity, uint8_t selector, int32_t v) {
  struct limits l;
  if (!control_limits(entity, selector, &l)) return false;
  struct apx_camera_controls c;
  apx_camera_controls_get((enum apx_camera_id)cam, &c);
  if (entity == ENT_CT) {
    if (selector == CT_AE_MODE) { if (v != (int32_t)AE_MANUAL && v != (int32_t)AE_APERTURE_PRIORITY) return false; c.auto_exposure = v == (int32_t)AE_APERTURE_PRIORITY; }
    else if (selector == CT_AE_PRIORITY) c.dynamic_frame_rate = v != 0;
    else { if (v < l.min || v > l.max) return false; c.exposure_lines = (uint16_t)exposure_to_lines((uint32_t)v); }
  } else {
    if (v < l.min || v > l.max) return false;
    switch (selector) {
      case PU_BRIGHTNESS: ui_brightness[cam] = (int)v; c.brightness = brightness_to_reg((int)v); break;
      case PU_CONTRAST: c.contrast = (uint8_t)v; break;
      case PU_GAIN: c.gain = (uint8_t)v; break;
      default: c.auto_white_balance = v != 0; break;
    }
  }
  if (!c.frame_interval_us) c.frame_interval_us = stream[cam].interval_us;
  apx_camera_controls_set((enum apx_camera_id)cam, &c);
  return true;
}

static bool entity_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request, unsigned cam, uint8_t entity) {
  const uint8_t selector = tu_u16_high(request->wValue);
  struct limits l;
  if (!control_limits(entity, selector, &l)) return false;
  if (request->bRequest == REQ_SET_CUR) {
    if (request->wLength != l.length) return false;
    if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, ctl_buf, l.length);
    if (stage == CONTROL_STAGE_ACK) {
      int32_t v = 0;
      if (l.length == 4) v = (int32_t)(ctl_buf[0] | ctl_buf[1] << 8 | ctl_buf[2] << 16 | (uint32_t)ctl_buf[3] << 24);
      else if (l.length == 2) v = (int16_t)(ctl_buf[0] | ctl_buf[1] << 8);
      else v = ctl_buf[0];
      (void)control_set(cam, entity, selector, v);
    }
    return true;
  }
  if (stage != CONTROL_STAGE_SETUP) return true;
  int32_t v;
  uint8_t length = l.length;
  switch (request->bRequest) {
    case REQ_GET_CUR: v = control_get(cam, entity, selector); break;
    case REQ_GET_DEF: v = l.def; break;
    case REQ_GET_MIN: if (entity == ENT_CT && selector == CT_AE_MODE) return false; v = l.min; break; /* the mode control has no range */
    case REQ_GET_MAX: if (entity == ENT_CT && selector == CT_AE_MODE) return false; v = l.max; break;
    case REQ_GET_RES: v = l.res; break;
    case REQ_GET_INFO: v = 0x03; length = 1; break; /* supports GET and SET */
    case REQ_GET_LEN: v = l.length; length = 2; break;
    default: return false;
  }
  for (unsigned i = 0; i < length; ++i) ctl_buf[i] = (uint8_t)((uint32_t)v >> (8 * i));
  return tud_control_xfer(rhport, request, ctl_buf, tu_min16(length, request->wLength));
}

static bool uvc_control_xfer(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS && request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
    const uint8_t entity = tu_u16_high(request->wIndex), itf = (uint8_t)tu_u16_low(request->wIndex);
    if ((entity == ENT_CT || entity == ENT_PU) && itf % 2 == 0 && itf / 2 < APX_UVC_STREAMS) return entity_control(rhport, stage, request, itf / 2u, entity);
  }
  return videod_control_xfer_cb(rhport, stage, request);
}

static bool uvc_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  for (unsigned s = 0; s < APX_UVC_STREAMS; ++s)
    if (ep_addr == stream_ep(s)) { if (payload_done(s, result)) return true; break; }
  return videod_xfer_cb(rhport, ep_addr, result, xferred_bytes);
}

static const usbd_class_driver_t uvc_driver = {
  .name = "UVC", .init = videod_init, .deinit = videod_deinit, .reset = videod_reset, .open = videod_open,
  .control_xfer_cb = uvc_control_xfer, .xfer_cb = uvc_xfer_cb, .xfer_isr = NULL, .sof = NULL };

usbd_class_driver_t const *apx_uvc_driver(void) { return &uvc_driver; }
#endif

#ifdef APX_DCD_DIAG
#include <stdio.h>
void apx_uvc_diag(void) {
  for (unsigned s = 0; s < APX_UVC_STREAMS; ++s)
    printf("uvc s%u: busy %d inflight %d abort %d offset %lu/%lu chunk %lu edpt_busy %d max_payload %lu session %lu sent %lu aborted %lu\n", s, stream[s].busy, tx[s].inflight, tx[s].abort_pending, (unsigned long)tx[s].offset,
           (unsigned long)tx[s].total, (unsigned long)tx[s].chunk, usbd_edpt_busy(0, stream_ep(s)), (unsigned long)stream[s].max_payload, (unsigned long)stream[s].session, (unsigned long)stream[s].sent, (unsigned long)stream[s].aborted);
}
#endif
