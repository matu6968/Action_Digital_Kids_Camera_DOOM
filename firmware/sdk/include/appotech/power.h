#ifndef APPOTECH_POWER_H
#define APPOTECH_POWER_H
#include <stdint.h>
#include <stddef.h>
enum apx_system_action { APX_SYSTEM_REBOOT=1,APX_SYSTEM_POWER_OFF=2 };
/* Graceful actions: sync/unmount storage, stop DMA/display/inputs, detach USB.
 * Return an APXD error (storage: 0x100+FRESULT) before shutdown if preparation
 * fails. Successful calls never return. Open files/DIRs must be closed first. */
uint32_t apx_reboot(void);
uint32_t apx_power_off(void);
/* Two-phase handoff for an acknowledged host command. Commit is valid only
 * after successful prepare with the same action, with no intervening I/O. */
uint32_t apx_system_prepare(enum apx_system_action action);
_Noreturn void apx_system_commit(enum apx_system_action action);
/* Internal always-powered bus, bounded and preserving unrelated control bits.
 * Read command addresses are even, 0x40..0x5a. Public writes not exposed. */
int apx_power_read(unsigned address,uint8_t *value);
/* Stock start-up power-domain sequence (stock 02026624(1) then 02026818(1)):
 * PM bus enable bit, then registers 0x40=0x06, 0x42=0xe0, 0x44=0x00,
 * 0x58|=0x08, 0x40|=0x10 (=0x16), 0x5a=0x08, and 0x54 bit 5 (battery divider, =0x21). These live in the always-powered
 * domain and PERSIST across power cycles; the vendor bootstrap does not set
 * them, and the SDK's own power-off leaves 0x40=0x02, 0x44=0x10. Without them a
 * cold boot has no keep-alive once Power is released and the battery sense
 * inputs read near zero. Register 0x5a must end at 0x08 (as recorded on a
 * running stock camera); clearing it cut USB power.
 * Returns 0, or -1 if any PM bus transaction fails. */
int apx_power_domain_enable(void);
uint32_t apx_system_command(unsigned command,const uint8_t *input,size_t length,
                             uint8_t *output,size_t *output_length);
#endif
