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
  with TWRP dd, readback matched, phone rebooted into it.
- **X1 result on the G960F (photo, 2026-10-28):** the shim runs and draws.
  Observed: title "EXYNOS X1 RUNS" (the pre-CPU screen; the final "X1: OK"
  screen never appeared, and the phone did NOT reset itself at 90 s: the CPU
  probe hung for good. The user could not get it to reboot either). Model "Samsung Star rev26 board based on
  EXYNOS9810", compatible "Samsung, Star rev26", S-Boot G960FXXUHFVG4,
  hardware samsungexynos9810, lcdtype 9503811. EL1, CNTFRQ 26000000. Shim at
  0x80128000, DTB at 0x8A000000 (296K). **PSCI_VERSION = 0xFFFFFFFF
  (NOT_SUPPORTED) over smc** (DT method "smc"). Memory 0x80000000 968M+,
  0xC0000000 512M+, 0xE1900000 487M+, 0x880000000 2048M. **GIC: DT
  interrupt-controller@10100000 arm,cortex-a15-gic, PFR0.GIC = 0: GICv2
  (GIC-400).** DECON nodes decon_f@0x16030000 and decon_t@0x16050000
  (samsung,exynos9-decon). **DECON-F trigger 0x3070 -> 0x1281 (second value
  partly hidden by the crack), and our pixels reached the glass: framebuffer
  0xCC000000 a8r8g8b8 is right, colours in order.** No reserved region named
  like a framebuffer (default used). Reserved nodes are named `name@0xADDR`
  and the base/size column did not match the unit addresses: X1 read them with
  the root's cells; X1.1 uses reserved-memory's own cells.
- **X1.1** (QEMU ok; boot.img sha256 f34ced00...8f83 flashed 2026-10-28 with verified readback): PSCI
  0.1 function IDs from the DT (`cpu_on`/`cpu_off`) when present; secondaries
  enter through `xentry.S`, which picks the stack from MPIDR because PSCI 0.1
  CPU_ON passes no context id (X1 used x0 for that: garbage on 0.1, a likely
  crash); each core's row is drawn *before* its CPU_ON so a hang names the
  call; one row per core; PSCI node compatible shown.
- **X1.1 result on the G960F (photo):** ran to the end ("X1.1: OK", timer
  counting, so it would reset at 90 s). PSCI node: compatible "arm,psci"
  (0.1), method smc, cpu_on 0xC4000003, cpu_off 0x84000002. CPU0 = MPIDR 000,
  A55 (MIDR 41?FD051, partly hidden), **16K YES** (MMFR0 0x00?01122), EL1.
  /cpus MPIDRs 001 002 003 100 101 102 103. **Every CPU_ON returned 0, but no
  core reported within 1 s ("never arrived")**, so the 16K answer for the M3
  cores is still open. Reserved-memory sizes still look off (unchanged from X1).
  Guess (not verified): CPU0 runs with its D-cache on and the new cores with
  theirs off, so they never saw the CPU table CPU0 wrote (X1.1 looked its own
  entry up there) and CPU0 never saw what they wrote.
- **X1.2** (QEMU ok, boot.img sha256 9e4c89d7..., flashed 2026-10-28 with verified readback): each new
  core writes into its own 64-byte-aligned report slot picked by MPIDR (same
  formula as xentry.S, so it reads nothing CPU0 wrote), cleans it with
  `dc civac`, and paints a 40 px square in the top-right corner (green = ran,
  red = took an exception), using constants only. CPU0 invalidates the slot
  before each read. Rows show SCTLR_EL1 of every core (bit 2 = D-cache on),
  to test the guess above. The final screen does not clear, so the squares stay.
- **X1.2 result on the G960F (photo):** ran to the end. **Two green squares**,
  positions match slots 1 and 4 = core 001 (A55) and core 100 (the first
  Mongoose M3): new cores DO run our code (entry, stack, writes to the panel).
  Still every row "CPU_ON ok, never arrived" (1 s wait). **CPU0 SCTLR_EL1 =
  0x30C5183E: M=0 (MMU off), C=1, A=1 (alignment check on), I=1** -> CPU0 data
  accesses are already non-cacheable, so the cache guess was wrong (unless
  Samsung's EL2 sets HCR_EL2.DC, unknown). Guess now: cores arrive after more
  than 1 s, and CPU_OFF from an arrived core may cancel the pending start of
  its cluster-mates (only the first core of each cluster painted).
  CPU0 MIDR reads 410FD051? (photo), MMFR0 0x00101122 (16K yes).
- **X1.3** (QEMU ok, boot.img sha256 b2b8c596..., flashed 2026-10-28 with verified readback): up to 5 s
  per core, arrival time in ms on each row (L = only found in the final look),
  cores park in WFE (no CPU_OFF), every slot read again 2 s after the last
  start, SLOTS row with the raw slot contents (U up, F fault, - empty, ? other).
- **X1.3 result on the G960F (photo):** ran to the end. Still two green
  squares; measured on the photo they sit at **slots 0 and 4** (my X1.2 reading
  "1 and 4" was probably off by one). Every row "never arrived (5+2 s)", but
  **SLOTS = `U-------`: a report sits in slot 0**, CPU0's own slot, which no
  core should compute from its /cpus reg. Conclusion (inference, fits X1,
  X1.1, X1.2 and X1.3): **the cores' real MPIDR_EL1 values do not follow the
  /cpus reg values in the bits used (Aff1 * 4 + Aff0)**, so several cores
  share slots 0 and 4 and CPU0 looks in the wrong ones. Slot 4 shows '-'
  because CPU0 clears a slot just before each CPU_ON.
- **X1.4** (QEMU ok, boot.img sha256 4633f73f..., flashed 2026-10-28 with verified readback): CPU0 hands
  each core its slot (= /cpus index) through `x_next_slot`, which `xentry.S`
  reads; no MPIDR arithmetic left. Rows show the core's real MPIDR_EL1 (in
  place of SCTLR).
- **X1.4 result on the G960F (photo): the A55 cluster is answered.**
  CPU0 MPIDR 0x81000000; CPU1-3 (/cpus reg 001-003) real MPIDR 0x81000100,
  0x81000200, 0x81000300 (MT bit set, core number in Aff1: the /cpus reg values
  are logical numbers the PSCI firmware translates). All four: MIDR 0x410FD051
  (Cortex-A55 r0p1), MMFR0 0x00101122, **16K YES**, EL1, arrived in 0 ms.
  **CPU4-7 (/cpus reg 100-103, Mongoose M3): CPU_ON returns 0, never arrive
  (5 s each + 2 s).** SLOTS `-UUU----`, three squares.
  **Correction:** the squares in X1.2/X1.3 were A55 cores (0x100 -> slot 4,
  0x200 -> slot 0, 0x300 -> slot 4 under the old formula); no M3 core has run
  our code yet. The "first M3 ran" reading above is wrong.
  Guess (not verified): Samsung's EL3 accepts CPU_ON for the big cluster, but
  its power domain / clock (CPUCL1) is left off by S-Boot and only Samsung's
  kernel drivers (exynos-cpupm / CAL / ACPM) turn it on before hotplug.
  Next: read TWRP's dmesg (its kernel brings the M3s up): how it starts them,
  their MIDR, and whether arm64 cpufeature prints "SANITY CHECK: Unexpected
  variation in SYS_ID_AA64MMFR0_EL1" (TGran16 is a strict field).
- **TWRP 3.7.0 kernel (2026-10-28, read-only):** Linux 4.9.118, 4K pages,
  CONFIG_ARCH_EXYNOS9. cpu0-3 MIDR 0x410FD051 (A55 r0p1), **cpu4-7 MIDR
  0x531F0020 (Samsung, variant 1, part 0x002 = Mongoose M3, r1p0)**. Linux
  hotplugs CPU5-7 over and over (exynos_hpgov), 12 boots in dmesg, 31 in
  last_kmsg: **Linux brings the M3s up with PSCI CPU_ON**. System-wide hwcaps
  `fp asimd evtstrm aes pmull sha1 sha2 crc32` (no atomics/fphp: the M3 is
  ARMv8.0, the A55 v8.2). tainted=0 and no "SANITY CHECK" line although the
  string is in the kernel: Samsung silences the feature-mismatch check
  (atomics alone would have tripped it), so its silence says nothing about
  TGran16. M3 16K still unknown.
- Public: mainline exynos9810.dtsi uses PSCI smc, cpu_on 0xc4000003, M3 at
  cpu@100; the 2024 upstream series says the vendor firmware does not set
  CNTFRQ_EL0 (hardcoded in the timer node).
- **X1.5** (QEMU ok at EL1 and with secondaries entering at EL2, boot.img
  sha256 ba6c15af..., flashed 2026-10-28 with verified readback): `xentry.S` now does what Linux's
  entry does before anything else: masks interrupts, drops EL2 -> EL1 if
  entered at EL2, writes SCTLR_EL1 = 0x30D00800 (MMU/caches off, LE), sets
  VBAR_EL1; then paints a **blue** bar (top 10 rows of the core's square)
  before any C. Per M3 slot: nothing = never entered, blue only = entered but
  died before the report, green = reported.
- **X1.5 result on the G960F (photo):** same as X1.4. Three green squares
  (A55 slots 1-3), **slots 4-7 completely empty: no blue bar**, rows "never
  arrived (5+2 s)", SLOTS `-UUU----`. The M3 cores never execute even the
  first instructions of `xentry.S`, so their reset state is not the problem.
  CPU_ON returns 0 for them but they never reach our entry point.
  Guess (not verified): the big cluster (CPUCL1) power domain and/or clock is
  off after S-Boot, and Samsung's kernel turns it on (exynos-cpupm / CAL PMU
  sequences / ACPM) before its CPU_ON. Mainline (6.14+) and ztOS claim 8 cores,
  so whatever they do is public and worth reading.

## Status of X1 (milestone 1 done, 2026-10-28)

**XNU can run on the Exynos S9 on the A55 cluster**: all four A55s (MPIDR
0x81000000-0x81000300) support the 16K granule and come up through PSCI 0.1.
The shim boots from S-Boot at EL1 and draws on the panel (fb 0xCC000000,
DECON-F HW trigger 0x1281). Interrupt controller is GICv2 (GIC-400) at
0x10100000. Open: whether the Mongoose M3 cores have 16K (they would only add
speed; XNU can simply leave them off) and how to power the big cluster.
