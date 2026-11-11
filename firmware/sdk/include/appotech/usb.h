#ifndef APPOTECH_USB_H
#define APPOTECH_USB_H
#include <stddef.h>
#include <stdint.h>
#ifndef APX_USB_VID
#define APX_USB_VID 0x1908
#endif
#ifndef APX_USB_PID
#define APX_USB_PID 0x3319
#endif
#ifndef APX_USB_FORCE_FULL_SPEED
#define APX_USB_FORCE_FULL_SPEED 0
#endif
#ifndef APX_USB_PID_MSC
#define APX_USB_PID_MSC 0x3318 /* mass-storage mode: a different PID, so apxtool (which looks for APX_USB_PID) never mistakes it for the debug link */
#endif

#ifndef APX_USB_PID_CAMERA
#define APX_USB_PID_CAMERA 0x3317 /* camera composite device: UVC video + mass storage */
#endif

/* USB device modes: the device presents one at a time and re-enumerates when the mode changes. */
enum apx_usb_mode {
    APX_USB_MODE_DEBUG = 0, /* APXD debug link: vendor-class bulk pair on EP1, VID:PID APX_USB_VID:APX_USB_PID (what apxtool talks to) */
    APX_USB_MODE_MSC = 1,   /* mass storage (bulk-only, SCSI) on EP1, backed by include/appotech/usb_msc.h; VID:PID APX_USB_VID:APX_USB_PID_MSC */
    APX_USB_MODE_CAMERA = 2,/* composite device like the stock firmware's: two UVC video functions (rear on EP2, front on EP3; MJPEG, bulk streaming) + mass storage on EP1, include/appotech/usb_uvc.h;
                             * VID:PID APX_USB_VID:APX_USB_PID_CAMERA. Needs APX_USB_UVC=1 */
};

void apx_usb_init(void);
/* Change the USB mode: drop off the bus, swap the descriptor set, reconnect. Takes about half a second and the host sees a
 * new device. Returns 0 on success, -1 if the mode is not available (the legacy stack has only the debug mode; MSC needs
 * APX_USB_MSC=1) or if called from inside a USB callback. In any mode but DEBUG the host cannot send APXD requests, so
 * the application has to switch back itself (a timer or a key); otherwise only a reset gets the debug link back. */
int apx_usb_set_mode(enum apx_usb_mode mode);
enum apx_usb_mode apx_usb_mode(void);
void apx_usb_poll(void);
int apx_usb_configured(void);
int apx_usb_high_speed(void);
int apx_usb_suspended(void);
unsigned apx_usb_suspend_count(void);
unsigned apx_usb_resume_count(void);
int apx_usb_receive(void *buffer, size_t capacity);
int apx_usb_send(const void *buffer, size_t length);
int apx_usb_tx_idle(void);
#endif
