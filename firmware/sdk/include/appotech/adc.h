#ifndef APPOTECH_ADC_H
#define APPOTECH_ADC_H
#include <stdint.h>
/* Cooperative, single-conversion ADC arbitration. Owners are single bits.
 * begin: 0 started, 1 busy, -1 invalid. result: 0 pending, 1 ready, -1 timeout.
 * A client must collect/cancel its conversion before releasing its reference. */
#define APX_ADC_BUTTONS 1u
#define APX_ADC_BATTERY 2u
int apx_adc_acquire(unsigned owner);
int apx_adc_begin(unsigned owner,unsigned channel);
int apx_adc_result(unsigned owner,uint16_t *result);
void apx_adc_release(unsigned owner);
#endif
