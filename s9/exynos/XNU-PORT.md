# XNU on the Galaxy S9 Exynos (SM-G960F): port plan

Started 2026-10-28. Facts come from the shim stages X1-X3 on the phone
(`NOTES.md` in this directory); "LineageOS" means the vendor kernel the
G960F's LineageOS builds use,
[android_kernel_samsung_universal9810](https://github.com/LineageOS/android_kernel_samsung_universal9810)
(branch `lineage-17.1`, Linux 4.9), plus the stock rev26 device tree from our
own `backup/exynos-boot-stock.img`.

## How the SDM845 port boots XNU (what we reuse)

1. Shim (`shim/loader.c`) plays iBoot: finds the `SWEETXNU` payload in the
   boot image's ramdisk (`/chosen/linux,initrd-*`), copies the kernel into the
   ADT's RAM window, puts ADT + `boot_args` behind it, jumps with the MMU off.
2. Payload = `tools/mkpayload.py KERNEL ADT OUT --cmdline ...`.
3. Kernel = xnu-8796.141.3 + `xnu/patches/0001-0101`, built by
   `.github/workflows/xnu.yml` on a macOS runner (CI on the GitHub fork).
4. ADT = `tools/mkadt.py` (identity, /chosen, cpus, arm-io/gic, fbcon...).

## Done for Exynos (this commit)

| Piece | File | Checked |
|---|---|---|
| Platform layer (DT, PSCI 0.1, display, core start) | `xplat.c/.h` | X2/X3 on the phone |
| ADT: 4x A55, GIC-400, 26 MHz, fb 0xCC000000, RAM 0x92000000-0xAF800000 | `mkadt.py` | dumped and read back |
| Boot image with the payload as ramdisk (dt section kept) | `repack-boot.py --ramdisk` | synthetic + real stock image |
| Shim stage X4 (DECON trigger, then `shim/loader.c`) | `x4.c`, `build.sh x4` | QEMU: finds initrd, refuses a non-payload |

## Kernel work (patch 0102 and on), in boot order

**Patch 0102 written** (`xnu/patches/0102-exynos9810-gicv2-psci-id-qcom-gates.patch`,
15 files): items 1-3 and 5 below, plus the early console (0082) moved off the
hard-coded SDM845 splash 0x9d400000 (inside our Exynos RAM window!) to
`boot_args->Video.v_baseAddr`, unchanged on the SDM845. Applies cleanly on
xnu-8796.141.3 + 0001-0101 (0031/0041 need CI's HFS staging, untouched by
0102). **Not compiled yet**: that needs the macOS CI run.

What breaks first is listed first. Each item names the SDM845 code it
replaces and the Exynos fact or LineageOS reference it rests on.

1. **"Is this Exynos?" switch.** One flag from the ADT root `compatible`
   (`samsung,exynos9810`, set by `mkadt.py`) that makes every Qualcomm-only
   init return early: MDSS/pingpong autorefresh (0089/0095/0096), RPMh
   RSC/TCS and CMD-DB (0090/0099), USB QSCRATCH/AHB2PHY/SMMU (0090), SPMI
   (0090/0099/0100), TLMM GPIO and GENI touch (0085/0088/0096), OSM CPU clocks
   (0089/0101). The zero-copy display path already turns itself off (it
   requires the framebuffer at the SDM845 splash address). Boot-args for the
   first boot as well: `sweettouch=0 sweetkeys=0 sweetusb=0 sweetbattery=0
   sweetcpu=0`.
2. **GICv2 instead of GICv3** (`pexpert/arm/pe_fiq.c` from 0004/0017,
   `sleh_irq` in 0004, IPIs in 0065). Selected by the ADT gic `compatible`
   (`arm,gic-400`); ADT `reg` = GICD, GICC (relative to /arm-io).
   - per CPU: banked `GICD_ITARGETSR0` byte = that CPU's interface bit (X2:
     0x01/0x02/0x04/0x08), store it per logical CPU; `GICD_IPRIORITYR` 0xA0
     for SGI 1 and INTID 27; `GICD_ISENABLER0` for both; `GICC_PMR` 0xF0,
     `GICC_BPR` 0, `GICC_CTLR |= 1`.
   - once: `GICD_CTLR |= 1` (S-Boot leaves 0).
   - `sleh_irq`: `iar = GICC_IAR` (MMIO), INTID = `iar & 0x3ff`,
     1020-1023 spurious, EOI by writing the whole `iar` to `GICC_EOIR`.
   - IPI: `GICD_SGIR = target_bits << 16 | SGI` (target = stored interface
     bits, not MPIDR).
   - no `ICC_SRE`/`ICC_*` system registers anywhere on this path
     (ID_AA64PFR0.GIC = 0 on the A55s: they would trap).
   All of it is what X2 did on the phone: 29 ticks/300 ms on every A55,
   10/10 self-SGIs, 5/5 cross-SGIs.
3. **PSCI CPU_ON target** (0065 `sweet_psci_cpu_on(cpu->phys_id, ...)`):
   use the ADT `psci-id` (0..3) when present; `reg` stays the real MPIDR
   affinity for CPU identification. PSCI 0.1 IDs (`cpu_on` 0xC4000003,
   `cpu_off` 0x84000002) match what 0065 calls. First boot `cpus=1`.
4. **Timer.** ADT timebase 26 MHz; virtual timer INTID 27 via the ADT.
   S-Boot leaves `CNTFRQ_EL0` 0 on secondaries (upstream Exynos 9810
   series): XNU must not read it there.
5. **Framebuffer.** `SweetFramebuffer.cpp` with only the plain path:
   fb 0xCC000000, 1440x2960, stride 5760, a8r8g8b8. Nothing to kick: DECON-F
   is in HW-trigger mode before XNU starts.
6. **Root filesystem.** UFS data DMA does not work yet (X3: IS.SBFES on
   every data-in transfer, even after FMPSECURITY/SMU_INIT). First XNU boots
   therefore aim at "IOKit up, waiting for root" as on the SDM845's first
   M4 boot, or use the `--ramdisk` root (`rd=md0`) inside the payload.
   Fix UFS in parallel (full host re-init as `ufs-exynos.c` + `ufs-cal-9810.c`
   do it).
7. **Later:** RAM beyond the 472 MB window (0091's carve-out-holes pmap with
   the Exynos banks: 0x80000000 968M+, 0xC0000000, 0xE1900000, 0x880000000
   2 GB); keys (Exynos GPIO/PMIC S2MPS19), touch (Samsung sec_ts on Exynos
   HSI2C/USI), USB (Exynos DWC3 + PHY), battery (MAX77705 on Exynos HSI2C),
   CPU DVFS (ACPM); the Mongoose M3 cluster (see `M3-POWER.md`). For each, the
   LineageOS driver is the reference for registers and sequences.

## First XNU boot (X4) checklist

- [x] patch 0102 written, applies on 0001-0101
- [x] CI build green: xbxarchivr/s9 run 40700590330, kernel sha256 27df345e...
- [x] `exynos/mkadt.py out/exynos/exynos.adt`
- [x] `tools/mkpayload.py kernel out/exynos/exynos.adt out/exynos/payload.bin
      --cmdline "cpus=1 sweettouch=0 sweetkeys=0 sweetusb=0 sweetbattery=0 sweetcpu=0 -v"`
- [ ] `PAYLOAD=out/exynos/payload.bin exynos/build.sh x4`, flash, photo
- Expected first stop: the loader's INITRD line, then either XNU's
  framebuffer log or a panic screen. The INITRD range must stay outside
  0x92000000-0xAF800000 (the loader refuses otherwise).
