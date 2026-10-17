#ifndef APPOTECH_WATCHDOG_H
#define APPOTECH_WATCHDOG_H
#include <stdint.h>
/* Hardware timeout selector: 0..7, increasing by factors of four. This board
 * measures about .46/1.82/7.31/29.10 s for selectors 4/5/6/7. The independent
 * RC oscillator is approximate; selectors 0..3 have not been physically timed.
 * Automatic mode feeds in SDK waits. Manual mode feeds only when requested,
 * so polling USB/SD/display cannot hide a missing application heartbeat. */
enum apx_watchdog_mode { APX_WATCHDOG_MANUAL,APX_WATCHDOG_AUTOMATIC };
struct apx_watchdog_info {
    uint32_t control,inherited_control,feeds;
    unsigned enabled,selector,automatic;
};
void apx_watchdog_init(void); /* Runtime-owned: inherit startup, then auto selector 5. */
int apx_watchdog_start(unsigned selector,enum apx_watchdog_mode mode);
int apx_watchdog_feed(void); /* -1 if disabled; count wraps modulo 2^32. */
void apx_watchdog_stop(void);
void apx_watchdog_get_info(struct apx_watchdog_info *info);
/* Fast hardware watchdog reset; no filesystem sync. Use apx_reboot for graceful reboot. */
_Noreturn void apx_watchdog_reset(void);
#endif
