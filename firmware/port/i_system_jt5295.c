/*
 * i_system_jt5295.c - GBADoom's platform layer for the Action kids camera.
 *
 * GBADoom keeps everything hardware dependent behind the *_e32 functions declared in i_system_e32.h. They are
 * implemented here, on top of the polled SDK in sdk/.
 *
 * Frame format: the engine renders 8-bit palette indices into an array of unsigned short, two identical indices per
 * word, so the 120-word-wide buffer is a 240x160 image. The panel is 220x176 landscape, so 10 pixels are cropped on each
 * side and 8 black rows are left above and below. The display is fed NV21 planes (luma + interleaved V,U at half
 * resolution) which suits this engine well: pixels come in identical pairs, so one palette lookup gives the chroma
 * of a whole 2-pixel group and one halfword store writes two luma bytes.
 */
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <appotech/soc.h>
#include <appotech/debug.h>
#include <appotech/buttons.h>
#include <appotech/display.h>
#include <appotech/power.h>
#include <appotech/audio.h>

#include "doomtype.h"
#include "doomdef.h"
#include "d_event.h"
#include "d_main.h"
#include "i_system_e32.h"

#include "plat.h"
#include "plat_video.h"

#define SRC_WORDS   120u                       /* SCREENWIDTH: words per source row */
#define CROP_WORDS  (BOARD_CROP_X / 2u)        /* words cropped on each side */
#define OUT_WORDS   (SRC_WORDS - 2u * CROP_WORDS)

/* ------------------------------------------------------------------ framebuffer handed to the engine */

#define FB_SHORTS (SRC_WORDS * BOARD_DOOM_HEIGHT)
static unsigned short framebuffer[FB_SHORTS] DOOM_BUFFER;

/* Front and back buffer are the same memory (as in the reference port): the screen melt then looks scruffy, nothing else. */
unsigned short *I_GetBackBuffer(void)  { return framebuffer; }
unsigned short *I_GetFrontBuffer(void) { return framebuffer; }

int  I_GetVideoWidth_e32(void)  { return (int)SRC_WORDS; }
int  I_GetVideoHeight_e32(void) { return (int)BOARD_DOOM_HEIGHT; }

void I_InitScreen_e32(void)       { }
void I_CreateBackBuffer_e32(void) { }
void I_BlitScreenBmp_e32(void)    { }
void I_CreateWindow_e32(void)     { }
void I_ClearWindow_e32(void)      { memset(framebuffer, 0, sizeof framebuffer); }

/* ------------------------------------------------------------------ palette -> luma / chroma tables */

static uint16_t lut_y2[256];     /* luma, duplicated into both bytes: one halfword store = two pixels */
static uint16_t lut_vu[256];     /* V in the low byte, U in the high byte (this panel's NV21 is V,U) */
static const byte *lut_source;

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void I_SetPallete_e32(const byte *pallete)
{
    if (!pallete) return;
    lut_source = pallete;
    for (unsigned i = 0; i < 256; i++) {
        const int r = pallete[i * 3 + 0], g = pallete[i * 3 + 1], b = pallete[i * 3 + 2];
        /* full-range BT.601 */
        const uint8_t y = clamp8((77 * r + 150 * g + 29 * b) >> 8);
        const uint8_t u = clamp8(128 + ((-43 * r - 85 * g + 128 * b) >> 8));
        const uint8_t v = clamp8(128 + ((128 * r - 107 * g - 21 * b) >> 8));
        lut_y2[i] = (uint16_t)(y | (y << 8));
        lut_vu[i] = (uint16_t)(v | (u << 8));
    }
}

#if DOOM_STATS
static uint32_t stat_frames, stat_t0, stat_tics;
#endif

void I_FinishUpdate_e32(const byte *srcBuffer, const byte *pallete, const unsigned int width, const unsigned int height)
{
    if (!srcBuffer) return;
    if (pallete && pallete != lut_source) I_SetPallete_e32(pallete);

    const uint16_t *src = (const uint16_t *)srcBuffer;
    uint8_t *py = plat_plane_y();
    uint8_t *pvu = plat_plane_vu();

    for (unsigned y = 0; y < height; y++) {
        const uint16_t *s = src + y * width + CROP_WORDS;
        uint16_t *dy = (uint16_t *)(py + (y + BOARD_ORIGIN_Y) * BOARD_NV21_PITCH);
        if (!(y & 1u)) {
            uint16_t *dc = (uint16_t *)(pvu + ((y + BOARD_ORIGIN_Y) >> 1) * BOARD_NV21_PITCH);
            for (unsigned x = 0; x < OUT_WORDS; x++) {
                const unsigned idx = s[x] & 0xffu;
                dy[x] = lut_y2[idx];
                dc[x] = lut_vu[idx];
            }
        } else {
            for (unsigned x = 0; x < OUT_WORDS; x++)
                dy[x] = lut_y2[s[x] & 0xffu];
        }
    }
    plat_present();

#if DOOM_STATS
    stat_frames++;
    const uint32_t now = plat_ms();
    if (now - stat_t0 >= 5000u) {
        printf("stats: %u frames in %u ms (%u fps*10), %u tics, audio underruns %u\n", (unsigned)stat_frames,
               (unsigned)(now - stat_t0), (unsigned)(stat_frames * 10000u / (now - stat_t0)), (unsigned)stat_tics,
               (unsigned)apx_audio_stream_underruns());
        stat_frames = 0; stat_t0 = now;
    }
#endif
}

/* ------------------------------------------------------------------ time: 35 tics per second from the 32.768 kHz counter */

int I_GetTime_e32(void)
{
    static uint32_t last, acc;
    static int tics;
    static uint32_t last_poll;

    const uint32_t now = apx_ticks();
    /* Poll the SDK about once a millisecond: USB, buttons (20 ms debounce, wants a poll at least every 5 ms), display DMA. */
    if (now - last_poll >= 33u) { last_poll = now; plat_poll(); }

    acc += (now - last) * 35u;        /* ticks * 35; one tic = 32768 */
    last = now;
    while (acc >= 32768u) { acc -= 32768u; tics++; }
#if DOOM_STATS
    stat_tics = (uint32_t)tics;
#endif
    return tics;
}

/* ------------------------------------------------------------------ input
 *
 * Five keys (SHUTTER, UP, DOWN, LEFT, RIGHT) share one ADC resistor ladder, so only one of them is ever seen at a time.
 * POWER is a separate digital input and can be combined with one of them. Mapping to GBADoom's GBA keys:
 *
 *   SHUTTER                 B      fire
 *   UP / DOWN / LEFT / RIGHT       walk and turn (menus: navigate)
 *   POWER + LEFT / RIGHT    L / R  strafe
 *   POWER + UP / DOWN       A+R / A+L   next / previous weapon
 *   POWER, tap              A      use / open / menu "enter"
 *   POWER, hold 0.65 s      START  menu
 *   POWER, hold 3 s         power the camera off
 */
#define KBIT(k) (1u << ((k) - KEYD_A))

static uint16_t keys_prev;
static uint32_t power_down_ms;
static unsigned power_state;        /* 0 idle, 1 held (candidate tap), 2 held and consumed (modifier / menu sent) */
static unsigned power_armed;        /* the press that powered the camera on is ignored until it is released */
static uint32_t pulse_until;        /* timed virtual key (tap) */
static uint16_t pulse_keys;

/* Development hook: tools/keys.py writes a bit mask (bit 0 = KEYD_A ... bit 9 = KEYD_SELECT) and the magic word, which makes the
 * game drivable from the host (menus, level start, movement) without touching the camera. Compiled in with DOOM_STATS. */
#if DOOM_STATS
volatile uint32_t doom_host_keys_magic;
volatile uint32_t doom_host_keys;
#define HOST_KEYS_MAGIC 0x5359454bu    /* "KEYS" */
#endif

void plat_input_init(void)
{
    keys_prev = 0;
    power_state = 0;
    power_armed = 0;
    pulse_until = 0;
    pulse_keys = 0;
}

void I_StartWServEvents_e32(void) { }
void I_PollWServEvents_e32(void)  { }

void I_ProcessKeyEvents(void)
{
    struct apx_buttons_info bi;
    apx_buttons_get_info(&bi);
    const uint32_t s = bi.state;
    const uint32_t now = plat_ms();
    const int power = (s & APX_BUTTON_POWER) != 0;
    uint16_t keys = 0;

    if (!power) power_armed = 1;

    if (power && power_armed) {
        if (power_state == 0) { power_state = 1; power_down_ms = now; }
        const uint32_t held = now - power_down_ms;
        if (s & (APX_BUTTON_LEFT | APX_BUTTON_RIGHT | APX_BUTTON_UP | APX_BUTTON_DOWN | APX_BUTTON_SHUTTER)) power_state = 2;
        if (s & APX_BUTTON_LEFT)  keys |= KBIT(KEYD_L);
        if (s & APX_BUTTON_RIGHT) keys |= KBIT(KEYD_R);
        if (s & APX_BUTTON_UP)    keys |= KBIT(KEYD_A) | KBIT(KEYD_R);
        if (s & APX_BUTTON_DOWN)  keys |= KBIT(KEYD_A) | KBIT(KEYD_L);
        if (s & APX_BUTTON_SHUTTER) keys |= KBIT(KEYD_B);
        if (power_state == 1 && held >= BOARD_POWER_MENU_MS) {
            power_state = 3;                           /* menu sent; keep counting towards power-off */
            pulse_keys = KBIT(KEYD_START); pulse_until = now + 120u;
        }
        if (power_state == 3 && held >= BOARD_POWER_OFF_MS) {
            printf("power off requested\n");
            (void)apx_power_off();
        }
    } else {
        if (power_state == 1 && power_armed) {         /* released before the menu threshold, nothing else pressed: tap */
            if (now - power_down_ms < BOARD_POWER_TAP_MS) { pulse_keys = KBIT(KEYD_A); pulse_until = now + 120u; }
        }
        power_state = 0;
        if (s & APX_BUTTON_SHUTTER) keys |= KBIT(KEYD_B);
        if (s & APX_BUTTON_UP)      keys |= KBIT(KEYD_UP);
        if (s & APX_BUTTON_DOWN)    keys |= KBIT(KEYD_DOWN);
        if (s & APX_BUTTON_LEFT)    keys |= KBIT(KEYD_LEFT);
        if (s & APX_BUTTON_RIGHT)   keys |= KBIT(KEYD_RIGHT);
    }

    /* Only the plain direction keys are suppressed while POWER is down; in the POWER branch above they were remapped. */
    if (pulse_until) {
        if ((int32_t)(now - pulse_until) >= 0) pulse_until = 0, pulse_keys = 0;
        else keys |= pulse_keys;
    }

#if DOOM_STATS
    if (doom_host_keys_magic == HOST_KEYS_MAGIC) keys |= (uint16_t)(doom_host_keys & 0x3ffu);
#endif

    const uint16_t changed = keys ^ keys_prev;
    if (changed) {
        for (unsigned k = KEYD_A; k <= KEYD_SELECT; k++) {
            if (changed & KBIT(k)) {
                event_t ev;
                ev.type = (keys & KBIT(k)) ? ev_keydown : ev_keyup;
                ev.data1 = (int)k;
                D_PostEvent(&ev);
            }
        }
        keys_prev = keys;
    }
}

/* ------------------------------------------------------------------ errors and exit */

void I_Error(const char *error, ...)
{
    char msg[96];
    va_list ap;
    va_start(ap, error);
    vsnprintf(msg, sizeof msg, error, ap);
    va_end(ap);
    printf("I_Error: %s\n", msg);

    /* Show the message on screen in 3 lines of up to 17 characters. */
    char l[3][18];
    memset(l, 0, sizeof l);
    for (unsigned i = 0; i < 51 && msg[i]; i++) l[i / 17][i % 17] = msg[i];
    plat_status(l[0], l[1][0] ? l[1] : 0, l[2][0] ? l[2] : 0, 1);
    for (;;) plat_poll();      /* never wfi: keeps USB alive so the camera can still be recovered */
}

void I_Quit_e32(void)
{
    for (;;) plat_poll();
}

/* Restart after the last map of the (single map) WAD; the SDK's graceful reboot restarts the image. */
void I_Reboot_console(void)
{
    (void)apx_reboot();
    for (;;) plat_poll();
}

/* ------------------------------------------------------------------ zone memory (GBADoom/source/z_zone.c) */
unsigned char doom_zone_storage[256 * 1024] DOOM_BUFFER;
