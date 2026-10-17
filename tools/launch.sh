#!/bin/bash
# Run a firmware build from RAM over USB. Nothing is written to flash; power-cycle the camera to get the stock/SPI firmware back.
#
#   tools/launch.sh [manifest.json]       default: firmware/build/doom.manifest.json
#
# Works from stock firmware USB mode, from a running SDK image, or from the BootROM loader.
set -e
here=$(cd "$(dirname "$0")" && pwd)
export PYTHONPATH="$here/python${PYTHONPATH:+:$PYTHONPATH}"
manifest=${1:-$here/../firmware/build/doom.manifest.json}
apx() { python3 -m apxtool "$@"; }

# find the camera: 1908:3319 is both the BootROM loader (product "Video050Loader"/BLDR) and the SDK debug image
port=""; kind=""
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  [ "$(cat "$d/idVendor")" = 1908 ] || continue
  port=$(basename "$d"); prod=$(cat "$d/product" 2>/dev/null || true)
  case "$prod" in *SDK*) kind=sdk;; *BLDR*|*oader*) kind=loader;; *) kind=stock;; esac
  break
done
[ -n "$port" ] || { echo "no 1908:xxxx camera found on USB" >&2; exit 1; }
echo "camera at USB port $port ($kind)"

if [ "$kind" = sdk ]; then
  # back to the BootROM loader, then launch immediately: the loader gives up after ~3 s
  dev=$(apx debug --port "$port" bootrom | sed -n 's/.*ready at \(\/dev\/sg[0-9]*\).*/\1/p')
else
  dev=$(ls /dev/sg* 2>/dev/null | head -1)
fi
[ -n "$dev" ] || { echo "no SCSI generic device for the camera (stock firmware must be in USB mass-storage mode)" >&2; exit 1; }
apx --device "$dev" sdk launch "$manifest" "${@:2}"
