#!/usr/bin/env python3
"""Shrink a GBADoom-processed single-map WAD so that it fits in the camera's RAM.

Why: the camera cannot read its WAD by pointer from flash like the Cortex-M33 consoles do. The WAD is copied into
RAM at start-up (firmware/port/w_sd_wad.c) and the region reserved for it is 1.5 MiB. The processed shareware E1M1 WAD
is 3.2 MiB, most of it graphics and sounds that this one map never touches.

How: every lump this map and the menus can reach is kept; every other lump keeps its directory entry (name and position,
so lump numbers, sprite/flat/texture ranges and name lookups behave exactly as before) but loses its data (size 0).
Directory order is never changed. Lump data is re-packed 4-byte aligned, as the engine casts lump data to typed pointers.

What is kept:
  * every lump outside the sprite, patch and flat ranges, except an explicit drop list (help/credit screens,
    intermission graphics, demos, text screen, GUS patch map)
  * sprites: those the map's things, their state chains, projectiles, drops and the player's weapons use
  * textures/patches: textures used by the map's sidedefs, their animation/switch partners, and the patches they use
  * flats: flats the map uses plus the rest of their animation group
  * sounds: sound effects of the kept monsters and a fixed set of weapon / player / world sounds

Usage:
  wad_trim.py --in seed.wad --engine ../firmware/GBADoom --out ../wad/doom1_e1m1_trim.wad [--map E1M1] [--no-sounds]
"""
import argparse
import re
import struct
import sys
from pathlib import Path

DROP = {  # whole lumps nobody on this map can show
    'HELP1', 'HELP2', 'CREDIT', 'DEMO1', 'DEMO2', 'DEMO3', 'DEMO4', 'ENDOOM', 'DMXGUS', 'DMXGUSC',
}
DROP_PREFIX = ('WI',)            # intermission graphics (the port restarts instead of showing the intermission)

# Weapons the player can reach on E1M1 (fist, pistol, shotgun, chaingun). The chainsaw, rocket launcher, plasma gun,
# BFG and super shotgun are left out; with a cheat they would simply be invisible. --all-weapons keeps them.
WEAPON_SPRITES = {'PUNG', 'PISG', 'PISF', 'SHTG', 'SHTF', 'CHGG', 'CHGF'}
ALL_WEAPON_SPRITES = WEAPON_SPRITES | {'SAWG', 'SHT2', 'MISG', 'MISF', 'PLSG', 'PLSF', 'BFGG', 'BFGF'}
FIXED_SPRITES = {'PLAY', 'PUFF', 'BLUD', 'TFOG', 'IFOG', 'BAL1', 'BEXP', 'BAR1'}

# extra thing types that can appear without being placed on the map
EXTRA_TYPES = ['MT_PLAYER', 'MT_PUFF', 'MT_BLOOD', 'MT_TFOG', 'MT_IFOG', 'MT_TELEPORTMAN', 'MT_CLIP', 'MT_SHOTGUN',
               'MT_MISC10', 'MT_MISC11', 'MT_MISC12', 'MT_TROOPSHOT']

# Player, weapon and world sounds. Monster sounds come from the kept monsters' mobjinfo entries.
FIXED_SOUNDS = [
    'pistol', 'shotgn', 'sgcock', 'punch', 'chgun', 'firsht', 'firxpl', 'itemup',
    'wpnup', 'getpow', 'oof', 'noway', 'plpain', 'pldeth', 'telept', 'swtchn', 'swtchx', 'doropn', 'dorcls', 'bdopn',
    'bdcls', 'stnmov', 'pstart', 'pstop', 'barexp', 'tink',
]


def resample(blob, target):
    """DMX sound lump: u16 format (3), u16 rate, u32 samples, then 8-bit unsigned PCM. Linear resample down to `target`."""
    fmt, rate, count = struct.unpack_from('<HHI', blob, 0)
    if fmt != 3 or rate <= target or count < 2:
        return blob
    pcm = blob[8:8 + count]
    n = max(2, int(len(pcm) * target / rate))
    out = bytearray(n)
    step = (len(pcm) - 1) / (n - 1)
    for i in range(n):
        x = i * step
        j = int(x)
        f = x - j
        a = pcm[j]
        b = pcm[min(j + 1, len(pcm) - 1)]
        out[i] = int(a + (b - a) * f + 0.5)
    return struct.pack('<HHI', 3, target, n) + bytes(out)


def read_wad(path):
    data = Path(path).read_bytes()
    magic, count, dirofs = struct.unpack_from('<4sii', data, 0)
    if magic != b'IWAD':
        sys.exit('not an IWAD')
    lumps = []
    for i in range(count):
        pos, size, raw = struct.unpack_from('<ii8s', data, dirofs + 16 * i)
        lumps.append([raw.rstrip(b'\0').decode('latin1').upper(), data[pos:pos + size] if size else b''])
    return lumps


def write_wad(path, lumps):
    out = bytearray(b'\0' * 12)
    entries = []
    for name, blob in lumps:
        if blob:
            while len(out) % 4:
                out.append(0)
            entries.append((len(out), len(blob), name))
            out += blob
        else:
            entries.append((0, 0, name))
    while len(out) % 4:
        out.append(0)
    dirofs = len(out)
    for pos, size, name in entries:
        out += struct.pack('<ii8s', pos, size, name.encode('latin1'))
    struct.pack_into('<4sii', out, 0, b'IWAD', len(lumps), dirofs)
    Path(path).write_bytes(out)
    return len(out)


# ---------------------------------------------------------------------------------------------- engine tables

def parse_engine(engine):
    src = Path(engine, 'source')
    inc = Path(engine, 'include')
    info = (src / 'info.c').read_text()
    sprnames = re.search(r'sprnames\[NUMSPRITES\+1\] = \{(.*?)\};', info, re.S).group(1)
    sprites = re.findall(r'"(\w{4})"', sprnames)
    body = re.search(r'const state_t states\[NUMSTATES\] = \{(.*?)\n\};', info, re.S).group(1)
    states = []   # (sprite name, nextstate name)
    names = {}
    for m in re.finditer(r'\{SPR_(\w+),\s*\d+,\s*-?\d+,\s*\w+,\s*(S_\w+),[^}]*\},?\s*//\s*(S_\w+)', body):
        names[m.group(3)] = len(states)
        states.append((m.group(1), m.group(2)))
    mob = re.search(r'const mobjinfo_t mobjinfo\[NUMMOBJTYPES\] = \{(.*?)\n\};', info, re.S).group(1)
    types = {}
    byednum = {}
    for blk in re.finditer(r'\{\s*//\s*(MT_\w+)\n(.*?)\n\s*\},?', mob, re.S):
        fields = {}
        for line in blk.group(2).splitlines():
            m = re.match(r'\s*([^,/]+?)\s*,\s*//\s*(\w+)', line)
            if m:
                fields[m.group(2)] = m.group(1).strip()
        types[blk.group(1)] = fields
        try:
            ednum = int(fields.get('doomednum', '-1'))
        except ValueError:
            ednum = -1
        if ednum > 0:
            byednum[ednum] = blk.group(1)
    sounds_h = (inc / 'sounds.h').read_text()
    enum = re.search(r'typedef enum \{(.*?)\} sfxenum_t', sounds_h, re.S).group(1)
    enum_names = re.findall(r'\bsfx_(\w+)', enum)
    sounds_c = (src / 'sounds.c').read_text()
    table = re.search(r'S_sfx\[\] = \{(.*?)\n\};', sounds_c, re.S).group(1)
    table_names = re.findall(r'\{\s*"(\w+)"', table)
    return sprites, states, names, types, byednum, enum_names, table_names


def state_chain(first, states, names, seen):
    """Sprite names reachable from state name `first` following nextstate."""
    out = set()
    cur = names.get(first)
    while cur is not None and cur != 0 and cur not in seen:
        seen.add(cur)
        sprite, nxt = states[cur]
        out.add(sprite)
        cur = names.get(nxt)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--in', dest='src', required=True)
    ap.add_argument('--engine', required=True, help='firmware/GBADoom directory (info.c, sounds.c, p_spec.c ...)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--map', default='E1M1')
    ap.add_argument('--no-sounds', action='store_true', help='drop every DS* lump')
    ap.add_argument('--keep-all-sprites', action='store_true')
    ap.add_argument('--all-weapons', action='store_true', help='keep the weapon sprites E1M1 cannot reach')
    ap.add_argument('--gamma-levels', type=int, default=2, help='gamma palettes to keep (PLAYPAL, PLAYPAL1..); the port clamps to this')
    ap.add_argument('--sound-rate', type=int, default=8000, help='resample kept sounds down to this rate (0 = leave alone)')
    ap.add_argument('--report', action='store_true')
    a = ap.parse_args()

    lumps = read_wad(a.src)
    index = {}
    for i, (n, _) in enumerate(lumps):
        index.setdefault(n, i)
    sprites, states, state_names, types, byednum, enum_names, table_names = parse_engine(a.engine)

    def lump(name):
        return lumps[index[name]][1] if name in index else b''

    # ---- map lumps
    mi = index[a.map]
    mapl = {lumps[mi + k][0]: lumps[mi + k][1] for k in range(1, 11)}
    things = mapl['THINGS']
    ednums = {struct.unpack_from('<hhHhH', things, i)[3] for i in range(0, len(things), 10)}
    wanted_types = {byednum[e] for e in ednums if e in byednum}
    missing = sorted(e for e in ednums if e not in byednum)
    wanted_types |= {t for t in EXTRA_TYPES if t in types}

    keep_sprites = set(ALL_WEAPON_SPRITES if a.all_weapons else WEAPON_SPRITES) | FIXED_SPRITES
    keep_sounds = set(FIXED_SOUNDS)
    for t in sorted(wanted_types):
        f = types[t]
        seen = set()
        for key in ('spawnstate', 'seestate', 'painstate', 'meleestate', 'missilestate', 'deathstate', 'xdeathstate',
                    'raisestate'):
            keep_sprites |= state_chain(f.get(key, 'S_NULL'), states, state_names, seen)
        for key in ('seesound', 'attacksound', 'painsound', 'deathsound', 'activesound'):
            s = f.get(key, 'sfx_None')
            if s.startswith('sfx_') and s != 'sfx_None':
                idx = enum_names.index(s[4:]) if s[4:] in enum_names else -1
                if idx >= 0:
                    keep_sounds.add(table_names[idx])   # sfx_None and the table's "none" entry share index 0
    keep_sprites = {s for s in keep_sprites if s}

    # ---- textures
    tex1 = lump('TEXTURE1')
    pnames_l = lump('PNAMES')
    npn = struct.unpack_from('<i', pnames_l, 0)[0]
    patch_names = [pnames_l[4 + 8 * i:12 + 8 * i].rstrip(b'\0').decode('latin1').upper() for i in range(npn)]
    ntex = struct.unpack_from('<i', tex1, 0)[0]
    tex_name, tex_patches = [], []
    for i in range(ntex):
        off = struct.unpack_from('<i', tex1, 4 + 4 * i)[0]
        name = tex1[off:off + 8].rstrip(b'\0').decode('latin1').upper()
        pc = struct.unpack_from('<h', tex1, off + 20)[0]
        plist = [struct.unpack_from('<hhhhh', tex1, off + 22 + 10 * k)[2] for k in range(pc)]
        tex_name.append(name)
        tex_patches.append([patch_names[p] for p in plist])
    tex_index = {n: i for i, n in enumerate(tex_name)}

    side = mapl['SIDEDEFS']
    used_tex = set()
    for i in range(0, len(side), 12):
        _, _, top, bot, mid, _ = struct.unpack_from('<hhhhhh', side, i)
        for t in (top, bot, mid):
            if 0 <= t < ntex:
                used_tex.add(t)
    sky = {'E1': 'SKY1', 'E2': 'SKY2', 'E3': 'SKY3', 'E4': 'SKY4'}.get(a.map[:2], 'SKY1')
    if sky in tex_index:
        used_tex.add(tex_index[sky])

    src_spec = Path(a.engine, 'source', 'p_spec.c').read_text()
    anims = re.findall(r'\{(true|false),\s*"(\w+)",\s*"(\w+)"', src_spec)   # (is texture, last, first)
    src_sw = Path(a.engine, 'source', 'p_switch.c').read_text()
    switches = re.findall(r'\{"(\w+)",\s*"(\w+)",\s*\d\}', src_sw)

    tex_seen = {tex_name[t] for t in used_tex}
    for istex, last, first in anims:
        if istex == 'true' and first in tex_index and last in tex_index:
            lo, hi = sorted((tex_index[first], tex_index[last]))
            if any(t in used_tex for t in range(lo, hi + 1)):
                used_tex |= set(range(lo, hi + 1))
    for n1, n2 in switches:
        for x, y in ((n1, n2), (n2, n1)):
            if x in tex_index and y in tex_index and tex_index[x] in used_tex:
                used_tex.add(tex_index[y])
    keep_patches = {p for t in used_tex for p in tex_patches[t]}

    # ---- flats
    f_start = index['F1_START'] if 'F1_START' in index else index['F_START']
    f_end = index['F1_END'] if 'F1_END' in index else index['F_END']
    flat_pos = {lumps[i][0]: i for i in range(f_start + 1, f_end)}
    sect = mapl['SECTORS']
    used_flats = set()
    for i in range(0, len(sect), 26):
        fl = sect[i + 4:i + 12].rstrip(b'\0').decode('latin1').upper()
        ce = sect[i + 12:i + 20].rstrip(b'\0').decode('latin1').upper()
        used_flats |= {fl, ce}
    used_flats.add('F_SKY1')
    for istex, last, first in anims:
        if istex == 'false' and first in flat_pos and last in flat_pos:
            lo, hi = sorted((flat_pos[first], flat_pos[last]))
            if any(flat_pos[f] in range(lo, hi + 1) for f in used_flats if f in flat_pos):
                used_flats |= {lumps[i][0] for i in range(lo, hi + 1)}

    # ---- decide per lump
    s_start, s_end = index['S_START'], index['S_END']
    p_start = index.get('P1_START', index.get('P_START'))
    p_end = index.get('P1_END', index.get('P_END'))
    kept = dropped = 0
    out = []
    saved = {'sprites': 0, 'patches': 0, 'flats': 0, 'sounds': 0, 'other': 0}
    for i, (name, blob) in enumerate(lumps):
        keep = True
        cat = 'other'
        if name in DROP or name.startswith(DROP_PREFIX):
            keep = False
        elif s_start < i < s_end:
            cat = 'sprites'
            keep = a.keep_all_sprites or name[:4] in keep_sprites
        elif p_start < i < p_end:
            cat = 'patches'
            keep = name in keep_patches
        elif f_start < i < f_end:
            cat = 'flats'
            keep = name in used_flats
        elif name.startswith('DS') and i > f_end:
            cat = 'sounds'
            keep = (not a.no_sounds) and name[2:].lower() in keep_sounds
            if keep and a.sound_rate:
                blob = resample(blob, a.sound_rate)
        elif re.fullmatch(r'PLAYPAL[1-5]', name):
            cat = 'other'
            keep = int(name[-1]) < a.gamma_levels
        if not keep and blob:
            saved[cat] += len(blob)
            dropped += 1
            blob = b''
        elif blob:
            kept += 1
        out.append([name, blob])

    size = write_wad(a.out, out)
    print(f'{a.src} -> {a.out}: {size} bytes ({size / 1024:.0f} KiB); kept {kept} lumps with data, emptied {dropped}')
    if missing:
        print(f'note: thing types without a mobjinfo entry (ignored): {missing}')
    if a.report:
        print('emptied bytes per group:', {k: v for k, v in saved.items() if v})
        print('map thing types:', sorted(wanted_types))
        print('sprites kept:', ' '.join(sorted(keep_sprites)))
        print('textures used:', len(used_tex), 'patches kept:', len(keep_patches), 'flats kept:', len(used_flats))
        print('sounds kept:', ' '.join(sorted(keep_sounds)))


if __name__ == '__main__':
    main()
