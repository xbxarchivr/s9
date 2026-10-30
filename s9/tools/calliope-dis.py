"""Disassemble Calliope SRAM firmware: vector table, then the code at given
addresses. python -I fwdis.py SRAM.bin [ADDR[:COUNT][t] ...]  (t = Thumb)"""
import struct, sys
import capstone

b = open(sys.argv[1], 'rb').read()
names = ['reset', 'undef', 'svc', 'pabort', 'dabort', 'reserved', 'irq', 'fiq']
print('vectors:')
for i, n in enumerate(names):
    ins, = struct.unpack_from('<I', b, 4 * i)
    tgt, = struct.unpack_from('<I', b, 0x20 + 4 * i)
    print(f'  {n:8} {ins:#010x} -> {tgt:#010x}')

arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
for arg in sys.argv[2:]:
    t = arg.endswith('t')
    a = arg.rstrip('t').split(':')
    addr, count = int(a[0], 0) & ~1, int(a[1]) if len(a) > 1 else 40
    md = thumb if t else arm
    print(f'--- {addr:#x} ({"thumb" if t else "arm"})')
    for n, ins in enumerate(md.disasm(b[addr:addr + 4 * count], addr)):
        if n >= count:
            break
        print(f'  {ins.address:#06x}: {ins.mnemonic:8} {ins.op_str}')
