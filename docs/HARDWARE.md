# Hardware notes

Two cameras, one SoC. The first sections describe the Vision (`XYX-001-5295C_V03`); the Silvergear's differences, including its thermal printer,
are in [The Silvergear](#the-silvergear) at the end. What is written here was reconstructed from the stock firmware and verified on the device; there is no
datasheet. The register-level detail, with the stock-firmware address each fact came from, is in the SDK's research notes
(`appotech-sdk/research/*.md`); the code that implements it is `firmware/sdk/src/`.

## Board

| | |
|---|---|
| Board | `XYX-001-5295C_V03` |
| SoC | JT5295, QFN64, OpenRISC little endian ([SOC_IDENTIFICATION.md](SOC_IDENTIFICATION.md)) |
| Flash | 25Q16ETJG, 2 MiB, JEDEC `5E4015`, W25Q command set |
| Display | ILI9225G, 176x220 native, used rotated: 220x176 landscape ("rotation 90": logical (x,y) -> GRAM (y, 219-x)) |
| Storage | microSD slot, 1-bit SD controller on `SD0` (PA0..PA2). **PA2 must stay an output driven low** - making it an input makes the card stop answering; probably a card-enable line |
| Audio | speaker through the SoC's 16-bit DAC; there is also a microphone input |
| Camera | sensor on a flex connector (unused here) |
| Buttons | one ADC ladder (SHUTTER, DOWN, UP, LEFT, RIGHT) + POWER on PB0 |
| LED | blue status LED on PB5 |
| Power | rechargeable 200 mAh battery; USB-C |

## Memory map

| Range | Contents |
|---|---|
| `0x00000000` | SRAM on the chip: the vendor bootstrap, the exception vector table (16-byte slots) and the SDK's boot-time breadcrumbs (`0x2f00`..`0x3500`) |
| `0x00100000` | BootROM, 64 KB |
| `0x02000000-0x023fffff` | 4 MiB RAM. The stock application runs from here; an SDK image owns `0x02180000-0x023fffff` |
| `0x02180000` | where the BootROM loads an SPI image's first block |
| SPR space | all peripheral registers are reached through `l.mfspr`/`l.mtspr` (not memory mapped): `0x8xxx` clocks and GPIO, `0x9000` SPI flash, `0x9140` DAC, `0x9170` ADC, `0x9880` SD, `0xa800`/`0xc800` display and compositor, ... |

The DOOM image's own map ([`firmware/linker/doom.ld`](../firmware/linker/doom.ld)):

| Range | Size | Contents |
|---|---|---|
| `0x02000000-0x0217ffff` | 1.5 MiB | **the WAD**, copied from SD / flash / USB at start-up. Not an SDK reservation: the SDK never touches it |
| `0x02180000-0x0219ffff` | 128 KiB (Vision: about 124 KiB used, Silvergear about 91 KiB) | the runtime window: SDK incl. TinyUSB, libc, `port/main.c`, 12 KiB heap |
| `0x02200000-0x02203fff` | 16 KiB | stack (about 1.2 KiB deepest use seen) |
| `0x02210000-0x0221ffff` | 64 KiB | display DMA workspace (SDK) |
| `0x02220000-0x022dffff` | 768 KiB | the SDK's `.extra_bss` (debug link buffers, TinyUSB state, 14 KiB) and the engine's zeroed state: 256 KiB zone, 38 KiB engine frame, the NV21 plane sets, mixer blocks, SPI/inflate buffers. Vision about 457 KiB used. **Silvergear about 723 KiB**: planes are 230 KiB, plus 68 KiB printer workspace, 38 KiB print capture, and 45 KiB of SDK printer/peripheral/USB code (`EXTRA_CODE=1`) |
| `0x022e0000-0x023fffff` | 1152 KiB (about 665 KiB used) | engine code, constants and initialised data (an overlay on SPI boot) |

The Vision keeps the SDK's peripheral and USB code in the runtime window (`EXTRA_CODE=0`), because a file-backed segment in the extra bank
makes the packed SPI image carry the ~0.5 MiB of zeros between the two windows. The Silvergear does not fit that way and uses the extra bank;
the BootROM loads that code in one block (see `docs/FLASHING.md`, Silvergear cold boot), so the engine sits right behind it as loaded code and its zeroed
state moves to the codec bank (`LAYOUT=flat`). The extra bank then holds about 738 KiB of its 768, with the engine state (about 657 KiB) in the codec bank.

## Display

The ILI9225G is not driven pixel by pixel. The path is the stock firmware's:

```
YUV (NV21) planes in RAM --> compositor/scaler (SPR 0xc800) --> LCD controller (SPR 0xa800) --> panel
```

* The planes are full-range BT.601 NV21: a luma plane and an interleaved **V,U** plane at half resolution, pitch a multiple of 32
  bytes (224 for 220 pixels). The compositor converts to RGB565 on the fly.
* A full frame takes about 17 ms (58 fps) when the planes are ready; the CPU conversion of RGB565 would add ~50 ms and a PIO
  RGB565 blit is ~255 ms, hence the port builds NV21 directly.
* The panel needs `apx_display_init()` (about half a second, includes the reset pulse). Initialisation from a cold boot is
  independent of what the stock firmware left behind.
* DOOM renders 240x160 indexed. Each source word holds two identical pixels, so one palette lookup per word gives two luma bytes
  and the chroma for the pair (`lut_y2`, `lut_vu` in `port/i_system_jt5295.c`); on the Vision 10 pixels are cropped from each side and the
  picture sits in rows 8-167 of the 176. The Silvergear's scaled path is described under [The Silvergear](#the-silvergear).

## Buttons

*(Vision; the Silvergear's ladder is under [The Silvergear](#the-silvergear).)* `ad-key` in the stock firmware reads ADC channel 3: idle 1023; pressed SHUTTER 0, DOWN ~306, UP ~484, LEFT ~658, RIGHT ~817, each
with a window of +-29. POWER is digital PB0, high when pressed. Only one ladder key is distinguishable at a time. The SDK debounces
(20 ms) and wants a poll at least every 5 ms. **The DOOM engine polls once per rendered frame (about 28 ms)**, which the SDK's new ADC reader
could not cope with (a conversion needs three polls a tick apart and used to time out after 5 ms, so no key was ever read, with `debug gpio
info` showing `adc 65535` and a rising `adc_timeouts`). The vendored copy has an opt-in `APX_ADC_SPARSE_POLL` (see `firmware/sdk/README.md`).

## Audio

* Mono signed 16-bit PCM at 8, 16 or 32 kHz, played by DMA from RAM: `0x9148` source, `0x9144` length in words minus one,
  `0x9158` halfwords still to play, `0x914c` event clear (7) and start (8), `0x9150` events.
* **The DMA has a one-deep next-block queue** (measured with `doom-probe`): writing a new source and length while a block is playing and
  then *adding* its halfword count to `0x9158` makes the engine continue with it when the current block ends, without a gap.
  `0x9158` therefore always equals the halfwords left across both blocks, which also tells where playback is. The port keeps two
  1024-sample (64 ms) blocks in flight.
* The measured sample clock matches the label for 8/16/32 kHz within 0.05 % at 144 MHz.

## SD card

*(Vision, measured with the old SDK driver; the DOOM image now uses the SDK's new one: multi-block reads, high-speed mode at 48 MHz, and a
4-bit probe on the Vision with a 1-bit fallback. The SDK's own `sd-bench` measured about 20 MB/s at 4-bit on the Vision and 5.8 MB/s at 1-bit
on the Silvergear; the WAD load time of this image has not been re-measured.)*

1-bit, 200 kHz identification, 24 MHz transfers, single-block reads (CMD17) with DMA. The SDK was first validated on a 2 GB SDSC card
only; the port found that SDHC works too once the receive-status check is relaxed: bits 22:21 of the controller status, required
to be `10` by the stock firmware, read `11`, `01` or `00` from run to run on a 16 GB SDHC card while the data was correct
(identical hash over 4096 sectors across runs). Bit 16 is the real CRC-error flag. Reads run at about 0.8 MB/s including the
progress display. **Writing to SDHC is untested.**

## Power

An always-powered power-management chip (driven through `firmware/sdk/src/power.c`): after a power cycle the SDK must run the stock sequence
(`apx_power_domain_enable`) or USB/keep-alive is lost. `apx_power_off()` is the supported way to turn the camera off; the port
calls it after POWER is held for 3 s.

## The Silvergear

Action article 3222882 "Silvergear", board `JX-01 E246715`. Same SoC and BootROM; what differs (values from the SDK's `boards/silvergear-3222882/board.h`
and its research notes, checked on the camera):

| | |
|---|---|
| Flash | NM25Q32A, 4 MiB, JEDEC `944016`. Sector `0x3ff000` holds the stock settings and `0x1fc000` stock data: neither may be used by an image |
| Display | ST7789, 240x320 native, rotation 3 (landscape 320x240). Reset is a dedicated output, the backlight is power-management bit PM54.6, not a GPIO |
| Buttons | **one ADC ladder for all seven keys, POWER included** (no separate POWER pin): idle 1023; POWER ~15, PRINT ~130, SHUTTER ~510, EXIT ~420, MODE ~620, LEFT ~330, RIGHT ~240 (window +-35; owner-measured PRINT 134, SHUTTER 512, EXIT 413, MODE 614). No UP or DOWN key |
| LED | flash LED on PB0, active high, a latched push-pull output |
| SD | 1-bit only: the stock 4-bit wiring would use PA3 and PE10/PE11, which are printer motor phases and camera power-down lines |
| Cameras | two sensors (front, rear). DOOM never starts them; the printer needs them off because the pads are shared |
| Printer | thermal head, 384 dots, see below |

**Printer.** PA7 is the paper sensor (high = no paper), PA6 the head thermistor on ADC channel 1 (with the SoC's pull-up as the bias), timer 3 times the
strobes (about 1.125 MHz measured). The SDK driver refuses to heat without a valid battery reading of 3400-4600 mV, with the camera off, or over a
temperature index of 65. It moves the paper **one half-row per motor step** and heats in 32 weighted passes per half-row; a half-row takes
12-14 ms with the SDK (the stock firmware 8.3 ms). Any delay between two polls of the driver pauses the paper and prints a stripe, so the DOOM port
does no display work while the head runs. The LCD stays alive during a print (verified by the SDK's printer demo); the shared pads are
the UART TX route (PA7), which the driver releases when it shuts down.

**Scaling.** The panel is 320x240 and the engine renders 240x160 with every pixel doubled, so there are only 120 distinct columns. `convert_scaled` in
`port/i_system_jt5295.c` maps 3 source words to 8 output pixels (`w0 w0 w0 w1 | w1 w1 w2 w2`: four luma halfwords and four V,U pairs, the pair taken
from the left pixel) and 20 source rows to 27 output rows (216 rows, 12 black rows above and below; the 1.35 vertical factor is 1.3 % off the exact
aspect ratio and keeps every row offset even for the half-resolution chroma). An output row that shows the same source row as the one above is a
`memcpy` of its luma. It costs no measurable frame rate (35 fps).
