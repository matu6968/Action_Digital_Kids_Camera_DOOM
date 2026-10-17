#ifndef APPOTECH_BATTERY_H
#define APPOTECH_BATTERY_H
#include <stdint.h>
#include <stddef.h>
struct apx_battery_info {
 uint32_t millivolts,updates,timeouts,invalid_samples,last_ticks,pm_control;
 uint16_t adc_battery,adc_reference,calibration;
 uint8_t initialized,valid,level,external_power;
};
/* Stock ADC9/ADC11 ratio, 16 pairs, +20mV, 30mV hysteresis.
 * level 0..5 is a coarse voltage indication, not state-of-charge percent.
 * Calibration is inherited from stock startup SRAM 0x1fec. No charger writes.
 * Poll with buttons regularly; stale/failed readings become invalid. */
int apx_battery_init(void);
void apx_battery_poll(void);
void apx_battery_get_info(struct apx_battery_info *info);
void apx_battery_shutdown(void);
/* Bring-up diagnostic (APX_SPI_TRACE builds): convert ADC channels 9 and 11 directly, in the
 * bootstrap's order, and log them to trace slots 0x50+n (second conversion) and 0x58+n (first),
 * as channel9<<12 | channel11. Safe at any point after the PM init; restores the ADC state. */
#include <appotech/trace.h>
#if defined(APX_SPI_TRACE) && APX_SPI_TRACE
void apx_battery_probe(unsigned n);
#else
static inline void apx_battery_probe(unsigned n) { (void)n; }
#endif
uint32_t apx_battery_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size);
#endif
