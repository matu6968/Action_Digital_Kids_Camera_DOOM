/* A USB audio class 1 microphone as an SDK class driver (the descriptors are in usb_tusb.c, the hardware facts in research/usb-musb.md).
 *
 * TinyUSB's own audio class is not used: it needs the DCD's iso alloc/activate API and brings a software FIFO and a big state machine for features (volume, several
 * sampling rates) the microphone does not offer. This driver claims the audio control and audio streaming interfaces, switches the isochronous endpoint on and off
 * with SET_INTERFACE, and keeps the endpoint fed from a ring of samples. The core's IN side is double buffered: one packet in transit and one loaded, each refill
 * is queued as soon as the previous packet has been taken, so there is about 2 ms of slack against a poll that comes late.
 *
 * The packet size follows the ring so that its fill level stays in a band (the sample clock is not the USB frame clock): the nominal 32 samples, one more when the ring
 * is filling up, one fewer when it is nearly empty. A packet of any size up to the endpoint maximum is valid for an asynchronous source. When the ring cannot supply a packet
 * at all, silence is sent so that the stream keeps its timing. */
#include "tusb.h"
#include "device/usbd_pvt.h"
#if CFG_TUD_ENABLED && APX_USB_UAC
#include <appotech/usb_uac.h>
#include <string.h>

extern bool apx_dcd_iso_activate(uint8_t ep_addr, uint16_t mps);
extern void apx_dcd_iso_deactivate(uint8_t ep_addr);

#define RING_SAMPLES 2048u /* 64 ms at 32 kHz; a power of two */
#define PREFILL (APX_UAC_SAMPLES_PER_PACKET * 8u) /* samples collected before the first packet goes out (8 ms) */
#define LOW_MARK (APX_UAC_SAMPLES_PER_PACKET * 4u)
#define TARGET (APX_UAC_SAMPLES_PER_PACKET * 6u) /* the fill level the packet size regulates towards */
#define MAX_SAMPLES (APX_UAC_SAMPLES_PER_PACKET * 2u) /* the endpoint's maximum packet */

static int16_t ring[RING_SAMPLES];
static volatile uint32_t head, tail; /* samples written / read, free running */
static uint8_t packet[2][MAX_SAMPLES * 2] TU_ATTR_ALIGNED(4);
static unsigned packet_index;
static uint8_t ac_itf = 0xff, as_itf = 0xff, ep_in, alt;
static uint16_t ep_mps;
static bool active, queued, primed;
static struct apx_usb_uac_stats stats;

bool apx_usb_uac_streaming(void) { return active; }
unsigned apx_usb_uac_free(void) { return RING_SAMPLES - (head - tail); }

static void feed(void);

unsigned apx_usb_uac_write_raw(const int16_t *samples, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    if (head - tail >= RING_SAMPLES) { ++tail; ++stats.overruns; } /* the oldest sample goes */
    ring[head & (RING_SAMPLES - 1u)] = samples[i];
    ++head;
  }
  return count;
}

/*--------------------------------------------------------------------+
 * Level stage: DC blocker, mute, volume, automatic gain control (the feature unit's controls)
 *--------------------------------------------------------------------*/

#define UNIT_FEATURE 2
#define VOLUME_MAX_DB 45
#define VOLUME_MAX ((int16_t)(VOLUME_MAX_DB * 256)) /* the volume control counts 1/256 dB, 0..+45 dB in 1 dB steps */
#ifndef APX_UAC_DEFAULT_GAIN_DB
#define APX_UAC_DEFAULT_GAIN_DB 36 /* with AGC: the highest gain it may use; without: the gain */
#endif
#ifndef APX_UAC_DEFAULT_AGC
#define APX_UAC_DEFAULT_AGC 1
#endif
#define AGC_TARGET 9000 /* the peak level the AGC aims for (about -11 dBFS) */
#define AGC_GATE 96     /* below this peak (raw counts; the quiet room's noise peaks reach about 70, speech 100..250 close to the camera) the gain is not raised (it only recovers towards the start gain) */
#define OUT_LIMIT 30000 /* a sample that would go above this makes the gain drop at once */
#define START_GAIN_DB 18 /* where the AGC starts when a stream starts */
static const uint16_t gain_q8[VOLUME_MAX_DB + 1] = { 256, 287, 322, 362, 406, 455, 511, 573, 643, 722, 810, 908, 1019, 1144, 1283, 1440, 1615, 1812, 2033, 2282, 2560, 2872, 3223, 3616, 4057, 4552, 5108, 5731, 6430, 7215, 8095, 9083, 10192, 11435, 12830, 14396, 16153, 18123, 20335, 22816, 25600, 28724, 32228, 36161, 40573, 45524 };
static uint8_t mute_on, agc_on = APX_UAC_DEFAULT_AGC;
static int16_t volume = APX_UAC_DEFAULT_GAIN_DB * 256;
static int32_t g16, hp_x, hp_y, env; /* gain (Q16), DC blocker state, peak envelope of the blocked input */
static unsigned agc_phase;

static unsigned volume_db(void) {
  int db = (volume + 128) >> 8;
  return (unsigned)(db < 0 ? 0 : db > VOLUME_MAX_DB ? VOLUME_MAX_DB : db);
}
static int32_t ceiling16(void) { return (int32_t)gain_q8[volume_db()] << 8; }
static void level_reset(void) {
  hp_x = hp_y = env = 0;
  agc_phase = 0;
  const int32_t start = (int32_t)gain_q8[START_GAIN_DB] << 8;
  g16 = !agc_on || start > ceiling16() ? ceiling16() : start;
}

void apx_usb_uac_get_stats(struct apx_usb_uac_stats *out) { *out = stats; out->level = head - tail; out->gain_q8 = (uint32_t)(g16 >> 8); out->agc = agc_on; }

unsigned apx_usb_uac_write(const int16_t *samples, unsigned count) {
  int16_t block[64];
  for (unsigned done = 0; done < count;) {
    const unsigned n = count - done < 64u ? count - done : 64u;
    for (unsigned i = 0; i < n; ++i) {
      /* DC blocker: y = x - x' + (255/256) y', a corner of about 20 Hz; the state is kept times 256 so that rounding leaves no dead zone (a plain integer state keeps up to 255 counts) */
      const int32_t x = samples[done + i];
      hp_y += ((x - hp_x) << 8) - (hp_y >> 8);
      hp_x = x;
      int32_t y = hp_y >> 8;
      if (y > 32767) y = 32767; else if (y < -32767) y = -32767;
      const int32_t a = y < 0 ? -y : y;
      if (a > env) env = a; else if (env > 0) env -= (env >> 9) + 1; /* peak envelope, falls with a time constant of about 16 ms */
      if (!agc_on) {
        g16 = ceiling16();
      } else if ((++agc_phase & 7u) == 0) {
        const int32_t top = ceiling16();
        if (g16 > top) g16 = top;
        if (env < AGC_GATE) { /* quiet: nothing to raise the gain for, but after a loud passage it comes back up to the start gain so the next speech is not left too soft */
          const int32_t rest = (int32_t)gain_q8[START_GAIN_DB] << 8 < top ? (int32_t)gain_q8[START_GAIN_DB] << 8 : top;
          if (g16 < rest) { g16 += (g16 >> 11) + 1; if (g16 > rest) g16 = rest; }
        } else {
          int32_t want = (int32_t)(((int32_t)AGC_TARGET << 16) / env);
          if (want > top) want = top;
          if (want < 65536) want = 65536;
          if (want < g16) g16 = want; /* louder than the target: down at once */
          else { const int32_t step = (g16 >> 11) + 1; g16 = want - g16 > step ? g16 + step : want; } /* quieter: up at about 17 dB per second */
        }
      }
      int32_t out = (y * (g16 >> 8)) >> 8;
      if (out > OUT_LIMIT || out < -OUT_LIMIT) {
        ++stats.clipped;
        if (agc_on && a > 0) g16 = (int32_t)(((int32_t)OUT_LIMIT << 16) / a); /* hard attack: the loud peak itself is clamped, what follows gets the lower gain */
        out = out > 0 ? OUT_LIMIT : -OUT_LIMIT;
      }
      block[i] = mute_on ? 0 : (int16_t)out;
    }
    apx_usb_uac_write_raw(block, n);
    done += n;
  }
  return count;
}

/* Queue the next packet if the endpoint can take one. */
static void feed(void) {
  if (!active || queued) return;
  const uint32_t level = head - tail;
  if (!primed && level >= PREFILL) primed = true;
  if (!primed) return; /* nothing is sent before the ring holds a little: an endpoint that answers a poll right at SET_INTERFACE made hosts fail the capture */
  /* Nominal 32 samples. Above the target the packet grows in proportion (a late poll leaves up to a few milliseconds of samples behind, which this works off at up to 32 samples
   * per millisecond); below the low mark it shrinks by one so the ring is not emptied. */
  unsigned samples = APX_UAC_SAMPLES_PER_PACKET;
  if (level > TARGET) {
    unsigned extra = (level - TARGET) / 8u;
    if (extra == 0) extra = 1;
    if (extra > MAX_SAMPLES - APX_UAC_SAMPLES_PER_PACKET) extra = MAX_SAMPLES - APX_UAC_SAMPLES_PER_PACKET;
    samples += extra;
    ++stats.long_packets;
  } else if (level < LOW_MARK && level >= samples) { --samples; ++stats.short_packets; }
  int16_t *out = (int16_t *)(void *)packet[packet_index];
  if (primed && level >= samples) {
    for (unsigned i = 0; i < samples; ++i) out[i] = ring[(tail + i) & (RING_SAMPLES - 1u)];
    tail += samples;
  } else {
    /* The ring ran dry (the microphone source skips a few milliseconds at every restart of its DMA): collect a little again before resuming, or every packet would be a
     * half-empty one. Nothing is sent meanwhile, the same as before the first packet: the host takes an empty poll as no data, whereas padding with zeros would add samples
     * that were never recorded. */
    if (primed) { ++stats.underruns; primed = false; }
    return;
  }
  if (usbd_edpt_claim(0, ep_in) && usbd_edpt_xfer(0, ep_in, packet[packet_index], (uint16_t)(samples * 2u), false)) {
    queued = true;
    ++stats.packets;
    packet_index ^= 1u;
  } else {
    ++stats.send_refused;
    usbd_edpt_release(0, ep_in);
  }
}

/*--------------------------------------------------------------------+
 * class driver
 *--------------------------------------------------------------------*/

static void uac_init(void) {}
static bool uac_deinit(void) { return true; }

static void uac_reset(uint8_t rhport) {
  (void)rhport;
  if (ep_in) apx_dcd_iso_deactivate(ep_in);
  ac_itf = as_itf = 0xff; ep_in = 0; alt = 0; ep_mps = 0;
  active = queued = primed = false;
  head = tail = 0;
  memset(&stats, 0, sizeof stats);
  mute_on = 0; agc_on = APX_UAC_DEFAULT_AGC; volume = APX_UAC_DEFAULT_GAIN_DB * 256;
  level_reset();
}

/* Length of the descriptors that belong to the interface at `itf`: everything up to the next interface with another number (alternate settings of the same interface
 * belong to it), the next interface association, or the end. */
static uint16_t span(tusb_desc_interface_t const *itf, uint16_t max_len) {
  uint8_t const *p = (uint8_t const *)itf, *end = p + max_len;
  uint8_t const number = itf->bInterfaceNumber;
  p = tu_desc_next(p);
  while (p < end) {
    if (tu_desc_type(p) == TUSB_DESC_INTERFACE_ASSOCIATION) break;
    if (tu_desc_type(p) == TUSB_DESC_INTERFACE && ((tusb_desc_interface_t const *)p)->bInterfaceNumber != number) break;
    p = tu_desc_next(p);
  }
  return (uint16_t)(p - (uint8_t const *)itf);
}

static uint16_t uac_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
  (void)rhport;
  if (itf->bInterfaceClass != TUSB_CLASS_AUDIO) return 0;
  const uint16_t length = span(itf, max_len);
  if (itf->bInterfaceSubClass == 1) { ac_itf = itf->bInterfaceNumber; return length; }
  if (itf->bInterfaceSubClass != 2) return 0;
  as_itf = itf->bInterfaceNumber;
  uint8_t const *p = (uint8_t const *)itf, *end = p + length;
  for (; p < end; p = tu_desc_next(p)) {
    if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
      tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
      ep_in = ep->bEndpointAddress;
      ep_mps = tu_edpt_packet_size(ep);
    }
  }
  return length;
}

/* The feature unit's controls (UAC1 5.2.2.4): mute (1 byte), volume (2 bytes, signed, 1/256 dB) and automatic gain control (1 byte) on the master channel. */
enum { REQ_SET_CUR = 0x01, REQ_GET_CUR = 0x81, REQ_GET_MIN = 0x82, REQ_GET_MAX = 0x83, REQ_GET_RES = 0x84 };
enum { CS_MUTE = 0x01, CS_VOLUME = 0x02, CS_AGC = 0x07 };
static uint8_t ctl_buf[4] TU_ATTR_ALIGNED(4);

static bool feature_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  if (tu_u16_high(request->wIndex) != UNIT_FEATURE || (uint8_t)tu_u16_low(request->wIndex) != ac_itf) return false;
  const uint8_t selector = tu_u16_high(request->wValue), channel = (uint8_t)tu_u16_low(request->wValue);
  if (channel != 0) return false; /* the unit has master controls only */
  uint16_t length;
  switch (selector) {
    case CS_MUTE: case CS_AGC: length = 1; break;
    case CS_VOLUME: length = 2; break;
    default: return false;
  }
  if (request->bRequest == REQ_SET_CUR) {
    if (request->wLength != length) return false;
    if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, ctl_buf, length);
    if (stage == CONTROL_STAGE_ACK) {
      if (selector == CS_MUTE) mute_on = ctl_buf[0] != 0;
      else if (selector == CS_AGC) { agc_on = ctl_buf[0] != 0; }
      else { volume = (int16_t)(ctl_buf[0] | ctl_buf[1] << 8); if (volume < 0) volume = 0; if (volume > VOLUME_MAX) volume = VOLUME_MAX; }
      if (selector != CS_MUTE && g16 > ceiling16()) g16 = ceiling16();
    }
    return true;
  }
  if (stage != CONTROL_STAGE_SETUP) return true;
  int value;
  switch (request->bRequest) {
    case REQ_GET_CUR: value = selector == CS_MUTE ? mute_on : selector == CS_AGC ? agc_on : volume; break;
    case REQ_GET_MIN: if (selector != CS_VOLUME) return false; value = 0; break;
    case REQ_GET_MAX: if (selector != CS_VOLUME) return false; value = VOLUME_MAX; break;
    case REQ_GET_RES: if (selector != CS_VOLUME) return false; value = 256; break;
    default: return false;
  }
  ctl_buf[0] = (uint8_t)value; ctl_buf[1] = (uint8_t)(value >> 8);
  return tud_control_xfer(rhport, request, ctl_buf, tu_min16(length, request->wLength));
}

static bool uac_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS) return request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE && feature_control(rhport, stage, request);
  if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_STANDARD) return false;
  const uint8_t itf = (uint8_t)tu_u16_low(request->wIndex);
  if (itf != as_itf && itf != ac_itf) return false;
  switch (request->bRequest) {
    case TUSB_REQ_GET_INTERFACE:
      if (stage != CONTROL_STAGE_SETUP) return true;
      return tud_control_xfer(rhport, request, &alt, 1);
    case TUSB_REQ_SET_INTERFACE: {
      if (stage != CONTROL_STAGE_SETUP) return true;
      if (itf == as_itf) {
        const uint8_t wanted = (uint8_t)request->wValue;
        if (wanted > 1) return false;
        if (wanted == 1 && !active) {
          if (!ep_in || !apx_dcd_iso_activate(ep_in, ep_mps)) return false;
          ++stats.alt_on;
          active = true; queued = false; primed = false; head = tail = 0;
          level_reset();
        } else if (wanted == 0 && active) {
          ++stats.alt_off;
          apx_dcd_iso_deactivate(ep_in);
          active = queued = false;
        }
        alt = wanted;
      }
      return tud_control_status(rhport, request);
    }
    default:
      return false;
  }
}

static bool uac_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  (void)rhport; (void)result; (void)xferred_bytes;
  if (ep_addr == ep_in) { queued = false; feed(); }
  return true;
}

static void uac_sof(uint8_t rhport, uint32_t frame_count) { (void)rhport; (void)frame_count; }

static const usbd_class_driver_t uac_driver = {
  .name = "UAC", .init = uac_init, .deinit = uac_deinit, .reset = uac_reset, .open = uac_open,
  .control_xfer_cb = uac_control, .xfer_cb = uac_xfer, .xfer_isr = NULL, .sof = uac_sof };

usbd_class_driver_t const *apx_uac_driver(void) { return &uac_driver; }

/* Called from apx_usb_poll(): start the stream and refill after the ring has run dry. */
static void (*service)(void);
void apx_usb_uac_set_service(void (*fn)(void)) { service = fn; }
void apx_uac_poll(void) {
  if (service) service();
  feed();
}
#endif
