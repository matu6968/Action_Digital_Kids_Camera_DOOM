#ifndef APPOTECH_CLOCK_H
#define APPOTECH_CLOCK_H
#include <stdint.h>
struct apx_clock_info {
 uint32_t cpu_hz_estimate,requested_mhz,pll1_control,pll1_coefficient;
 uint32_t baseline_loop_ticks,active_loop_ticks,error;
};
/* Startup-only fixed 96/144 MHz profiles. Initialize before peripheral drivers;
 * restore the loader pair after all DMA/peripherals have been shut down.
 * MHz is inferred; the startup loop measures relative throughput.
 * Returns 1 for an unsupported incoming profile, 2 for a readback failure. */
int apx_clock_init(void);
int apx_clock_prepare_spi(void);
unsigned apx_clock_prepare_attempts(void); /* timer-control writes the last apx_clock_prepare_spi() needed */
int apx_clock_restore(void);
void apx_clock_get_info(struct apx_clock_info *info);
#endif
