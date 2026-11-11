/*
 * input_action.c - key mapping for the Action Vision camera.
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
#include <stdint.h>
#include <stdio.h>

#include <appotech/buttons.h>
#include <appotech/power.h>

#include "plat.h"
#include "plat_input.h"

static uint32_t power_down_ms;
static unsigned power_state;        /* 0 idle, 1 held (candidate tap), 2 held and consumed (modifier), 3 menu sent */
static unsigned power_armed;        /* the press that powered the camera on is ignored until it is released */
static uint32_t pulse_until;        /* timed virtual key (tap) */
static uint16_t pulse_keys;

void plat_input_init(void)
{
    plat_keys_reset();
    power_state = 0;
    power_armed = 0;
    pulse_until = 0;
    pulse_keys = 0;
}

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

    keys |= plat_host_keys();
    plat_post_keys(keys);
}
