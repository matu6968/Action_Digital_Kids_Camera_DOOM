# Flashing, running and getting back

Everything goes over the camera's USB port. For the Vision camera, nothing has to be opened up unless a bad flash stops the camera from booting.

Tools live in `tools/`; they need `python3` with `pyusb` and `pyelftools` (`Pillow` for screenshots) and a udev rule so you
can talk to the camera without root: `appotech-sdk/udev/99-appotech-sdk.rules` (copy it to `/etc/udev/rules.d/`, reload udev).

## 0. The three ways of running something

| | Writes flash? | Survives power cycle? | Use it for |
|---|---|---|---|
| **USB launch** (`tools/launch.sh`) | no | no | development, trying a build, first test of everything |
| **SPI install** (`tools/install.sh` Vision, `tools/install_silvergear.sh` Silvergear) | **yes**, most of the chip | yes | the finished camera |
| **Stock firmware** | - | - | what the camera does when you do nothing |

A USB launch is the default for a reason: whatever goes wrong, a power cycle brings the camera back.

## 1. Make a backup first

`dumps/flash.bin` is the stock firmware of the author's camera with the user-saved photos removed, the image this port is built on.
Your camera's stock image differs in the user-data area at the end (photos, settings). `tools/install.sh` reads the whole chip into `backups/`
before it writes and refuses to continue unless that read is complete, so you always have *your* image. Keep it.

## 2. Run from RAM

> [!NOTE]
> Dissassembly is required for the Silvergear camera to force it into BootROM mode; in the stock firmware the camera will not accept entering into MSC mode 
> for the BootROM entrypoint as it will shut off immediately and refuse to power on when USB power is present.
> See [Silvergear flashing instructions](#silvergear) for more details.

### Vision

Plug the camera into a computer with a USB data cable while it is on (or let the plug turn it on). It shows the USB screen
([`images/action-camera-msc-mode.jpg`](../images/action-camera-msc-mode.jpg)) and enumerates as `1908:3283`
"Buildwin Media-Player" with a mass-storage interface.

```sh
cd firmware && make && cd ..
tools/launch.sh --reclaim-jpeg-ram      # stock firmware -> BootROM loader -> DOOM, over USB
```

`--reclaim-jpeg-ram` is needed while the camera runs its stock firmware (or after a reset that put the stock firmware back): the
engine is loaded to `0x022e0000`, where the stock firmware leaves data, and the launcher refuses to run over non-zero RAM unless
asked. It first saves that 1.1 MB to `jpeg-ram-before-*.bin` next to the manifest (disposable). `apxtool: SDK reservation at 0x22e0000
is occupied` means you left it out. When the camera already runs an SDK image (a previous DOOM) it is not needed.

### Silvergear (or fallback method for Vision)

To run custom firmware on the Silvergear camera, dissassembly is required (however no soldering is needed), see [DISSASEMBLY.md](DISSASEMBLY.md) on how to open the camera.

Once open, find the SPI flash (`NM25Q32A` for Silvergear or `25Q16ETJG` for Vision, 8 pins, next to the SoC). **Pins 2 and 3** are `DO` and `/WP` on the standard 25Qxx pinout.

Then hold RESET (or disconnect the battery), short pins 2 and 3 with tweezers, release RESET after a few seconds, and hold POWER for about a second.

Release the short **as soon as BLDR shows (`lsusb` lists `1908:3319 BLDR v1.00`)** 

Be sure to run the loader script as quickly as possible as the loader's watchdog resets it a couple of seconds after the last USB traffic.

```sh
cd firmware && make BOARD=silvergear-3222882 && cd ..
BOARD=silvergear-3222882 tools/launch.sh     # BootROM loader (or a running SDK image) -> DOOM, over USB
```

Once the Silvergear runs any SDK image (a previous DOOM, or the SDK's own demos) it can be relaunched the same way **without** the short:
`launch.sh` sends it back to the loader itself. Add `--reclaim-jpeg-ram` if the launcher reports the RAM at `0x22e0000` as occupied.
(`apxtool sdk launch` also has `--stock-backup` / `--stage` for starting from the forced BootROM with the DDR set up from a verified
Silvergear backup, RAM only; see the apxtool README.)

`launch.sh` finds the camera, sends the vendor command that reboots it into the BootROM loader (`1908:3319` "BLDR"), checks that
the RAM it needs is zero, uploads the image with read-back verification, and starts it. If something else is already running as
an SDK image (a previous DOOM), it returns to the loader first - the loader gives up after about three seconds, which is why this
is one script and not two commands. With **several cameras attached** it refuses to pick one (a Silvergear image on the Vision would be
wrong); name the USB port: `USB_PORT=5-1 BOARD=silvergear-3222882 tools/launch.sh`. For a running SDK image it also compares the board
name in its `debug info` with the manifest's and stops on a mismatch.

The game needs a WAD:

* **SD card:** put `wad/doom1_e1m1.wad` in the root of a FAT or exFAT card as `DOOM.WAD`.
* **Over USB, no card:** `tools/load_wad.py wad/doom1_e1m1.wad` while the DOOM image is running (it falls back to waiting for
  one). RAM survives a relaunch, so this is once per power-on.
* **Inside the firmware:** see step 3; used automatically when there is no usable card.

Useful while developing (builds with `DOOM_STATS=1`, the default):

```sh
python3 -m apxtool debug --port 7-1 logs     # the firmware's printf output (use PYTHONPATH=tools/python); it drains the ring
tools/screenshot.py shot.png                 # what the display is being sent, read back over USB (--board silvergear-3222882 --port 5-1)
tools/keys.py tap START                      # press a key from the host (same --board / --port options)
python3 -m apxtool debug --port 5-1 gpio info    # the key ladder as the firmware sees it: "adc", "state", "adc_timeouts"
python3 -m apxtool debug --port 5-1 printer info # Silvergear: the last print's step timing (period_hist), temperature, battery
tools/bot.py 120                             # god mode + random walk for two minutes, then read the log for EMPTY LUMP / I_Error
```

## 3. Install for cold boot (optional)

The Vision and the Silvergear have different flash layouts, so they have different scripts and images. Vision first, Silvergear [below](#silvergear-cold-boot).

> [!WARNING]
> **The Vision image is built on the new SDK and has not been cold-booted yet.** The SDK has run TinyUSB from RAM on the Vision but, by its
> own notes, never cold-booted a TinyUSB image from SPI there (it has on the Silvergear). The packed image is byte-identical between the
> old and the new packer, but that says nothing about the new runtime. Take the backup, and keep step 5 in mind.

```sh
python3 tools/spi_pack.py --original dumps/flash.bin --manifest firmware/build/doom.manifest.json \
        --wad wad/doom1_e1m1.wad --output build-spi/doom.spi.bin
tools/install.sh build-spi/doom.spi.bin
```

`spi_pack.py` builds the image on the host and writes nothing to the camera. What is in it:

| Flash range | Contents |
|---|---|
| `0x000000-0x0021ff` | BootROM header and the vendor's SRAM bootstrap (DDR/PLL calibration). Not touched except for three fields that point the BootROM at the image. |
| `0x002200-0x01b3ff` | the runtime window (SDK incl. TinyUSB, libc, `port/main.c`), ~101 KB, loaded by the BootROM to `0x02180000` |
| `0x01c000-0x0c2fff` | the engine overlay (~667 KB), copied to `0x022e0000` by `apx_spi_overlay_load()` at the start of `main` |
| `0x0c3000-0x168fff` | the WAD, deflate-compressed, 681 KB (`"WZAD"`, sizes, CRC-32, stream) |
| `0x169000-0x1fbfff` | free (old vendor data) |
| `0x1fc000` | overlay table |

`install.sh` (1) launches the DOOM image over USB to get a flash driver, (2) reads the chip into `backups/`, (3) shows the backup's
hash and waits for you to type `install`, (4) writes only the sectors that differ, verifies each, commits sector 0 last, and
compares the whole chip at the end. A power cut in the middle can leave the camera needing step 5 below.

Unplug USB and power-cycle to test. Watching UART0 TX (PA7, 115200 8N1) shows the boot trail; with no SD card the WAD is unpacked from flash, which takes about 4.5 s (a progress screen is shown). The first thing on screen is the DOOM title; with no SD card it says `UNPACKING WAD FROM
FLASH` for about a second first. If the camera sits on a dark screen, see "When the camera does not start".

### Silvergear cold boot

```sh
make -C firmware BOARD=silvergear-3222882
USB_PORT=5-1 tools/install_silvergear.sh          # the camera must be running an SDK image (tools/launch.sh gives it one)
```

The Vision stores the engine as an "overlay" behind a table at `0x1fc000` and copies it to RAM at start-up. On the Silvergear that sector holds
stock data (and the SDK's packer refuses overlays for it), so this build uses a **flat layout** (`LAYOUT=flat`, see `firmware/linker/doom.ld`): the
engine is ordinary loaded code in the extra RAM bank behind the SDK's code there, so the BootROM's single block carries all of it, and the engine's
zeroed state lives in the codec bank, which stores nothing. `main()` does not call `apx_spi_overlay_load()` on this board.

| Flash range | Contents |
|---|---|
| `0x000000-0x0021ff` | BootROM header and the vendor bootstrap. Only three fields change: the load address and sector count in the header, and the entry hook at `0x540` |
| `0x002200-0x156fff` | the BootROM block, 1,396,224 bytes loaded to `0x02180000`: the runtime window (~68 KB), a gap of zeros up to `0x02220000` (stack and display DMA live there, nothing is stored for them), then the SDK's printer / USB / peripheral code and the engine (~740 KB) |
| `0x157000-0x1fdfff` | the WAD, deflate-compressed, 681 KB (`"WZAD"`): this unit has no SD card, so it is needed. Unpacking takes about 4.6 s at every boot |
| rest | untouched: the leftovers of the previous image and the stock data, the stock photos from `0x381000`, the SDK flight-recorder sectors `0x3fd000` / `0x3fe000`, the settings sector `0x3ff000` |

`install_silvergear.sh` (1) checks the camera is a Silvergear with the 944016 4 MiB chip, (2) reads the **whole chip twice** into `backups/` and
requires the reads to be identical, (3) packs the image **on top of that read** with `tools/spi_pack.py --board silvergear` (the chip's bootstrap is
already patched by whatever SDK image it holds, so `--stock-prefix dumps/stock-silvergear.bin` supplies the stock prefix after checking that only those
three fields differ), (4) prints the image and backup hashes and the number of sectors it will rewrite, and waits for you to type `install`, (5) lets
`apxtool debug flash install` erase and write only the sectors that differ, verify each, write the boot header sector **last**, and compare the whole chip
at the end. It refuses to erase anything if the chip no longer hashes to the backup. `EXPECT_CHIP_SHA256` / `EXPECT_IMAGE_SHA256` make it stop before the
prompt unless both hashes are exactly the ones you approved.

**Status (2026-11-11):** installed once, over USB, on the author's Silvergear (372 sectors, read-back identical to the image). A host `debug system
reboot` started it from SPI (`boot_source: spi`), it unpacked the WAD from flash and ran at 35 fps; the owner then confirmed that a cold boot from the
battery, the keys, the sound, printing and the POWER power-off all work from the SPI image. One camera, no SD card in it: the SD card path of the SPI image
and a second unit are untested.

The build keeps the SDK's flash driver (`DOOM_STATS=1`, the default; the script refuses a build without it), so the camera can be re-flashed over USB
without the BootROM short: launch or reboot into the installed DOOM and run the script again.

**Going back to the SDK printer demo** (what the camera held before): the backup the script saved is exactly that image. From the running DOOM:
`python3 -m apxtool debug --port 5-1 flash install backups/silvergear-flash-before-doom-STAMP.bin --backup backups/new.bin --expect-current-sha256 HASH_OF_THE_CHIP_NOW`
(this restore path was not rehearsed). **Going back to the stock firmware** and **a camera that does not start** need the BootROM: see below.

## 4. Going back

```sh
tools/rescue.sh --yes restore dumps/flash.bin --backup before-restore.bin   # needs the BootROM loader, see 5
```

or, with the camera running a DOOM/SDK image over USB, `python3 -m apxtool debug --port 7-1 flash install YOUR_BACKUP.bin
--backup new-backup.bin --expect-current-sha256 HASH_OF_WHAT_IS_ON_THE_CHIP_NOW`.

## 5. When the camera does not start

A bad image can leave the camera black: no stock firmware, no USB. The BootROM is in the chip, not in the flash, so it is still
there. Forcing it:

1. Open the camera (see [DISSASEMBLY.md](DISSASEMBLY.md) on how to open either cameras).
2. Find the SPI flash (`25Q16ETJG`, 8 pins, next to the SoC). **Pins 2 and 3** are `DO` and `/WP` on the standard 25Qxx pinout.
3. Hold RESET (or disconnect the battery), short pins 2 and 3 with tweezers, release RESET after a few seconds, and hold POWER for about a second.
4. Release the short **as soon as BLDR shows (`lsusb` lists `1908:3319 BLDR v1.00`)** - the loader needs the flash. Do not leave the camera idle: the loader's watchdog
   resets it a couple of seconds after the last USB traffic, which is why the tools run it continuously.
5. `tools/rescue.sh jedec` (read-only; expect `5E4015` on the Vision, `944016` on the Silvergear), then `tools/rescue.sh dump bricked.bin`, then the `restore` above.

`tools/rescue.sh` uses the SPI flasher payload in `tools/flasher/` (built on first use with `make -C tools/flasher`) and detects a 2 MiB
or 4 MiB chip. Keep POWER held if the power-management chip is in a bad state.

**Silvergear.** The same idea, with the SDK's Silvergear restore tool instead of `tools/rescue.sh restore`: build the payload with `make -C tools/flasher
silvergear-restore` and run `python3 -m apxtool.silvergear_install IMAGE --expect-sha256 H --payload tools/flasher/build/silvergear-full-restore.bin
--expect-payload-sha256 ... --output PREFIX --yes` from the forced BootROM (keep POWER held). It writes the image you give it (the pre-install backup, or
`dumps/stock-silvergear.bin` for a pure stock camera), journals every sector and verifies the chip. The exact command line and the payload hash are in the
SDK's `research/silvergear/spi-install-plan.md` (Result log). That path was not exercised for DOOM. More detail, and the history of why the SPI boot needs what it needs, is in `appotech-sdk/research/spi-boot-recovery.md`.

Things that made SPI cold boots fail while the SDK was being written, in case a modified image misbehaves: the BootROM leaves its
watchdog armed (the SDK feeds it from the first instruction), the PM chip needs its stock power sequence on every cold boot
(`apx_power_domain_enable`), USB needs two registers the BootROM sets, and the timer-control gate opens a fixed time after
reset (the SDK retries for up to 3 s). `firmware/sdk/src/runtime.c` does all of it; do not strip it.
