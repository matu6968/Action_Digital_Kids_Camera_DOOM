#!/bin/bash
# Run a firmware build from RAM over USB. Nothing is written to flash; power-cycle the camera to get the stock/SPI firmware back.
#
#   tools/launch.sh [manifest.json] [apxtool sdk launch options]
#   tools/launch.sh --reclaim-jpeg-ram            from stock firmware (see below)
#   BOARD=silvergear-3222882 tools/launch.sh      the Silvergear build (firmware/build/silvergear-3222882/doom.manifest.json)
#
# Default manifest: firmware/build/doom.manifest.json (Action Vision), or firmware/build/$BOARD/doom.manifest.json.
# Works from stock firmware USB mode, from a running SDK image, or from the BootROM loader. Extra arguments go to
# `apxtool sdk launch` (from stock firmware the engine's code bank 0x022e0000 holds vendor data, so the launch needs --reclaim-jpeg-ram:
# it saves that RAM to jpeg-ram-before-*.bin next to the manifest first; further options e.g. --reclaim-low-ram, or --stock-backup/--stage for the Silvergear's forced-ROM start).
set -e
here=$(cd "$(dirname "$0")" && pwd)
export PYTHONPATH="$here/python${PYTHONPATH:+:$PYTHONPATH}"
board=${BOARD:-action-vision}
case "$board" in
  action-vision) default_manifest=$here/../firmware/build/doom.manifest.json;;
  *) default_manifest=$here/../firmware/build/$board/doom.manifest.json;;
esac
# the manifest is optional: a first argument starting with '-' is already an apxtool option
case "${1:-}" in
  ""|-*) manifest=$default_manifest;;
  *) manifest=$1; shift;;
esac
apx() { python3 -m apxtool "$@"; }

# find the camera: 1908:3319 is both the BootROM loader (product "Video050Loader"/BLDR) and the SDK debug image.
# With several cameras attached the script does not guess (an image for one board must never go to the other): set USB_PORT=5-1.
port=""; kind=""; found=""
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  [ "$(cat "$d/idVendor")" = 1908 ] || continue
  name=$(basename "$d")
  found="$found $name($(cat "$d/product" 2>/dev/null || echo '?'))"
  [ -z "${USB_PORT:-}" ] || [ "$name" = "$USB_PORT" ] || continue
  [ -z "$port" ] || { echo "several cameras on USB:$found - set USB_PORT=<port> (e.g. USB_PORT=5-1)" >&2; exit 1; }
  port=$name; prod=$(cat "$d/product" 2>/dev/null || true)
  case "$prod" in *SDK*) kind=sdk;; *BLDR*|*oader*) kind=loader;; *) kind=stock;; esac
done
[ -n "$port" ] || { echo "no 1908:xxxx camera found on USB${USB_PORT:+ at port $USB_PORT}" >&2; exit 1; }
echo "camera at USB port $port ($kind)"

if [ "$kind" = sdk ]; then
  # a running SDK image names its board: it must be the board of the manifest
  want=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('board',''))" "$manifest")
  have=$(apx debug --port "$port" info | python3 -c "import json,sys; print(json.load(sys.stdin).get('board',''))")
  case "$have" in "$want"*) ;; *) echo "the camera at $port runs a '$have' image but the manifest is for '$want'" >&2; exit 1;; esac
  # back to the BootROM loader, then launch immediately: the loader gives up after ~3 s
  dev=$(apx debug --port "$port" bootrom | sed -n 's/.*ready at \(\/dev\/sg[0-9]*\).*/\1/p')
else
  dev=$(ls /dev/sg* 2>/dev/null | head -1)
fi
[ -n "$dev" ] || { echo "no SCSI generic device for the camera (stock firmware must be in USB mass-storage mode)" >&2; exit 1; }
apx --device "$dev" sdk launch "$manifest" "$@"
