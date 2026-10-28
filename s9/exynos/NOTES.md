# Galaxy S9 Exynos (SM-G960F / G965F, Exynos 9810): working notes

The rest of `s9/` targets the Snapdragon S9 (SM-G9600, starqlte, SDM845). This
directory is the Exynos port, started 2026-10-28. Shared, SoC-neutral shim code
(entry `start.S`, `fb.c`, `con.c`, `fdt.c`, `lib.c`, `shim.ld`) stays in
`../shim`; everything Exynos-specific lives here.

## What differs from SDM845 (expectations, not yet observed)

| | SDM845 (done, M1-M11) | Exynos 9810 (to find out) |
|---|---|---|
| Boot | Qualcomm ABL, TWRP dd | Samsung S-Boot, no `fastboot boot`: TWRP dd or Odin |
| Cores | A55 + A75, 16K granule confirmed | A55 + Mongoose M3: **16K unknown, decides everything** |
| GIC | GICv3 | probably GIC-400 (GICv2): XNU needs a new backend |
| Display | MDSS, ABL autorefresh | DECON-F, command-mode S6E3HA8: needs HW trigger |
| Storage | Qualcomm UFS | Exynos UFS |
| GPU | Adreno 630 | Mali-G72 (M10/M11 do not apply) |

## Stage X1 (built, NOT run on a phone yet)

`exynos/x1.c`. What it does, in order:

1. Parses the DTB S-Boot passes in x0 (plain RAM reads): model, compatible,
   `androidboot.bootloader`/`hardware`, `lcdtype`, `/cpus`, memory banks,
   reserved-memory, PSCI method, GIC and DECON nodes.
2. Paints a grey screen with red/green/blue bands at the framebuffer: a
   reserved-memory region named fb/framebuffer/splash/decon if the DT has one
   big enough for 1440x2960x4, otherwise 0xcc000000. Skips painting if that
   would overlap the shim or the DTB.
3. DECON: reads HW_SW_TRIG_CONTROL (+0x70) of the lowest-addressed DECON node
   (default 0x16030000) and writes 0x1281 if it differs (what uniLoader's
   starlte board does so the command-mode panel keeps getting frames). The
   screen shows the value before and after. **First device register touched.**
4. PSCI_VERSION, then PSCI CPU_ON for every other `/cpus` entry. Each core
   reports MIDR, MPIDR, ID_AA64MMFR0/PFR0, EL, then CPU_OFF. A core that faults
   reports ESR/ELR instead.
5. Full report including **VERDICT** (16K on every core or not) and a seconds
   counter. After 90 s it calls PSCI SYSTEM_RESET. No watchdog is touched and
   there is no RAM log: the screen is the log. **If the screen stays dark but
   the phone reboots on its own about 90 s after the Samsung logo, the shim ran**
   and only the display part is wrong.

Starting values (framebuffer 0xcc000000 a8r8g8b8, DECON-F 0x16030000, trigger
0x1281) come from public Exynos 9810 bring-up work (uniLoader board-starlte,
mainline exynos9810-starlte.dts), not from this phone.

Verified so far (2026-10-28, no phone): builds with Ubuntu 24.04 WSL
`gcc-aarch64-linux-gnu` 13.3 with no warnings and no absolute relocations
(Image 90112 bytes). Ran under QEMU 8.2 `virt` with 8 CPUs: DT parse, the hvc
PSCI conduit, CPU_ON of all 7 secondaries, both verdicts (cortex-a55 -> "XNU can
run", cortex-a53 -> "CANNOT run") and both GIC readouts (gic-version 2 and 3)
rendered correctly in a framebuffer dump. `repack-boot.py` passed synthetic
v0 raw Image, v0 gzip + appended DTBs, Samsung `dt_size` and v1 recovery_dtbo
images. SDM845 stages m1/m2/m4 still compile with the extended `fdt.c`.

## Procedure

Needs: bootloader unlocked (OEM unlock), TWRP for starlte/star2lte installed in
recovery, adb on the host, `gcc-aarch64-linux-gnu` and python3 (WSL Ubuntu works).

1. Phone in TWRP (Vol Up + Bixby + Power). Back up the stock boot partition
   (only once; it is never overwritten):
   `exynos/twrp-boot.sh backup` -> `backup/exynos-boot-stock.img` (git-ignored).
   The script stops unless `ro.product.device` is starlte or star2lte.
2. `exynos/build.sh x1` -> `out/exynos/boot.img` = the stock boot.img with only
   the kernel replaced (header, ramdisk, dt and signature tail kept as they are).
3. `exynos/twrp-boot.sh flash` writes it, reads it back, reboots only if the
   sha256 matches.
4. Photograph the screen. To get back to TWRP: hold Vol Up + Bixby + Power while
   it reboots (after 90 s, or force it with Vol Down + Power).
5. `exynos/twrp-boot.sh restore` puts stock boot back whenever needed.

Odin alternative: `out/exynos/boot.tar.md5` in the AP slot. On SDM845 a Heimdall
retry once wrote a shifted image (see `../NOTES.md`): prefer TWRP dd with
readback.

## Next after X1

- 16K NO on the M3 cores: XNU cannot use them. Options: run XNU on the A55
  cluster only (4 cores; the M3s stay off), or stop.
- 16K YES everywhere: X2 = GIC bring-up (GICv2 distributor + CPU interface if
  that is what PFR0/DT say), timer IRQ, SGI between cores, read-only UFS probe.
- Dark screen but timed reboot: compare the DECON nodes / trigger value on the
  photo-less run by rebuilding with a different trigger, or find the framebuffer
  in the stock DT (`reserved-memory`) from TWRP: `/sys/firmware/fdt`.

## Handoff

- 2026-10-28: X1 written and checked in QEMU only.
- 2026-10-28 (phone): SM-G960F starlte, bootloader G960FXXUHFVG4, TWRP 3.7.0_9-0.
  TWRP names partitions in upper case: BOOT = sda10 (57671680 bytes), RECOVERY
  sda11, DTB sda8, DTBO sda9, USERDATA sda25. Stock boot backed up:
  `backup/exynos-boot-stock.img`, sha256 665c0087...8c4f. Its header: page 2048,
  raw (uncompressed) Image 30961672 bytes, kernel_addr 0x10008000, ramdisk 901991,
  no second, **Samsung dt_size = 1820672 in the header_version slot** (the DT
  lives inside boot.img even though a DTB partition exists), cmdline
  `androidboot.selinux=permissive`. X1 boot.img sha256 793e44d2...2591 flashed
  with TWRP dd, readback matched, phone rebooted into it. **Result: waiting for
  the user's photo / observation.**
