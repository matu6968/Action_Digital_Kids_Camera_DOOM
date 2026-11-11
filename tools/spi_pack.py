#!/usr/bin/env python3
"""Build the SPI image for cold boot: the SDK packer's image (BootROM load block, plus the engine overlay on the Vision) and, optionally, the
WAD stored compressed in the free flash right after it, so the camera needs no SD card.

  Vision (2 MiB):
  spi_pack.py --original dumps/flash.bin --manifest firmware/build/doom.manifest.json \
              --wad wad/doom1_e1m1.wad --output build-spi/doom.spi.bin
  Silvergear (4 MiB, the base must be a read of THAT camera's chip: tools/install_silvergear.sh does it):
  spi_pack.py --board silvergear --original backups/flash-before-doom-STAMP.bin --stock-prefix dumps/stock-silvergear.bin \
              --manifest firmware/build/silvergear-3222882/doom.manifest.json --wad wad/doom1_e1m1.wad --output build-spi/doom-silvergear.spi.bin

Nothing is written to a camera. Install with tools/install.sh / tools/install_silvergear.sh (back up first, ask before writing).
The WAD record sits at apx_spi_image_end() (what firmware/port/w_sd_wad.c looks for): "WZAD", compressed length, raw length,
CRC-32 of the raw WAD, then a raw deflate stream. It must end before BOARD_WAD_FLASH_LIMIT (firmware/boards/*/doom_board.h).
"""
import argparse, json, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'python'))
from apxtool.spi_boot import build_image, OVERLAY_TABLE  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--board', choices=('action', 'silvergear'), default='action')
ap.add_argument('--original', type=Path, required=True, help='flash dump of the camera (2 MiB Vision, 4 MiB Silvergear), the base the image is built on')
ap.add_argument('--manifest', type=Path, required=True)
ap.add_argument('--output', type=Path, required=True)
ap.add_argument('--wad', type=Path, help='embed this WAD, compressed (optional)')
ap.add_argument('--stock-prefix', type=Path, help="a stock dump of this camera model. When the chip already runs an SDK image its boot header record and "
                "entry hook are patched, and the packer (which pins the stock bootstrap) refuses it: the first 0x2200 bytes of the base are then taken "
                "from this file, after checking that ONLY those fields differ. Everything else of the base stays as read from the chip.")
a = ap.parse_args()

# keep in step with BOARD_WAD_FLASH_LIMIT in firmware/boards/<board>/doom_board.h
WAD_LIMIT = {'action': OVERLAY_TABLE, 'silvergear': 0x300000}[a.board]
original = a.original.read_bytes()
if a.stock_prefix:
    stock = a.stock_prefix.read_bytes()
    if len(stock) != len(original):
        sys.exit('--stock-prefix is not the same size as --original')
    # the fields spi_boot.build_image rewrites: header load address (0x20), sector count (0x28), bootstrap entry hook (Action 0x528, Silvergear 0x540)
    allowed = set(range(0x20, 0x24)) | set(range(0x28, 0x2c)) | set(range(0x540, 0x548 if a.board == 'silvergear' else 0x530))
    extra = [i for i in range(0x2200) if original[i] != stock[i] and i not in allowed]
    if extra:
        sys.exit(f'the chip\'s bootstrap differs from the stock one outside the SDK packer\'s three fields (first at {extra[0]:#x}, {len(extra)} bytes): '
                 'refusing to treat it as an SDK-patched stock bootstrap')
    original = stock[:0x2200] + original[0x2200:]
image, report = build_image(original, a.manifest, a.board)
image = bytearray(image)
end = 0x2200 + report['load_sectors'] * 512
for o in report['overlays']:
    end = max(end, o['flash_offset'] + o['bytes'])
base = (end + 4095) & ~4095
report['wad_flash_offset'] = None
if a.wad:
    raw = a.wad.read_bytes()
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    comp = c.compress(raw) + c.flush()
    blob = struct.pack('<4sIII', b'WZAD', len(comp), len(raw), zlib.crc32(raw)) + comp
    stop = (base + len(blob) + 4095) & ~4095
    if stop > WAD_LIMIT:
        sys.exit(f'WAD blob ({len(blob)} bytes) does not fit before {WAD_LIMIT:#x} (free from {base:#x})')
    image[base:base + len(blob)] = blob
    image[base + len(blob):stop] = b'\xff' * (stop - base - len(blob))
    report.update(wad_flash_offset=base, wad_raw_bytes=len(raw), wad_compressed_bytes=len(comp), wad_end=stop)
    import hashlib
    report['image_sha256'] = hashlib.sha256(image).hexdigest()
if a.output.exists():
    sys.exit(f'{a.output} exists')
a.output.write_bytes(image)
a.output.with_suffix(a.output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: report[k] for k in ('build_id', 'load_bytes', 'overlays', 'wad_flash_offset', 'wad_compressed_bytes', 'image_sha256') if k in report}, indent=2))
