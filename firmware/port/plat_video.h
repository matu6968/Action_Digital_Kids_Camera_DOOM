#ifndef PLAT_VIDEO_H
#define PLAT_VIDEO_H
#include <stdint.h>
uint8_t *plat_plane_y(void);
uint8_t *plat_plane_vu(void);
void plat_present(void);
#endif
