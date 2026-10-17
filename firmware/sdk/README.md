# JT5295 SDK (vendored)

A copy of the bare-metal SDK for the Action Vision / AppoTech JT5295 camera (MIT, see `LICENSE`), from the companion SDK repository
(`appotech-sdk`). It is built here as-is except for these changes, each marked in the source:

| File | Change |
|---|---|
| `src/audio.c`, `include/appotech/audio.h` | gapless streaming: `apx_audio_stream_start/queue/ready/active/underruns` using the DAC DMA's one-deep next-block queue |
| `src/uart.c` | the console mirror sends CR+LF for a bare `\n` (terminals showed a staircase); `apx_uart_console_put` only |
| `src/hwmuldiv.S` (new), `Makefile` link flags | hardware `l.mul`/`l.div` behind `__mulsi3`, `__udivsi3`, `__divsi3`, `__umodsi3`, `__modsi3` (6-20x faster than libgcc's software routines on this chip); the link line forces the members in with `-u` |
| `src/sd.c` | the receive-status check no longer requires bits 22:21 of the controller status to be `10` (they are noise on SDHC cards); bit 16, the CRC error flag, still fails the read |
| `src/storage.c` | `APX_NO_STORAGE_COMMAND` leaves out the host SD/FatFs command set (15 KiB of the 128 KiB runtime window) |
| `fatfs/ffconf.h` | `FF_CODE_PAGE 437` instead of the all-codepage build (saves a 483 KB table); `f_setcp` is compiled out accordingly |
| `src/lvgl_port.c`, `src/image.c` | not vendored (LVGL and the stb image decoder are not used) |

The `board/board.h` is the SDK's `boards/action-vision/board.h`.
