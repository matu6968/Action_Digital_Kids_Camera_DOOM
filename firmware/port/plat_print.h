/*
 * Screenshot printing on the Silvergear's thermal printer (port/doom_print.c). On boards without a printer
 * (BOARD_HAS_PRINTER 0) these are empty stubs, so the frame path does not need #ifdefs.
 */
#ifndef PLAT_PRINT_H
#define PLAT_PRINT_H
#include <stdint.h>

/* The PRINT key was pressed: print the next finished frame. Ignored while a print is running. */
void plat_print_request(void);

/* Called by I_FinishUpdate_e32 after the frame has been converted into the plane set, before it is shown. If a print was requested it
 * captures this frame (frame = the engine's palette frame, 120 words per row of two identical 8-bit indices; lut_y2 = the palette's luma
 * lookup), runs the whole print with on-screen progress and returns 1 (the caller then converts the frame again to wipe the message
 * box). Returns 0 at once when nothing was requested. The game is stopped while it runs and its clock is resynchronised afterwards. */
int plat_print_service(const uint16_t *frame, unsigned words_per_row, const uint16_t *lut_y2);
#endif
