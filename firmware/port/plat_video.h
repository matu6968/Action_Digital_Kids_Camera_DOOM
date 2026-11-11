#ifndef PLAT_VIDEO_H
#define PLAT_VIDEO_H
#include <stdint.h>
uint8_t *plat_plane_y(void);
uint8_t *plat_plane_vu(void);
int plat_present(void);   /* 1 = frame queued, 0 = dropped (display busy for 100 ms) */
#endif
