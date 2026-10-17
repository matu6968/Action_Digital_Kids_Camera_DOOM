# Flashing, running and getting back

Everything goes over the camera's USB port. Nothing has to be opened up unless a bad flash stops the camera from booting.

Tools live in `tools/`; they need `python3` with `pyusb` and `pyelftools` (`Pillow` for screenshots) and a udev rule so you
can talk to the camera without root: `appotech-sdk/udev/99-appotech-sdk.rules` (copy it to `/etc/udev/rules.d/`, reload udev).

## 0. The three ways of running something

| | Writes flash? | Survives power cycle? | Use it for |
|---|---|---|---|
| **USB launch** (`tools/launch.sh`) | no | no | development, trying a build, first test of everything |
| **SPI install** (`tools/install.sh`) | **yes**, the whole chip | yes | the finished camera |
| **Stock firmware** | - | - | what the camera does when you do nothing |

A USB launch is the default for a reason: whatever goes wrong, a power cycle brings the camera back.

## 1. Make a backup first

`dumps/flash.bin` is the stock firmware of the author's camera with the user-saved photos removed, the image this port is built on.
Your camera's stock image differs in the user-data area at the end (photos, settings). `tools/install.sh` reads the whole chip into `backups/`
before it writes and refuses to continue unless that read is complete, so you always have *your* image. Keep it.

## 2. Run from RAM

Plug the camera into a computer with a USB data cable while it is on (or let the plug turn it on). It shows the USB screen
([`images/action-camera-msc-mode.jpg`](../images/action-camera-msc-mode.jpg)) and enumerates as `1908:3283`
"Buildwin Media-Player" with a mass-storage interface.

```sh
cd firmware && make && cd ..
tools/launch.sh                         # stock firmware -> BootROM loader -> DOOM, over USB
```

`launch.sh` finds the camera, sends the vendor command that reboots it into the BootROM loader (`1908:3319` "BLDR"), checks that
the RAM it needs is zero, uploads the image with read-back verification, and starts it. If something else is already running as
an SDK image (a previous DOOM), it returns to the loader first - the loader gives up after about three seconds, which is why this
is one script and not two commands.

The game needs a WAD:

* **SD card:** put `wad/doom1_e1m1.wad` in the root of a FAT or exFAT card as `DOOM.WAD`.
* **Over USB, no card:** `tools/load_wad.py wad/doom1_e1m1.wad` while the DOOM image is running (it falls back to waiting for
  one). RAM survives a relaunch, so this is once per power-on.
* **Inside the firmware:** see step 3; used automatically when there is no usable card.

Useful while developing (builds with `DOOM_STATS=1`, the default):

```sh
python3 -m apxtool debug --port 7-1 logs     # the firmware's printf output (use PYTHONPATH=tools/python)
tools/screenshot.py shot.png                 # what the display is being sent, read back over USB
tools/keys.py tap START                      # press a key from the host
tools/bot.py 120                             # god mode + random walk for two minutes, then read the log for EMPTY LUMP / I_Error
```

## 3. Install for cold boot (optional)

```sh
python3 tools/spi_pack.py --original dumps/flash.bin --manifest firmware/build/doom.manifest.json \
        --wad wad/doom1_e1m1.wad --output build-spi/doom.spi.bin
tools/install.sh build-spi/doom.spi.bin
```

`spi_pack.py` builds the image on the host and writes nothing to the camera. What is in it:

| Flash range | Contents |
|---|---|
| `0x000000-0x0021ff` | BootROM header and the vendor's SRAM bootstrap (DDR/PLL calibration). Not touched except for three fields that point the BootROM at the image. |
| `0x002200-0x015fff` | the runtime window (SDK, libc, `port/main.c`), ~78 KB, loaded by the BootROM to `0x02180000` |
| `0x016000-0x0bcfff` | the engine overlay (~666 KB), copied to `0x022e0000` by `apx_spi_overlay_load()` at the start of `main` |
| `0x0bd000-0x166fff` | the WAD, deflate-compressed, 681 KB (`"WZAD"`, sizes, CRC-32, stream) |
| `0x167000-0x1fbfff` | free (old vendor data) |
| `0x1fc000` | overlay table |

`install.sh` (1) launches the DOOM image over USB to get a flash driver, (2) reads the chip into `backups/`, (3) shows the backup's
hash and waits for you to type `install`, (4) writes only the sectors that differ, verifies each, commits sector 0 last, and
compares the whole chip at the end. A power cut in the middle can leave the camera needing step 5 below.

Unplug USB and power-cycle to test. Watching UART0 TX (PA7, 115200 8N1) shows the boot trail; with no SD card the WAD is unpacked from flash, which takes about 4.5 s (a progress screen is shown). The first thing on screen is the DOOM title; with no SD card it says `UNPACKING WAD FROM
FLASH` for about a second first. If the camera sits on a dark screen, see "When the camera does not start".

## 4. Going back

```sh
tools/rescue.sh --yes restore dumps/flash.bin --backup before-restore.bin   # needs the BootROM loader, see 5
```

or, with the camera running a DOOM/SDK image over USB, `python3 -m apxtool debug --port 7-1 flash install YOUR_BACKUP.bin
--backup new-backup.bin --expect-current-sha256 HASH_OF_WHAT_IS_ON_THE_CHIP_NOW`.

## 5. When the camera does not start

A bad image can leave the camera black: no stock firmware, no USB. The BootROM is in the chip, not in the flash, so it is still
there. Forcing it:

1. Open the camera (the board is `XYX-001-5295C_V03`).
2. Find the SPI flash (`25Q16ETJG`, 8 pins, next to the SoC). **Pins 2 and 3** are `DO` and `/WP` on the standard 25Qxx pinout.
3. Hold RESET (or disconnect the battery), short pins 2 and 3 with tweezers, release RESET after a few seconds, and hold POWER for about a second.
4. Release the short **as soon as BLDR shows (`lsusb` lists `1908:3319 BLDR v1.00`)** - the loader needs the flash. Do not leave the camera idle: the loader's watchdog
   resets it a couple of seconds after the last USB traffic, which is why the tools run it continuously.
5. `tools/rescue.sh jedec` (read-only; expect `5E4015`), then `tools/rescue.sh dump bricked.bin`, then the `restore` above.

Keep POWER held if the power-management chip is in a bad state. More detail, and the history of why the SPI boot needs what it needs, is in `appotech-sdk/research/spi-boot-recovery.md`.

Things that made SPI cold boots fail while the SDK was being written, in case a modified image misbehaves: the BootROM leaves its
watchdog armed (the SDK feeds it from the first instruction), the PM chip needs its stock power sequence on every cold boot
(`apx_power_domain_enable`), USB needs two registers the BootROM sets, and the timer-control gate opens a fixed time after
reset (the SDK retries for up to 3 s). `firmware/sdk/src/runtime.c` does all of it; do not strip it.
