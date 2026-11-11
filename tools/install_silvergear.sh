#!/bin/bash
# Install DOOM on the Silvergear for cold boot (the Silvergear counterpart of tools/install.sh).
#
#   USB_PORT=5-1 tools/install_silvergear.sh [backup-dir]
#
# THIS OVERWRITES THE CAMERA'S FIRMWARE (the sectors 0x000000-0x1fdfff of the 4 MiB flash; the stock photos, the settings sector and the
# SDK's flight-recorder sectors are not touched). Needs: a Silvergear that is running ANY SDK image on USB (a DOOM launched over USB with
# tools/launch.sh is the usual one; so is the SDK's printer demo), and a build:  make -C firmware BOARD=silvergear-3222882
#
# It (1) checks that the camera is a Silvergear with the 944016 4 MiB chip, (2) reads the WHOLE chip into backup-dir twice and requires both
# reads to be identical, (3) packs the image on top of THAT read (so everything not part of DOOM stays exactly as it is), (4) shows the
# hashes and waits for you to type 'install', (5) lets apxtool write only the sectors that differ, verify each, and write the boot header
# sector last; apxtool refuses to erase anything if the chip no longer hashes to the backup.
# EXPECT_CHIP_SHA256 / EXPECT_IMAGE_SHA256 (optional) abort before the prompt unless the chip read / the packed image have exactly those hashes.
# Recovery if the camera does not start afterwards: docs/FLASHING.md, "Silvergear: when the camera does not start".
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
export PYTHONPATH="$here/python${PYTHONPATH:+:$PYTHONPATH}"
port=${USB_PORT:?set USB_PORT to the USB port of the Silvergear, e.g. USB_PORT=5-1 (lsusb -t shows it)}
backups=${1:-$root/backups}
manifest=$root/firmware/build/silvergear-3222882/doom.manifest.json
elf=$root/firmware/build/silvergear-3222882/doom.elf
wad=${WAD:-$root/wad/doom1_e1m1.wad}
stock=${STOCK:-$root/dumps/stock-silvergear.bin}   # a stock dump of the Silvergear: the source of the bootstrap prefix (see spi_pack.py --stock-prefix)
apx() { python3 -m apxtool "$@"; }

[ -f "$manifest" ] || { echo "no build: run  make -C firmware BOARD=silvergear-3222882" >&2; exit 1; }
# the installed image must keep the SDK's flash driver (DOOM_STATS=1, the default) or it can only be re-flashed through the BootROM short
python3 - "$elf" <<'PY' || { echo "this build has no host debug hooks (DOOM_STATS=0): build with the default DOOM_STATS=1 so the camera can be re-flashed over USB" >&2; exit 1; }
import sys
from elftools.elf.elffile import ELFFile
with open(sys.argv[1], 'rb') as f:
    sys.exit(0 if ELFFile(f).get_section_by_name('.symtab').get_symbol_by_name('doom_host_keys_magic') else 1)
PY

board=$(apx debug --port "$port" info | python3 -c "import json,sys; print(json.load(sys.stdin).get('board',''))")
case "$board" in silvergear-3222882*) ;; *) echo "the camera at $port is '$board', not a Silvergear" >&2; exit 1;; esac
echo "Silvergear at USB port $port"
apx debug --port "$port" flash info | python3 -c "
import json, sys
d = json.load(sys.stdin)
print('flash chip: JEDEC %06X, %d bytes' % (d['jedec'], d['capacity']))
sys.exit(0 if d['jedec'] == 0x944016 and d['capacity'] == 4 * 1024 * 1024 else 1)
" || { echo "the flash chip is not the Silvergear's 944016 / 4 MiB" >&2; exit 1; }

mkdir -p "$backups" "$root/build-spi"
stamp=$(date +%Y%m%d-%H%M%S)
backup="$backups/silvergear-flash-before-doom-$stamp.bin"
check="$backups/silvergear-flash-before-doom-$stamp.reread.bin"
echo "reading the whole chip into $backup (about 12 s) ..."
apx debug --port "$port" flash read "$backup"
[ "$(stat -c %s "$backup")" = 4194304 ] || { echo "backup is not 4 MiB, aborting" >&2; exit 1; }
echo "reading it a second time to make sure the read is stable ..."
apx debug --port "$port" flash read "$check"
cmp "$backup" "$check" || { echo "the two reads differ, aborting (nothing written)" >&2; exit 1; }
rm -f "$check"
sha=$(sha256sum "$backup" | cut -d' ' -f1)
echo "backup sha256 $sha"
[ -z "${EXPECT_CHIP_SHA256:-}" ] || [ "$sha" = "$EXPECT_CHIP_SHA256" ] || { echo "the chip is not the one you approved ($EXPECT_CHIP_SHA256), aborting, nothing written" >&2; exit 1; }

image="$root/build-spi/doom-silvergear-$stamp.spi.bin"
[ -f "$stock" ] || { echo "need a stock dump of the Silvergear for its bootstrap: dumps/stock-silvergear.bin, or STOCK=/path/to/stock.bin" >&2; exit 1; }
python3 "$here/spi_pack.py" --board silvergear --original "$backup" --stock-prefix "$stock" --manifest "$manifest" --wad "$wad" --output "$image"
isha=$(sha256sum "$image" | cut -d' ' -f1)
changed=$(python3 - "$backup" "$image" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
print(sum(a[i:i + 4096] != b[i:i + 4096] for i in range(0, len(a), 4096)))
PY
)
[ -z "${EXPECT_IMAGE_SHA256:-}" ] || [ "$isha" = "$EXPECT_IMAGE_SHA256" ] || { echo "the image is not the one you approved ($EXPECT_IMAGE_SHA256), aborting, nothing written" >&2; exit 1; }
echo
echo "image        $image"
echo "image sha256 $isha"
echo "sectors that will be rewritten: $changed of 1024"
read -r -p "Install it now? This replaces the camera firmware. Type 'install' to continue: " answer
[ "$answer" = install ] || { echo "aborted, nothing written"; exit 1; }
apx debug --port "$port" flash install "$image" --backup "$backups/silvergear-flash-install-$stamp.bin" --expect-current-sha256 "$sha"
echo "done. Unplug USB, power the camera off and on to test the cold boot (hold POWER for about a second)."
echo "To go back: docs/FLASHING.md (restore $backup)."
