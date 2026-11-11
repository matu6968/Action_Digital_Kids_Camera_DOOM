# JT5295 SDK (vendored)

A copy of the bare-metal SDK for the AppoTech JT5295 cameras (MIT, see `LICENSE`) from the companion SDK repository
(`appotech-sdk`), commit **b5163cc** ("lots of USB/SD card speed improvements"). It supports both boards this repository builds:
`boards/action-vision` (Action Vision, the default) and `boards/silvergear-3222882` (the thermal-printer camera).

Built here as-is except for the changes below, each marked in the source. TinyUSB is the USB stack (the SDK default; the legacy polled
`usb.c` is vendored but not built).

| File | Change |
|---|---|
| `src/audio.c`, `include/appotech/audio.h` | gapless streaming: `apx_audio_stream_start/queue/ready/active/underruns` using the DAC DMA's one-deep next-block queue (not in the upstream SDK) |
| `src/storage.c` | `APX_NO_STORAGE_COMMAND` leaves out the host SD/FatFs command set (15 KiB of the runtime window); `f_setcp` is compiled out when `FF_CODE_PAGE` is not 0 |
| `third_party/fatfs/ffconf.h` | `FF_CODE_PAGE 437` instead of the all-codepage build (saves a 483 KB table) |

These were carried over from the previous vendored SDK. Three older changes are now upstream and were dropped: the UART CR+LF mirror
(`src/uart.c`), the relaxed SD receive-status check (`src/sd.c`) and the hardware multiply/divide (`src/hwmuldiv.S`, `HW_MULDIV=1`).

Not vendored: `lvgl_port.c`, `image.c` and the stb image decoder (LVGL and the JPEG decoder are not used), the examples, tests and
research notes, and the parts of TinyUSB this configuration does not compile (only `tusb.c`, `common/`, `device/`, `osal/` and the MSC, video and
audio class directories are copied, with its `LICENSE`).

The SDK's board headers live in `boards/<board>/board.h`. The DOOM port's own per-board file is `firmware/boards/<board>/doom_board.h`
(a different name on purpose: both are on the include path).

To refresh: copy the same file set from a newer `appotech-sdk`, re-apply the table above, and rebuild both boards.
