#!/usr/bin/env python3
"""Send a WAD into the running firmware's RAM over USB (development shortcut, no SD card needed).

The firmware keeps its WAD in 0x02000000..0x0217ffff and checks the last 16 bytes of that region for a footer
(magic "DWAD", length, ~length) before it asks the SD card. This writes the WAD and then the footer, so the firmware only
ever sees a complete WAD. RAM survives a firmware relaunch, so send the WAD once per camera power-on.

  load_wad.py ../wad/doom1_e1m1.wad            # camera port is detected
  load_wad.py ../wad/doom1_e1m1.wad --port 7-1
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'python'))
from apxtool.debug import DebugClient, USBTransport  # noqa: E402

WAD_BASE = 0x02000000
WAD_END = 0x02180000
FOOTER = 16


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('wad', type=Path, nargs='?')
    ap.add_argument('--clear', action='store_true', help='invalidate the footer (the firmware then uses the SD card)')
    ap.add_argument('--port', help='physical USB port such as 7-1 (default: the only SDK camera)')
    a = ap.parse_args()
    if a.clear:
        with USBTransport(port=a.port, vid=0x1908, pid=0x3319, timeout_ms=5000) as transport:
            DebugClient(transport).write(WAD_END - FOOTER, b'\0' * FOOTER, width=4)
        print('footer cleared')
        return
    if a.wad is None:
        ap.error('wad file required')
    data = a.wad.read_bytes()
    if data[:4] != b'IWAD':
        sys.exit('not an IWAD')
    if len(data) > WAD_END - WAD_BASE - FOOTER:
        sys.exit(f'WAD is {len(data)} bytes, the buffer holds {WAD_END - WAD_BASE - FOOTER}')
    data += b'\0' * (-len(data) % 4)
    with USBTransport(port=a.port, vid=0x1908, pid=0x3319, timeout_ms=5000) as transport:
        client = DebugClient(transport)
        info = client.info()
        print(f"camera port {transport.port}, SDK build {info.get('build_id')}")
        # invalidate any old footer first
        client.write(WAD_END - FOOTER, b'\0' * FOOTER, width=4)
        t0 = time.monotonic()
        step = 4096 * 32
        for off in range(0, len(data), step):
            client.write(WAD_BASE + off, data[off:off + step], width=4)
            print(f'\r{min(off + step, len(data)) * 100 // len(data):3d}%', end='', flush=True)
        dt = time.monotonic() - t0
        print(f'\r{len(data)} bytes in {dt:.1f} s ({len(data) / dt / 1024:.0f} KiB/s)')
        back = client.read(WAD_BASE, 64, width=4)
        if back != data[:64]:
            sys.exit('read-back mismatch at the start of the WAD')
        client.write(WAD_END - FOOTER, struct.pack('<4sIII', b'DWAD', len(data), ~len(data) & 0xffffffff, 0), width=4)
        print('footer written; the firmware will pick the WAD up')


if __name__ == '__main__':
    main()
