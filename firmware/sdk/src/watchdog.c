#include <appotech/soc.h>
#include <appotech/watchdog.h>
#include <appotech/exception.h>
#define CONTROL 0x880cu
static unsigned enabled=1,selector=5,automatic=1;
static uint32_t inherited_control,feeds;
/* Stock 020237dc: 0x5500 disables; the 0x5 key at bits 18:16 reloads,
 * enables reset and watchdog (readback bits 3/4), and sets timeout bits 2:0.
 * Stock 02023858 writes 0x39 for a fast watchdog reset. */
void apx_watchdog_init(void) {
    inherited_control=apx_spr_read(CONTROL);feeds=0;
    enabled=automatic=1;selector=5;apx_watchdog_feed();
}
int apx_watchdog_feed(void) {
    if(!enabled)return -1;
    apx_spr_write(CONTROL,0x00050000u|selector);++feeds;return 0;
}
void apx_watchdog_stop(void) {
    enabled=automatic=0;apx_spr_write(CONTROL,0x5500);
}
int apx_watchdog_start(unsigned timeout,enum apx_watchdog_mode mode) {
    if(timeout>7||(mode!=APX_WATCHDOG_MANUAL&&mode!=APX_WATCHDOG_AUTOMATIC))return -1;
    apx_watchdog_stop();selector=timeout;enabled=1;automatic=mode;
    return apx_watchdog_feed();
}
void apx_watchdog_get_info(struct apx_watchdog_info *info) {
    if(!info)return;
    info->control=apx_spr_read(CONTROL);info->inherited_control=inherited_control;
    info->enabled=!!(info->control&0x10);info->selector=info->control&7;
    info->automatic=automatic;info->feeds=feeds;
}
__attribute__((weak,noinline)) void apx_audio_poll(void) {}
__attribute__((weak,noinline)) void apx_mic_poll(void) {}
__attribute__((weak,noinline)) void apx_camera_poll(void) {}
__attribute__((weak,noinline)) void apx_battery_poll(void) {}
__attribute__((weak,noinline)) void apx_uart_poll(void) {}
void apx_maintenance(void) {
    if(automatic)apx_watchdog_feed();
    apx_audio_poll();
    apx_mic_poll();
    apx_camera_poll();
    apx_battery_poll();
    apx_uart_poll();
}
_Noreturn void apx_watchdog_reset(void) {
    apx_exception_expect_reset();
    enabled=automatic=0;apx_spr_write(CONTROL,0x39);
    for(;;)apx_cpu_relax();
}
