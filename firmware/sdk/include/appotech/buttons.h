#ifndef APPOTECH_BUTTONS_H
#define APPOTECH_BUTTONS_H
#include <stddef.h>
#include <stdint.h>
enum apx_button { APX_BUTTON_SHUTTER=1u,APX_BUTTON_DOWN=2u,APX_BUTTON_UP=4u,
                  APX_BUTTON_LEFT=8u,APX_BUTTON_RIGHT=16u,APX_BUTTON_POWER=32u };
struct apx_button_event { uint32_t ticks,state,pressed,released; };
struct apx_buttons_info {
    uint32_t state,raw_state,adc_timeouts,events_dropped;
    uint16_t adc;uint8_t power,led;
};
/* Poll at least every 5 ms; 20 ms debounce, 16 queued press/release events.
 * No power button policy: applications decide what a press does.
 * Owns board PB0/PB1/PB5 and ADC channel 3 until shutdown. */
void apx_buttons_init(void);
void apx_buttons_poll(void);
void apx_buttons_get_info(struct apx_buttons_info *info);
int apx_buttons_next(struct apx_button_event *event);
void apx_buttons_shutdown(void);
int apx_led_set(int enabled);
int apx_led_get(void);
/* APXD IO extension, linked by examples that initialize board buttons. */
uint32_t apx_gpio_command(unsigned command,const uint8_t *input,size_t length,
                          uint8_t *output,size_t *output_length);
#endif
