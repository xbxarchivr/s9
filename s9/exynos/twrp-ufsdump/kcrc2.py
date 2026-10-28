"""CRCs of exported symbols in a KASLR (RELOCATABLE) arm64 4.9 Image.

Every pointer in __ksymtab and every CRC in __kcrctab is a R_AARCH64_RELATIVE
addend in .rela (the 4.9 arm64 quirk: absolute __crc_* symbols get relocated).
__ksymtab entry i = {value, name} at place P and P+8; __kcrctab entry i at
C + 8*i holds the CRC as addend. Pair them by index.
"""
import struct, sys

k = open(sys.argv[1], 'rb').read()
q = lambda o: struct.unpack_from('<Q', k, o)[0]
BASE = 0xffffff8008080000

# the RELA table: 24-byte records; walk out from a known RELATIVE record
o = int(sys.argv[2], 16)
while q(o - 16) in (0, 0x403, 0x101) or (q(o - 16) & 0xffffffff) in (0x403, 0x101):
    o -= 24
rel = {}
while (q(o + 8) & 0xffffffff) in (0, 0x403, 0x101) and o + 24 <= len(k):
    if q(o + 8):
        rel[q(o)] = q(o + 16)
    o += 24
print('relocs', len(rel))

def cstr(va):
    f = va - BASE
    if not 0 <= f < len(k):
        return None
    e = k.find(b'\0', f, f + 128)
    s = k[f:e]
    return s.decode() if e > f and all(32 < c < 127 for c in s) else None

# ksymtab entries: place P with P+8 relocated to a C identifier string
places = sorted(rel)
syms = {p: cstr(rel[p + 8]) for p in places if p + 8 in rel and cstr(rel[p + 8])}
# group contiguous 16-byte runs
runs, cur = [], []
for p in sorted(syms):
    if cur and p != cur[-1] + 16:
        runs.append(cur)
        cur = []
    cur.append(p)
runs.append(cur)
runs = [r for r in runs if len(r) > 50]
# kcrctab: contiguous 8-byte runs of addends < 2^32
crcs = [p for p in places if rel[p] < 1 << 32]
cruns, cur = [], []
for p in crcs:
    if cur and p != cur[-1] + 8:
        cruns.append(cur)
        cur = []
    cur.append(p)
cruns.append(cur)
cruns = [r for r in cruns if len(r) > 50]
print('ksymtab runs', [(hex(r[0]), len(r)) for r in runs])
print('kcrctab runs', [(hex(r[0]), len(r)) for r in cruns])

want = set(sys.argv[3:])
for sr in runs:
    for cr in cruns:
        if len(sr) != len(cr):
            continue
        for sp, cp in zip(sr, cr):
            n = syms[sp]
            if n in want:
                print('%-16s 0x%08x' % (n, rel[cp]))
