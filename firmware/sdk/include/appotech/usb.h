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
void apx_usb_init(void);
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
