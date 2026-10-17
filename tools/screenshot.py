#!/usr/bin/env python3
"""Save what the camera is showing as a PNG, read from the firmware's NV21 frame planes over USB.

  screenshot.py out.png [--elf firmware/build/doom.elf] [--port 7-1]

Needs the ELF of the running build for the plane addresses (the symbols plane_y / plane_vu / cur in port/plat.c).
The plane set that was sent last is saved; the one being drawn is skipped.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'python'))
from apxtool.debug import DebugClient, USBTransport  # noqa: E402
from elftools.elf.elffile import ELFFile              # noqa: E402
from PIL import Image                                  # noqa: E402

PITCH, W, H = 224, 220, 176


def symbols(elf_path, names):
    with open(elf_path, 'rb') as f:
        tab = ELFFile(f).get_section_by_name('.symtab')
        out = {n: tab.get_symbol_by_name(n)[0]['st_value'] for n in names}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('png', type=Path)
    ap.add_argument('--elf', type=Path, default=Path(__file__).resolve().parents[1] / 'firmware/build/doom.elf')
    ap.add_argument('--port')
    a = ap.parse_args()
    sym = symbols(a.elf, ['plane_y', 'plane_vu', 'cur'])
    with USBTransport(port=a.port, timeout_ms=5000) as t:
        c = DebugClient(t)
        cur = struct.unpack('<I', c.read(sym['cur'], 4, width=4))[0] & 1
        shown = cur ^ 1   # the set presented last
        y = c.read(sym['plane_y'] + shown * PITCH * H, PITCH * H)
        vu = c.read(sym['plane_vu'] + shown * PITCH * H // 2, PITCH * H // 2)
    img = Image.new('RGB', (W, H))
    px = img.load()
    for row in range(H):
        for x in range(W):
            Y = y[row * PITCH + x]
            o = (row // 2) * PITCH + (x & ~1)
            V, U = vu[o] - 128, vu[o + 1] - 128
            r = Y + 1.402 * V
            g = Y - 0.344136 * U - 0.714136 * V
            b = Y + 1.772 * U
            px[x, row] = tuple(max(0, min(255, int(v))) for v in (r, g, b))
    img.resize((W * 3, H * 3), Image.NEAREST).save(a.png)
    print(f'saved {a.png} (plane set {shown})')


if __name__ == '__main__':
    main()
