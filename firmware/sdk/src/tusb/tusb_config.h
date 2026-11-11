/* TinyUSB configuration for the JT5295 (USB_STACK=tinyusb).
 *
 * The USB core is a MUSB-style controller reached through SPR byte registers, so there is no upstream DCD for it:
 * src/tusb/dcd_apx.c is the device controller driver. CFG_TUSB_MCU is OPT_MCU_NONE and the per-MCU facts that
 * tusb_mcu.h would normally provide are given here. Hardware facts: research/usb-musb.md.
 */
#ifndef APX_TUSB_CONFIG_H
#define APX_TUSB_CONFIG_H
#include <appotech/usb.h>
#include "board.h"

#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED 1
#if APX_USB_FORCE_FULL_SPEED
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#else
#define CFG_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#endif

/* Phase 0 probe: endpoints 1..6 exist (7 slots with EP0) and the core can do high speed. */
#define TUP_DCD_ENDPOINT_MAX 7
#define TUP_RHPORT_HIGHSPEED 1

/* The FIFOs are reached one byte per SPR access, never through a memory window: class drivers keep their own buffers. */
#define CFG_TUD_EDPT_DEDICATED_HWFIFO 0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(32))) /* the SD driver DMAs straight into the MSC buffer; its cache maintenance needs 16-byte lines */

#define CFG_TUD_ENDPOINT0_SIZE 64

/* Classes. The APXD debug pipe is the SDK's own class driver (src/tusb/usb_tusb.c), not one of these. */
#define CFG_TUD_CDC 0
#ifndef APX_USB_MSC
#define APX_USB_MSC 1 /* mass-storage mode (src/tusb/msc_disk.c); the Makefile defaults it off except for usb-modes (it needs ~9 KB of extra RAM) */
#endif
#define CFG_TUD_MSC APX_USB_MSC
#define CFG_TUD_MSC_EP_BUFSIZE 512 /* one 512-byte block per read10/write10 callback */
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#ifndef APX_USB_UAC
#define APX_USB_UAC 0 /* USB audio class 1 microphone in the camera mode (src/tusb/uac.c); needs APX_USB_UVC */
#endif
#ifndef APX_USB_UVC
#define APX_USB_UVC 0 /* camera composite device (UVC + mass storage), src/tusb/uvc.c; the Makefile defaults it off except for examples that ask */
#endif
#define CFG_TUD_VIDEO (APX_CAMERA_COUNT * APX_USB_UVC) /* one video function per sensor (two on the Silvergear, one on the Action): the host picks the camera by choosing the video device */
#define CFG_TUD_VIDEO_STREAMING (APX_CAMERA_COUNT * APX_USB_UVC)
#define CFG_TUD_VIDEO_STREAMING_BULK 1 /* the core's packet RAM (4 KiB) has no room for stock's 3 x 1024-byte isochronous packets; bulk streaming is what Linux uvcvideo accepts too */
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE 16384 /* the largest payload (UVC header + data) the host may negotiate; payloads are sent straight from the application's buffer (uvc.c), TinyUSB's own payload buffer of this size is unused */
#define CFG_TUD_AUDIO 0

#endif
