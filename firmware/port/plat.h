/*
 * Platform glue between GBADoom and the JT5295 SDK (shared by the files in this directory).
 */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>
#include <stddef.h>

/* Build-time board constants (panel, crop, WAD path, button timing). */
#include "board.h"

/* Memory for the engine's zeroed state lives in the codec-RAM bank, see linker/doom.ld. */
#define DOOM_BUFFER __attribute__((section(".doom_buffer"), aligned(32)))

/* Service everything the SDK needs polled: USB debug link, buttons, display DMA, audio DMA, watchdog.
 * Cheap to call very often; the engine calls it from I_GetTime. */
void plat_poll(void);

/* Milliseconds since boot (32.768 kHz tick counter, no division). */
uint32_t plat_ms(void);

/* Frame planes. */
void plat_display_init(void);
void plat_display_clear(void);
/* Print up to a few lines of text on a black screen and show it. Used for boot progress and fatal errors. */
void plat_status(const char *line1, const char *line2, const char *line3, int error);
void plat_progress(unsigned percent);

/* Input: virtual keys, KEYD_A + i, see i_system_jt5295.c. */
void plat_input_init(void);

/* Sound mixer; called from plat_poll. */
void plat_audio_poll(void);

/* The WAD. */
int plat_load_wad(void);

#endif
