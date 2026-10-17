#!/usr/bin/env python3
"""Structural check of a trimmed WAD: header/directory, 4-byte alignment, required lumps present, and everything E1M1's sidedefs
and sectors name still has data (patches of every used texture, flats)."""
import struct, sys
from pathlib import Path
path = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'wad/doom1_e1m1.wad')
d = path.read_bytes()
magic, n, ofs = struct.unpack_from('<4sii', d, 0)
assert magic == b'IWAD' and ofs % 4 == 0 and ofs + 16 * n == len(d), 'header/directory'
L = []
for i in range(n):
    pos, size, nm = struct.unpack_from('<ii8s', d, ofs + 16 * i)
    nm = nm.rstrip(b'\0').decode('latin1')
    if size:
        assert pos % 4 == 0 and pos + size <= ofs, f'{nm}: bad position'
    L.append((nm, d[pos:pos + size] if size else b''))
idx = {}
for i, (nm, _) in enumerate(L): idx.setdefault(nm, i)
need = ['PLAYPAL', 'PLAYPAL1', 'COLORMAP', 'TEXTURE1', 'PNAMES', 'TITLEPIC', 'STBAR', 'M_DOOM', 'M_NGAME', 'M_SKILL', 'STFST00',
        'E1M1', 'THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS', 'SSECTORS', 'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP',
        'S_START', 'S_END', 'F_START', 'F_END', 'P1_START', 'P1_END', 'F_SKY1', 'SKY1']
bad = []
for nm in need:
    if nm not in idx: bad.append(f'missing {nm}')
    elif nm.endswith(('_START', '_END', 'E1M1')) is False and not L[idx[nm]][1] and nm != 'F_SKY1': bad.append(f'empty {nm}')
m = idx['E1M1']
mapl = {L[m + k][0]: L[m + k][1] for k in range(1, 11)}
pn = L[idx['PNAMES']][1]
names = [pn[4 + 8 * i:12 + 8 * i].rstrip(b'\0').decode('latin1').upper() for i in range(struct.unpack_from('<i', pn)[0])]
t1 = L[idx['TEXTURE1']][1]
used = set()
side = mapl['SIDEDEFS']
for i in range(0, len(side), 12):
    used |= set(struct.unpack_from('<hhh', side, i + 4))
used = {t for t in used if t >= 0}
for t in sorted(used):
    off = struct.unpack_from('<i', t1, 4 + 4 * t)[0]
    tn = t1[off:off + 8].rstrip(b'\0').decode()
    pc = struct.unpack_from('<h', t1, off + 20)[0]
    for k in range(pc):
        p = names[struct.unpack_from('<hhhhh', t1, off + 22 + 10 * k)[2]]
        if not L[idx[p]][1]: bad.append(f'texture {tn}: patch {p} is empty')
sect = mapl['SECTORS']
for i in range(0, len(sect), 26):
    for nm in (sect[i + 4:i + 12], sect[i + 12:i + 20]):
        f = nm.rstrip(b'\0').decode().upper()
        if f != 'F_SKY1' and not L[idx[f]][1]: bad.append(f'flat {f} is empty')
print(f'{path.name}: {n} lumps, {sum(1 for _, b in L if b)} with data, {len(d)} bytes; {len(used)} textures used on the map')
if bad:
    print('\n'.join(sorted(set(bad)))); sys.exit(1)
print('WAD structure OK')
