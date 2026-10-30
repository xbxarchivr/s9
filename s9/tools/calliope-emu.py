"""Emulate Calliope (ABOX CA7 firmware) from its reset vector with unicorn and
report SP/mode when it reaches the scatter-load decompressor (0x8004fcbc).
   python -I calemu.py FWDIR [MAX_INSNS]
SRAM image at 0 and aliased at 0x17d00000 (ABOX_REMAP), DRAM firmware + the
ext_bin files at 0x80000000 (18 MB), IVA at 0x90000000; device windows are
plain zeroed RAM. Unmapped accesses are logged and mapped on demand."""
import ctypes, os, struct, sys
from unicorn import *
from unicorn.arm_const import *

fw = sys.argv[1]
MAX = int(sys.argv[2]) if len(sys.argv) > 2 else 30_000_000
rd = lambda n: open(os.path.join(fw, n), 'rb').read()

uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
try:
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A7)
except Exception as e:
    print('cpu model:', e)

SRAM_SZ = 0x40000
sram = ctypes.create_string_buffer(SRAM_SZ)
img = rd('calliope_sram.bin')
ctypes.memmove(sram, img, len(img))
uc.mem_map_ptr(0x0, SRAM_SZ, UC_PROT_ALL, ctypes.addressof(sram))
uc.mem_map_ptr(0x17d00000, SRAM_SZ, UC_PROT_ALL, ctypes.addressof(sram))

uc.mem_map(0x80000000, 0x1200000)
uc.mem_write(0x80000000, rd('calliope_dram.bin'))
for name, off in (('dsm.bin', 0x502000), ('AP_AUDIO_SLSI.bin', 0x7e0000),
                  ('APBargeIn_AUDIO_SLSI.bin', 0x7dc000), ('SoundBoosterParam.bin', 0x4fc000),
                  ('APDV_AUDIO_SLSI.bin', 0x4d0000), ('APBiBF_AUDIO_SLSI.bin', 0x7df000),
                  ('dsm_tune.bin', 0x601000)):
    uc.mem_write(0x80000000 + off, rd(name))
uc.mem_map(0x90000000, 0x100000)
uc.mem_write(0x90000000, rd('calliope_iva.bin'))
for base in (0x10000000, 0x10c00000, 0x17c00000, 0x17e00000):
    uc.mem_map(base, 0x100000)

state = {'n': 0, 'last': []}

def on_unmapped(uc, access, addr, size, value, _):
    pc = uc.reg_read(UC_ARM_REG_PC)
    kind = {UC_MEM_READ_UNMAPPED: 'read', UC_MEM_WRITE_UNMAPPED: 'write',
            UC_MEM_FETCH_UNMAPPED: 'fetch'}.get(access, access)
    print(f'unmapped {kind} {addr:#010x} at pc {pc:#010x} sp {uc.reg_read(UC_ARM_REG_SP):#x}')
    page = addr & ~0xfffff
    try:
        uc.mem_map(page, 0x100000)
        return True
    except UcError:
        return False

def on_code(uc, addr, size, _):
    state['n'] += 1
    state['last'].append(addr)
    if len(state['last']) > 40:
        state['last'].pop(0)
    if addr in (0x3c, 0x54f0, 0x2ae4, 0x2960, 0x104c) or (addr & ~1) == 0x8004fcbc:
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        print(f'@{addr:#010x} sp {uc.reg_read(UC_ARM_REG_SP):#010x} cpsr {cpsr:#x} mode {cpsr & 0x1f:#x} '
              f'r0 {uc.reg_read(UC_ARM_REG_R0):#x} r6 {uc.reg_read(UC_ARM_REG_R6):#x} n={state["n"]}')
        if (addr & ~1) == 0x8004fcbc and not os.environ.get('CAL_GO'):
            uc.emu_stop()

def on_intr(uc, intno, _):
    pc = uc.reg_read(UC_ARM_REG_PC)
    print(f'exception {intno} at pc {pc:#x}; last pcs: {[hex(a) for a in state["last"][-12:]]}')
    uc.emu_stop()

uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
uc.hook_add(UC_HOOK_CODE, on_code)
uc.hook_add(UC_HOOK_INTR, on_intr)
uc.reg_write(UC_ARM_REG_CPSR, 0x1d3)          # SVC, IRQ/FIQ masked, ARM
try:
    uc.emu_start(0x0, 0xffffffff, count=MAX)
except UcError as e:
    print('emu error:', e, 'pc', hex(uc.reg_read(UC_ARM_REG_PC)),
          'last', [hex(a) for a in state['last'][-12:]])
print('instructions', state['n'], 'pc', hex(uc.reg_read(UC_ARM_REG_PC)), 'sp', hex(uc.reg_read(UC_ARM_REG_SP)))
