#!/usr/bin/env python3
"""Drive the game from the host (builds with DOOM_STATS=1): hold/tap GBADoom keys over USB.

  keys.py tap START              # press and release
  keys.py hold UP --ms 1500      # keep a key down for 1.5 s
  keys.py hold UP B --ms 800     # chord
  keys.py seq "START,wait:300,A,A"   # taps with waits in ms

Keys: A B L R UP DOWN LEFT RIGHT START SELECT (A = use/enter, B = fire, L/R = strafe, START = menu).
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'python'))
from apxtool.debug import DebugClient, USBTransport  # noqa: E402
from elftools.elf.elffile import ELFFile              # noqa: E402

NAMES = ['A', 'B', 'L', 'R', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'START', 'SELECT']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['tap', 'hold', 'seq', 'release'])
    ap.add_argument('keys', nargs='*')
    ap.add_argument('--ms', type=int, default=120)
    ap.add_argument('--elf', type=Path, default=Path(__file__).resolve().parents[1] / 'firmware/build/doom.elf')
    ap.add_argument('--port')
    a = ap.parse_args()
    with open(a.elf, 'rb') as f:
        tab = ELFFile(f).get_section_by_name('.symtab')
        magic = tab.get_symbol_by_name('doom_host_keys_magic')[0]['st_value']
        keys = tab.get_symbol_by_name('doom_host_keys')[0]['st_value']
    with USBTransport(port=a.port, timeout_ms=5000) as t:
        c = DebugClient(t)

        def setkeys(mask):
            c.write(keys, struct.pack('<I', mask), width=4, raw=True)

        c.write(magic, struct.pack('<I', 0x5359454b), width=4, raw=True)
        if a.mode == 'release':
            setkeys(0)
            return
        if a.mode == 'seq':
            for item in ','.join(a.keys).split(','):
                item = item.strip()
                if item.startswith('wait:'):
                    time.sleep(int(item[5:]) / 1000)
                elif item:
                    setkeys(1 << NAMES.index(item.upper()))
                    time.sleep(a.ms / 1000)
                    setkeys(0)
                    time.sleep(a.ms / 1000)
            return
        mask = 0
        for k in a.keys:
            mask |= 1 << NAMES.index(k.upper())
        setkeys(mask)
        time.sleep(a.ms / 1000)
        setkeys(0)


if __name__ == '__main__':
    main()
