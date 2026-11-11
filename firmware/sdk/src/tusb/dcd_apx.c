/* TinyUSB device controller driver for the JT5295 USB core.
 *
 * The core is a Mentor MUSB-style controller (research/usb-musb.md) reached through byte-wide SPRs at
 * 0xa000+4*offset, so TinyUSB's own portable/mentor/musb/dcd_musb.c (which dereferences a memory-mapped register
 * struct) cannot be used. This file ports that driver's EP0 state machine (Copyright (c) 2021 Koji Kitayama,
 * 2024 Brent Kowal, 2021 Ha Thach; MIT) onto SPR accessors and keeps the behaviour the SDK's earlier driver
 * (src/usb.c) established on hardware:
 *   - polled, PIO, one byte per FIFO SPR access; the bulk endpoints are serviced from their CSRs, not from the
 *     interrupt flags, exactly as before. dcd_int_handler() is the polling entry, called from apx_usb_poll();
 *   - the EP0 SETUP packet is read from memory at 0x8000, the FIFO data through SPR 0xa080+16*ep;
 *   - SET_ADDRESS takes effect after its status stage, and a SETUPEND cancels it;
 *   - the endpoint FIFO is treated as shared between IN and OUT (APX_USB_SHARED_FIFO): TXCSRH bit 5 is set only
 *     while an IN packet is loaded and sent, as src/usb.c did.
 * Static FIFOs (CONFIGDATA reports no dynamic sizing); only EP1 (512 bytes) is characterised so far.
 */
#include "tusb_option.h"

#if CFG_TUD_ENABLED
#include "device/dcd.h"
#include <appotech/soc.h>
#include <appotech/trace.h> /* flight-recorder hooks, as src/usb.c had; no-ops unless APX_SPI_TRACE=1 */

#ifndef APX_USB_SHARED_FIFO
#define APX_USB_SHARED_FIFO 1
#endif
#ifndef APX_USB_SETUP_FIFO /* the host model test replaces the memory-mapped SETUP window */
#define APX_USB_SETUP_FIFO(i) (*(volatile uint8_t *)(uintptr_t)(0x8000 + (i)))
#endif

enum { FADDR = 0x00, POWER = 0x01, INTRTX = 0x02, INTRRX = 0x04, INTRTXE = 0x06, INTRRXE = 0x08, INTRUSB = 0x0a,
       INTRUSBE = 0x0b, INDEX = 0x0e, TXMAXP = 0x10, CSR0L = 0x12, TXCSRL = 0x12, TXCSRH = 0x13, RXMAXP = 0x14,
       RXCSRL = 0x16, RXCSRH = 0x17, RXCOUNT = 0x18 };
enum { POWER_RESUME = 0x04, POWER_HSMODE = 0x10, POWER_HSENAB = 0x20, POWER_SOFTCONN = 0x40 };
enum { IS_SUSPEND = 0x01, IS_RESUME = 0x02, IS_RESET = 0x04 };
enum { C0_RXRDY = 0x01, C0_TXRDY = 0x02, C0_STALLED = 0x04, C0_DATAEND = 0x08, C0_SETEND = 0x10, C0_STALL = 0x20,
       C0_RXRDYC = 0x40, C0_SETENDC = 0x80 };
enum { TX_RDY = 0x01, TX_FIFONE = 0x02, TX_UNDRN = 0x04, TX_FLUSH = 0x08, TX_STALL = 0x10, TX_STALLED = 0x20,
       TX_CLRDT = 0x40 };
enum { RX_RDY = 0x01, RX_OVER = 0x04, RX_FLUSH = 0x10, RX_STALL = 0x20, RX_STALLED = 0x40, RX_CLRDT = 0x80 };
enum { CSRH_MODE_TX = 0x20, CSRH_ISO = 0x40 };

/* Largest packet each endpoint FIFO is known to hold. EP2..EP6 exist but their FIFO sizes are not measured yet. */
#ifdef APX_USB_PROBE_EPS
static const uint16_t fifo_limit[TUP_DCD_ENDPOINT_MAX] = { 64, 512, 512, 512, 512, 512, 512 }; /* the endpoint probe opens them all to find out */
#else
static const uint16_t fifo_limit[TUP_DCD_ENDPOINT_MAX] = { 64, 512, 512, 512, 512, 0, 0 }; /* EP2..4: IN only, through the packet RAM */
#endif

static uint8_t rd8(unsigned offset) { return (uint8_t)apx_spr_read(0xa000u + 4u * offset); }
static void wr8(unsigned offset, unsigned value) { apx_spr_write(0xa000u + 4u * offset, value & 255u); }
static unsigned rd16(unsigned offset) { return rd8(offset) | (unsigned)rd8(offset + 1u) << 8; }
static void wr16(unsigned offset, unsigned value) { wr8(offset, value); wr8(offset + 1u, value >> 8); }
/* INDEX is only ever written here, so its value is cached and a select of the endpoint already selected costs nothing (an SPR write is about 70 ns, and the
 * poll used to select EP0 and every open endpoint on each call). Anything that may reset the register (PHY init, a bus reset) invalidates the cache. */
static unsigned index_cache = 0xffu;
static void ep_select(unsigned epn) {
  if (index_cache != epn) { wr8(INDEX, epn); index_cache = epn; }
}
/* The endpoint FIFO RAM of the core is memory mapped at 0x8000, 4 KiB, 0x8000..0x8fff (measured with examples/usb-dma-probe and usb-ep-probe,
 * research/usb-musb.md). Each endpoint direction has an address register (SPR 0x9850 + 8 * (ep - 1) for IN, EP1 OUT 0x9854) holding the start of a pair of
 * 512-byte buffers that the core uses alternately: a packet is stored into the buffer the toggle bit of SPR 0x9844 names (bit 19 + ep for IN, bit 26 for
 * EP1 OUT), sent by writing its length with the start bit (SPR 0x9844 = length | 1 << (13 + ep)) and setting TxPktRdy. Copying through that RAM with
 * 32-bit accesses moves 512 bytes in about 8 us, against about 49 us one byte at a time through the FIFO SPR. Layout used here:
 *   0x8000 EP0 (64) | 0x8040/0x8240 EP1 IN, 0x8240/0x8440 EP1 OUT (BootROM defaults) | 0x8800/0x8a00 EP2 IN | 0x8c00/0x8e00 EP3 IN.
 * EP1..EP3 IN and EP1 OUT are characterised; anything else keeps the byte-wide FIFO SPR (and EP2..6 OUT is refused: untested). */
#ifndef APX_USB_PACKET_RAM
#define APX_USB_PACKET_RAM 1
#endif
#define VENDOR_CTRL 0x9844u
#define PRAM_OUT_BASE 0x8040u /* EP1 OUT shares EP1 IN's two buffers (0x9854 = 0x9850): a bulk-only protocol never has both directions in flight */
#define PRAM_OUT(buffer) (PRAM_OUT_BASE + 0x200u * (buffer))
#define VENDOR_OUT_BUFFER (1u << 26)
#ifndef APX_EP4_BASE
#define APX_EP4_BASE 0x8c40 /* iso EP4: this unit and the one 0x800 below it (0x8440), see research/usb-musb.md */
#endif
#if APX_USB_PACKET_RAM
/* EP2 and EP3 (the two video streams) share one pair of buffers: the packet RAM is too small for more, see shared_busy(). */
static const uint16_t pram_in_base[TUP_DCD_ENDPOINT_MAX] = { 0, 0x8040, 0x8800, 0x8800, APX_EP4_BASE, 0, 0 }; /* 0 = not characterised */
/* The core's second IN buffer: 0x200 above the first for the bulk endpoints, 0x800 BELOW it for the isochronous EP4 (measured). */
static unsigned pram_in_second(unsigned epn) { return epn == 4 ? pram_in_base[epn] - 0x800u : pram_in_base[epn] + 0x200u; }
static bool packet_ram(unsigned epn) { return epn == 1; } /* OUT */
static bool packet_ram_in(unsigned epn) { return pram_in_base[epn] != 0; }
static void pram_store(uintptr_t dst, const uint8_t *src, unsigned n) { /* n bytes to the RAM at dst; may write up to 3 padding bytes after them */
  volatile uint32_t *d = (volatile uint32_t *)dst;
  if (((uintptr_t)src & 3u) == 0) {
    const uint32_t *w = (const uint32_t *)src;
    for (unsigned i = 0; i < (n + 3u) / 4u; ++i) d[i] = w[i];
  } else {
    /* Not word aligned (a UVC payload starts two bytes before the JPEG so that its header can be written in place): aligned loads, shifted together, one load per word.
     * May read up to 7 bytes beyond the n it stores, within the aligned words that hold them. */
    const unsigned shift = 8u * ((uintptr_t)src & 3u);
    const uint32_t *w = (const uint32_t *)((uintptr_t)src & ~3u);
    uint32_t cur = w[0];
    for (unsigned i = 0; i < (n + 3u) / 4u; ++i) {
      const uint32_t next = w[i + 1u];
      d[i] = (cur >> shift) | (next << (32u - shift));
      cur = next;
    }
  }
}
static void pram_load(uint8_t *dst, uintptr_t src, unsigned n) { /* n bytes from the RAM at src, never writing past dst + n */
  const volatile uint32_t *s = (const volatile uint32_t *)src;
  unsigned i = 0;
  if (((uintptr_t)dst & 3u) == 0) {
    uint32_t *w = (uint32_t *)dst;
    for (; i + 4u <= n; i += 4u) w[i / 4u] = s[i / 4u];
  }
  for (; i < n; i += 4u) {
    const uint32_t v = s[i / 4u];
    for (unsigned b = 0; b < 4u && i + b < n; ++b) dst[i + b] = (uint8_t)(v >> (8 * b));
  }
}
#endif
static uint32_t fifo_spr(unsigned epn) { return 0xa080u + 16u * epn; }
static uint8_t fifo_read_byte(unsigned epn) { return (uint8_t)apx_spr_read(fifo_spr(epn)); }
static void fifo_write_byte(unsigned epn, uint8_t value) { apx_spr_write(fifo_spr(epn), value); }

typedef struct {
  uint8_t *buf;
  uint16_t length, remaining, mps;
  bool armed;
  bool halted; /* STALL sent: the poll still has to clear the core's STALLED flag */
  bool iso;    /* isochronous IN endpoint (apx_dcd_iso_activate): the FIFO is not shared with OUT */
  uint32_t blocked_since; /* ticks (|1) since a write waited for the other endpoint that shares its buffers, 0 = not waiting */
} pipe_t;

enum { PIPE0_IDLE = 0, PIPE0_DATA_IN, PIPE0_DATA_OUT, PIPE0_STATUS_IN, PIPE0_STATUS_OUT, PIPE0_STATUS_OUT_PENDING_XFER,
       PIPE0_STATUS_OUT_PENDING_IRQ };

typedef struct {
  uint8_t *buf;           /* DATA OUT drain target */
  uint16_t xact_len;      /* DATA IN chunk length armed by edpt0_xfer */
  uint16_t remain_wlength;
  uint8_t state, pending_addr;
  bool rxrdy_consumed;    /* RxPktRdy left set for an already-consumed packet (NAK flow control) */
  bool deferred_setup_valid;
  uint32_t deferred_setup[2];
} pipe0_t;

static pipe0_t pipe0;
static pipe_t pipes[2][TUP_DCD_ENDPOINT_MAX]; /* [0] = OUT, [1] = IN, indexed by endpoint number; EP0 unused */

static pipe_t *pipe_get(unsigned epn, unsigned dir_in) { return &pipes[dir_in ? 1 : 0][epn]; }

/*--------------------------------------------------------------------+
 * EP0
 *--------------------------------------------------------------------*/

static bool pipe0_read_setup(uint32_t setup[2]) {
  ep_select(0);
  if (rd16(RXCOUNT) != 8) { /* not a SETUP packet: serve it and carry on, as src/usb.c did */
    wr8(CSR0L, C0_RXRDYC);
    return false;
  }
  uint8_t bytes[8];
  for (unsigned i = 0; i < 8; ++i) bytes[i] = APX_USB_SETUP_FIFO(i);
  setup[0] = bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
  setup[1] = bytes[4] | (uint32_t)bytes[5] << 8 | (uint32_t)bytes[6] << 16 | (uint32_t)bytes[7] << 24;
  return true;
}

static void pipe0_start_setup(uint8_t rhport, const uint32_t setup[2]) {
  tusb_control_request_t const *req = (tusb_control_request_t const *)setup;
  {
    static unsigned seen;
    if (!seen) { seen = 1; apx_trace(APX_TRACE_USB_FIRST_SETUP, apx_ticks() & 0xffffff); } /* enumeration timeline, trace builds only */
  }
  apx_trace_usb(2, (unsigned)req->bmRequestType << 24 | (unsigned)req->bRequest << 16 | req->wValue,
                (unsigned)req->wIndex << 16 | req->wLength); /* U2: bmRequestType bRequest wValue, wIndex wLength */
  pipe0.remain_wlength = req->wLength;
  if (req->wLength == 0) {
    pipe0.state = PIPE0_STATUS_IN; /* RXRDY stays set; edpt0_xfer(STATUS IN) acks it together with DATAEND */
    pipe0.rxrdy_consumed = true;
  } else if (req->bmRequestType & TUSB_DIR_IN_MASK) {
    pipe0.state = PIPE0_DATA_IN;
    if (!pipe0.rxrdy_consumed) wr8(CSR0L, C0_RXRDYC);
  } else {
    pipe0.state = PIPE0_DATA_OUT; /* edpt0_xfer() acks RXRDY once usbd has armed the drain buffer */
    pipe0.rxrdy_consumed = true;
  }
  dcd_event_setup_received(rhport, (const uint8_t *)setup, false);
}

static void pipe0_try_deferred_setup(uint8_t rhport) {
  if (!pipe0.deferred_setup_valid) return;
  pipe0.deferred_setup_valid = false;
  pipe0_start_setup(rhport, pipe0.deferred_setup);
}

static bool pipe0_data_stage_done(uint16_t xfer_len) {
  return pipe0.remain_wlength == 0 || xfer_len < CFG_TUD_ENDPOINT0_SIZE;
}

static bool edpt0_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes) {
  TU_ASSERT(total_bytes <= CFG_TUD_ENDPOINT0_SIZE);
  const unsigned dir_in = tu_edpt_dir(ep_addr);
  ep_select(0);
  switch (pipe0.state) {
    case PIPE0_DATA_IN:
      TU_ASSERT(dir_in);
      pipe0.xact_len = total_bytes;
      if (pipe0.rxrdy_consumed) { /* replayed SETUP: ack its parked RXRDY before loading the FIFO */
        wr8(CSR0L, C0_RXRDYC);
        pipe0.rxrdy_consumed = false;
      }
      for (unsigned i = 0; i < total_bytes; ++i) fifo_write_byte(0, buffer[i]);
      pipe0.remain_wlength -= total_bytes;
      wr8(CSR0L, pipe0_data_stage_done(total_bytes) ? (C0_TXRDY | C0_DATAEND) : C0_TXRDY);
      break;
    case PIPE0_DATA_OUT:
      TU_ASSERT(!dir_in);
      pipe0.xact_len = total_bytes;
      pipe0.buf = buffer;
      wr8(CSR0L, C0_RXRDYC);
      pipe0.rxrdy_consumed = false;
      break;
    case PIPE0_STATUS_IN:
      TU_ASSERT(dir_in && total_bytes == 0);
      wr8(CSR0L, C0_RXRDYC | C0_DATAEND);
      pipe0.rxrdy_consumed = false;
      break;
    case PIPE0_STATUS_OUT:
      TU_ASSERT(!dir_in && total_bytes == 0);
      pipe0.state = PIPE0_STATUS_OUT_PENDING_XFER; /* first event of the pair: the IRQ fires the completion */
      break;
    case PIPE0_STATUS_OUT_PENDING_IRQ:
      pipe0.state = PIPE0_IDLE;
      dcd_event_xfer_complete(rhport, ep_addr, 0, XFER_RESULT_SUCCESS, false);
      pipe0_try_deferred_setup(rhport);
      break;
    default:
      break;
  }
  return true;
}

/* Advance EP0's status-stage state machine on a tail event: the csrl==0 confirmation, or such a confirmation
 * combined with a new SETUP (the caller sets deferred_setup_valid first). */
static void pipe0_process_xfer_state(uint8_t rhport) {
  switch (pipe0.state) {
    case PIPE0_DATA_IN:
      if (pipe0_data_stage_done(pipe0.xact_len)) {
        pipe0.state = pipe0.deferred_setup_valid ? PIPE0_STATUS_OUT_PENDING_IRQ : PIPE0_STATUS_OUT;
      }
      dcd_event_xfer_complete(rhport, TU_EP0_IN, pipe0.xact_len, XFER_RESULT_SUCCESS, false);
      break;
    case PIPE0_STATUS_OUT:
      pipe0.state = PIPE0_STATUS_OUT_PENDING_IRQ;
      break;
    case PIPE0_STATUS_OUT_PENDING_XFER:
      pipe0.state = PIPE0_IDLE;
      dcd_event_xfer_complete(rhport, TU_EP0_OUT, 0, XFER_RESULT_SUCCESS, false);
      pipe0_try_deferred_setup(rhport);
      break;
    case PIPE0_STATUS_OUT_PENDING_IRQ:
      break;
    case PIPE0_STATUS_IN:
      if (pipe0.pending_addr) { /* the address takes effect only after the status stage */
        apx_trace_usb(4, pipe0.pending_addr, 0); /* U4: address applied */
        wr8(FADDR, pipe0.pending_addr);
        pipe0.pending_addr = 0;
      }
      pipe0.state = PIPE0_IDLE;
      dcd_event_xfer_complete(rhport, TU_EP0_IN, 0, XFER_RESULT_SUCCESS, false);
      pipe0_try_deferred_setup(rhport);
      break;
    default:
      break;
  }
}

static void process_ep0(uint8_t rhport) {
  ep_select(0);
  const unsigned csrl = rd8(CSR0L);

  if (csrl & C0_STALLED) {
    wr8(CSR0L, 0);
    pipe0.state = PIPE0_IDLE;
    pipe0.deferred_setup_valid = false;
    pipe0.rxrdy_consumed = false;
    return;
  }
  if (csrl & C0_SETEND) {
    /* The host abandoned the control transfer (it resets and retries quickly while it enumerates). That also
     * cancels a SET_ADDRESS still waiting for its status stage: src/usb.c wrote 0xff into FADDR otherwise. */
    wr8(CSR0L, C0_SETENDC);
    pipe0.state = PIPE0_IDLE;
    pipe0.deferred_setup_valid = false;
    pipe0.rxrdy_consumed = false;
    pipe0.pending_addr = 0;
    if (!(csrl & C0_RXRDY)) return;
  }
  if (csrl & C0_RXRDY) {
    if (pipe0.rxrdy_consumed) return; /* stale: this packet was already drained */
    switch (pipe0.state) {
      case PIPE0_IDLE: {
        uint32_t setup[2];
        if (pipe0_read_setup(setup)) pipe0_start_setup(rhport, setup);
        break;
      }
      case PIPE0_DATA_OUT: {
        const unsigned count0 = rd16(RXCOUNT);
        for (unsigned i = 0; i < count0; ++i) {
          const uint8_t value = fifo_read_byte(0);
          if (pipe0.buf && i < pipe0.xact_len) pipe0.buf[i] = value;
        }
        pipe0.remain_wlength -= tu_min16((uint16_t)count0, pipe0.remain_wlength);
        pipe0.rxrdy_consumed = true; /* RXRDY stays set until the next arm acks it: NAK flow control */
        if (pipe0_data_stage_done((uint16_t)count0)) pipe0.state = PIPE0_STATUS_IN;
        dcd_event_xfer_complete(rhport, TU_EP0_OUT, (uint32_t)count0, XFER_RESULT_SUCCESS, false);
        break;
      }
      case PIPE0_DATA_IN:
      case PIPE0_STATUS_OUT:
      case PIPE0_STATUS_OUT_PENDING_XFER:
      case PIPE0_STATUS_OUT_PENDING_IRQ:
      case PIPE0_STATUS_IN:
        /* A new SETUP arrived while the old transfer's tail event is still pending: save it, finish the old
         * transfer, then replay it. */
        if (pipe0_read_setup(pipe0.deferred_setup)) {
          pipe0.deferred_setup_valid = true;
          pipe0.rxrdy_consumed = true;
          pipe0_process_xfer_state(rhport);
        }
        break;
      default:
        break;
    }
    return;
  }
  if (csrl & C0_DATAEND) return; /* the last DATA IN chunk / STATUS IN arm has not completed yet */
  /* csrl == 0: a packet was sent (TxPktRdy clear), or the status stage after DataEnd completed. */
  pipe0_process_xfer_state(rhport);
}

/*--------------------------------------------------------------------+
 * Bulk / interrupt endpoints
 *--------------------------------------------------------------------*/

static void pipe_write(unsigned epn, pipe_t *pipe) { /* INDEX is already epn */
  const uint16_t n = tu_min16(pipe->mps, pipe->remaining);
#if APX_USB_SHARED_FIFO
  wr8(TXCSRH, rd8(TXCSRH) | CSRH_MODE_TX);
#endif
#if APX_USB_PACKET_RAM
  if (packet_ram_in(epn)) {
    const uint32_t ctrl = apx_spr_read(VENDOR_CTRL);
    pram_store((ctrl & (1u << (19u + epn))) ? pram_in_second(epn) : pram_in_base[epn], pipe->buf, n);
    apx_spr_write(VENDOR_CTRL, n | 1u << (13u + epn));
  } else
#endif
  for (uint16_t i = 0; i < n; ++i) fifo_write_byte(epn, pipe->buf[i]);
  pipe->buf += n;
  pipe->remaining -= n;
  wr8(TXCSRL, TX_RDY);
}

static void flush_endpoint(unsigned epn, unsigned is_rx);

/* EP2 and EP3 use the same two buffers. A packet of one that is still waiting for the host must not be overwritten by the other's: wait until it is out. If the host
 * never takes it (a stream nobody reads) the wait ends after about 3 ms, the stale packet is flushed and the write goes ahead. */
static bool shared_busy(unsigned epn) {
  if (epn != 2 && epn != 3) return false;
  const unsigned other = epn == 2 ? 3 : 2;
  if (!pipes[1][other].mps) return false;
  ep_select(other);
  const unsigned csrl = rd8(TXCSRL);
  ep_select(epn);
  return (csrl & (TX_RDY | TX_FIFONE)) != 0;
}

static void try_write(unsigned epn, pipe_t *pipe) { /* INDEX is already epn */
  if (shared_busy(epn)) {
    if (!pipe->blocked_since) { pipe->blocked_since = apx_ticks() | 1u; return; }
    if ((int32_t)(apx_ticks() - pipe->blocked_since) < 100) return;
    const unsigned other = epn == 2 ? 3 : 2;
    ep_select(other);
    flush_endpoint(other, 0);
    ep_select(epn);
    /* The packet is gone, so the transfer it belonged to cannot finish: report it failed. (Letting it carry on as if the packet had been taken made the stalled stream look
     * alive, and two streams nobody reads then kept each other going for ever.) */
    if (pipes[1][other].armed) {
      pipes[1][other].armed = false;
      dcd_event_xfer_complete(0, tu_edpt_addr(other, TUSB_DIR_IN), 0, XFER_RESULT_FAILED, false);
    }
  }
  pipe->blocked_since = 0;
  pipe_write(epn, pipe);
  /* TxPktRdy clears as soon as the packet is in the core's FIFO (the first CSR read after the write shows it clear and the FIFO not empty), so a second packet can be loaded
   * at once into the other buffer. Doing so is what lets the host get two packets back to back: with one packet at a time the host drains it, finds the endpoint empty while
   * the next is being loaded, is NAKed and does not come back for a microframe, which caps a bulk IN stream at one 512-byte packet per 125 us = 4 MB/s. */
  /* Not for EP1: its FIFO is shared with OUT, so a second queued packet would have to be drained before the FIFO is handed back, and waiting for that deadlocks when the host's
   * next request (OUT) has already landed in the same FIFO (seen on the Action camera, whose slower CPU polls late: the IN transfer never completed). EP1 carries the debug link and
   * mass storage, which gained nothing from it. */
  if (epn != 1 && pipe->remaining > 0 && !(rd8(TXCSRL) & TX_RDY)) pipe_write(epn, pipe);
}

static void process_epin(uint8_t rhport, unsigned epn) {
  pipe_t *pipe = pipe_get(epn, 1);
  ep_select(epn);
  const unsigned csrl = rd8(TXCSRL);
  if (csrl & TX_STALLED) {
    wr8(TXCSRL, csrl & ~(unsigned)(TX_STALLED | TX_UNDRN));
    return;
  }
  if (!pipe->armed || (csrl & TX_RDY)) return; /* nothing posted, or the packet is still going out */
  if (pipe->remaining > 0) {
    try_write(epn, pipe);
    return;
  }
#if APX_USB_SHARED_FIFO
  /* EP2 and up carry no OUT traffic, so waiting for the last of two queued packets to leave the FIFO cannot deadlock; EP1 must not wait (see try_write). */
  if (!pipe->iso && epn != 1 && (csrl & TX_FIFONE)) return;
  pipe->armed = false;
  if (!pipe->iso) wr8(TXCSRH, rd8(TXCSRH) & ~(unsigned)CSRH_MODE_TX); /* hand the shared FIFO back to OUT */
#else
  pipe->armed = false;
#endif
  dcd_event_xfer_complete(rhport, tu_edpt_addr(epn, TUSB_DIR_IN), pipe->length, XFER_RESULT_SUCCESS, false);
}

static void process_epout(uint8_t rhport, unsigned epn) {
  pipe_t *pipe = pipe_get(epn, 0);
  ep_select(epn);
  const unsigned csrl = rd8(RXCSRL);
  if (csrl & RX_STALLED) {
    wr8(RXCSRL, csrl & ~(unsigned)(RX_STALLED | RX_OVER));
    return;
  }
  /* A packet with no transfer posted stays in the FIFO: the core NAKs further OUT tokens, which is the backpressure. */
  if (!pipe->armed || !(csrl & RX_RDY)) return;
  const unsigned count = rd16(RXCOUNT);
  const uint16_t take = (uint16_t)tu_min16(tu_min16(pipe->remaining, pipe->mps), (uint16_t)count);
#if APX_USB_PACKET_RAM
  if (packet_ram(epn)) {
    pram_load(pipe->buf, PRAM_OUT((apx_spr_read(VENDOR_CTRL) & VENDOR_OUT_BUFFER) ? 1u : 0u), take);
  } else
#endif
  for (unsigned i = 0; i < count; ++i) { /* drain the whole packet; bytes beyond the buffer are dropped */
    const uint8_t value = fifo_read_byte(epn);
    if (i < take) pipe->buf[i] = value;
  }
  pipe->buf += take;
  pipe->remaining -= take;
  wr8(RXCSRL, 0); /* clear RXRDY: release the FIFO */
  if (take < pipe->mps || pipe->remaining == 0) {
    pipe->armed = false;
    dcd_event_xfer_complete(rhport, epn, pipe->length - pipe->remaining, XFER_RESULT_SUCCESS, false);
  }
}

static void flush_endpoint(unsigned epn, unsigned is_rx) { /* INDEX is epn; clears the data toggle, flushes twice */
  for (unsigned i = 0; i < 2; ++i) {
    if (is_rx) wr8(RXCSRL, RX_CLRDT | RX_FLUSH);
    else wr8(TXCSRL, TX_CLRDT | TX_FLUSH);
  }
  (void)epn;
}

/*--------------------------------------------------------------------+
 * Controller
 *--------------------------------------------------------------------*/

static void phy_init(void) {
  /* The same sequence src/usb.c ran (BootROM 105d20..105f28 and 105e40..105e88); the pull-up stays off throughout. */
  apx_spr_write(0x18, apx_spr_read(0x18) & ~2u);
  apx_spr_write(0x4800, apx_spr_read(0x4800) & ~0x8000u);
  apx_spr_write(0x884c, apx_spr_read(0x884c) | 0x80000000u);
  apx_spr_write(0x9840, apx_spr_read(0x9840) | 0x10000u); apx_delay_ms(1);
  apx_spr_write(0x9840, (apx_spr_read(0x9840) & 0xfc0df3ffu) | 0x1080u); apx_delay_ms(1);
  apx_spr_write(0x9840, apx_spr_read(0x9840) | 0x40u); apx_delay_ms(10);
  /* A RAM launch inherits these two from the ROM loader; a cold SPI boot has nobody to set them. */
  apx_spr_write(0x9840, apx_spr_read(0x9840) | 0x1u); apx_delay_ms(1);
  apx_spr_write(0x9840, apx_spr_read(0x9840) | 0x4u); apx_delay_ms(1);
  apx_spr_write(0x9870, (apx_spr_read(0x9870) & 0x3fffffffu) | 0x80000000u);
  apx_spr_write(0x8850, (apx_spr_read(0x8850) & ~0x08000000u) | 0x03000000u);
  apx_spr_write(0x8844, apx_spr_read(0x8844) & ~1u);
  wr8(POWER, rd8(POWER) & ~(unsigned)POWER_SOFTCONN); apx_delay_ms(100);
  apx_spr_write(0xd840, apx_spr_read(0xd840) & ~1u);
  apx_spr_write(0xd804, 31);
  const uint32_t start = apx_ticks();
  while (!(apx_spr_read(0xd804) & 8)) {
    apx_maintenance();
    if (apx_ticks() - start > 32768) break;
  }
  apx_spr_write(0xd800, 0xf0); apx_spr_write(0xd808, 0);
  apx_spr_write(0xd80c, apx_spr_read(0xd80c) | 0x1800);
  apx_spr_write(0x984c, 0x8000); apx_spr_write(0x9850, 0x8040); apx_spr_write(0x9854, 0x8240);
  apx_trace_usb(0xb, apx_ticks() - start, apx_spr_read(0xd804)); /* U11: PHY wait in ticks, then 0xd804 */
}

static void trace_phy(void) { /* ring builds: the registers the PHY and USB clocks depend on */
  static const uint16_t list[] = { 0xd800, 0xd804, 0xd808, 0xd80c, 0xd840, 0x9840, 0x9870, 0x984c, 0x9850, 0x9854,
                                   0x884c, 0x8850, 0x8844, 0x8848, 0x9800, 0x0018, 0x4800 };
  for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); ++i) apx_trace_spr(list[i]);
}

static void reset_state(void) {
  index_cache = 0xffu;
  tu_memclr(&pipe0, sizeof(pipe0));
  tu_memclr(pipes, sizeof(pipes));
  wr8(FADDR, 0);
}

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
  (void)rhport; (void)rh_init;
  phy_init();
  wr16(INTRTXE, 1); wr16(INTRRXE, 0); wr8(INTRUSBE, 7);
  reset_state();
  dcd_connect(rhport);
  trace_phy();
  return true;
}

bool dcd_deinit(uint8_t rhport) {
  dcd_disconnect(rhport);
  return true;
}

void dcd_int_enable(uint8_t rhport) { (void)rhport; } /* polled: the SDK has no USB interrupt routing */
void dcd_int_disable(uint8_t rhport) { (void)rhport; }

void dcd_set_address(uint8_t rhport, uint8_t dev_addr) {
  (void)rhport;
  pipe0.pending_addr = dev_addr;
  pipe0.buf = NULL;
  pipe0.xact_len = 0;
  pipe0.state = PIPE0_STATUS_IN;
  ep_select(0);
  wr8(CSR0L, C0_RXRDYC | C0_DATAEND); /* STATUS IN ZLP; the host's ACK is the confirmation that latches the address */
  pipe0.rxrdy_consumed = false;
}

void dcd_remote_wakeup(uint8_t rhport) {
  (void)rhport;
  wr8(POWER, rd8(POWER) | POWER_RESUME);
  apx_delay_ms(10);
  wr8(POWER, rd8(POWER) & ~(unsigned)POWER_RESUME);
}

void dcd_connect(uint8_t rhport) {
  (void)rhport;
  wr8(POWER, TUD_OPT_HIGH_SPEED ? (POWER_HSENAB | POWER_SOFTCONN) : POWER_SOFTCONN);
}

void dcd_disconnect(uint8_t rhport) {
  (void)rhport;
  wr8(POWER, rd8(POWER) & ~(unsigned)POWER_SOFTCONN);
}

void dcd_sof_enable(uint8_t rhport, bool en) { (void)rhport; (void)en; }

/*--------------------------------------------------------------------+
 * Endpoint API
 *--------------------------------------------------------------------*/

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *ep_desc) {
  (void)rhport;
  const unsigned epn = tu_edpt_number(ep_desc->bEndpointAddress);
  const unsigned dir_in = tu_edpt_dir(ep_desc->bEndpointAddress);
  const unsigned mps = tu_edpt_packet_size(ep_desc);
  TU_ASSERT(epn >= 1 && epn < TUP_DCD_ENDPOINT_MAX);
  TU_ASSERT(mps <= fifo_limit[epn]); /* an endpoint whose FIFO size is unknown cannot be opened yet */
#if APX_USB_PACKET_RAM && !defined(APX_USB_PROBE_EPS)
  TU_ASSERT(epn == 1 || dir_in); /* EP2.. OUT: the receive buffer register is not characterised */
  if (dir_in && packet_ram_in(epn)) apx_spr_write(0x9850u + 8u * (epn - 1u), pram_in_base[epn]); /* where this endpoint's IN buffers are */
  if (!dir_in && epn == 1) apx_spr_write(0x9854u, PRAM_OUT_BASE); /* and EP1's OUT buffers */
#endif

  pipe_t *pipe = pipe_get(epn, dir_in);
  pipe->buf = NULL;
  pipe->length = pipe->remaining = 0;
  pipe->mps = (uint16_t)mps;
  pipe->armed = false;
  pipe->halted = false;

  ep_select(epn);
  if (dir_in) { wr16(TXMAXP, mps); wr8(TXCSRH, 0); }
  else { wr16(RXMAXP, mps); wr8(RXCSRH, 0); }
  flush_endpoint(epn, !dir_in);
  if (dir_in) wr16(INTRTXE, rd8(INTRTXE) | (unsigned)rd8(INTRTXE + 1) << 8 | (1u << epn));
  else wr16(INTRRXE, rd8(INTRRXE) | (unsigned)rd8(INTRRXE + 1) << 8 | (1u << epn));
  ep_select(0);
  return true;
}

/* Isochronous IN endpoints for the SDK's own class drivers (the UAC microphone in src/tusb/uac.c). Not through TinyUSB's usbd_edpt_iso_* (TUP_DCD_EDPT_ISO_ALLOC
 * would also change how its video class treats its bulk endpoints): the driver activates the endpoint itself on SET_INTERFACE and queues packets with
 * usbd_edpt_xfer(). TXCSRH bit 6 = ISO, bit 5 = TX mode (set for good: no OUT side). */
bool apx_dcd_iso_activate(uint8_t ep_addr, uint16_t mps) {
  const unsigned epn = tu_edpt_number(ep_addr);
  if (!tu_edpt_dir(ep_addr) || epn < 2 || epn >= TUP_DCD_ENDPOINT_MAX || mps > fifo_limit[epn] || !packet_ram_in(epn)) return false;
  pipe_t *pipe = pipe_get(epn, 1);
  pipe->buf = NULL;
  pipe->length = pipe->remaining = 0;
  pipe->mps = mps;
  pipe->armed = false;
  pipe->halted = false;
  pipe->iso = true;
  ep_select(epn);
  apx_spr_write(0x9850u + 8u * (epn - 1u), pram_in_base[epn]);
  wr16(TXMAXP, mps);
  wr8(TXCSRH, CSRH_ISO | CSRH_MODE_TX);
  flush_endpoint(epn, 0);
  wr16(INTRTXE, rd8(INTRTXE) | (unsigned)rd8(INTRTXE + 1) << 8 | (1u << epn));
  ep_select(0);
  return true;
}

void apx_dcd_iso_deactivate(uint8_t ep_addr) {
  const unsigned epn = tu_edpt_number(ep_addr);
  if (epn < 2 || epn >= TUP_DCD_ENDPOINT_MAX) return;
  pipe_t *pipe = pipe_get(epn, 1);
  if (pipe->armed) { /* a packet that will never go out: report the transfer as over, or TinyUSB keeps the endpoint busy for ever (its close call is a no-op with ISO alloc) and the next start cannot send */
    pipe->armed = false;
    dcd_event_xfer_complete(0, ep_addr, 0, XFER_RESULT_FAILED, false);
  }
  pipe->armed = false;
  pipe->mps = 0;
  pipe->iso = false;
  ep_select(epn);
  flush_endpoint(epn, 0);
  wr8(TXCSRH, 0);
  ep_select(0);
}

bool dcd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size) {
  (void)rhport; (void)ep_addr; (void)largest_packet_size;
  return false; /* isochronous endpoints arrive with the UVC phase, after their FIFOs are measured */
}

bool dcd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const *ep_desc) {
  (void)rhport; (void)ep_desc;
  return false;
}

void dcd_edpt_close_all(uint8_t rhport) {
  (void)rhport;
  wr16(INTRTXE, 1); wr16(INTRRXE, 0);
  for (unsigned epn = 1; epn < TUP_DCD_ENDPOINT_MAX; ++epn) {
    ep_select(epn);
    for (unsigned is_rx = 0; is_rx < 2; ++is_rx) {
      flush_endpoint(epn, is_rx);
      wr16(is_rx ? RXMAXP : TXMAXP, 0);
      wr8(is_rx ? RXCSRH : TXCSRH, 0);
    }
  }
  ep_select(0);
  tu_memclr(pipes, sizeof(pipes));
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes, bool is_isr) {
  (void)is_isr;
  const unsigned epn = tu_edpt_number(ep_addr);
  if (epn == 0) return edpt0_xfer(rhport, ep_addr, buffer, total_bytes);
  const unsigned dir_in = tu_edpt_dir(ep_addr);
  pipe_t *pipe = pipe_get(epn, dir_in);
  pipe->buf = buffer;
  pipe->length = pipe->remaining = total_bytes;
  pipe->armed = true;
  if (dir_in) {
    ep_select(epn);
    pipe->blocked_since = 0;
    try_write(epn, pipe);
    ep_select(0);
  }
  return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
  (void)rhport;
  const unsigned epn = tu_edpt_number(ep_addr);
  if (epn == 0) {
    if (ep_addr == TU_EP0_OUT) { /* ignore EP0 IN */
      pipe0.state = PIPE0_IDLE;
      pipe0.buf = NULL;
      if (pipe0.deferred_setup_valid) {
        /* The stalled transfer already ended on the wire and the host's next request was ACKed: replay it. */
        pipe0_try_deferred_setup(rhport);
      } else {
        pipe0.rxrdy_consumed = false;
        ep_select(0);
        wr8(CSR0L, C0_RXRDYC | C0_STALL); /* 0x60, as src/usb.c's control_stall() */
      }
    }
    return;
  }
  const unsigned dir_in = tu_edpt_dir(ep_addr);
  ep_select(epn);
  pipe_get(epn, dir_in)->halted = true;
  /* A halt aborts the transfer: flush the staged packet first or its bytes join the next transfer after the un-halt. */
  if (dir_in) { wr8(TXCSRL, TX_FLUSH); wr8(TXCSRL, TX_STALL); }
  else { wr8(RXCSRL, RX_FLUSH); wr8(RXCSRL, RX_STALL); }
  ep_select(0);
  pipe_get(epn, dir_in)->armed = false;
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) {
  (void)rhport;
  const unsigned epn = tu_edpt_number(ep_addr);
  ep_select(epn);
  pipe_get(epn, tu_edpt_dir(ep_addr))->halted = false;
  flush_endpoint(epn, !tu_edpt_dir(ep_addr)); /* clears SENDSTALL and the data toggle (the values src/usb.c used) */
  ep_select(0);
}

/*--------------------------------------------------------------------+
 * Polling entry
 *--------------------------------------------------------------------*/

static void process_bus_reset(uint8_t rhport) {
  reset_state();
  wr16(INTRTXE, 1); wr16(INTRRXE, 0);
  for (unsigned epn = 1; epn < TUP_DCD_ENDPOINT_MAX; ++epn) {
    ep_select(epn);
    flush_endpoint(epn, 0);
    flush_endpoint(epn, 1);
  }
  ep_select(0);
  dcd_event_bus_reset(rhport, (rd8(POWER) & POWER_HSMODE) ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL, false);
}

void dcd_int_handler(uint8_t rhport) {
  /* Reads clear the flags. Only endpoints 1..6 exist, so the high bytes (bits 8..15) are never needed. */
  const unsigned intr_usb = rd8(INTRUSB);
  const unsigned intr_tx = rd8(INTRTX);
  (void)rd8(INTRRX);
  if (intr_usb) apx_trace_usb(1, intr_usb << 8 | rd8(POWER), intr_tx); /* U1: INTRUSB (4 reset, 1 suspend, 2 resume) | POWER, INTRTX */

  if (intr_usb & IS_RESET) {
    static unsigned first_reset;
    process_bus_reset(rhport);
    if (!first_reset) { first_reset = 1; trace_phy(); }
    return;
  }
  if (intr_usb & IS_RESUME) dcd_event_bus_signal(rhport, DCD_EVENT_RESUME, false);
  if (intr_usb & IS_SUSPEND) dcd_event_bus_signal(rhport, DCD_EVENT_SUSPEND, false);

  /* EP0 on its interrupt flag, or whenever its CSR shows something to do; a bare csrl==0 is a confirmation only
   * when the flag says so. */
  ep_select(0);
  const unsigned csr0 = rd8(CSR0L);
  if ((intr_tx & 1u) || (csr0 & (C0_RXRDY | C0_SETEND | C0_STALLED))) process_ep0(rhport);

  /* The bulk endpoints are serviced from their CSRs, not from the interrupt flags (what src/usb.c did). */
  for (unsigned epn = 1; epn < TUP_DCD_ENDPOINT_MAX; ++epn) {
    if (pipes[1][epn].armed || pipes[1][epn].halted) process_epin(rhport, epn);
    if (pipes[0][epn].armed || pipes[0][epn].halted) process_epout(rhport, epn);
  }
  ep_select(0);
}

#endif

#ifdef APX_DCD_DIAG /* temporary diagnostics: the state of the two video endpoints, printed by examples/uvc-camera when built with -DAPX_DCD_DIAG */
#include <stdio.h>
void apx_dcd_diag(void) {
  for (unsigned epn = 2; epn <= 3; ++epn) {
    pipe_t *pipe = pipe_get(epn, 1);
    ep_select(epn);
    printf("dcd ep%u: txcsrl %02x txcsrh %02x armed %d rem %u len %u blocked %lu halted %d | vctrl %08lx\n", epn, (unsigned)rd8(TXCSRL), (unsigned)rd8(TXCSRH), pipe->armed, (unsigned)pipe->remaining,
           (unsigned)pipe->length, (unsigned long)pipe->blocked_since, pipe->halted, (unsigned long)apx_spr_read(VENDOR_CTRL));
  }
}
#endif
