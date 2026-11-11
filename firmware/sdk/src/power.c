#include <appotech/power.h>
#include <appotech/watchdog.h>
#include <appotech/clock.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <appotech/usb.h>
#include <appotech/buttons.h>
#include "board.h"
#include <stdio.h>
#define PM_CONTROL 0x88e0u
#define PM_DATA 0x88ecu
extern uint32_t apx_storage_prepare_bootrom(void);
extern void apx_jpeg_shutdown(void);
extern void apx_spi_shutdown(void),apx_battery_shutdown(void),apx_uart_shutdown(void);
extern void apx_camera_shutdown(void),apx_printer_shutdown(void);
extern void apx_audio_shutdown(void);
extern void apx_mic_shutdown(void);
extern void apx_display_shutdown(void),apx_sd_shutdown(void),apx_buttons_shutdown(void);
static enum apx_system_action prepared;
static uint8_t power_config;
static int byte(unsigned value,uint8_t *result) {
    apx_spr_write(PM_DATA,value);
    uint32_t deadline=apx_ticks()+164; /* 5 ms, stock busy bit 3. */
    while(apx_spr_read(PM_CONTROL)&8) {
        if(apx_deadline_expired(deadline))return -1;
        apx_maintenance();
    }
    if(result)*result=apx_spr_read(PM_DATA)&255;
    return 0;
}
static int transfer(unsigned address,unsigned value,uint8_t *result) {
    if(apx_spr_read(PM_CONTROL)&0x100)return -1; /* Do not steal another transaction. */
    apx_spr_write(PM_CONTROL,apx_spr_read(PM_CONTROL)|0x140);
    int error=byte(address,NULL);
    if(!error)error=byte(value,result);
    apx_spr_write(PM_CONTROL,apx_spr_read(PM_CONTROL)&~0x100u);
    return error;
}
int apx_power_read(unsigned address,uint8_t *value) {
    if(!value||address<0x40||address>0x5a||(address&1))return -1;
    return transfer(address,0,value);
}
static int write(unsigned address,unsigned value) { return transfer(address,value,NULL); }
int apx_power_update(unsigned address,uint8_t mask,uint8_t bits) {
    uint8_t value;
    if(bits&~mask || apx_power_read(address,&value))return -1;
    return write(address+1,(value&~mask)|bits);
}
int apx_power_domain_enable(void) {
    uint8_t value;
#if APX_BOARD_SILVERGEAR
    /* D-426 V03, stock 0202b2cc / 0202b54c. PM42 uses the stock 0xe0
     * acknowledgement; PM44 must be read and echoed, not written zero.
     * Preserve existing domain enables during an already-running RAM boot. */
    if(apx_spr_read(PM_CONTROL)&0x108)return -1;
    if(apx_power_update(0x40,0x04,0x04)||write(0x43,0xe0)||
       apx_power_read(0x44,&value)||write(0x45,value))return -1;
    if(apx_power_update(0x58,0x08,0x08)||apx_power_update(0x40,0x10,0x10)||
       apx_power_update(0x5a,0x08,0x08))return -1;
#else
    apx_spr_write(PM_CONTROL,apx_spr_read(PM_CONTROL)|0x40);
    if(write(0x41,0x06))return -1; /* stock 02026624(1): register 0x40 = 6 */
    if(write(0x43,0xe0)||write(0x45,0x00))return -1; /* stock 02026818(1) follows */
    if(apx_power_read(0x58,&value)||write(0x59,value|0x08))return -1;
    if(apx_power_read(0x40,&value)||write(0x41,value|0x10))return -1; /* 0x06|0x10 = 0x16 */
    if(write(0x5b,0x08))return -1;
#endif
    /* Register 0x54 bit 5 is set on a running stock camera (0x21) and cleared by its power-off.
     * It powers the battery divider (ADC channel 9 reads ~0 without it). The vendor bootstrap
     * only sets it on some boot paths, so make sure it is. */
    if(apx_power_read(0x54,&value))return -1;
    return (value&0x20)?0:write(0x55,value|0x20);
}

uint32_t apx_system_prepare(enum apx_system_action action) {
    prepared=0;
    if(action!=APX_SYSTEM_REBOOT&&action!=APX_SYSTEM_POWER_OFF)return APXD_INVALID;
    if(action==APX_SYSTEM_POWER_OFF&&apx_power_read(0x54,&power_config))return APXD_IO;
    uint32_t result=apx_storage_prepare_bootrom();
    if(!result)prepared=action;
    return result;
}
_Noreturn void apx_system_commit(enum apx_system_action action) {
    if(!prepared||action!=prepared) { /* Invalid handoff preserves the live debugger. */
        for(;;)apx_debug_poll();
    }
    apx_watchdog_stop();
    uint32_t deadline=apx_ticks()+16384;
    while(!apx_usb_tx_idle()&&!apx_deadline_expired(deadline))apx_usb_poll();
    apx_delay_ms(100); /* Finish the last host bulk URB. */
    apx_printer_shutdown();apx_camera_shutdown();apx_mic_shutdown();apx_audio_shutdown();apx_jpeg_shutdown();apx_uart_shutdown();apx_spi_shutdown();apx_battery_shutdown();apx_buttons_shutdown();apx_display_shutdown();apx_sd_shutdown();
    apx_spr_write(0xa004,apx_spr_read(0xa004)&~0x40u);
    apx_spr_write(0x18,apx_spr_read(0x18)&~2u);
    if(apx_clock_restore())apx_watchdog_reset();
    if(action==APX_SYSTEM_REBOOT)apx_watchdog_reset();
    /* Stock 0200ad7c..ad94: clear PM54 bit5, PM45=10, PM43=e0, PM41=02.
     * The final byte releases the main power domain; PM/USB power may remain.
     * Stop on a bus error instead of continuing with an incomplete sequence. */
    if(!write(0x55,power_config&~0x20u)&&!write(0x45,0x10)&&!write(0x43,0xe0))
        (void)write(0x41,0x02);
    for(;;)apx_cpu_relax(); /* USB supply can keep the domain alive; no restart/feed. */
}
uint32_t apx_reboot(void) {
    uint32_t result=apx_system_prepare(APX_SYSTEM_REBOOT);
    if(!result)apx_system_commit(APX_SYSTEM_REBOOT);
    return result;
}
#if APX_BOARD_SILVERGEAR
/* The key ladder carries Power on this board. The stock shutdown (0200eb2c) waits until it reads "no key" before cutting the supply, otherwise
 * the still-held key restarts the board. Bounded so a stuck key cannot block power-off forever; a no-op when the demo never started buttons. */
static void wait_for_power_release(void) {
    uint32_t deadline=apx_ticks()+10u*32768u;
    for(;;) {
        apx_buttons_poll();
        struct apx_buttons_info info;apx_buttons_get_info(&info);
        if(!(info.state&APX_BUTTON_POWER)||apx_deadline_expired(deadline))return;
        apx_usb_poll();apx_maintenance(); /* keep USB and the watchdog serviced, as the commit path does while it drains TX */
    }
}
#endif
uint32_t apx_power_off(void) {
#if APX_BOARD_SILVERGEAR
    wait_for_power_release();
#endif
    uint32_t result=apx_system_prepare(APX_SYSTEM_POWER_OFF);
    if(!result)apx_system_commit(APX_SYSTEM_POWER_OFF);
    return result;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
uint32_t apx_system_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *size) {
    *size=0;
    if(command==0&&!length) {
        struct apx_watchdog_info info;apx_watchdog_get_info(&info);
        uint8_t pm40,pm42,pm44,pm54;
        if(apx_power_read(0x40,&pm40)||apx_power_read(0x42,&pm42)||
           apx_power_read(0x44,&pm44)||apx_power_read(0x54,&pm54))return APXD_IO;
        int n=snprintf((char *)output,APXD_MAX_PAYLOAD,
            "{\"watchdog\":{\"enabled\":%u,\"selector\":%u,\"automatic\":%u,\"control\":%lu,"
            "\"inherited_control\":%lu,\"feeds\":%lu},\"power\":{\"pm40\":%u,\"pm42\":%u,\"pm44\":%u,\"pm54\":%u}}",
            info.enabled,info.selector,info.automatic,(unsigned long)info.control,
            (unsigned long)info.inherited_control,(unsigned long)info.feeds,pm40,pm42,pm44,pm54);
        if(n<0||n>=APXD_MAX_PAYLOAD)return APXD_INVALID;
        *size=n;return APXD_OK;
    }
    if(command==1&&length==8)return apx_watchdog_start(get32(input),(enum apx_watchdog_mode)get32(input+4))?APXD_RANGE:APXD_OK;
    if(command==2&&!length)return apx_watchdog_feed()?APXD_IO:APXD_OK;
    if(command==3&&!length) { apx_watchdog_stop();return APXD_OK; }
    return APXD_INVALID;
}
