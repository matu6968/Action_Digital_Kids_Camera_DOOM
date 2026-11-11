/*
 * Everything specific to the Silvergear printer camera (Action article 3222882, board JX-01 E246715, AppoTech JT5295) that the DOOM
 * port needs on top of the SDK's sdk/boards/silvergear-3222882/board.h: panel geometry, how the frame is laid out for the display DMA,
 * button timing, and where the WAD comes from.
 */
#ifndef BOARD_SILVERGEAR_H
#define BOARD_SILVERGEAR_H

/* Panel (ST7789, 240x320 native, mounted landscape): logical 320x240. */
#define BOARD_LCD_WIDTH   320u
#define BOARD_LCD_HEIGHT  240u

/* The display DMA wants NV21 planes with a 32-byte aligned pitch: 320 qualifies as it is. */
#define BOARD_NV21_PITCH  320u

/* GBADoom renders 240x160, but only as 120 distinct columns (every pixel is doubled in the engine's frame buffer).
 * They are scaled to 320x216 (8 output pixels per 3 source columns, 27 output rows per 20 source rows: 1.35x vertically,
 * 1.3% off the exact aspect ratio, so every row offset stays even for the NV21 chroma rows) and centred with 12 black rows above and below. */
#define BOARD_DOOM_WIDTH  240u
#define BOARD_DOOM_HEIGHT 160u
#define BOARD_SCALE_8_3   1
#define BOARD_VIEW_HEIGHT 216u
#define BOARD_ORIGIN_Y    ((BOARD_LCD_HEIGHT - BOARD_VIEW_HEIGHT) / 2u)
#define BOARD_CROP_X      0u

/* Thermal printer (384 dots wide): PRINT key -> screenshot sticker, see port/doom_print.c. */
#define BOARD_HAS_PRINTER 1

/* SPI cold boot: no overlay (the sector the overlay table would use, 0x1fc000, holds stock data here); the BootROM loads the whole engine
 * ("flat" layout in linker/doom.ld). A WAD stored deflated in flash goes right behind that image and must end below 0x300000: the stock
 * photos start at 0x381000, the settings are at 0x3ff000 and the SDK's flight recorder uses 0x3fd000/0x3fe000. */
#define BOARD_SPI_OVERLAY     0
#define BOARD_WAD_FLASH_LIMIT 0x300000u

/* Name used in the boot banner. */
#define BOARD_NAME        "Silvergear camera"

/* Boot / error text screen (port/plat.c): top-left of line 0, line pitch, and the progress bar box. */
#define BOARD_TEXT_X      16u
#define BOARD_TEXT_Y      70u
#define BOARD_TEXT_STEP   24u
#define BOARD_BAR_X       60u
#define BOARD_BAR_Y       150u
#define BOARD_BAR_W       200u
#define BOARD_BAR_H       10u

/* Game data: a processed GBADoom WAD (see wad/README.md) in the root directory of a FAT/exFAT SD card. */
#ifndef BOARD_WAD_PATH
#define BOARD_WAD_PATH    "0:/DOOM.WAD"
#endif

/* POWER button gestures (milliseconds). POWER shares the key ladder here, so it never combines with another key. */
#define BOARD_POWER_TAP_MS      350u   /* released sooner than this = next weapon */
#define BOARD_POWER_MENU_MS     650u   /* held this long = menu key */
#define BOARD_POWER_OFF_MS     3000u   /* held this long = power the camera off */

/* Key handling: input_silvergear.c (one key at a time, no UP/DOWN). */
#define BOARD_INPUT_SILVERGEAR 1

#endif
