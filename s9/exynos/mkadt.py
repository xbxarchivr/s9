#!/usr/bin/env python3
"""Apple Device Tree for the SWEET xnu on the Galaxy S9 Exynos (SM-G960F,
Exynos 9810). Usage:  exynos/mkadt.py OUT.adt [--dump] [--scale2]

Starts from the SDM845 tree (tools/mkadt.py: /chosen, identity, buttons,
product, panic log...) and replaces only what is hardware. Every value below
was read on the G960F by the shim stages X1-X3 (exynos/NOTES.md):

  RAM      span 0x92000000-0xf0000000 with holes (kernel patch 0104 reads
           /chosen/sweet-ram-holes). Live banks (TWRP /proc/device-tree):
           0x80000000+0x3c800000, 0xc0000000+0x20000000, 0xe1900000+0x1e700000,
           0x880000000+0x80000000. Below the span: sec_debug_magic, the shim
           (~0x800a0000), the DTB (0x8a000000), zswap_debug 0x91300000; above:
           modem_if from 0xf0000000. --small: the old 472 MB window, no holes.
           The panic log is carved off the top of the span.
  CPUs     the four Cortex-A55 only (MIDR 0x410FD051, 16K yes). reg = real
           MPIDR affinity 0x000/0x100/0x200/0x300 (MT set, core in Aff1), as
           on the SDM845 tree; psci-id = the /cpus reg PSCI 0.1 CPU_ON wants
           (0..3). The Mongoose M3s are left out: they do not start yet.
  GIC      GIC-400 (GICv2): GICD 0x10101000, GICC 0x10102000; virtual timer
           PPI 11 = INTID 27; counter 26 MHz.
  Display  framebuffer 0xcc000000, 1440x2960 a8r8g8b8 (the shim switches the
           DECON-F HW trigger on before jumping to XNU).
  UFS      /sweet-ufs compatible samsung,exynos9810-ufs, HCI 0x11120000+0x8000
           (patch 0106 re-inits the host as shim stage X5.6 did on the phone).
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import adt  # noqa: E402
import mkadt as base  # noqa: E402

u32, u64, cstr, node = base.u32, base.u64, base.cstr, base.node

PHYS_BASE, MEM_TOP = 0x92000000, 0xf0000000
SMALL_MEM_TOP = 0xaf800000
# [start, end) inside the span that XNU must never allocate (pmap_is_bad_ram)
RAM_HOLES = (
    (0xaf800000, 0xb1800000),   # removed-dma-pool, 32 MB
    (0xb8000000, 0xb8200000),   # removed-dma-pool, 2 MB
    (0xbc800000, 0xc0080000),   # end of bank 0 .. bank 1, + seclog 512 KB
    (0xcc000000, 0xcd100000),   # framebuffer (S-Boot splash, DECON-F)
    (0xe0000000, 0xe3700000),   # end of bank 1 .. bank 2, + secure_camera 30 MB
)
PANIC_LOG_SIZE = base.PANIC_LOG_SIZE
TIMEBASE = 26000000
VTIMER_INTID = 27
GICD, GICD_SIZE = 0x10101000, 0x1000
GICC, GICC_SIZE = 0x10102000, 0x2000
FB, FB_SIZE = 0xcc000000, 0x1100000
UFS_HCI, UFS_HCI_SIZE = 0x11120000, 0x8000
S9_ECID = 0x5357454554009810    # 'SWEET' + 9810, synthetic (the sweet uses ...0001)
A55_MPIDR = (0x000, 0x100, 0x200, 0x300)


def find(n, name):
    for c in n.children:
        if c.name == name:
            return c
    raise KeyError(name)


def set_prop(n, name, value):
    for i, (k, f, _) in enumerate(n.props):
        if k == name:
            n.props[i] = (k, f, value)
            return
    n.props.append((name, 0, value))


def build(scale=3, small=False):
    root = base.build(scale=scale)
    soc = base.SOC_BASE

    top = SMALL_MEM_TOP if small else MEM_TOP
    mem_size = top - PHYS_BASE - PANIC_LOG_SIZE
    chosen = find(root, 'chosen')
    set_prop(chosen, 'dram-base', u64(PHYS_BASE))
    set_prop(chosen, 'dram-size', u64(mem_size))
    if not small:
        for a, b in RAM_HOLES:
            assert PHYS_BASE <= a < b <= PHYS_BASE + mem_size
        set_prop(chosen, 'sweet-ram-holes', b''.join(u64(a, b) for a, b in RAM_HOLES))
    set_prop(find(root, 'pram'), 'reg', u64(PHYS_BASE + mem_size) + u64(PANIC_LOG_SIZE))
    # Own synthetic ECID -> own IOPlatformUUID. MobileGestalt's disk cache is
    # keyed by build + UUID only: a new UUID makes it recompute (boots before
    # the D22AP /product node existed cached answers without artwork traits).
    set_prop(chosen, 'unique-chip-id', struct.pack('<Q', S9_ECID))

    cpus = find(root, 'cpus')
    a55 = []
    for i, mpidr in enumerate(A55_MPIDR):
        n = cpus.children[i]
        set_prop(n, 'reg', u32(mpidr))
        set_prop(n, 'psci-id', u32(i))
        set_prop(n, 'cluster-core-id', u32(i))
        set_prop(n, 'timebase-frequency', u32(TIMEBASE))
        set_prop(n, 'peripheral-frequency', u32(TIMEBASE))
        set_prop(n, 'fixed-frequency', u32(TIMEBASE))
        set_prop(n, 'clock-frequency', u32(1794000000))
        a55.append(n)
    cpus.children = a55

    arm_io = find(root, 'arm-io')
    gic = find(arm_io, 'gic')
    set_prop(gic, 'compatible', cstr('arm,gic-400'))
    set_prop(gic, 'virtual-timer-intid', u32(VTIMER_INTID))
    set_prop(gic, 'reg', u64(GICD - soc, GICD_SIZE, GICC - soc, GICC_SIZE))

    fbcon = find(root, 'sweet-fbcon')
    set_prop(fbcon, 'reg', u64(FB, FB_SIZE))

    # bsd/dev/sweet_ufs.c (patch 0106): Exynos host re-initialised by the
    # kernel; reg covers standard HCI, vendor HCI (+0x1100) and M-PHY PMA (+0x4000).
    ufs = find(root, 'sweet-ufs')
    set_prop(ufs, 'compatible', cstr('samsung,exynos9810-ufs'))
    set_prop(ufs, 'reg', u64(UFS_HCI, UFS_HCI_SIZE))

    # SweetPlatformExpert matches 'xiaomi,sweet'; the kernel's Exynos gates
    # (Qualcomm-only drivers off) look for 'samsung,exynos9810'.
    compat = root.prop('compatible').split(b'\0')
    compat = [c for c in compat if c and c not in (b'samsung,starqltechn', b'qcom,sdm845')]
    compat[3:3] = [b'samsung,starlte', b'samsung,exynos9810']
    set_prop(root, 'compatible', b''.join(c + b'\0' for c in compat))
    return root


if __name__ == '__main__':
    root = build(scale=2 if '--scale2' in sys.argv else 3, small='--small' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
