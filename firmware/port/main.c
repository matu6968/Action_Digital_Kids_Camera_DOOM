/*
 * Entry point. The SDK's startup code (sdk/src/startup.S) has already switched the CPU to 144 MHz, enabled the caches and
 * started the watchdog by the time main() runs. This brings up the peripherals, loads the WAD from the SD card and then
 * hands over to GBADoom's own main, which i_main.c renames to doom_main at build time.
 */
#include <stdio.h>
#include <string.h>

#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/buttons.h>
#include <appotech/display.h>
#include <appotech/spi.h>
#include <appotech/uart.h>

#include "plat.h"

extern unsigned char __doom_bss_start[], __doom_bss_end[];
extern int doom_main(int argc, const char *const *argv);

/* Boot trail on UART0 TX (PA7, 115200 8N1), the same pin the vendor bootstrap prints "PSRM" on: a camera that resets in a loop
 * is otherwise a black box. Each line is flushed before the next step so the last one printed is the last one reached. */
void boot_trail(const char *text)
{
    printf("%s\n", text);
    for (unsigned i = 0; i < 200000; i++) {
        struct apx_uart_info u;
        apx_uart_get_info(&u);
        if (!u.tx_queued) break;
        apx_uart_poll();
        apx_maintenance();
    }
}

int main(void)
{
    if (!apx_uart_init(115200, 6, -1)) apx_uart_console_enable(1);
    boot_trail("DOOM: main");
    apx_usb_init();
    boot_trail("DOOM: usb up, loading overlay");
    /* The engine's code lives above the window the BootROM loads: on an SPI boot copy it into place first, before anything
     * from build/doom/ (all of it except this file) is called. A USB launch has loaded every segment already: no-op. */
    const int overlay = apx_spi_overlay_load();
    if (overlay) {
        printf("SPI overlay load failed: %d\n", overlay);
        boot_trail("DOOM: SPI overlay load FAILED");
        for (;;) apx_debug_poll();
    }
#if DOOM_STATS
    /* Development builds keep the SPI flash driver initialised so `apxtool debug flash read/install` works on a launched image
     * (tools/install.sh). The WAD loader then leaves it alone. Release builds (DOOM_STATS=0) only open it while unpacking. */
    apx_spi_init();
#endif
    boot_trail("DOOM: overlay in place");
    /* The engine's zeroed state lives outside the SDK's .bss: a USB launch has it zeroed, an SPI boot does not. */
    memset(__doom_bss_start, 0, (size_t)(__doom_bss_end - __doom_bss_start));

    boot_trail("DOOM: engine state cleared");
    apx_buttons_init();
    apx_display_init();
    printf("DOOM for the Action kids camera, build %s\n", APX_BUILD_ID);

    plat_display_init();     /* waits for the panel */
    boot_trail("DOOM: display ready");
    plat_input_init();

    if (plat_load_wad() != 0)
        for (;;) plat_poll();   /* message is on screen; keep USB alive so the camera can be recovered */

    boot_trail("DOOM: WAD loaded, starting engine");
    plat_display_clear();
    return doom_main(0, 0);
}
