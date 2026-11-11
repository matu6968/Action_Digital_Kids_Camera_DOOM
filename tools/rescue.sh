#!/bin/bash
# Talk to the camera through the forced BootROM loader (1908:3319 "BLDR"), the way back from a firmware that does not start.
# See docs/FLASHING.md, "When the camera does not start", for how to force the loader.
#
#   tools/rescue.sh jedec                                 read-only: chip ID, boot header check (start this first)
#   tools/rescue.sh dump bricked.bin                      read the whole chip into a new file
#   tools/rescue.sh --yes restore dumps/flash.bin --backup bricked-backup.bin
#
# Arguments are passed straight to `apxtool rom-rescue`. The payload it needs (tools/flasher/, the SDK's SPI flasher) is built on first use.
# Works for both cameras (the flash size, 2 MiB or 4 MiB, is detected). For the Silvergear see docs/FLASHING.md.
here=$(cd "$(dirname "$0")" && pwd)
export PYTHONPATH="$here/python${PYTHONPATH:+:$PYTHONPATH}"
[ -f "$here/flasher/build/flasher.bin" ] || make -C "$here/flasher" >&2
export APXTOOL_FLASHER="$here/flasher/build/flasher.bin"
[ $# -gt 0 ] || { sed -n 2,12p "$0"; exit 1; }
exec python3 -m apxtool rom-rescue "$@"
