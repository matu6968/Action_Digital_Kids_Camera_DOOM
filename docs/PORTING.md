# How this port was built, and how to take it further

## Where the engine came from

[GBADoom](https://github.com/doomhack/GBADoom) is a prboom derivative squeezed into a GBA's memory. That makes it a good base for a
small RAM-only target: all hardware access is behind a handful of functions (`GBADoom/include/i_system_e32.h`), game data is read **by
pointer** out of one flat WAD (`&doom_iwad[filepos]`), and its zone heap is a single block. The camera port implements those
functions in `firmware/port/`:

| File | What it does |
|---|---|
| `main.c` | the only part linked into the runtime window; starts USB, loads the overlay (SPI boot), clears the engine's state, loads the WAD, starts the engine |
| `plat.c`, `plat_video.h` | polling of everything the SDK needs polled, the NV21 planes, the boot/error text screen, the message box drawn over the game |
| `i_system_jt5295.c` | video (palette -> NV21, cropped or scaled per board), 35 Hz time base, key-event posting (`plat_post_keys`), `I_Error`, the engine's static zone |
| `input_action.c`, `input_silvergear.c` (+ `plat_input.h`) | the key mapping of each board: exactly one is built (`BOARD_TAG` in the Makefile) |
| `doom_print.c` (+ `plat_print.h`) | Silvergear: print the current frame on the thermal printer; a stub where `BOARD_HAS_PRINTER` is 0 |
| `i_sound_jt5295.c` | the 8-channel mixer and the gapless DAC block queue (`plat_audio_suspend` for blocking jobs) |
| `w_sd_wad.c`, `inflate.c` | WAD loading: RAM footer (USB), SD card, deflated copy in flash |
| `fixed_hw.h` | `FixedMul` from four `l.mul` (the libgcc entry points are `firmware/sdk/src/hwmuldiv.S`, part of the SDK) |

`GBADoom/engine-patches.diff` lists the nine engine files that differ from upstream, with the reason in a comment at each change.

## Lessons that cost time

* **The SDK launcher and SPI packer only accept fixed RAM reservations.** The layout in `doom.ld` is a puzzle built around them:
  code in the codec-RAM bank because that is what the packer stores densely as an overlay; zeroed state in the extra bank; the WAD
  in RAM the SDK does not claim at all. A segment outside the reservations makes `launch` refuse the image.
* **`APX_BUILD_ID` is baked into three objects.** If the id in the manifest and the one in the running image differ, `launch`
  reports `SDK build identifier mismatch` *after* the image has already started. The Makefile rebuilds those objects through a
  stamp file whenever the id changes.
* **Check that the SDK's pollers still work at the engine's polling rate.** After the SDK update the Silvergear showed DOOM and ignored every
  key: the new ADC reader gives up 5 ms after it starts a conversion and needs three polls a tick apart, while the engine polls once per frame
  (about 28 ms). `apxtool debug gpio info` said it (`adc 65535`, `adc_timeouts` rising). The vendored `adc.c` has an opt-in `APX_ADC_SPARSE_POLL`. The
  Vision uses the same reader, so it had the same fault.
* **Two headers called `board.h` do not coexist.** The SDK's `display.h` includes `board.h`, so the port's own file is `doom_board.h`.
* **The SDK reads its linker symbols literally.** `__large_buffer_start/end` mean "the application buffers in `0x02080000-0x020fffff`" to `debug.c`
  (it refuses host access there) and to `spi_boot.c` (it clears it); that range is where the WAD lives. The SPI driver's 4 KiB bounce buffer
  uses the `.large_buffer` section, so `doom.ld` places it in the runtime window and publishes the symbols as an empty range.
* **The Silvergear has no room for an overlay.** The overlay table sector (`0x1fc000`) holds stock data on its flash, and `apx_spi_overlay_load()` returns
  an error on it, which `main` treats as fatal. So the Silvergear build is `LAYOUT=flat` (engine as loaded code in the extra bank, zeroed state in the
  codec bank, no overlay call) and `BOARD_WAD_FLASH_LIMIT` replaces the overlay table as the end of the free flash.
* **The SDK packer pins the stock bootstrap, but an SDK image already on the chip patches three fields of it.** `spi_pack.py --stock-prefix` puts the
  stock prefix back after checking that only those fields differ, so the base can still be a fresh read of the chip (everything else stays untouched).
* **A file-backed segment in the extra bank costs the packed SPI image the zero gap below it** (about 0.5 MiB). That is why the Vision's
  peripheral and USB code stays in the runtime window (`EXTRA_CODE=0`, about 4 KiB to spare of the 128 KiB).
* **A progress bar can ruin a print.** The thermal driver steps the paper once per half-row; anything that delays its next poll leaves a stripe.
  Redrawing the screen every 250 ms stalled it for up to 36 ms (62 times in one print). The status box is drawn once, before the head starts.
* **Two cameras on one USB bus: never let a script guess.** `launch.sh` would take the first 1908 device. It refuses now unless `USB_PORT` names one.
* **Never `wfi`, never spin without polling USB.** `I_Error` shows the message on screen and keeps calling the SDK's poll so the
  camera can still be reached over USB.
* **Check what the SD status bits mean on a second card.** The stock check looked like protocol; it was noise.
* **The 64-bit divide per `I_GetTime` call** that killed the Cortex-M33 port's first version is avoided here in the same way: the
  tick counter is accumulated and compared, never divided.
* **Anything the SDK calls before the overlay is copied must live in the runtime window.** The first SPI image reset every two
  seconds with only the vendor's `PSRM: 4MB pass` on the UART: the multiply/divide routines had been placed in the overlay, and the clock and power
  set-up call `__mulsi3`/`__udivsi3` before `main` runs. The overlay area was still zeroed, an all-zero word is `l.j 0`, the core
  spun until the watchdog fired. A USB launch hides this because it loads every segment first. `doom.ld` now keeps `main.c` out of the overlay and the routines
  live in the SDK (`hwmuldiv.S`), which is always in the runtime window; to check a change, list the runtime-window functions that call into the overlay (only `main`,
  and libgcc helpers used after it, may).
* **Give a cold boot a voice.** `main()` prints a flushed trail on UART0 TX (PA7, 115200 8N1, the pin the vendor bootstrap prints on):
  `DOOM: main`, `usb up`, `overlay in place`, `engine state cleared`, `display ready`, `WAD loaded`. The last line printed is the last
  step reached. No line at all means the hang is before `main`: read the SDK's SRAM boot marks (`tools/rescue.sh marks`).
* **Test cold.** A USB launch hides everything the stock firmware initialised for you. The SDK's cold-boot requirements are in
  `docs/FLASHING.md`.

## Using more of DOOM

* **Another map**: the WAD buffer is 1.5 MiB and the full processed shareware WAD is 3.8 MB, so a map needs its own trimmed WAD.
  `tools/wad_trim.py --map E1M2` is written for that (map lumps, thing/sprite closure, textures, flats, sounds are all derived from
  the map), but it has only been run on E1M1: the seed WAD in this repository contains no other map. Build a processed WAD with
  GBADoom's `GbaWadUtil` from a shareware `doom1.wad`, then trim. The single-map patches (demo loop, game-mode detection,
  reboot at the end of the level, see `engine-patches.diff`) have to be reworked for a multi-map WAD.
* **More RAM for the WAD**: the zone, planes and mixer are in the extra bank (about 450 of 768 KiB used). The WAD must be one contiguous
  region, so only the bottom 1.5 MiB works unless the SDK reservations are widened (`tools/python/apxtool/sdk.py` and `spi_boot.py`
  each hard-code them).
* **Music** would need an OPL or MUS synthesizer in the mixer; the DAC has the bandwidth, the core has not much time to spare.
* **Faster**: the game already holds its frame rate; a `-mhard-mul` compiler profile (docs/TOOLCHAIN.md) would shave the call overhead,
  and the SDK's USB poll (about once per millisecond from `I_GetTime`) could be rarer.

## Another board with the same SoC

Everything specific to a board is in three places: `firmware/sdk/boards/<board>/board.h` (the SDK's pins, ADC ladder values, panel, SD, SPI,
printer; belongs to the SDK), `firmware/boards/<board>/doom_board.h` (what the port adds: panel geometry, scaling, text layout, WAD path, button timing,
`BOARD_HAS_PRINTER`), and one key mapping, `firmware/port/input_<tag>.c`. The Silvergear was added this way. To add a board:

1. Get the SDK's board working first (`appotech-sdk`: display, buttons, SD, power); copy `boards/<board>/` into `firmware/sdk/boards/`.
2. Write `firmware/boards/<board>/doom_board.h`: copy the closest one, set `BOARD_LCD_*`, `BOARD_NV21_PITCH` (a multiple of 32), the text/progress layout and the
   POWER timings. If the panel is not 220x176, either crop (`BOARD_CROP_X`) or add a scaled path like `BOARD_SCALE_8_3`.
3. Write `firmware/port/input_<tag>.c` (`plat_input_init` and `I_ProcessKeyEvents`, building a key mask for `plat_post_keys`), and add the board to `BOARDS`
   and `BOARD_TAG` in `firmware/Makefile`.
4. Decide where the SDK's code goes (`EXTRA_CODE`): the runtime window if it fits (smaller SPI image), the extra bank if it does not.
5. Build, RAM launch with `tools/launch.sh`, then check the polling rate (`debug gpio info`), the frame rate (the `stats:` log line) and the sound before anything else.

The SDK and the BootROM protocol are the same for the other AppoTech/Buildwin kids' cameras that use the JT5295; their panel controllers and button
ladders differ.
