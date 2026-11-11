/*
 * input_silvergear.c - key mapping for the Silvergear printer camera.
 *
 * All seven keys, POWER included, sit on one ADC resistor ladder, so exactly one key is seen at a time and POWER can never be a
 * modifier. There are no UP/DOWN keys: LEFT, RIGHT, MODE, SHUTTER, EXIT, PRINT, POWER. (Walking backwards, strafing and weapon
 * selection by direction are not available; the next weapon is a POWER tap.)
 *
 *                    in the game                         in a menu / message
 *   LEFT / RIGHT     turn left / right                   up / down
 *   EXIT             walk forward (hold)                 back (START: leaves the menu)
 *   MODE             use / open doors  (A)               value + (RIGHT)
 *   SHUTTER          fire (B)                            enter (A)
 *   PRINT            print a screenshot                  value - (LEFT)
 *   POWER, tap       next weapon (A+R)                   -
 *   POWER, hold 0.65 s  menu (START)
 *   POWER, hold 3 s     power the camera off (the SDK waits for the key to be released first)
 *
 * The meaning of a key is decided when it goes down and kept until it is released, so a key that closes the menu does not turn
 * into a walk-forward the moment the menu is gone.
 */
#include <stdint.h>
#include <stdio.h>

#include <appotech/buttons.h>
#include <appotech/power.h>

#include "doomdef.h"
#include "global_data.h"
#include "plat.h"
#include "plat_input.h"
#include "plat_print.h"

static uint32_t power_down_ms;
static unsigned power_state;        /* 0 idle, 1 held (candidate tap), 2 menu sent (counting to power-off) */
static unsigned power_armed;        /* the press that powered the camera on is ignored until it is released */
static uint32_t pulse_until;        /* timed virtual key */
static uint16_t pulse_keys;
static uint32_t last_state;
static int latched_menu;

void plat_input_init(void)
{
    plat_keys_reset();
    power_state = 0;
    power_armed = 0;
    pulse_until = 0;
    pulse_keys = 0;
    last_state = 0;
    latched_menu = 0;
}

void I_ProcessKeyEvents(void)
{
    struct apx_buttons_info bi;
    apx_buttons_get_info(&bi);
    const uint32_t s = bi.state;
    const uint32_t now = plat_ms();
    const int power = (s & APX_BUTTON_POWER) != 0;
    uint16_t keys = 0;

    if (s != last_state) {                  /* a key went down (or up): decide which mapping applies to it */
        latched_menu = _g->menuactive ? 1 : 0;
        if ((s & APX_BUTTON_PRINT) && !(last_state & APX_BUTTON_PRINT) && !latched_menu) plat_print_request();
        last_state = s;
    }
    if (!power) power_armed = 1;

    if (power && power_armed) {
        if (power_state == 0) { power_state = 1; power_down_ms = now; }
        const uint32_t held = now - power_down_ms;
        if (power_state == 1 && held >= BOARD_POWER_MENU_MS) {
            power_state = 2;
            pulse_keys = KBIT(KEYD_START); pulse_until = now + 120u;
        }
        if (power_state == 2 && held >= BOARD_POWER_OFF_MS) {
            printf("power off requested\n");
            (void)apx_power_off();          /* waits for the key to be released, then cuts the power */
        }
    } else {
        if (power_state == 1 && power_armed && now - power_down_ms < BOARD_POWER_TAP_MS && !latched_menu) {
            pulse_keys = KBIT(KEYD_A) | KBIT(KEYD_R); pulse_until = now + 120u;     /* next weapon */
        }
        power_state = 0;
        if (latched_menu) {
            if (s & APX_BUTTON_LEFT)    keys |= KBIT(KEYD_UP);
            if (s & APX_BUTTON_RIGHT)   keys |= KBIT(KEYD_DOWN);
            if (s & APX_BUTTON_SHUTTER) keys |= KBIT(KEYD_A);
            if (s & APX_BUTTON_MODE)    keys |= KBIT(KEYD_RIGHT);
            if (s & APX_BUTTON_PRINT)   keys |= KBIT(KEYD_LEFT);
            if (s & APX_BUTTON_EXIT)    keys |= KBIT(KEYD_START);
        } else {
            if (s & APX_BUTTON_LEFT)    keys |= KBIT(KEYD_LEFT);
            if (s & APX_BUTTON_RIGHT)   keys |= KBIT(KEYD_RIGHT);
            if (s & APX_BUTTON_EXIT)    keys |= KBIT(KEYD_UP);
            if (s & APX_BUTTON_MODE)    keys |= KBIT(KEYD_A);
            if (s & APX_BUTTON_SHUTTER) keys |= KBIT(KEYD_B);
        }
    }

    if (pulse_until) {
        if ((int32_t)(now - pulse_until) >= 0) pulse_until = 0, pulse_keys = 0;
        else keys |= pulse_keys;
    }

    keys |= plat_host_keys();
    plat_post_keys(keys);
}
