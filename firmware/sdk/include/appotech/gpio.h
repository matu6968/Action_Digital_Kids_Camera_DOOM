#ifndef APPOTECH_GPIO_H
#define APPOTECH_GPIO_H
#include <stdint.h>
enum apx_gpio_group { APX_GPIO_A,APX_GPIO_B,APX_GPIO_C,APX_GPIO_D,APX_GPIO_E };
enum apx_gpio_direction { APX_GPIO_OUTPUT,APX_GPIO_INPUT };
enum apx_gpio_pull { APX_GPIO_FLOATING,APX_GPIO_PULL_UP,APX_GPIO_PULL_DOWN };
enum apx_gpio_override { APX_GPIO_NORMAL,APX_GPIO_FORCE_HIGH,APX_GPIO_FORCE_LOW };
/* Cooperative callers only. Masked RMW preserves other pins in the bank.
 * Caller owns the pins: this can disconnect an existing peripheral.
 * high and initial_high are bitmasks, each a subset of mask.
 * initial_high is applied before output enable; mask must be nonzero/16-bit. */
int apx_gpio_configure(unsigned group,uint32_t mask,enum apx_gpio_direction direction,
                       enum apx_gpio_pull pull,uint32_t initial_high);
int apx_gpio_write(unsigned group,uint32_t mask,uint32_t high);
int apx_gpio_read(unsigned group,uint32_t mask,uint32_t *high);
/* Stock LED path uses pad overrides while direction remains INPUT. */
int apx_gpio_set_override(unsigned group,uint32_t mask,enum apx_gpio_override drive);
#endif
