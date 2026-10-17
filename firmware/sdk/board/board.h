#ifndef APPOTECH_BOARD_H
#define APPOTECH_BOARD_H
#define APX_BOARD_NAME "action-vision-jt5295"
#define APX_SD_CLOCK_SOURCE_HZ 144000000u
#define APX_SD_CLOCK_HZ 24000000u
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
#define APX_POWER_BUTTON_MASK 0x0001u
/* Stock ad-key open also configures PB1; its analog pad assignment is unverified. */
#define APX_BUTTON_AUX_INPUT_MASK 0x0002u
#define APX_BUTTON_ADC_CHANNEL 3u
#define APX_BUTTON_ADC_DIVIDER 71u /* 144 MHz / (2*(71+1)) = 1 MHz */
#define APX_STATUS_LED_GROUP 1u
#define APX_STATUS_LED_MASK 0x0020u
/* XYX-001-5295C_V03 / firmware iLi9225G configuration. GPIO masks, not pin numbers. */
#define APX_LCD_RESET_GROUP 1u
#define APX_LCD_RESET_MASK 0x0040u
#define APX_LCD_BACKLIGHT_GROUP 0u
#define APX_LCD_BACKLIGHT_MASK 0x0040u
#define APX_LCD_RS_GROUP 4u
#define APX_LCD_RS_MASK 0x0004u
#define APX_LCD_CLOCK_DIVIDER 20u
#define APX_LCD_DEFAULT_ROTATION 1u /* landscape: GRAM (y,219-x) */
#define APX_LCD_PIO_TIMEOUT_TICKS 164u /* approximately 5 ms */
#endif
