/*
 * i_system_jt5295.c - GBADoom's platform layer for the Action kids cameras (Vision and Silvergear).
 *
 * GBADoom keeps everything hardware dependent behind the *_e32 functions declared in i_system_e32.h. They are
 * implemented here, on top of the polled SDK in sdk/.
 *
 * Frame format: the engine renders 8-bit palette indices into an array of unsigned short, two identical indices per
 * word, so the 120-word-wide buffer is a 240x160 image. The display is fed NV21 planes (luma + interleaved V,U at half
 * resolution) which suits this engine well: pixels come in identical pairs, so one palette lookup gives the chroma
 * of a whole 2-pixel group and one halfword store writes two luma bytes.
 *
 *   Action Vision: the panel is 220x176 landscape, so 10 pixels are cropped on each side and 8 black rows are left above and
 *                  below; the image is drawn 1:1.
 *   Silvergear:    the panel is 320x240 landscape. The 120 distinct columns are scaled to 320 pixels (8 out per 3 in) and the
 *                  160 rows to 216 (27 per 20), centred with black bars above and below (BOARD_SCALE_8_3).
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
#include "plat_input.h"
#include "plat_print.h"
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

#if BOARD_SCALE_8_3
/* Silvergear: 120 source words -> 320 output pixels. Output pixel x shows source word floor(x * 3 / 8), i.e. every group of 3 words
 * (w0 w1 w2) becomes the 8 pixels  w0 w0 w0 w1 | w1 w1 w2 w2  = 4 luma halfwords  [w0 w0] [w0 w1] [w1 w1] [w2 w2]  and 4 chroma
 * halfwords  vu(w0) vu(w0) vu(w1) vu(w2)  (one V,U pair per 2 output pixels, taken from the left pixel). */
#define SCALE_GROUPS (BOARD_LCD_WIDTH / 8u)

static inline __attribute__((always_inline)) void scale_row(const uint16_t *s, uint16_t *dy, uint16_t *dc, int luma, int chroma)
{
    for (unsigned g = 0; g < SCALE_GROUPS; g++, s += 3, dy += 4, dc += 4) {
        const unsigned i0 = s[0] & 0xffu, i1 = s[1] & 0xffu, i2 = s[2] & 0xffu;
        if (luma) {
            const uint16_t y0 = lut_y2[i0], y1 = lut_y2[i1];
            dy[0] = y0;
            dy[1] = (uint16_t)((y0 & 0x00ffu) | (y1 & 0xff00u));
            dy[2] = y1;
            dy[3] = lut_y2[i2];
        }
        if (chroma) {
            dc[0] = lut_vu[i0];
            dc[1] = lut_vu[i0];
            dc[2] = lut_vu[i1];
            dc[3] = lut_vu[i2];
        }
    }
}

/* Output row dy shows source row dy * 20 / 27. Built once. */
static uint8_t row_map[BOARD_VIEW_HEIGHT];
static unsigned row_map_ready;

static void convert_scaled(const uint16_t *src, unsigned width, uint8_t *py, uint8_t *pvu)
{
    if (!row_map_ready) {
        for (unsigned y = 0; y < BOARD_VIEW_HEIGHT; y++) row_map[y] = (uint8_t)(y * BOARD_DOOM_HEIGHT / BOARD_VIEW_HEIGHT);
        row_map_ready = 1;
    }
    unsigned prev = ~0u;
    for (unsigned y = 0; y < BOARD_VIEW_HEIGHT; y++) {
        const unsigned sy = row_map[y];
        const unsigned oy = y + BOARD_ORIGIN_Y;                 /* even offset: y and oy have the same parity */
        uint16_t *dy = (uint16_t *)(py + oy * BOARD_NV21_PITCH);
        uint16_t *dc = (uint16_t *)(pvu + (oy >> 1) * BOARD_NV21_PITCH);
        const uint16_t *s = src + sy * width;
        const int chroma = !(oy & 1u);
        if (sy == prev) {                                       /* same source row as the output row above: copy its luma */
            memcpy(dy, (uint8_t *)dy - BOARD_NV21_PITCH, BOARD_LCD_WIDTH);
            if (chroma) scale_row(s, 0, dc, 0, 1);
        } else if (chroma) scale_row(s, dy, dc, 1, 1);
        else scale_row(s, dy, 0, 1, 0);
        prev = sy;
    }
}
#endif

void I_FinishUpdate_e32(const byte *srcBuffer, const byte *pallete, const unsigned int width, const unsigned int height)
{
    if (!srcBuffer) return;
    if (pallete && pallete != lut_source) I_SetPallete_e32(pallete);

    const uint16_t *src = (const uint16_t *)srcBuffer;
    uint8_t *py = plat_plane_y();
    uint8_t *pvu = plat_plane_vu();

#if BOARD_SCALE_8_3
    (void)height;
    convert_scaled(src, width, py, pvu);
#else
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
#endif
#if BOARD_HAS_PRINTER
    /* A requested print runs here, on the finished frame. It leaves a message box in the planes: convert the frame again over it. */
    if (plat_print_service(src, width, lut_y2)) {
#if BOARD_SCALE_8_3
        convert_scaled(src, width, py, pvu);
#endif
    }
#endif
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

static volatile unsigned time_resync;
void plat_time_resync(void) { time_resync = 1; }

int I_GetTime_e32(void)
{
    static uint32_t last, acc;
    static int tics;
    static uint32_t last_poll;

    const uint32_t now = apx_ticks();
    if (time_resync) { time_resync = 0; last = now; acc = 0; }      /* skip the time a blocking job (printing) took */
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
 * The key mapping is per board (input_action.c, input_silvergear.c): they build a mask of GBADoom's virtual keys and pass it to
 * plat_post_keys(), which posts the events for what changed since the previous call.
 */
static uint16_t keys_prev;

/* Development hook: tools/keys.py writes a bit mask (bit 0 = KEYD_A ... bit 9 = KEYD_SELECT) and the magic word, which makes the
 * game drivable from the host (menus, level start, movement) without touching the camera. Compiled in with DOOM_STATS. */
#if DOOM_STATS
volatile uint32_t doom_host_keys_magic;
volatile uint32_t doom_host_keys;
#define HOST_KEYS_MAGIC 0x5359454bu    /* "KEYS" */
uint16_t plat_host_keys(void)
{
    return doom_host_keys_magic == HOST_KEYS_MAGIC ? (uint16_t)(doom_host_keys & 0x3ffu) : 0;
}
#else
uint16_t plat_host_keys(void) { return 0; }
#endif

void plat_keys_reset(void) { keys_prev = 0; }

void plat_post_keys(uint16_t keys)
{
    const uint16_t changed = keys ^ keys_prev;
    if (!changed) return;
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

void I_StartWServEvents_e32(void) { }
void I_PollWServEvents_e32(void)  { }

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
