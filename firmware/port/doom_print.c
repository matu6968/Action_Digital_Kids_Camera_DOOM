/*
 * doom_print.c - print a screenshot on the Silvergear's thermal printer (modelled on LoopyDOOM's print feature: a request, a blocking
 * print with a message on screen, then back to the game).
 *
 *   1. The PRINT key (input_silvergear.c) calls plat_print_request().
 *   2. I_FinishUpdate_e32 has just converted a clean frame (no menu: PRINT does nothing in menus) and calls plat_print_service(),
 *      which copies that frame as 8-bit luma into its own buffer. The printer borrows the buffer until the job ends.
 *   3. The game stands still while the SDK's cooperative printer driver is polled in a loop that also keeps USB, buttons and battery
 *      serviced. A message box is shown BEFORE the head starts and is not touched while it runs: the printer moves the paper one
 *      half-row at a time (about 14 ms each), and anything that delays its next poll, such as redrawing the screen for a progress
 *      bar (a stall of up to 36 ms about every 250 ms in the first version), shows up as a visible stripe. EXIT cancels. Paper,
 *      head temperature and battery problems end the job with a message instead of a silent failure.
 *   4. The game clock is resynchronised (otherwise the engine would run the 15+ seconds of missed tics at once) and the sound resumes.
 *
 * The frame is 240x160 (3:2). The SDK rotates it clockwise and scales it to fill the 384-dot head: 384x576 dots, the long side along
 * the paper. Gray mode (the SDK's stock-derived photo rendering) is used.
 */
#include "plat.h"
#include "plat_print.h"

#if BOARD_HAS_PRINTER

#include <stdio.h>
#include <string.h>

#include <appotech/soc.h>
#include <appotech/debug.h>
#include <appotech/buttons.h>
#include <appotech/display.h>
#include <appotech/battery.h>
#include <appotech/power.h>
#include <appotech/camera.h>
#include <appotech/printer.h>
#include "../sdk/src/printer_image.h"

#define SRC_W  BOARD_DOOM_WIDTH
#define SRC_H  BOARD_DOOM_HEIGHT
#define OUT_W  APX_PRINTER_WIDTH
#define OUT_H  (OUT_W * SRC_W / SRC_H)          /* 384 * 240 / 160 = 576 after the clockwise rotation (160 wide, 240 tall) */

static uint8_t luma[SRC_W * SRC_H] DOOM_BUFFER; /* borrowed by the printer until the job ends: never touched meanwhile */
static unsigned requested;
static unsigned battery_started;

/* The printer driver checks the camera state (the sensors share pads with the printer). DOOM never starts a camera, so the
 * driver's own camera code is not linked: this stub says "off", like the SDK's printer-test does. */
void apx_camera_get_info(struct apx_camera_info *out) { memset(out, 0, sizeof *out); }

void plat_print_request(void) { requested = 1; }

/* Everything the SDK wants polled around a job (plat_poll() minus the audio mixer, plus the battery and the printer). */
static void service(void)
{
    apx_debug_poll();
    apx_buttons_poll();
    apx_battery_poll();
    apx_display_poll();
    apx_printer_poll();
    apx_maintenance();
}

/* The same while the head is running: no display work, so nothing but the printer's own cost sits between two half-rows. */
static void service_printing(void)
{
    apx_debug_poll();
    apx_buttons_poll();
    apx_battery_poll();
    apx_printer_poll();
    apx_maintenance();
}

static int exit_held(void)
{
    struct apx_buttons_info b;
    apx_buttons_get_info(&b);
    return (b.state & APX_BUTTON_EXIT) != 0;
}

static void wait_ms(unsigned ms)
{
    const uint32_t t0 = apx_ticks();
    while (!apx_deadline_expired(t0 + ms * 33u)) service();      /* 32768 ticks per second: about 33 per ms */
}

static const char *error_text(uint32_t error)
{
    switch (error) {
    case APX_PRINTER_PAPER:       return "NO PAPER";
    case APX_PRINTER_TEMPERATURE: return "HEAD TOO HOT";
    case APX_PRINTER_POWER:       return "BATTERY LOW";
    case APX_PRINTER_BUSY:        return "PRINTER BUSY";
    case APX_PRINTER_TIMEOUT:     return "PRINTER TIMEOUT";
    default:                      return "PRINTER ERROR";
    }
}

/* Runs one job. Returns the final state (APX_PRINTER_DONE / ERROR / CANCELLED); on ERROR *error is the reason. */
static unsigned run_job(uint32_t *error)
{
    *error = APX_PRINTER_OK;

    (void)apx_power_domain_enable();            /* returns -1 when the domain is already on (a RAM launch from stock): fine */
    if (!battery_started) { (void)apx_battery_init(); battery_started = 1; }

    /* The printer refuses to heat without a valid battery reading, which takes a few conversions after init. */
    plat_overlay_show("PRINTING...", "CHECKING BATTERY", -1);
    for (uint32_t t0 = apx_ticks(); !apx_deadline_expired(t0 + 2u * 32768u);) {
        struct apx_battery_info b;
        service();
        apx_battery_get_info(&b);
        if (b.valid) break;
    }
    /* The one message that stays up while the head runs. Let the display finish sending it first. */
    plat_overlay_show("PRINTING...", "EXIT CANCELS", -1);
    wait_ms(250);

    (void)apx_printer_set_mode(APX_PRINTER_GRAY);
    const int e = apx_printer_submit_photo_luma(luma, sizeof luma, SRC_W, SRC_H, SRC_W, OUT_W, OUT_H);
    if (e) {
        printf("print: submit failed %d\n", e);
        *error = (uint32_t)e;
        return APX_PRINTER_ERROR;
    }

    int cancel_sent = 0;
    for (const uint32_t t0 = apx_ticks(); apx_printer_busy();) {
        service_printing();
        if (!cancel_sent && (exit_held() || apx_deadline_expired(t0 + 120u * 32768u))) {
            apx_printer_cancel();
            cancel_sent = 1;
        }
    }
    struct apx_printer_info p;
    apx_printer_get_info(&p);
    printf("print ended: state %u error %u row %u/%u temperature_adc %u battery %u mV\n", (unsigned)p.state, (unsigned)p.error,
           (unsigned)p.row, (unsigned)p.rows, (unsigned)p.temperature_adc, (unsigned)p.battery_mv);
    /* step period per half-row in 32768 Hz ticks (30.5 us): min/max/average, and how many half-rows fell in each of the bins
     * <9, <11, <13, <15, <18, <24, >=24 ms. A print without visible stripes has nothing in the last bins. */
    printf("print steps: period min %u max %u avg %u ticks, histogram %u %u %u %u %u %u %u\n", (unsigned)p.period_min, (unsigned)p.period_max,
           p.period_count ? (unsigned)(p.period_sum / p.period_count) : 0u, (unsigned)p.period_hist[0], (unsigned)p.period_hist[1],
           (unsigned)p.period_hist[2], (unsigned)p.period_hist[3], (unsigned)p.period_hist[4], (unsigned)p.period_hist[5], (unsigned)p.period_hist[6]);
    *error = p.error;
    return p.state;
}

int plat_print_service(const uint16_t *frame, unsigned words_per_row, const uint16_t *lut_y2)
{
    if (!requested) return 0;
    requested = 0;

    /* The engine's frame holds each pixel twice; lut_y2 gives the luma of an index already doubled, so one halfword = two pixels. */
    uint16_t *dst = (uint16_t *)luma;
    for (unsigned y = 0; y < SRC_H; y++)
        for (unsigned x = 0; x < words_per_row; x++)
            dst[y * words_per_row + x] = lut_y2[frame[y * words_per_row + x] & 0xffu];

    plat_audio_suspend(1);
    uint32_t error;
    const unsigned state = run_job(&error);
    if (state == APX_PRINTER_DONE)           plat_overlay_show("PRINT FINISHED", "TEAR OFF THE STICKER", -1);
    else if (state == APX_PRINTER_CANCELLED) plat_overlay_show("PRINT CANCELLED", 0, -1);
    else                                     plat_overlay_show("PRINT FAILED", error_text(error), -1);
    apx_printer_shutdown();                     /* give the shared pads (and the UART routing) back */
    wait_ms(state == APX_PRINTER_DONE ? 1500u : 3000u);

    plat_audio_suspend(0);
    plat_time_resync();
    return 1;
}

#else  /* no printer on this board */

void plat_print_request(void) { }
int plat_print_service(const uint16_t *frame, unsigned words_per_row, const uint16_t *lut_y2)
{
    (void)frame; (void)words_per_row; (void)lut_y2;
    return 0;
}

#endif
