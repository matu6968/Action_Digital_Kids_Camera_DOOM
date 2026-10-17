#!/usr/bin/env python3
"""Build the 2 MiB SPI image for cold boot: the SDK packer's image (BootROM load block + overlay) plus, optionally, the WAD
stored compressed in the free flash right after it, so the camera needs no SD card.

  spi_pack.py --original dumps/flash.bin --manifest firmware/build/doom.manifest.json \
              --wad wad/doom1_e1m1.wad --output build-spi/doom.spi.bin

Nothing is written to a camera. Install with tools/install.sh (backs up first, asks before writing).
The WAD record sits at apx_spi_image_end() (what firmware/port/w_sd_wad.c looks for): "WZAD", compressed length, raw length,
CRC-32 of the raw WAD, then a raw deflate stream.
"""
import argparse, json, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'python'))
from apxtool.spi_boot import build_image, OVERLAY_TABLE  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--original', type=Path, required=True, help='stock flash dump (2 MiB), the base the image is built on')
ap.add_argument('--manifest', type=Path, required=True)
ap.add_argument('--output', type=Path, required=True)
ap.add_argument('--wad', type=Path, help='embed this WAD, compressed (optional)')
a = ap.parse_args()

image, report = build_image(a.original.read_bytes(), a.manifest)
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
    if stop > OVERLAY_TABLE:
        sys.exit(f'WAD blob ({len(blob)} bytes) does not fit before the overlay table at {OVERLAY_TABLE:#x} (free from {base:#x})')
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
