#!/bin/bash
# Install a packed SPI image (tools/spi_pack.py) on the camera so DOOM starts from a cold boot.
#
#   tools/install.sh build-spi/doom.spi.bin [backup-dir]
#
# THIS OVERWRITES THE CAMERA'S FIRMWARE. It first launches the DOOM build over USB (RAM only) to get a flash driver, reads
# the whole chip into backup-dir (default: ./backups) and refuses to continue unless that backup is complete, then lets
# apxtool install the image with --expect-current-sha256 so it cannot overwrite a chip that changed in between.
# EXPECT_CHIP_SHA256 (optional) aborts before the prompt unless the chip read has exactly that hash. USB_PORT names the camera when several are attached.
# The stock firmware can be restored from the backup or from dumps/flash.bin: see docs/FLASHING.md.
set -e
here=$(cd "$(dirname "$0")" && pwd)
export PYTHONPATH="$here/python${PYTHONPATH:+:$PYTHONPATH}"
# Cold-boot images exist for the Action Vision only: the Silvergear's SPI layout has no room for the overlay scheme this build uses
# (see docs/FLASHING.md), so the Silvergear is RAM-launch only (tools/launch.sh) for now.
[ "${BOARD:-action-vision}" = action-vision ] || { echo "SPI install is Action Vision only; use tools/launch.sh for BOARD=$BOARD" >&2; exit 1; }
image=${1:?usage: install.sh IMAGE [backup-dir]}
backups=${2:-backups}
mkdir -p "$backups"
apx() { python3 -m apxtool "$@"; }

echo "launching the flash driver over USB (RAM only)..."
"$here/launch.sh" >/dev/null
sleep 3
port=${USB_PORT:-$(for d in /sys/bus/usb/devices/*; do [ "$(cat $d/idVendor 2>/dev/null)" = 1908 ] && basename $d && break; done)}   # USB_PORT=5-1 when several cameras are attached
stamp=$(date +%Y%m%d-%H%M%S)
backup="$backups/flash-before-doom-$stamp.bin"
echo "reading the current chip into $backup ..."
apx debug --port "$port" flash read "$backup"
[ "$(stat -c %s "$backup")" = 2097152 ] || { echo "backup is not 2 MiB, aborting" >&2; exit 1; }
sha=$(sha256sum "$backup" | cut -d' ' -f1)
echo "backup sha256 $sha"
[ -z "${EXPECT_CHIP_SHA256:-}" ] || [ "$sha" = "$EXPECT_CHIP_SHA256" ] || { echo "the chip is not the one you approved ($EXPECT_CHIP_SHA256), aborting, nothing written" >&2; exit 1; }
read -r -p "Install $image now? This replaces the camera firmware. Type 'install' to continue: " answer
[ "$answer" = install ] || { echo "aborted, nothing written"; exit 1; }
apx debug --port "$port" flash install "$image" --backup "$backups/flash-install-$stamp.bin" --expect-current-sha256 "$sha"
echo "done. Unplug USB and power-cycle the camera to test the cold boot."
