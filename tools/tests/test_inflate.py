#!/usr/bin/env python3
"""Host test for firmware/port/inflate.c: build it with the host compiler and compare against zlib on the trimmed WAD and
on a few awkward inputs (stored blocks, tiny, random)."""
import os, random, subprocess, sys, tempfile, zlib
from pathlib import Path
root = Path(__file__).resolve().parents[2]
harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include "inflate.h"
static const unsigned char *data; static size_t len, pos;
static int refill(void *c, const unsigned char **b, const unsigned char **e) {
    (void)c; if (pos >= len) return 1;
    size_t n = len - pos > 777 ? 777 : len - pos;      /* odd chunk size on purpose */
    *b = data + pos; *e = data + pos + n; pos += n; return 0;
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *in = malloc(len); fread(in, 1, len, f); fclose(f); data = in;
    size_t cap = (size_t)atol(argv[2]); unsigned char *out = malloc(cap);
    long n = inflate_raw(out, cap, refill, 0);
    if (n < 0) { printf("ERR %ld\n", n); return 1; }
    f = fopen(argv[3], "wb"); fwrite(out, 1, n, f); fclose(f); printf("%ld\n", n); return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d / 'h.c').write_text(harness)
    subprocess.check_call(['cc', '-O1', '-Wall', '-I', str(root / 'firmware/port'), '-o', str(d / 'h'), str(d / 'h.c'),
                           str(root / 'firmware/port/inflate.c')])
    wad = (root / 'wad/doom1_e1m1.wad').read_bytes()
    cases = {'wad': wad, 'empty': b'', 'one': b'a', 'zeros': bytes(100000), 'random': os.urandom(70000),
             'text': (b'the quick brown fox ' * 5000), 'mixed': bytes(random.Random(1).choices(range(4), k=200000))}
    for name, raw in cases.items():
        for level in (0, 1, 9):
            if name == 'wad' and level == 1: continue
            c = zlib.compressobj(level, zlib.DEFLATED, -15)
            comp = c.compress(raw) + c.flush()
            (d / 'in').write_bytes(comp)
            r = subprocess.run([str(d / 'h'), str(d / 'in'), str(len(raw) + 16), str(d / 'out')], capture_output=True, text=True)
            ok = r.returncode == 0 and (d / 'out').read_bytes() == raw
            print(f'{name:7s} level {level}: {len(raw):8d} -> {len(comp):8d}  {"OK" if ok else "FAIL " + r.stdout}')
            if not ok: sys.exit(1)
    # output too small must fail cleanly
    c = zlib.compressobj(9, zlib.DEFLATED, -15); comp = c.compress(wad[:5000]) + c.flush()
    (d / 'in').write_bytes(comp)
    r = subprocess.run([str(d / 'h'), str(d / 'in'), '100', str(d / 'out')], capture_output=True, text=True)
    print('overflow ->', r.stdout.strip()); assert r.returncode != 0
print('inflate host tests passed')
