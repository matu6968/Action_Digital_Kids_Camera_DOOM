#!/bin/bash
# Talk to the camera through the forced BootROM loader (1908:3319 "BLDR"), the way back from a firmware that does not start.
# See docs/FLASHING.md, "When the camera does not start", for how to force the loader.
#
#   tools/rescue.sh jedec                                 read-only: chip ID, boot header check (start this first)
#   tools/rescue.sh dump bricked.bin                      read the whole 2 MiB chip into a new file
#   tools/rescue.sh --yes restore dumps/flash.bin --backup bricked-backup.bin
#
# Arguments are passed straight to `apxtool rom-rescue`. The payload it needs (tools/rescue/) is built on first use.
here=$(cd "$(dirname "$0")" && pwd)
export PYTHONPATH="$here/python${PYTHONPATH:+:$PYTHONPATH}"
[ -f "$here/rescue/build/rom-rescue.bin" ] || make -C "$here/rescue" >&2
export APXTOOL_ROM_RESCUE="$here/rescue/build/rom-rescue.bin"
[ $# -gt 0 ] || { sed -n 2,12p "$0"; exit 1; }
exec python3 -m apxtool rom-rescue "$@"
