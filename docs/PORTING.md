# How this port was built, and how to take it further

## Where the engine came from

[GBADoom](https://github.com/doomhack/GBADoom) is a prboom derivative squeezed into a GBA's memory. That makes it a good base for a
small RAM-only target: all hardware access is behind a handful of functions (`GBADoom/include/i_system_e32.h`), game data is read **by
pointer** out of one flat WAD (`&doom_iwad[filepos]`), and its zone heap is a single block. The camera port implements those
functions in `firmware/port/`:

| File | What it does |
|---|---|
| `main.c` | the only part linked into the runtime window; starts USB, loads the overlay (SPI boot), clears the engine's state, loads the WAD, starts the engine |
| `plat.c`, `plat_video.h` | polling of everything the SDK needs polled, the NV21 planes, the boot/error text screen |
| `i_system_jt5295.c` | video (palette -> NV21), 35 Hz time base, input and the POWER gestures, `I_Error`, the engine's static zone |
| `i_sound_jt5295.c` | the 8-channel mixer and the gapless DAC block queue |
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

Everything specific to this PCB is in `firmware/boards/action-camera/board.h` and in `firmware/sdk/board/board.h` (pins, ADC ladder
values, panel rotation, SD clock). The SDK and the BootROM protocol are the same for the other AppoTech/Buildwin kids' cameras
that use the JT5295; their panel controllers and button ladders differ.
