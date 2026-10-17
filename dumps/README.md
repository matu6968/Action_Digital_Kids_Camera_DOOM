# Dumps

| File | What it is |
|---|---|
| `flash.bin` | The full 2 MiB SPI flash of the author's camera: the stock firmware, with the user-saved photos removed (SHA-256 `2d0d864f...`). Restoring it brings the camera back to stock, and the SPI image packer builds on it. |
| `bootrom_0x00100000_64K.bin` | The 64 KB BootROM, read through the loader at `0x00100000` (SHA-256 `f5b68ff4...`). |

## `flash.bin`: user photos removed

The firmware never really removes a saved photo. Deleting it in the menu only unlists it, and the menu's Sys. Reset (factory reset)
only resets the saved settings; in both cases the JPEG stays in the SPI flash. A dump of a camera that has been used therefore carries
its owner's pictures. This dump comes from a camera with saved photos, and they were removed from the flash itself before it was
published:

- `0x000000..0x1baf00`, the BLDR boot header, the application and all of its resources, is the stock firmware untouched.
- The photo store (the firmware calls it `udisk`: `udisk_addr=0x1ba000`, `udisk_capacity=0x45000`, `pic_max_num=6`, a `0xfb0`-byte
  head) is blanked with `0xff` (erased flash) from `0x1bb000` to the end of the chip (`0x45000` bytes). In the unmodified dump that
  area was mostly zeros around the photo data below `0x1c0000`, and erased above it.
- Four bytes in the 80 bytes just below the store (`0x1bafd8` and `0x1baffa..0x1baffc`) differ from the unmodified dump. That record is
  leftover RAM (heap pointers in `0x020ad200..0x023fec00`, with values that differ between dumps), not a photo flag: the udisk head
  itself (`0x1ba000..0x1bafb0`, `pic_num=0`) is unchanged.

**Status: boots.** Flashed to the author's camera (2026-10-17) with `debug flash install`; the write verified against a full-chip
readback (`2d0d864f...`). The camera enumerated as the stock firmware (USB `0219:3280`, then `1908:3283`, UVC plus mass storage), stayed
up, and showed the stock USB mass-storage screen. Only the boot and USB mode were checked; the camera, gallery and game menus were not.

An earlier version of this image padded the same area with `0x00` instead of `0xff`. Its first start after flashing reset itself a few
seconds in (the UART, PA7 at 115200, showed a normal start: `PSRM: 4MB pass`, `XOSC`, `spi_udisk_init ok`, `udisk_head pic_max_num=6
pic_num=0`); a manual reset then brought it up normally into mass-storage mode, and it did not repeat. The `0xff` version did not reset
on its first start, flashed and rebooted the same way. That is one observation each, so the cause is not proven, but erased flash is the
safer padding: the stock firmware may treat a non-erased area as something to clean up.

If your camera behaves oddly with this image, reset it once more. **Make your own dump before you flash anything**: `tools/install.sh`
does it automatically (`backups/`), and the "Going back" commands in [docs/FLASHING.md](../docs/FLASHING.md#4-going-back) restore it.
Your camera's flash holds its own settings and photos, which this image does not reproduce.

## A note on redistribution

The flash dump contains the vendor's application and resources, none of which are ours to license. It is included because without it
the reverse engineering cannot be checked or carried on. If that is a problem, delete this folder - nothing in the build depends on it
except the default `--original` argument of `tools/spi_pack.py`, for which any stock dump of the same board works.
