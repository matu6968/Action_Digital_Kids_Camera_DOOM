/*
 * plat.c - polling, frame planes and the boot/error text screen.
 *
 * The display takes NV21 planes by DMA (apx_display_blit_nv21): a luma plane and an interleaved V,U plane, 32-byte
 * aligned, pitch 224. Two plane sets are used alternately so the engine can start the next frame while the previous
 * one is still being sent. The engine's 8-bit palette frame is turned into planes in i_system_jt5295.c.
 */
#include <stdio.h>
#include <string.h>

#include <appotech/soc.h>
#include <appotech/debug.h>
#include <appotech/buttons.h>
#include <appotech/display.h>

#include "plat.h"
#include "plat_video.h"

#define PITCH   BOARD_NV21_PITCH
#define ROWS    BOARD_LCD_HEIGHT

static uint8_t plane_y[2][PITCH * ROWS]       DOOM_BUFFER;
static uint8_t plane_vu[2][PITCH * ROWS / 2u] DOOM_BUFFER;
static unsigned cur;

/* ------------------------------------------------------------------ polling */

void plat_poll(void)
{
    apx_debug_poll();
    apx_buttons_poll();
    apx_display_poll();
    apx_maintenance();           /* watchdog feed + the SDK audio poll */
    plat_audio_poll();
}

uint32_t plat_ms(void)
{
    /* 32768 Hz ticks -> ms without a divide: x * 1000 / 32768 = x * 125 / 4096 */
    const uint32_t t = apx_ticks();
    return (t >> 12) * 125u + (((t & 4095u) * 125u) >> 12);
}

/* ------------------------------------------------------------------ frame planes */

uint8_t *plat_plane_y(void)  { return plane_y[cur]; }
uint8_t *plat_plane_vu(void) { return plane_vu[cur]; }

void plat_display_init(void)
{
    while (apx_display_status() != APX_DISPLAY_READY) {
        plat_poll();
        if (apx_display_status() == APX_DISPLAY_ERROR) { printf("display init failed\n"); break; }
    }
    plat_display_clear();
}

void plat_display_clear(void)
{
    for (unsigned i = 0; i < 2; i++) {
        memset(plane_y[i], 0, sizeof plane_y[i]);
        memset(plane_vu[i], 128, sizeof plane_vu[i]);
    }
}

/* Queue the current plane set and switch to the other one. Waits (polling) while the previous DMA is still running. */
void plat_present(void)
{
    uint32_t t0 = apx_ticks();
    while (apx_display_blit_nv21(0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, plane_y[cur], plane_vu[cur], PITCH) != 0) {
        plat_poll();
        if (apx_deadline_expired(t0 + 3277u)) {    /* 100 ms: drop the frame rather than hang */
            if (apx_display_status() == APX_DISPLAY_ERROR) apx_display_init();
            return;
        }
    }
    cur ^= 1u;
}

/* ------------------------------------------------------------------ text screen */

/* 5x7 glyphs, column-major, bit 0 = top row. ASCII 0x20..0x5A, lower case is folded to upper case. */
static const uint8_t font[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};

/* Draw at 2x scale into the current luma plane. colour: luma value (255 white, 0 black). */
static void draw_text(unsigned x, unsigned y, const char *s, uint8_t colour)
{
    uint8_t *p = plane_y[cur];
    for (; *s; s++, x += 12) {
        unsigned c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 0x20 || c > 0x5A) c = '?';
        const uint8_t *g = font[c - 0x20];
        for (unsigned col = 0; col < 5; col++)
            for (unsigned row = 0; row < 7; row++)
                if (g[col] & (1u << row))
                    for (unsigned dy = 0; dy < 2; dy++)
                        for (unsigned dx = 0; dx < 2; dx++) {
                            unsigned px = x + col * 2 + dx, py = y + row * 2 + dy;
                            if (px < BOARD_LCD_WIDTH && py < ROWS) p[py * PITCH + px] = colour;
                        }
    }
}

void plat_status(const char *l1, const char *l2, const char *l3, int error)
{
    memset(plane_y[cur], error ? 0x30 : 0, PITCH * ROWS);
    memset(plane_vu[cur], 128, PITCH * ROWS / 2u);
    if (error) {               /* red-ish tint for errors: V high, U low */
        for (unsigned i = 0; i < PITCH * ROWS / 2u; i += 2) { plane_vu[cur][i] = 200; plane_vu[cur][i + 1] = 100; }
    }
    const char *lines[3] = { l1, l2, l3 };
    for (unsigned i = 0; i < 3; i++)
        if (lines[i]) draw_text(6, 40 + i * 24, lines[i], 255);
    plat_present();
}

void plat_progress(unsigned percent)
{
    if (percent > 100) percent = 100;
    uint8_t *p = plane_y[cur];
    for (unsigned y = 130; y < 140; y++)
        for (unsigned x = 10; x < 210; x++) {
            int edge = (y == 130 || y == 139 || x == 10 || x == 209);
            p[y * PITCH + x] = edge || (x - 11) < percent * 198u / 100u ? 255 : 0;
        }
    plat_present();
}
