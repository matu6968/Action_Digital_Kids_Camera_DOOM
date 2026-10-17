#ifndef APPOTECH_LVGL_PORT_H
#define APPOTECH_LVGL_PORT_H

#include <lvgl.h>

/* Initialize the SDK LCD and buttons, then the LVGL display and keypad. */
int apx_lvgl_port_init(void);
/* Poll SDK services, complete async display transfers, and run LVGL timers. */
int apx_lvgl_port_poll(void);
/* Add focusable widgets to the group navigated by the camera keys. */
lv_group_t *apx_lvgl_port_group(void);

#endif
