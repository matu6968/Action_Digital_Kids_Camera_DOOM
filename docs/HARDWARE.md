# Hardware notes

One camera, one SoC. What is written here was reconstructed from the stock firmware and verified on the device; there is no
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
| `0x02000000-0x0217ffff` | 1.5 MiB | **the WAD**, copied from SD / flash / USB at start-up |
| `0x02180000-0x0219ffff` | 128 KiB (about 100 KiB used) | the runtime window: SDK, libc, `port/main.c`, 12 KiB heap |
| `0x02200000-0x02203fff` | 16 KiB | stack (about 1.2 KiB deepest use seen) |
| `0x02220000-0x022dffff` | 768 KiB (about 450 KiB used) | engine zeroed state: 256 KiB zone, 38 KiB engine frame, two NV21 plane sets (156 KiB), mixer blocks, SPI/inflate buffers |
| `0x022e0000-0x023fffff` | 1152 KiB (about 665 KiB used) | engine code, constants and initialised data (an overlay on SPI boot) |

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
  and the chroma for the pair (`lut_y2`, `lut_vu` in `port/i_system_jt5295.c`); 10 pixels are cropped from each side and the picture
  sits in rows 8-167 of the 176.

## Buttons

`ad-key` in the stock firmware reads ADC channel 3: idle 1023; pressed SHUTTER 0, DOWN ~306, UP ~484, LEFT ~658, RIGHT ~817, each
with a window of +-29. POWER is digital PB0, high when pressed. Only one ladder key is distinguishable at a time. The SDK debounces
(20 ms) and wants a poll at least every 5 ms.

## Audio

* Mono signed 16-bit PCM at 8, 16 or 32 kHz, played by DMA from RAM: `0x9148` source, `0x9144` length in words minus one,
  `0x9158` halfwords still to play, `0x914c` event clear (7) and start (8), `0x9150` events.
* **The DMA has a one-deep next-block queue** (measured with `doom-probe`): writing a new source and length while a block is playing and
  then *adding* its halfword count to `0x9158` makes the engine continue with it when the current block ends, without a gap.
  `0x9158` therefore always equals the halfwords left across both blocks, which also tells where playback is. The port keeps two
  1024-sample (64 ms) blocks in flight.
* The measured sample clock matches the label for 8/16/32 kHz within 0.05 % at 144 MHz.

## SD card

1-bit, 200 kHz identification, 24 MHz transfers, single-block reads (CMD17) with DMA. The SDK was first validated on a 2 GB SDSC card
only; the port found that SDHC works too once the receive-status check is relaxed: bits 22:21 of the controller status, required
to be `10` by the stock firmware, read `11`, `01` or `00` from run to run on a 16 GB SDHC card while the data was correct
(identical hash over 4096 sectors across runs). Bit 16 is the real CRC-error flag. Reads run at about 0.8 MB/s including the
progress display. **Writing to SDHC is untested.**

## Power

An always-powered power-management chip (driven through `firmware/sdk/src/power.c`): after a power cycle the SDK must run the stock sequence
(`apx_power_domain_enable`) or USB/keep-alive is lost. `apx_power_off()` is the supported way to turn the camera off; the port
calls it after POWER is held for 3 s.
