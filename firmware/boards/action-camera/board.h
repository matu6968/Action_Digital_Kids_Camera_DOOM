/*
 * Everything specific to the Action kids camera (board XYX-001-5295C_V03, AppoTech JT5295).
 *
 * The SDK in sdk/ already knows the pins (sdk/board/board.h: LCD reset/backlight/RS, button ladder, LED). What is here is
 * what the DOOM port needs on top: the panel geometry, how the frame is laid out for the display DMA, the button
 * timing, and where the WAD comes from.
 */
#ifndef BOARD_ACTION_CAMERA_H
#define BOARD_ACTION_CAMERA_H

/* Panel (ILI9225G, 176x220 native, mounted landscape): logical 220x176. */
#define BOARD_LCD_WIDTH   220u
#define BOARD_LCD_HEIGHT  176u

/* The display DMA wants NV21 planes with a 32-byte aligned pitch. */
#define BOARD_NV21_PITCH  224u

/* GBADoom renders 240x160 (120 words of two identical pixels). 10 pixels (5 words) are cropped on each side. */
#define BOARD_DOOM_WIDTH  240u
#define BOARD_DOOM_HEIGHT 160u
#define BOARD_CROP_X      10u
#define BOARD_ORIGIN_Y    ((BOARD_LCD_HEIGHT - BOARD_DOOM_HEIGHT) / 2u)

/* Game data: a processed GBADoom WAD (see wad/README.md) in the root directory of a FAT/exFAT SD card. */
#ifndef BOARD_WAD_PATH
#define BOARD_WAD_PATH    "0:/DOOM.WAD"
#endif

/* POWER button gestures (milliseconds). */
#define BOARD_POWER_TAP_MS      350u   /* released sooner than this = "use" */
#define BOARD_POWER_MENU_MS     650u   /* held this long, alone = menu key */
#define BOARD_POWER_OFF_MS     3000u   /* held this long, alone = power the camera off */

#endif
