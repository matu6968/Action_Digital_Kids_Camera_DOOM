#ifndef APPOTECH_USB_UAC_H
#define APPOTECH_USB_UAC_H
#include <stdbool.h>
#include <stdint.h>

/* USB audio class 1 microphone in APX_USB_MODE_CAMERA (USB_STACK=tinyusb, APX_USB_UAC=1): mono, signed 16-bit PCM, APX_UAC_RATE samples per second, on the
 * isochronous IN endpoint EP4 (64-byte packets, one per millisecond, like the stock firmware). The application writes samples with apx_usb_uac_write(); the host
 * reads them while it has the streaming interface open. Everything runs inside apx_usb_poll(). */
#define APX_UAC_RATE 32000
#define APX_UAC_SAMPLES_PER_PACKET (APX_UAC_RATE / 1000)

/* The host has selected the streaming alternate setting (an application is recording). */
bool apx_usb_uac_streaming(void);
/* Room for samples in the ring (about 64 ms is held). */
unsigned apx_usb_uac_free(void);
/* A function the USB poll calls on every pass (so also while the application waits inside apx_delay_ms or a camera call): the audio source's service routine. */
void apx_usb_uac_set_service(void (*service)(void));
/* Queue samples from the microphone. They pass the level stage first: a DC blocker, then the gain the host chose in its mixer (the device has a mute, a volume of 0..+45 dB
 * and an automatic gain control: with AGC on the gain follows the signal, at most the volume setting; below a noise gate it holds). When the ring is full the OLDEST samples
 * are dropped (counted in overruns), so the latency stays bounded. Returns the number accepted (all of them). */
unsigned apx_usb_uac_write(const int16_t *samples, unsigned count);
/* The same without the level stage (test tones). */
unsigned apx_usb_uac_write_raw(const int16_t *samples, unsigned count);
struct apx_usb_uac_stats {
    uint32_t packets, underruns, overruns, short_packets, long_packets, level, alt_on, alt_off, send_refused; /* level = samples in the ring now */
    uint32_t gain_q8, clipped, agc; /* the gain now in force (256 = 1.0), samples the output limiter had to clamp, AGC switch */
};
void apx_usb_uac_get_stats(struct apx_usb_uac_stats *stats);
#endif
