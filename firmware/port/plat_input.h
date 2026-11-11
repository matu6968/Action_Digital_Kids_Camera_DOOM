/*
 * Shared input plumbing for the per-board key mappings (input_action.c, input_silvergear.c).
 *
 * A board file turns the SDK's button state into a bit mask of GBADoom's virtual keys (KEYD_A + i) and hands it to
 * plat_post_keys(), which posts the key down / key up events for whatever changed.
 */
#ifndef PLAT_INPUT_H
#define PLAT_INPUT_H
#include <stdint.h>
#include "doomdef.h"

#define KBIT(k) (1u << ((k) - KEYD_A))

/* Called once per I_ProcessKeyEvents with the complete key mask of this moment. */
void plat_post_keys(uint16_t keys);

/* Keys injected from the host (tools/keys.py) in DOOM_STATS builds, 0 otherwise: OR it into the mask. */
uint16_t plat_host_keys(void);

/* Forget the previous mask (plat_input_init). */
void plat_keys_reset(void);
#endif
