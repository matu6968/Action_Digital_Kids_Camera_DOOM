#ifndef APPOTECH_BOARD_H
#define APPOTECH_BOARD_H
#define APX_BOARD_SILVERGEAR 1
#define APX_BOARD_ID "silvergear-3222882"
#define APX_CAMERA_COUNT 2u
#define APX_CAMERA_REAR_ENABLE_MASK 0x800u
#define APX_CAMERA_FRONT_ENABLE_MASK 0x400u
#define APX_HAS_PRINTER 1
/* Stock gray tail160 + owner-calibrated96 (about6mm) + requested64-step
 * tear buffer (about4mm), 2026-11-07. Each manual feed is32 steps/about2mm. */
#define APX_PRINTER_PHOTO_TEAR_STEPS 320u
/* Default strobe energy per rendering mode, percent of the stock-derived pulse timing (100..400).
 * Provisional, from SD wedge scans of the same image on the stock firmware and the SDK, 2026-11-07
 * (runs printer-sd-compare-20261107-05..-11, summary report and raw runs in cleanup-archive-20261108). The stock curve depends on the tone direction: across the
 * head (like a photo) it prints much darker than along the feed, while the SDK does not. Calibrated on the across-head
 * wedge: Gray 155, Dot 112. The same setting varies by about +-20 grey levels between runs (battery, head temperature),
 * and the cause of the orientation dependence is not found yet. apx_printer_set_energy() overrides these at run time. */
/* Photo print pacing. Measured on hardware 2026-11-07 (run printer-sd-compare-20261107-15, summary report and raw run in cleanup-archive-20261108): Gray half-rows took 12.5-13.8 ms,
 * Dot half-rows alternated between about 9.5 and 15.5 ms (renderer once per row); a 20-sample temperature read added ~4.6 ms in front of
 * every step. The stock firmware needs 8.3 ms (Gray) and 5.8 ms (Dot) per half-row. Spreading the thermistor average over several half-rows removes most of that
 * read; the fixed half-row periods stay 0 (off) until the new natural period has been measured. */
#ifndef APX_PRINTER_SPREAD_TEMPERATURE
#define APX_PRINTER_SPREAD_TEMPERATURE 1
#endif
#ifndef APX_PRINTER_ROW_PERIOD_GRAY_US
#define APX_PRINTER_ROW_PERIOD_GRAY_US 0u
#endif
#ifndef APX_PRINTER_ROW_PERIOD_DOT_US
#define APX_PRINTER_ROW_PERIOD_DOT_US 0u
#endif
#ifndef APX_PRINTER_ENERGY_GRAY_PERCENT
#define APX_PRINTER_ENERGY_GRAY_PERCENT 155u
#endif
#ifndef APX_PRINTER_ENERGY_DOT_PERCENT
#define APX_PRINTER_ENERGY_DOT_PERCENT 112u
#endif
#define APX_DISPLAY_NATIVE_WIDTH 240u
#define APX_DISPLAY_NATIVE_HEIGHT 320u
#define APX_DISPLAY_WIDTH 320u
#define APX_DISPLAY_HEIGHT 240u
#define APX_BUTTON_INPUT_MASK 0u
#define APX_BUTTON_TOLERANCE 35u
#define APX_BUTTON_CENTERS {15,330,240,620,510,420,130}
/* Owner-identified physical keys, 2026-11-07: PRINT134, SHUTTER512,
 * EXIT413, MODE614. Centers remain the stock resistor-ladder values. */
#define APX_BUTTON_FLAGS {APX_BUTTON_POWER,APX_BUTTON_LEFT,APX_BUTTON_RIGHT,APX_BUTTON_MODE,APX_BUTTON_SHUTTER,APX_BUTTON_EXIT,APX_BUTTON_PRINT}
#define APX_BOARD_NAME "silvergear-3222882-jt5295"
/* SPI NOR: 944016, 4 MiB. The controller recipe is the one the BootROM flasher payload already drove on this chip
 * (read, 4 KiB sector erase 0x20, 256-byte page program 0x02); the SDK has not yet been run against it on hardware. */
#define APX_SPI_JEDEC 0x944016u
#define APX_SPI_FLASH_BYTES 0x400000u
/* Same routine as the Action bootstrap but its data is 0x1d8 lower (SPI 0x558: movhi r4,0 / ori r4,r4,0x1c94). */
#define APX_SPI_BOOT_SAVED_STACK 0x1c94u
/* Flight-recorder sectors: erased sectors below the stock settings sector (0x3ff000, 255 bytes in use). The Action
 * addresses 0x1fe000/0x1ff000 hold stock data on this chip and must never be erased by a trace build. */
#define APX_SPI_TRACE_SECTOR 0x003fe000u
#define APX_SPI_TRACE_LAUNCH_SECTOR 0x003fd000u
#define APX_BATTERY_CALIBRATION_ADDRESS 0x1e14u
#define APX_BATTERY_OFFSET_MV 30u
#define APX_SD_CLOCK_SOURCE_HZ 144000000u
#define APX_SD_CLOCK_HZ 24000000u
/* One data line only: stock's 4-bit route would use PA3 and PE10/PE11, which here are printer motor phases and the cameras' power-down controls. */
#ifndef APX_SD_BUS_WIDTH_MAX
#define APX_SD_BUS_WIDTH_MAX 1
#endif
#ifndef APX_CPU_MHZ
#define APX_CPU_MHZ 144
#endif
#if APX_CPU_MHZ != 96 && APX_CPU_MHZ != 144
#error Unsupported APX_CPU_MHZ
#endif
#define APX_CPU_HZ_ESTIMATE (APX_CPU_MHZ*1000000u)
#define APX_REFERENCE_HZ_ESTIMATE 32768u
/* Stock 02033a68/02024774: UART baud divider is write-only (reads zero).
 * Restore the known inherited 144MHz/115200-1 setting at teardown. */
#define APX_UART_BOOT_DIVIDER 1249u
/* Shared divider b is unchanged; compensate the codec rates for each PLL. */
#if APX_CPU_MHZ == 144
#define APX_AUDIO_SELECTOR_8000 10u
#define APX_AUDIO_SELECTOR_16000 6u
#define APX_AUDIO_SELECTOR_32000 2u
/* At 144 MHz the low rate field cannot encode 48 directly. The stock
 * recording path's bit 22 with field 31 gives the measured 8 kHz rate. */
#define APX_MIC_CONTROL_8000 0x005f4000u
#define APX_MIC_CONTROL_16000 0x00174000u
#define APX_MIC_CONTROL_32000 0x000b4000u
#else
#define APX_AUDIO_SELECTOR_8000 8u
#define APX_AUDIO_SELECTOR_16000 4u
#define APX_AUDIO_SELECTOR_32000 0u
/* Recording prescalers verified with the same shared audio clock divider b. */
#define APX_MIC_CONTROL_8000 0x001f4000u
#define APX_MIC_CONTROL_16000 0x000f4000u
#define APX_MIC_CONTROL_32000 0x00074000u
#endif
#define APX_BUTTON_GROUP 1u
#define APX_POWER_BUTTON_MASK 0u
/* Stock ad-key open also configures PB1; its analog pad assignment is unverified. */
#define APX_BUTTON_AUX_INPUT_MASK 0x0002u
#define APX_BUTTON_ADC_CHANNEL 3u
#define APX_BUTTON_ADC_DIVIDER 71u /* 144 MHz / (2*(71+1)) = 1 MHz */
#define APX_STATUS_LED_GROUP 1u
/* Flash LED = PB0, active high. Stock registers it as board device "ir"
 * (string 02082e62, open 02002eb4, ioctl 0200323c): open and ioctl 0x2000011
 * both call 02034d54(1,1,0,0) (push-pull output, bank B mask 1) then
 * 02034f00(1,1,level). Power click (event 37, 02011790) toggles it. */
#define APX_STATUS_LED_MASK 0x0001u
/* Hardware-verified 2026-11-07: forcing PB0 through 81c0/8200 does not light the LED;
 * driving it as an output and writing the latch (as stock does) does. */
#define APX_STATUS_LED_LATCH 1
/* D-426 V03; LCD reset uses dedicated 886c/8874 output. Backlight is PM54 bit6, not GPIO.
 * Unlabelled rear keys use stock codes 28/29/27/34/38; physical roles above
 * were matched to owner-identified key presses and SDK ADC logs.
 * PB5 is the printer latch, never a status LED. */
#define APX_LCD_RESET_GROUP 1u
#define APX_LCD_RESET_MASK 0u
#define APX_LCD_BACKLIGHT_GROUP 0u
#define APX_LCD_BACKLIGHT_MASK 0u
#define APX_LCD_RS_GROUP 4u
#define APX_LCD_RS_MASK 0x0004u
#define APX_LCD_CLOCK_DIVIDER 15u
#define APX_LCD_DEFAULT_ROTATION 3u /* landscape; 0x60 was visibly upside down in SRAM PIO test */
#define APX_LCD_PIO_TIMEOUT_TICKS 164u /* approximately 5 ms */
#endif
