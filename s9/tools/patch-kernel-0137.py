#!/usr/bin/env python3
"""Apply the effect of xnu patch 0137 to the built 0136 kernel, without CI
(GitHub Actions minutes ran out; XNU needs macOS to compile).

  patch-kernel-0137.py IN OUT [--dry-run]

clang folded exy_abox_boot's SysMMU table into immediate stores to pt[]
(x1 = pt, entry = pa >> 4 | 2, offset = (iova >> 20) * 4). Patched, all at
file offsets of xnu33/kernel.development.sweet, each old word checked first:
  0x4ee078  str w8,[x1,#0x2840] -> #0x2818: its value 0xfd30002 (rmem +0x2100000)
            becomes IOVA 0xa0600000, the 7th VSS MB; the code zeroes 7 MB from
            rmem +0x1b00000 already.
  nops      write IOVA 0xa1000000/0xa1100000 = rmem +0x1900000/+0x1a00000, the
            last 2 MB of the RDMA area, zeroed too. IOVA 0x91600000/0x91700000
            still point there: an alias until RDMA is really used.
  poll      `cmp wN, #100` -> #300 (10 s -> 30 s), unique match in the function.
Not done (needs a compile): post-mortem after the timeout; the log still says
"no boot done after 10 s".
"""
import struct, sys
import capstone

EDITS = [
    (0x4ee008, 0xd503201f, 0x5280004a),  # nop -> mov w10, #2
    (0x4ee014, 0xd503201f, 0x72a1f96a),  # nop -> movk w10, #0xfcb, lsl #16
    (0x4ee020, 0xd503201f, 0xb928402a),  # nop -> str w10, [x1, #0x2840]
    (0x4ee02c, 0xd503201f, 0x1140414a),  # nop -> add w10, w10, #0x10, lsl #12
    (0x4ee050, 0xd503201f, 0xb928442a),  # nop -> str w10, [x1, #0x2844]
    (0x4ee078, 0xb9284028, 0xb9281828),  # str w8, [x1, #0x2840] -> #0x2818
]
FUNC = (0x4ee000, 0x4f0640)              # inside exy_abox_boot .. abox_rm_write

src, dst = sys.argv[1], sys.argv[2]
b = bytearray(open(src, 'rb').read())
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
dis = lambda off: next(md.disasm(bytes(b[off:off + 4]), off))

for off, old, new in EDITS:
    cur, = struct.unpack_from('<I', b, off)
    assert cur == old, f'{off:#x}: {cur:#x}, expected {old:#x}'
    i0 = dis(off)
    struct.pack_into('<I', b, off, new)
    i1 = dis(off)
    print(f'{off:#x}: {i0.mnemonic} {i0.op_str} -> {i1.mnemonic} {i1.op_str}')

hits = []
for off in range(FUNC[0], FUNC[1], 4):
    w, = struct.unpack_from('<I', b, off)
    if (w & 0xfffffc1f) == (0x7100001f | (100 << 10)):      # cmp wN, #100
        hits.append(off)
print('cmp #100 in range:', [hex(h) for h in hits])
for h in hits:
    for i in md.disasm(bytes(b[h - 16:h + 12]), h - 16):
        print(f'  {i.address:#x}: {i.mnemonic} {i.op_str}')
assert len(hits) == 1, 'poll compare is not unique'
w, = struct.unpack_from('<I', b, hits[0])
struct.pack_into('<I', b, hits[0], (w & ~(0xfff << 10)) | (300 << 10))
print(f'{hits[0]:#x}: {dis(hits[0]).mnemonic} {dis(hits[0]).op_str}')

if '--dry-run' not in sys.argv:
    open(dst, 'wb').write(b)
    print('wrote', dst)
