#!/usr/bin/env python3
"""Random-walk smoke test: god mode cheat, then wander and shoot for N seconds through the host key hook (DOOM_STATS builds).
Prints nothing by itself; read the camera log afterwards (apxtool debug logs) for EMPTY LUMP / I_Error lines."""
import random, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'python'))
from apxtool.debug import DebugClient, USBTransport
from elftools.elf.elffile import ELFFile
NAMES = ['A', 'B', 'L', 'R', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'START', 'SELECT']
secs = int(sys.argv[1]) if len(sys.argv) > 1 else 60
elf = Path(__file__).resolve().parents[1] / 'firmware/build/doom.elf'
with open(elf, 'rb') as f:
    tab = ELFFile(f).get_section_by_name('.symtab')
    magic = tab.get_symbol_by_name('doom_host_keys_magic')[0]['st_value']
    keys = tab.get_symbol_by_name('doom_host_keys')[0]['st_value']
with USBTransport(timeout_ms=5000) as t:
    c = DebugClient(t)
    c.write(magic, struct.pack('<I', 0x5359454b), width=4, raw=True)
    def setk(*names):
        m = 0
        for n in names: m |= 1 << NAMES.index(n)
        c.write(keys, struct.pack('<I', m), width=4, raw=True)
    def tap(*names, ms=120):
        setk(*names); time.sleep(ms / 1000); setk(); time.sleep(ms / 1000)
    # menu: START, New Game, skill
    for k in ['START', 'A', 'A', 'A']:
        tap(k, ms=200); time.sleep(0.5)
    time.sleep(3)
    for k in ['UP', 'UP', 'DOWN', 'DOWN', 'LEFT', 'LEFT', 'RIGHT', 'RIGHT']:   # god mode
        tap(k, ms=80)
    end = time.time() + secs
    while time.time() < end:
        r = random.random()
        if r < 0.5: setk('UP', 'B') if random.random() < 0.3 else setk('UP')
        elif r < 0.7: setk('LEFT')
        elif r < 0.9: setk('RIGHT')
        else: setk('A')
        time.sleep(random.uniform(0.3, 1.5))
        setk(); time.sleep(0.05)
        if random.random() < 0.15:
            tap('B', ms=300)
    setk()
