/* The SDK's USB API (include/appotech/usb.h) on top of TinyUSB, for USB_STACK=tinyusb.
 *
 * Debug mode is the device src/usb.c always presented: VID:PID APX_USB_VID:APX_USB_PID, one vendor-class interface
 * with a bulk OUT and a bulk IN on endpoint 1, strings "AppoTech SDK" / "JT5295 SDK Debug". apxtool and
 * tools/launch.sh depend on that identity. The APXD pipe is a small class driver of its own rather than TinyUSB's
 * buffered vendor class: apx_usb_send() posts the caller's buffer directly (no copy) and apx_usb_receive() hands
 * out one packet at a time, which is what src/debug.c expects.
 */
#include "tusb.h"
#include "device/usbd_pvt.h"
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/trace.h>
#include "board.h"
#include <string.h>

extern void apx_debug_reset(void);
extern _Noreturn void apx_watchdog_reset(void);

#ifndef APX_USB_ENUM_TIMEOUT_TICKS
#define APX_USB_ENUM_TIMEOUT_TICKS 0 /* diagnostic builds: reset if the host has not configured the device by then */
#endif

/*--------------------------------------------------------------------+
 * Time
 *--------------------------------------------------------------------*/

uint32_t tusb_time_millis_api(void) { return (uint32_t)(((uint64_t)apx_ticks() * 125u) >> 12); } /* 32768 Hz ticks */

/*--------------------------------------------------------------------+
 * Descriptors (debug mode)
 *--------------------------------------------------------------------*/

#define DEVICE_DESCRIPTOR_CLASS(pid, cls, sub, proto) { 18, TUSB_DESC_DEVICE, 0, 2, (cls), (sub), (proto), CFG_TUD_ENDPOINT0_SIZE, \
    APX_USB_VID & 255, APX_USB_VID >> 8, (pid) & 255, (pid) >> 8, 0, 1, 1, 2, 0, 1 }
#define DEVICE_DESCRIPTOR(pid) DEVICE_DESCRIPTOR_CLASS(pid, 0, 0, 0)
static const uint8_t qualifier_descriptor[] = { 10, TUSB_DESC_DEVICE_QUALIFIER, 0, 2, 0, 0, 0, CFG_TUD_ENDPOINT0_SIZE, 1, 0 };

/* One configuration, one interface with a bulk OUT (0x01) and a bulk IN (0x81): the debug link and mass storage differ
 * only in the interface class triple. Both are 32 bytes. */
#define BULK_CONFIG(type, packet, cls, sub, proto) { 9, (type), 32, 0, 1, 1, 0, 0x80, 50, \
    9, TUSB_DESC_INTERFACE, 0, 0, 2, (cls), (sub), (proto), 0, \
    7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_BULK, (packet) & 255, (packet) >> 8, 0, \
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_BULK, (packet) & 255, (packet) >> 8, 0 }
#ifndef APX_USB_PROBE_EPS
#define DEBUG_CONFIG(type, packet) BULK_CONFIG(type, packet, TUSB_CLASS_VENDOR_SPECIFIC, 0, 1)
#endif
#define MSC_CONFIG(type, packet) BULK_CONFIG(type, packet, TUSB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTOCOL_BOT)

#ifdef APX_USB_PROBE_EPS /* hardware probe of endpoints 2..6 (examples/usb-ep-probe): the debug interface plus a second one with five bulk IN endpoints */
#define PROBE_EP(n, packet) 7, TUSB_DESC_ENDPOINT, 0x80 | (n), TUSB_XFER_BULK, (packet) & 255, (packet) >> 8, 0
#define PROBE_CONFIG(type, packet) { 9, (type), 32 + 9 + 5 * 7, 0, 2, 1, 0, 0x80, 50, \
    9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, 0, 1, 0, \
    7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_BULK, (packet) & 255, (packet) >> 8, 0, \
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_BULK, (packet) & 255, (packet) >> 8, 0, \
    9, TUSB_DESC_INTERFACE, 1, 0, 5, TUSB_CLASS_VENDOR_SPECIFIC, 0, 2, 0, \
    PROBE_EP(2, packet), PROBE_EP(3, packet), PROBE_EP(4, packet), PROBE_EP(5, packet), PROBE_EP(6, packet) }
#define DEBUG_CONFIG(type, packet) PROBE_CONFIG(type, packet)
#endif

#if CFG_TUD_VIDEO
/* The camera composite device, laid out like the stock firmware's UVC configuration (an IAD with the video control and streaming interfaces, then mass storage) but
 * with bulk streaming: interface 0 = video control, 1 = video streaming (EP2 IN), then (second sensor, if the board has one: 2 = video control, 3 = video streaming on EP3), mass storage (EP1 IN/OUT), and the microphone. One MJPEG format, two frame sizes, 15 fps. */
#define UVC_CLOCK_FREQUENCY 27000000
#define UVC_ENTITY_CAP_INPUT_TERMINAL 0x01
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL 0x02
#define UVC_ENTITY_PROCESSING_UNIT 0x03
/* Controls offered (handled in src/tusb/uvc.c): camera terminal: auto-exposure mode (D1), auto-exposure priority (D2), exposure time absolute (D3); processing unit:
 * brightness (D0), contrast (D1), gain (D9), white balance temperature auto (D12). */
#define UVC_CT_CONTROLS 0x00000eu
#define UVC_PROCESSING_UNIT_LEN 13
#define UVC_PROCESSING_UNIT(id, src) \
    UVC_PROCESSING_UNIT_LEN, TUSB_DESC_CS_INTERFACE, VIDEO_CS_ITF_VC_PROCESSING_UNIT, (id), (src), 0, 0, 3, 0x03, 0x12, 0x00, 0, 0
#define UVC_FRAME(index, w, h) TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(index, 0, w, h, (w) * (h) * 16, (w) * (h) * 16 * 15, (w) * (h) * 2, 666666, 333333, 10000000, 333333)
/* One video function (IAD + control interface + streaming interface with its bulk IN endpoint). The device has one per sensor (two on the Silvergear, one on the Action), so the host
 * sees a video device per camera and picks the camera by opening one of them. `first` is the control interface number, the streaming interface is `first + 1`. */
#define UVC_FUNCTION(first, ep, packet, stridx) \
    TUD_VIDEO_DESC_IAD((first), 2, (stridx)), \
    TUD_VIDEO_DESC_STD_VC((first), 0, (stridx)), \
      TUD_VIDEO_DESC_CS_VC(0x0150, TUD_VIDEO_DESC_CAMERA_TERM_LEN + UVC_PROCESSING_UNIT_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, UVC_CLOCK_FREQUENCY, (first) + 1), \
      TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, 0, 0, 0, UVC_CT_CONTROLS), \
      UVC_PROCESSING_UNIT(UVC_ENTITY_PROCESSING_UNIT, UVC_ENTITY_CAP_INPUT_TERMINAL), \
      TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0, UVC_ENTITY_PROCESSING_UNIT, 0), \
    TUD_VIDEO_DESC_STD_VS((first) + 1, 0, 1, (stridx)), \
      TUD_VIDEO_DESC_CS_VS_INPUT(1, TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + 2 * TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
        (ep), 0, UVC_ENTITY_CAP_OUTPUT_TERMINAL, 0, 0, 0, 0), \
      TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(1, 2, 0, 1, 0, 0, 0, 0), \
        UVC_FRAME(1, 640, 480), \
        UVC_FRAME(2, 320, 240), \
      TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M), \
      TUD_VIDEO_DESC_EP_BULK((ep), (packet), 1)
#define UVC_FUNCTION_LENGTH (TUD_VIDEO_DESC_IAD_LEN + TUD_VIDEO_DESC_STD_VC_LEN + TUD_VIDEO_DESC_CS_VC_LEN + 1 + TUD_VIDEO_DESC_CAMERA_TERM_LEN + UVC_PROCESSING_UNIT_LEN + \
    TUD_VIDEO_DESC_OUTPUT_TERM_LEN + TUD_VIDEO_DESC_STD_VS_LEN + TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + \
    2 * TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN + 7)
/* A USB audio class 1 microphone: interface `first` = audio control (an input terminal "microphone" feeding an output terminal "USB streaming"), `first + 1` = audio streaming
 * with alternate 0 (no endpoint) and alternate 1 (one isochronous IN endpoint, mono PCM16 at APX_UAC_RATE). Laid out like the stock firmware's (stock: 32 kHz, 64-byte
 * packets, bInterval 4). Terminal 1 = microphone, feature unit 2 = mute + volume + automatic gain control (master channel), terminal 3 = USB streaming. Handled by src/tusb/uac.c. */
#define UAC_RATE 32000
#define UAC_PACKET (UAC_RATE / 1000 * 2 * 2) /* maximum packet: twice the nominal 64 bytes, so a late main loop can catch up (a packet may be shorter than this) */
#define UAC_FUNCTION(first, ep, interval) \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, (first), 2, TUSB_CLASS_AUDIO, 0, 0, 0, \
    9, TUSB_DESC_INTERFACE, (first), 0, 0, TUSB_CLASS_AUDIO, 1, 0, 0, \
    9, TUSB_DESC_CS_INTERFACE, 1, 0x00, 0x01, 39, 0, 1, (first) + 1, \
    12, TUSB_DESC_CS_INTERFACE, 2, 1, 0x01, 0x02, 0, 1, 1, 0, 0, 0, \
    9, TUSB_DESC_CS_INTERFACE, 6, 2, 1, 1, 0x43, 0x00, 0, \
    9, TUSB_DESC_CS_INTERFACE, 3, 3, 0x01, 0x01, 0, 2, 0, \
    9, TUSB_DESC_INTERFACE, (first) + 1, 0, 0, TUSB_CLASS_AUDIO, 2, 0, 0, \
    9, TUSB_DESC_INTERFACE, (first) + 1, 1, 1, TUSB_CLASS_AUDIO, 2, 0, 0, \
    7, TUSB_DESC_CS_INTERFACE, 1, 3, 1, 0x01, 0x00, \
    11, TUSB_DESC_CS_INTERFACE, 2, 1, 1, 2, 16, 1, UAC_RATE & 255, (UAC_RATE >> 8) & 255, UAC_RATE >> 16, \
    9, TUSB_DESC_ENDPOINT, (ep), 0x05, UAC_PACKET, 0, (interval), 0, 0, \
    7, TUSB_DESC_CS_ENDPOINT, 1, 0, 0, 0, 0,
#define UAC_FUNCTION_LENGTH (8 + 9 + 9 + 12 + 9 + 9 + 9 + 9 + 7 + 11 + 9 + 7)
#define VIDEO_FUNCTIONS APX_CAMERA_COUNT
_Static_assert(VIDEO_FUNCTIONS == 1 || VIDEO_FUNCTIONS == 2, "camera composite descriptors: one or two video functions");
#define MSC_INTERFACE (2 * VIDEO_FUNCTIONS)
#if APX_USB_UAC
#define UAC_PART(interval) UAC_FUNCTION(MSC_INTERFACE + 1, 0x84, interval)
#define UAC_EXTRA_LENGTH UAC_FUNCTION_LENGTH
#define CAMERA_INTERFACES (MSC_INTERFACE + 3)
#else
#define UAC_PART(interval)
#define UAC_EXTRA_LENGTH 0
#define CAMERA_INTERFACES (MSC_INTERFACE + 1)
#endif
#define CAMERA_CONFIG_LENGTH (9 + VIDEO_FUNCTIONS * UVC_FUNCTION_LENGTH + 9 + 14 + UAC_EXTRA_LENGTH)
#if VIDEO_FUNCTIONS == 2
#define VIDEO_PARTS(packet) UVC_FUNCTION(0, 0x82, packet, 3), UVC_FUNCTION(2, 0x83, packet, 4),
#else
#define VIDEO_PARTS(packet) UVC_FUNCTION(0, 0x82, packet, 3),
#endif
/* Interfaces: 0/1 video function 0 (camera 0, EP2), 2/3 video function 1 (camera 1, EP3, only with two sensors), then mass storage (EP1), then the microphone (two interfaces, EP4) when
 * APX_USB_UAC. */
#define CAMERA_CONFIG(type, packet, interval) { 9, (type), U16_TO_U8S_LE(CAMERA_CONFIG_LENGTH), CAMERA_INTERFACES, 1, 0, 0x80, 250, \
    VIDEO_PARTS(packet) \
    UAC_PART(interval) \
    9, TUSB_DESC_INTERFACE, MSC_INTERFACE, 0, 2, TUSB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTOCOL_BOT, 0, \
    7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_BULK, (packet) & 255, (packet) >> 8, 0, \
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_BULK, (packet) & 255, (packet) >> 8, 0 }
static const uint8_t camera_device[] = DEVICE_DESCRIPTOR_CLASS(APX_USB_PID_CAMERA, TUSB_CLASS_MISC, MISC_SUBCLASS_COMMON, MISC_PROTOCOL_IAD);
static const uint8_t camera_config_hs[] = CAMERA_CONFIG(TUSB_DESC_CONFIGURATION, 512, 4);
static const uint8_t camera_config_fs[] = CAMERA_CONFIG(TUSB_DESC_CONFIGURATION, 64, 1);
static const uint8_t camera_other_hs[] = CAMERA_CONFIG(TUSB_DESC_OTHER_SPEED_CONFIG, 512, 4);
static const uint8_t camera_other_fs[] = CAMERA_CONFIG(TUSB_DESC_OTHER_SPEED_CONFIG, 64, 1);
_Static_assert(sizeof(camera_config_hs) == CAMERA_CONFIG_LENGTH, "camera configuration descriptor length");
#endif

struct mode_descriptors {
  const uint8_t *device, *config_hs, *config_fs, *other_hs, *other_fs;
  const char *product;
};

static const uint8_t debug_device[] = DEVICE_DESCRIPTOR(APX_USB_PID);
static const uint8_t debug_config_hs[] = DEBUG_CONFIG(TUSB_DESC_CONFIGURATION, 512);
static const uint8_t debug_config_fs[] = DEBUG_CONFIG(TUSB_DESC_CONFIGURATION, 64);
static const uint8_t debug_other_hs[] = DEBUG_CONFIG(TUSB_DESC_OTHER_SPEED_CONFIG, 512);
static const uint8_t debug_other_fs[] = DEBUG_CONFIG(TUSB_DESC_OTHER_SPEED_CONFIG, 64);
#if CFG_TUD_MSC
static const uint8_t msc_device[] = DEVICE_DESCRIPTOR(APX_USB_PID_MSC);
static const uint8_t msc_config_hs[] = MSC_CONFIG(TUSB_DESC_CONFIGURATION, 512);
static const uint8_t msc_config_fs[] = MSC_CONFIG(TUSB_DESC_CONFIGURATION, 64);
static const uint8_t msc_other_hs[] = MSC_CONFIG(TUSB_DESC_OTHER_SPEED_CONFIG, 512);
static const uint8_t msc_other_fs[] = MSC_CONFIG(TUSB_DESC_OTHER_SPEED_CONFIG, 64);
extern void apx_usb_msc_enter(void);
#endif

static const struct mode_descriptors modes[] = {
  [APX_USB_MODE_DEBUG] = { debug_device, debug_config_hs, debug_config_fs, debug_other_hs, debug_other_fs, "JT5295 SDK Debug" },
#if CFG_TUD_VIDEO
  [APX_USB_MODE_CAMERA] = { camera_device, camera_config_hs, camera_config_fs, camera_other_hs, camera_other_fs, "JT5295 SDK Camera" },
#endif
#if CFG_TUD_MSC
  [APX_USB_MODE_MSC] = { msc_device, msc_config_hs, msc_config_fs, msc_other_hs, msc_other_fs, "JT5295 SDK Mass Storage" },
#endif
};
#define MODE_COUNT (sizeof(modes) / sizeof(modes[0]))

static enum apx_usb_mode current_mode = APX_USB_MODE_DEBUG;

static bool high_speed(void) { return tud_speed_get() == TUSB_SPEED_HIGH; }

uint8_t const *tud_descriptor_device_cb(void) { return modes[current_mode].device; }
uint8_t const *tud_descriptor_device_qualifier_cb(void) { return qualifier_descriptor; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return high_speed() ? modes[current_mode].config_hs : modes[current_mode].config_fs;
}
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index) {
  (void)index;
  return high_speed() ? modes[current_mode].other_fs : modes[current_mode].other_hs;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  static uint16_t string[40];
  const char *text;
  (void)langid;
  if (index == 0) {
    string[1] = 0x0409;
    string[0] = (uint16_t)(TUSB_DESC_STRING << 8 | 4);
    return string;
  }
  text = index == 1 ? "AppoTech SDK" : index == 2 ? modes[current_mode].product : index == 3 ? (APX_CAMERA_COUNT > 1 ? "JT5295 Rear Camera" : "JT5295 Camera") : index == 4 && APX_CAMERA_COUNT > 1 ? "JT5295 Front Camera" : NULL;
  if (!text) return NULL;
  const size_t n = strlen(text);
  for (size_t i = 0; i < n; ++i) string[1 + i] = (uint8_t)text[i];
  string[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n + 2));
  return string;
}

/*--------------------------------------------------------------------+
 * APXD bulk pipe class driver
 *--------------------------------------------------------------------*/

static uint8_t ep_out, ep_in;
static uint8_t rx_buffer[512] TU_ATTR_ALIGNED(4);
static volatile uint16_t rx_count;
static volatile bool rx_ready, tx_active, tx_zlp;

static uint16_t packet_size(void) { return high_speed() ? 512 : 64; }

static void arm_receive(void) {
  if (ep_out && !usbd_edpt_busy(0, ep_out)) usbd_edpt_xfer(0, ep_out, rx_buffer, packet_size(), false);
}

static void apxd_init(void) {}
static bool apxd_deinit(void) { return true; }

static void apxd_reset(uint8_t rhport) {
  (void)rhport;
  ep_out = ep_in = 0;
  rx_ready = tx_active = tx_zlp = false;
  apx_debug_reset();
}

static uint16_t apxd_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
  const uint16_t length = sizeof(tusb_desc_interface_t) + 2 * sizeof(tusb_desc_endpoint_t);
  TU_VERIFY(itf->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC && itf->bNumEndpoints == 2 && max_len >= length, 0);
  TU_ASSERT(usbd_open_edpt_pair(rhport, tu_desc_next(itf), 2, TUSB_XFER_BULK, &ep_out, &ep_in), 0);
  rx_ready = tx_active = tx_zlp = false;
  apx_debug_reset();
  arm_receive();
  return length;
}

static bool apxd_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  static uint8_t alternate;
  if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_STANDARD) return false;
  switch (request->bRequest) {
    case TUSB_REQ_GET_INTERFACE:
      return stage == CONTROL_STAGE_SETUP ? tud_control_xfer(rhport, request, &alternate, 1) : true;
    case TUSB_REQ_SET_INTERFACE:
      if (stage != CONTROL_STAGE_SETUP) return true;
      if (request->wValue != 0) return false;
      apx_debug_reset();
      return tud_control_status(rhport, request);
    default:
      return false;
  }
}

static bool apxd_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  if (ep_addr == ep_in) {
    if (result == XFER_RESULT_SUCCESS && tx_zlp) { /* the response ended on a packet boundary: close it with a ZLP */
      tx_zlp = false;
      if (usbd_edpt_xfer(rhport, ep_in, NULL, 0, false)) return true;
    }
    tx_active = false;
  } else if (ep_addr == ep_out) {
    if (result == XFER_RESULT_SUCCESS) {
      rx_count = (uint16_t)xferred_bytes;
      rx_ready = true;
    } else {
      arm_receive();
    }
  }
  return true;
}

static const usbd_class_driver_t apxd_driver = {
  .name = "APXD", .init = apxd_init, .deinit = apxd_deinit, .reset = apxd_reset, .open = apxd_open,
  .control_xfer_cb = apxd_control, .xfer_cb = apxd_xfer, .xfer_isr = NULL, .sof = NULL };

#ifdef APX_USB_PROBE_EPS
/* Claims the probe interface and opens its five bulk IN endpoints; the probe example drives them by hand. */
static void probe_init(void) {}
static bool probe_deinit(void) { return true; }
static void probe_reset(uint8_t rhport) { (void)rhport; }
static uint16_t probe_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
  const uint16_t length = sizeof(tusb_desc_interface_t) + 5 * sizeof(tusb_desc_endpoint_t);
  TU_VERIFY(itf->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC && itf->bNumEndpoints == 5 && max_len >= length, 0);
  uint8_t const *p = tu_desc_next(itf);
  for (unsigned i = 0; i < 5; ++i, p = tu_desc_next(p)) TU_ASSERT(usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p), 0);
  return length;
}
static bool probe_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) { (void)rhport; (void)stage; (void)request; return false; }
static bool probe_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) { (void)rhport; (void)ep_addr; (void)result; (void)xferred_bytes; return true; }
static const usbd_class_driver_t probe_driver = {
  .name = "PROBE", .init = probe_init, .deinit = probe_deinit, .reset = probe_reset, .open = probe_open,
  .control_xfer_cb = probe_control, .xfer_cb = probe_xfer, .xfer_isr = NULL, .sof = NULL };
#endif

#if APX_USB_UAC
extern usbd_class_driver_t const *apx_uac_driver(void);
extern void apx_uac_poll(void);
#endif
#if CFG_TUD_VIDEO
extern usbd_class_driver_t const *apx_uvc_driver(void);
extern void apx_uvc_poll(void);
#endif
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
#ifdef APX_USB_PROBE_EPS
  static usbd_class_driver_t both[2];
  both[0] = apxd_driver;
  both[1] = probe_driver;
  *driver_count = 2;
  return both;
#else
  static usbd_class_driver_t list[3];
  uint8_t n = 0;
  list[n++] = apxd_driver;
#if CFG_TUD_VIDEO
  list[n++] = *apx_uvc_driver(); /* before TinyUSB's own video class: it answers the camera and processing unit controls first and passes the rest on */
#endif
#if APX_USB_UAC
  list[n++] = *apx_uac_driver();
#endif
  *driver_count = n;
  return list;
#endif
}

/*--------------------------------------------------------------------+
 * SDK API
 *--------------------------------------------------------------------*/

static unsigned suspend_count, resume_count;
static bool polling;

void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; ++suspend_count; }
void tud_resume_cb(void) { ++resume_count; }
void tud_mount_cb(void) {
  static unsigned seen;
  /* Enumeration complete, trace builds only. Each trace id has one flash slot and programming can only clear bits, so a
   * second write (the host re-enumerating) would AND into the first: the cold-boot trace of build c577eb9b read 0 here. */
  if (!seen) { seen = 1; apx_trace(APX_TRACE_USB_CONFIGURED, apx_ticks() & 0xffffff); }
  apx_trace_usb(5, 1, 0);
}

int apx_usb_set_mode(enum apx_usb_mode mode) {
  if ((unsigned)mode >= MODE_COUNT || !modes[mode].device || polling) return -1; /* unavailable, or inside a USB callback */
  if (mode == current_mode) return 0;
  tud_disconnect();
  apx_delay_ms(400); /* long enough for the host to see the device go, and to tear its driver down */
  current_mode = mode;
#if CFG_TUD_MSC
  if (mode == APX_USB_MODE_MSC || mode == APX_USB_MODE_CAMERA) apx_usb_msc_enter();
#endif
  ep_out = ep_in = 0;
  rx_ready = tx_active = tx_zlp = false;
  tud_connect(); /* the host's bus reset makes the stack drop its old configuration and re-read the descriptors */
  return 0;
}

enum apx_usb_mode apx_usb_mode(void) { return current_mode; }

static bool usb_ready; /* apx_usb_init() has finished: the poll may run (a delay inside the init itself must not enter it) */

void apx_usb_init(void) {
  usb_ready = false;
  if (tud_inited()) tud_deinit(0);
  current_mode = APX_USB_MODE_DEBUG;
  ep_out = ep_in = 0;
  rx_ready = tx_active = tx_zlp = false;
  const tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE,
                                    .speed = APX_USB_FORCE_FULL_SPEED ? TUSB_SPEED_FULL : TUSB_SPEED_HIGH };
  tud_rhport_init(0, &init);
  usb_ready = true;
}

void apx_delay_service(void) { apx_usb_poll(); }

void apx_usb_poll(void) {
  apx_maintenance();
  if (!usb_ready) return;
  (void)apx_ticks();
  {
    static unsigned polled;
    if (!polled) { polled = 1; apx_trace(APX_TRACE_USB_FIRST_POLL, apx_ticks() & 0xffffff); }
  }
#if APX_USB_ENUM_TIMEOUT_TICKS
  {
    static uint32_t first;
    static unsigned started;
    if (!started) { started = 1; first = apx_ticks(); }
    if (!tud_mounted() && apx_ticks() - first > APX_USB_ENUM_TIMEOUT_TICKS) { apx_trace_usb(9, 0xdead, 0); apx_watchdog_reset(); }
  }
#endif
  if (polling) return; /* a class or storage callback that waits (and so cooperates) must not re-enter the stack */
  polling = true;
  dcd_int_handler(0);
  if (tud_task_event_ready()) tud_task_ext(0, false); /* the stack is event driven: with nothing queued the call is pure overhead (about 1.9 us) */
#if APX_USB_UAC
  apx_uac_poll();
#endif
#if CFG_TUD_VIDEO
  apx_uvc_poll();
#endif
  /* A host halt/clear-halt on the OUT endpoint aborts the posted receive and nothing else would post it again */
  if (!rx_ready && tud_mounted()) arm_receive();
  polling = false;
}

int apx_usb_receive(void *buffer, size_t capacity) {
  if (!rx_ready) return 0;
  const unsigned count = rx_count;
  memcpy(buffer, rx_buffer, count < capacity ? count : capacity);
  rx_ready = false;
  arm_receive();
  return count > capacity ? -1 : (int)count;
}

int apx_usb_send(const void *buffer, size_t length) {
  if (!tud_mounted() || !ep_in || tx_active || length > 0xffffu || usbd_edpt_stalled(0, ep_in)) return -1;
  tx_zlp = length != 0 && length % packet_size() == 0;
  tx_active = true;
  if (!usbd_edpt_xfer(0, ep_in, (uint8_t *)(uintptr_t)buffer, (uint16_t)length, false)) {
    tx_active = tx_zlp = false;
    return -1;
  }
  return 0;
}

int apx_usb_tx_idle(void) { return !tx_active; }
int apx_usb_configured(void) { return tud_mounted(); }
int apx_usb_high_speed(void) { return high_speed(); }
int apx_usb_suspended(void) { return tud_suspended(); }
unsigned apx_usb_suspend_count(void) { return suspend_count; }
unsigned apx_usb_resume_count(void) { return resume_count; }
