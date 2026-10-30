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

## Stage X2: interrupts on the A55 cluster (PASSED on the G960F, 2026-10-28)

`./exynos/build.sh x2` (`exynos/x2.c` + new shared `exynos/xplat.c`: DT with
proper per-node #address-cells, PSCI 0.1 IDs, display init, CPU_ON through
`x_next_slot`; X1 keeps its own copy as it ran). UFS was left for X3: the
SDM845 `shim/ufs.c` assumes Qualcomm descriptor offsets in dwords, Exynos
UFS wants them in bytes plus vendor setup.

What it does, each step on CPU0 and then on every A55 (/cpus reg < 0x100;
the M3s are not started): reads the GIC's reg from the DT (falls back to
0x10101000 / 0x10102000), the virtual-timer PPI from /timer (3rd entry,
INTID = 16 + PPI), enables Group 1 in GICD_CTLR (NS view) once; per core:
banked GICD_ITARGETSR0 (own CPU interface bit), IGROUPR0, enables SGI 1/2/3 +
vtimer PPI only (no SPI), GICC PMR 0xF0 / BPR 0 / CTLR |= 1; vtimer every 10 ms
for 300 ms (expect ~30); 10 SGIs to itself (GICD_SGIR filter "self"). Then CPU0
sends 5 SGIs to each waiting core by its interface bit, and a final uncounted
SGI 3 to end their WFI. Rows: `if` interface bit, `tmr`, `sgi`, `x` (cross),
`oth` other IRQs (/last INTID), `grp` IGROUPR0. Green square = core finished.
Runs 60 s, then PSCI reset.

QEMU (virt, GICv2, 8x A55): all 8 cores tmr 28-29, sgi 10, x 5, verdict OK,
all squares green. boot.img sha256 5b9c9422..., flashed 2026-10-28 with verified readback.

**X2 result on the G960F (photo): "EXYNOS X2: OK", 4 cores, 3 green squares.**
- GIC from the DT reg: **GICD 0x10101000, GICC 0x10102000**, compatible
  arm,cortex-a15-gic. GICD_CTLR 0x00 -> 0x01 (S-Boot left forwarding off).
  **GICD_TYPER 0x0000FCEF**: ITLinesNumber 15 -> 512 interrupt IDs,
  CPUNumber 7 -> 8 CPU interfaces, SecurityExtn 1, LSPI 31.
  **GICD_IIDR 0x0200143B, GICC_IIDR 0x0202143B = GIC-400.**
- /timer PPIs 13 14 11 10 -> **virtual timer INTID 27**; 29 ticks in 300 ms at
  10 ms on every core (26 MHz counter).
- CPU interface bits: CPU0 0x01, CPU1 0x02, CPU2 0x04, CPU3 0x08.
  IGROUPR0 reads 0 from non-secure (RAZ), yet SGIs and the PPI are delivered:
  firmware has them as Group 1.
- Every A55: sgi 10/10 to itself, cross 5/5 from CPU0, no other IRQs.
- **Conclusion: a GICv2 backend for XNU on the A55s has everything it needs**
  (distributor, banked CPU interface, PPI timer, IPIs by interface bit).
- Next: X3 = read-only UFS probe (Exynos UFS: byte-granular PRDT/offsets,
  vendor regs; controller as S-Boot left it).

## Stage X3: read-only UFS probe (built, NOT run on a phone yet)

Stock DT extracted from `backup/exynos-boot-stock.img` (dt section at
0x1E64800, "DTBH" v2 container, 6 DTBs rev16/17/18/20/23/26; the phone uses
rev26). UFS node `ufs@0x11120000`, "samsung,exynos-ufs": reg hci 0x11120000
(0x200), vendor hci 0x11121100 (0x200), unipro 0x11110000 (0x8000), ufsp/SMU
0x11130000 (0x100); SPI 0xF2; dma-coherent; ufs-phy 0x11124000, phy-sys PMU
0x14060724; io-coherency 0x11010700 mask/bits 0x300; ufs-exynos-smu;
2 lanes, HS gear 3.

`./exynos/build.sh x3` (`exynos/x3.c`, CPU0 only, adapted from
`shim/ufs.c`). Dumps the UFSHCI + vendor registers S-Boot left, stops unless
the host is enabled/ready with no pending requests, takes over the transfer
request list (only rings a doorbell if the host accepted our list), then only
TEST UNIT READY, READ CAPACITY(10), READ(10) of one 4 KB block at a time.
Samsung specifics handled empirically and safely: the UTRD response/PRDT
offset fields are chosen so a byte reading (Linux's PRDT_BYTE_GRAN quirk for
Exynos) and a dword reading both land inside our 16 KB descriptor buffer,
with a copy of the PRDT entry at both places; the TUR shows where the
response UPIU landed (= which granularity); READ CAPACITY tries PRDT length
16 then 128 bytes (plain vs FMP-extended entry); HCI_UTRL_NEXUS_TYPE bit 0
set before each doorbell. Then MBR, GPT header, partitions >= 64 MiB plus
BOOT/RECOVERY, USERDATA/CACHE superblock type, and an end-to-end check: the
first block of BOOT must be our own boot.img (ANDROID!, kernel_size = shim).
QEMU: only the "no controller" path (stops at "requests still pending").
boot.img sha256 b07eda09..., flashed 2026-10-28 with verified readback.

**X3 result on the G960F (photo): STOP before any command, but the host is up.**
Shim at 0x800A4000. UFSHCI ver 0x210 (2.1), cap 0x1303FF0F (16 slots),
**HCE 0 but HCS 0x0000010F** (DP, UTRLRDY, UTMRLRDY, UCRDY; UPMCRS 1), IS 0,
**UTRLRSR 1 with S-Boot's list at 0xEF002000**, DBR 0. Vendor: TXPRDT and
RXPRDT 0x0C (= PRDT_SET_SIZE(12): 4 KB per PRDT entry, as assumed), NEXUS
0x0000FFFF (S-Boot marked all 16 slots SCSI). io-coherency sysreg 0x11 (bits
0x300 clear: UFS DMA not coherent; irrelevant with the MMU off). HCE reading 0
on a running host = Linux's UFSHCI_QUIRK_BROKEN_HCE for Exynos; X3 gated on
it and stopped.
- **X3.1** (boot.img sha256 00be2332..., flashed 2026-10-28 with verified readback): same probe, gate
  without HCE (DP + UTRLRDY + UCRDY + no pending requests).
- M3 power research (separate session): `exynos/M3-POWER.md`, proposes an
  X1.6 experiment (six PMU writes from Samsung's pmucal_lpm_init before
  CPU_ON). Not reviewed or tried yet.

**X3.1 result on the G960F (photo): first command answered, data command hung.**
Shim at 0x80098000; our list 0x8009E000, ucd 0x800A1000 (accepted: the host
switched to our list). **TUR: response UPIU at 512 = offsets in BYTES**
(PRDT_BYTE_GRAN confirmed), OCS 0x07, SCSI status 0x02 CHECK CONDITION (the
usual first-command unit attention; X3.1 did not retry). OCS 7 + response =
Linux's UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR (Exynos reports FATAL for any
non-GOOD status). **READ CAPACITY(10) with PRDT length 16: timeout** (the
ocs/st printed were stale from TUR). The retry with 128 meant nothing: X3.1
cleared the slot with the standard "write 0" while Exynos clears on a 1
(UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR), so slot 0 stayed busy.
Guess (not verified): the PRDT entry is Samsung's 128-byte FMP-extended
entry, so a length of 16 bytes = 0 entries and the host waits forever.
- **X3.2** (boot.img sha256 669a721c..., flashed 2026-10-28 with verified readback): OCS 7 with a
  response counts as the SCSI status; TUR repeated until GOOD, first sense
  key/ASC/ASCQ shown; after a timeout UTRLCLR gets both ~bit and bit, and a
  slot that stays busy is abandoned for the next (16 slots); IS shown on
  timeouts; slot shown on the CAPACITY rows.

**X3.2 result on the G960F (photo):** shim at 0x80128000, list 0x8012F000,
ucd 0x80132000. **First TUR: CHECK CONDITION, sense 2/04/02 = NOT READY,
"logical unit not ready, initializing command required"**: S-Boot leaves the
UFS device in a low-power mode (Linux never sees this: it resets and
re-initializes the device). Second TUR: timeout, but its UTRD OCS had been
rewritten 0x0F -> 0x07, so the host did process it (another refusal) and then
kept the slot busy; IS 0x00000001 (UTRCS). So X3.1's READ CAPACITY hang was
very likely the same refusal, not the PRDT length.
- **X3.3** (boot.img sha256 0e98a70e..., flashed 2026-10-28 with verified readback): TUR, then START
  STOP UNIT power condition ACTIVE (CDB 1B 00 00 00 10 00) to the UFS Device
  W-LUN 0xD0 (5 s timeout, as ufshcd_set_dev_pwr_mode), TUR, START STOP UNIT
  START=1 to LUN 0, TUR, TUR; stops at the first GOOD. IS cleared (W1C) before
  each command; timeouts show IS, DBR and the next slot; CHECK CONDITIONs
  show their sense. Then READ CAPACITY / GPT / BOOT as before.

**X3.3 result on the G960F (photo): the device wakes up.** Shim at 0x800B0000.
TUR: CHECK CONDITION 2/04/02 again (bytes granularity again). **START STOP
UNIT (ACTIVE) to W-LUN 0xD0: rc 0, OCS 0, GOOD. Next TUR: GOOD.** So waking
the device as Linux does is the missing step. READ CAPACITY with PRDT length
16: OCS 0, GOOD, but no data in our buffer (last 0, blk 0). Then with 128:
DBR cleared but OCS still our preset 0x0F and no response UPIU.
Guess (not verified): the PRDT entry is the 128-byte FMP-extended one, so 16
bytes = 0 entries (data dropped, GOOD anyway), and that left the DMA side in a
bad state for the next command.
- **X3.4** (boot.img sha256 d51926a3..., flashed 2026-10-28 with verified readback): PRDT length 128
  first, then 16; a fresh request slot per command (skipping busy ones);
  CAPACITY rows show slot and residual count; IS shown on OCS errors; if
  CAPACITY fails both ways, READ(10) of LBA 1 is tried with each length and
  "EFI PART" accepted as proof, then GPT/BOOT as before.

**X3.4 result on the G960F (photo):** TUR 2/04/02 -> SSU ACTIVE GOOD -> TUR
GOOD again. **READ CAPACITY with PRDT 128 (slot 3): OCS 0, GOOD, residual 0,
buffer still zero.** Every later command (CAPACITY 16 on slot 4, both GPT
tries): OCS left at our 0x0F, no response UPIU, IS 0. So the order does not
matter: the first data-in command "succeeds" without reaching our buffer,
and the host writes no status for anything after it. No-data commands work,
including the host's DMA of the response UPIU into our memory.
- **X3.5** (boot.img sha256 296b6ef0..., flashed 2026-10-28 with verified readback): one data command
  only: READ(10) LBA 1, 4 KB, PRDT 128, into a buffer pre-filled with 0xA5;
  shows how many bytes changed and where, scans the rest of the 16 KB
  descriptor buffer for bytes the host wrote, and reads (only) vendor +0x38,
  +0xC0 and IS afterwards. Continues to the GPT only if "EFI PART" arrives.

**X3.5 result on the G960F (photo):** wake-up as before (TUR 2/04/02, SSU
ACTIVE GOOD, TUR GOOD). READ(10) LBA 1, 4 KB, PRDT 128 (slot 3): OCS 0, GOOD,
residual 0, **buffer unchanged (0 of 4096 bytes, still 0xA5)**, nothing else
in the descriptor buffer changed, vendor +0x38 / +0xC0 = 0. **IS = 0x00020001:
bit 17 SBFES, system bus fatal error** (+ UTRCS). So the host's data write was
rejected on the bus, and a host with SBFES set stops processing until reset:
that is why no later command in X3.3/X3.4 got a status.
Not the address range as such (S-Boot DMAs our own kernel into the same
0x80xxxxxx RAM). Guess: the host reads our PRDT entry in another layout
(Linux's exynos FMP code mentions a secure FMP setting selecting the entry
format) and DMAs to a bogus address.
- **X3.6** (boot.img sha256 2afe2892..., flashed 2026-10-28 with verified readback): before touching the
  host, reads S-Boot's own request list (UTRLBA, 0xEF002000) from RAM: per used
  slot DW0, OCS, UCD address, the raw response and PRDT fields, the command's
  opcode and expected length, and 64 bytes from its PRDT offset. That is the
  layout Samsung's own code used to load us. Then the X3.5 probe as before.

**X3.6 result on the G960F (photo):** S-Boot's list (0xEF002000) has one used
slot: **slot 0, DW0 0x01000000 (CT 0, no data, I), OCS 0, UCD 0xEF000000,
response field 0x04000400 (offset 1024, length 1024 bytes), PRDT field 0**.
Its command UPIU: 01 00 D0 00 ... CDB **1B 00 00 00 30**: START STOP UNIT to
the UFS Device W-LUN with **power condition 3 (power down)**: S-Boot powers
the device down right before jumping to the kernel (hence our 2/04/02). The
UCD is reused for every command, so no data-command descriptor is left.
Our probe: same as X3.5 (READ 1 GOOD, buffer untouched, IS 0x00020001 SBFES).
- **X3.7** (boot.img sha256 145adcff..., flashed 2026-10-28 with verified readback): additionally dumps
  S-Boot's UCD from +0x800 (+0x800..0x83F, +0x880, +0x890, +0x900, +0x980),
  where the PRDT of its last data read (loading us) probably still lies;
  4 KB entries with addresses 0x1000 apart would show the stride and layout.

**X3.7 result on the G960F (photo):** S-Boot's UCD at +0x800..0x83F, +0x880,
+0x890, +0x900, +0x980 is all zero: S-Boot clears the whole UCD per command
(or keeps the PRDT elsewhere), so no trace of its data-read layout survives.
Rest as X3.5 (READ 1 GOOD, buffer untouched, IS 0x00020001).

**UFS status (end of 2026-10-28):** host up after S-Boot (HCS 0x10F, HCE reads
0), our request list accepted, offsets in bytes, device powered down by
S-Boot and woken with START STOP UNIT (ACTIVE) to W-LUN 0xD0, no-data
commands work, **every data-in transfer ends in IS.SBFES with nothing in our
buffer**, after which the host must be reset. Next: read Samsung's vendor
UFS driver for this SoC (LineageOS android_kernel_samsung_universal9810:
drivers/scsi/ufs/ufs-exynos.c, FMP/SMU code) for the exact PRDT entry layout
and what is set up before the first data transfer, instead of more guesses.

### Samsung's UFS driver for this SoC (read 2026-10-28)

Source: LineageOS android_kernel_samsung_universal9810, branch lineage-17.1
(https://github.com/LineageOS/android_kernel_samsung_universal9810/tree/lineage-17.1):
`drivers/scsi/ufs/{ufs-exynos.c,ufs-exynos-smu.c,ufshcd.c,ufshci.h}`,
`drivers/crypto/fmp/smu_dev.c`, `include/linux/smc.h`, `include/crypto/{fmp,smu}.h`.
- `ufshci.h`: with CONFIG_EXYNOS_FMP, struct ufshcd_sg_entry is des0..des31 =
  **128 bytes** (base_addr, upper_addr, reserved, size, then file IV/keys,
  tweak keys, disk IV, reserved). UCD = command UPIU 1024 B + response UPIU
  1024 B (ALIGNED_UPIU_SIZE) + PRDT, i.e. **response at 1024, PRDT at 2048**.
- `ufshcd.c` ufshcd_map_sg: PRDT_BYTE_GRAN -> prd_table_length =
  segments * sizeof(entry) (bytes); size = dma_len - 1; reserved = 0; then
  the crypto engine config per entry. `include/crypto/fmp.h`: des3 bits 28-29
  FAS, 30-31 DAS, 26/27 key lengths; bypass = all zero (as our entries).
- `ufs-exynos.c` init (and again on resume): "FMPSECURITY & SMU":
  `exynos_ufs_smu_sec_cfg()` then `exynos_ufs_smu_init()`, which
  (`smu_dev.c`) are the secure-firmware calls
  **`exynos_smc(SMC_CMD_FMP_SECURITY 0xC2001810, 0, SMU_EMBEDDED 0,
  CFG_DESCTYPE_3 3)`** and **`exynos_smc(SMC_CMD_SMU 0xC2001850, SMU_INIT 0,
  SMU_EMBEDDED 0, 0)`**. There is also an SMU_ABORT (command 2). Neither is
  persistent: S-Boot and the kernel set them up on every boot/resume.
- io-coherency sysreg bits are set only for a dma-coherent setup (not needed
  with the MMU off).
Inference (not verified): after S-Boot hands over, the FMP/SMU state does not
allow normal-world data DMA until the OS makes those two calls; that would
give exactly IS.SBFES on data while descriptor/response DMA works.
- **X3.8** (boot.img sha256 1e673b9b..., flashed 2026-10-28 with verified readback): makes the two
  calls Samsung's driver makes before any I/O, FMP_SECURITY(0, SMU_EMBEDDED,
  CFG_DESCTYPE_3) and SMU(SMU_INIT, SMU_EMBEDDED), and shows their return
  values (0 = success); UCD layout now Samsung's (response at 1024, PRDT at
  2048; the dword readings 4096/8192 still land inside our 16 KB buffer).
  Then the X3.5 probe: wake-up, one 4 KB READ(10) of LBA 1 into a 0xA5
  buffer, and on "EFI PART" the GPT, partitions and BOOT self-check. S-Boot
  descriptor dump removed (findings above).

**X3.8 result on the G960F (photo): both firmware calls return 0** (FMP_SECURITY
desc 3 -> 0, SMU_INIT -> 0), UCD in Samsung's layout, wake-up as before, but
READ(10) LBA 1 again GOOD with the buffer untouched and **IS 0x00020001
(SBFES)**. So FMPSECURITY/SMU_INIT alone do not fix the data path. Still open.
Not tried yet: a full host re-init as Linux does (vendor HCI_SW_RST, UniPro /
PHY setup from ufs-cal-9810.c, link startup, NOP OUT, fDeviceInit) and the
io-coherency setting; and checking which AXI/DMA vendor registers S-Boot left.
UFS is parked while XNU bring-up starts (the SDM845 M4 also ran without
storage writes).

## XNU port started (2026-10-28)

Plan and status: `XNU-PORT.md`. Done: `mkadt.py` (Exynos ADT on top of the
SDM845 one: 4 A55 with reg = real MPIDR affinity and psci-id 0..3, GIC-400
GICD 0x10101000 / GICC 0x10102000, vtimer INTID 27, timebase 26 MHz, fb
0xCC000000, RAM window 0x92000000-0xAF800000 with the panic log at its top,
no UFS node); `repack-boot.py --ramdisk` (tested on the real stock image: dt
section kept); stage `x4.c` (`build.sh x4` with PAYLOAD=; QEMU: reads the
initrd from /chosen, refuses a non-payload). Next: kernel patch 0102 (Exynos
switch for the Qualcomm-only drivers, GICv2, PSCI psci-id), built by CI.
- **Patch 0102 written (2026-10-28, not compiled):** `sweet_is_exynos()` from
  the ADT root compatible gates USB, keys, touch, SPMI, MAX77705 battery, OSM
  clocks, MDSS autorefresh; GICv2 path in pe_fiq.c (GICD/GICC from the ADT
  reg, per-CPU interface bit from banked ITARGETSR0, PMR 0xF0, GICD_CTLR |= 1)
  with `sweet_gic_ack/eoi` in sleh_irq and `sweet_gicv2_send_sgi` in
  SweetCPU::signalCPU; PSCI CPU_ON uses the ADT `psci-id`; start.s skips
  ICC_SRE_EL2 when ID_AA64PFR0.GIC = 0; the early console takes its
  framebuffer from boot_args (it was hard-coded to 0x9d400000, which is RAM
  in the Exynos window). Built locally only as far as `git apply`.
- **First Exynos kernel built (2026-10-28):** pushed main to the private repo
  xbxarchivr/s9 (remote `xbx`); its xnu workflow ran on the push, run
  40700590330, 4 min, applied 0001-0102 (the skipped patches are
  darwin-xnu-build's own) and linked `kernel.development.sweet`, 17265608
  bytes, sha256 27df345e729d62a0... No compile errors in 0102's files.
  Downloaded to `out/exynos/xnu/`. Payload `out/exynos/payload.bin`
  (16650944 bytes): phys 0x92000000, size 0x1d700000, kernel+ADT end
  0x944d0000 (36.8 MB), entry 0xfffffe000748c488, cmdline `debug=0xc serial=3
  cpus=1 keepsyms=1 -v sweettouch=0 sweetkeys=0 sweetusb=0 sweetbattery=0
  sweetcpu=0`. ADT has no /product node (research/ipsw-x/d22ap.adt is not in
  this checkout; only matters for userland). X4 boot.img sha256 95fb6751...
  (42543104 bytes), flashed 2026-10-28 with verified readback. Expected: loader screen (INITRD must be
  outside 0x92000000-0xaf800000), then the early console / XNU log on the
  panel, most likely ending at "waiting for root device" (no UFS yet).

## X4 result: XNU boots on the G960F (2026-10-28, photo)

The CI kernel (0001-0102) boots to "waiting for root", the same milestone as
the SDM845's first M4 boot. Seen on the panel (framebuffer console, white on
black, wrapped): VM bootstrap / zone_init in the 0x92000000 window,
"SWEET: starting device-tree platform expert", AppleKeyStore, IOWatchdog,
HID buttons/touch registered (stubs), **"SWEET BATTERY: static fallback"**,
**"SWEET CPU: Exynos, no Qualcomm OSM: clocks left as S-Boot set them"**,
"SWEET CPU: requested=1 selected=1", **"cpu=0 mpidr=0x0 came alive; SGI1 OK"**
and **"1/1 CPUs initialized, SGI1 verified"** (GICv2 SGI + timer IRQs work),
BSD init, MAC framework, **"SWEET FB: AppleCLCD 1440x2960 stride 5760 at
0xcc000000, default surface 1, presented as 1125x2436"** (zero-copy
unavailable, as intended off the SDM845 splash), AMFI/Sandbox policies, HFS
registered, dlil/ifnet lo0/gif0/stf0, and finally **"waiting on <dict>
IOProviderClass IOMedia, Content Apple_HFS"**: no root device (no UFS yet).
The early console's stage bar is drawn at the bottom of the panel.
Next: cpus=4 (secondary A55s through PSCI psci-id + GICv2 cross-CPU SGIs),
then a root filesystem (UFS data path, or a RAM disk root while UFS is open).
- **X4.1** (boot.img sha256 78cb1550..., copy `out/exynos/boot-x4.1.img`, flashed
  2026-10-28 with verified readback): same kernel and ADT, cmdline cpus=4 (payload-cpus4.bin):
  SweetCPU starts CPU1-3 with PSCI CPU_ON(psci-id 1..3); each runs
  pe_init_fiq's GICv2 path (records its interface bit) and must answer SGI1.
  Expected lines: "cpu=N ... PSCI CPU_ON rc=0", "came alive; SGI1 OK",
  "4/4 CPUs initialized".
- **X4.1 result on the G960F (photo, rotated): 4/4 A55 cores under XNU.**
  "SWEET CPU: cpu1 PSCI CPU_ON target=0x1 ... context=1", rc=0,
  "cpu=1 mpidr=0x100 came alive; SGI1 OK"; same for cpu2 (target 0x2,
  mpidr 0x200) and cpu3 (target 0x3, mpidr 0x300); **"SWEET CPU: 4/4 CPUs
  initialized, SGI1 verified"**; "cache: 4 CPU(s)"; framebuffer vsync thread
  on cpu 3; ends at "waiting on ... IOMedia ... Apple_HFS" as before. So
  0102's PSCI psci-id, per-CPU GICv2 setup and cross-CPU SGIs all work.
  The XNU port now has SMP on the A55 cluster; the open item before userland
  is a root filesystem.

## Root filesystem, step 1: RAM-disk PID 1 (X4.2)

As on the sweet (NOTES.md 0021-0027): `tools/sweet-init.S` assembled with
Ubuntu clang 18.1.3 (`clang -target arm64-apple-ios16.0 -c`, installed in
WSL) and wrapped by `tools/mkinit.py` into a 16635-byte static ad-hoc signed
Mach-O (`out/exynos/init/sweet-init`); mockfs serves the RAM disk itself as
/sbin/launchd. Payload `out/exynos/payload-rd.bin` (16667664 bytes): RAM disk
at 0x944d0000 (0x8000), cmdline `debug=0xc serial=3 cpus=4 keepsyms=1 -v
rd=md0 sweettouch=0 sweetkeys=0 sweetusb=0 sweetbattery=0 sweetcpu=0`.
X4.2 boot.img sha256 6b001223... (copy `out/exynos/boot-x4.2.img`), flashed
2026-10-28 with verified readback. Expected: "BSD root: md0", "load_init_program: attempting to
load /sbin/launchd", "*** SWEET userspace: init running as PID 1 ***", then
"SWEET init: still alive" every 5 s.
Step 2 (a real iOS root) needs storage: the iOS 16 root is GBs, the RAM
window 472 MB.

**X4.2 result on the G960F (photo): USERSPACE PID 1 RUNS.** "Added memory
device md0/rmd0 ... at FFFFFE0008400000", "BSD root: md0, major 1, minor 0",
"load_init_program: attempting to load /usr/appleinternal/sbin/
launchd.development" then "/sbin/launchd", **"*** SWEET userspace: init
running as PID 1 ***"**, then "SWEET init: still alive" every 5 s (8 seen),
no panic; "mcache: 4 CPU(s)". Exec of an ad-hoc signed static Mach-O,
syscalls, devfs /dev/console and select timeouts work on the Exynos, on 4
A55 cores. Same milestone as the sweet's 0025 boot.
Next for a real iOS root: (1) UFS data path (full host re-init as
ufs-exynos.c / ufs-cal-9810.c do it; X3.x showed every data-in DMA ends in
IS.SBFES on S-Boot's host state), (2) more RAM than the 472 MB window
(0091-style holes in pmap for the Exynos banks), (3) an Exynos UFS driver
for XNU (bsd/dev/sweet_ufs.c is Qualcomm's ufshc).

## UFS, continued: host clocks and DMA config as Linux sets them (X3.9)

Samsung's driver (ufs-exynos.c, universal9810 lineage-17.1) with clocks on:
pre_setup_clocks(on) = FORCE_HCS(+0xB4) &= ~BIT4 (core clock auto-stop off),
CLKSTOP_CTRL(+0xB0) &= ~0x17 (REFCLKOUT/REFCLK/UNIPRO_MCLK/PCLK not
stopped), UFS_ACG_DISABLE(+0xFC) |= 1; link_startup_notify(PRE) adds
FORCE_HCS |= CLK_STOP_CTRL_EN_ALL (0xDE0); init_host: DATA_REORDER(+0x60)=0xA,
TXPRDT(+0x00)=PRDT_PREFECT_EN|12 = 0x8000000C, RXPRDT(+0x04)=0xC, NEXUS
(+0x40/+0x44)=0xFFFFFFFF, AXIDMA_RWDATA_BURST_LEN(+0x6C)=WLU_EN|BURST_LEN(3)
= 0x98000003, IOP_ACG_DISABLE(+0x100) &= ~1. setup_clocks(off) does the
reverse (gate everything, HW ACG on): very likely what S-Boot leaves after
powering the device down. Diagnostics in the vendor block: VENDOR_SPECIFIC_IS
+0x38, SMU_ABORT_MATCH_INFO +0x10C, FSM_MONITOR +0xC0, DMA0/1_MONITOR_STATE
+0xC8/+0xD0. HCI_SW_RST is +0x50 (for a full re-init later), and the DMA mask
is 32 bits.
- **X3.9** (boot.img sha256 ad84d706..., copy `out/exynos/boot-x3.9.img`,
  flashed 2026-10-28 with verified readback): X3.8 + those writes on the running link before taking the
  request list, dumped before ("VS BEFORE") and after ("VS AFTER"); after the
  4 KB read: IS, vendor IS, SMU abort info, FSM and DMA monitors ("AFTER").
  If data still fails: full re-init (HCI_SW_RST, ufs-cal-9810.c pre/post
  link tables, DME_LINKSTARTUP, NOP OUT, fDeviceInit).
- **X3.9 result on the G960F (photo):** VS BEFORE (what S-Boot leaves):
  TXPRDT 0x0C, RXPRDT 0x0C, DATA_REORDER 0xA, BURST 0x18000003 (BURST_LEN(3),
  no WLU_EN), CLKSTOP 0 (nothing stopped), FORCE_HCS 0xDE0 (= Linux),
  UFS_ACG 1 (HW ACG off), IOP_ACG 0, NEXUS 0xFFFF / TM 0xF. So the clocks
  were already on; the guess was wrong. After our writes only TXPRDT
  0x8000000C and BURST 0x98000003 differ. READ(10) LBA 1 still GOOD with the
  buffer untouched, IS 0x00020001 (SBFES), vendor IS 0, **SMU_ABORT_MATCH_INFO
  0 (not the SMU)**, FSM 0, DMA0/DMA1 monitor 0x01010101.
- **X3.10** (boot.img sha256 f230edaf..., copy `out/exynos/boot-x3.10.img`,
  flashed 2026-10-28 with verified readback): the 4 KB data buffer is the last 8 KB of ucd (ucd+8192,
  4 KB aligned), memory the host's response DMA demonstrably writes. Data
  arriving = an address/window problem; SBFES again = the data path itself
  (then the full host re-init: HCI_SW_RST 0x50, init_host, GPIO_OUT device
  reset, BROKEN_HCE enable via DME_RESET/ENABLE, init_cfg with UniPro mclk
  period read back by DME_GET 0x9514 first, DME_LINKSTARTUP, post_init_cfg
  and hibern8 values, NOP OUT, fDeviceInit).
- **X3.10 result on the G960F (photo):** data buffer at ucd+8192 (ucd
  0x80159000): READ(10) GOOD, **buffer untouched (changed 0)**, IS 0x00020001
  (SBFES), vendor IS 0, SMU abort 0, DMA monitors 0x01010101. So it is NOT the
  address: the host's data-in path is broken in the state S-Boot hands over,
  while command/response DMA works. Next: full host re-init (list above).

## More RAM and the first Exynos driver (2026-10-28, patches written, not built)

- **Keys (0103-exynos9810-gpio-keys):** stock rev26 DT gpio_keys: KEY_WINK
  (Bixby) gpa0-6, KEY_VOLUMEDOWN gpa0-4, KEY_VOLUMEUP gpa0-3, KEY_POWER
  gpa2-4, flags 0x0f (active low). Banks in pinctrl@14050000 (alive);
  offsets from the universal9810 kernel's pinctrl-exynos.c
  (exynos9810_pin_banks0): gpa0 +0x020, gpa2 +0x060; DAT at bank +0x04 ->
  **gpa0 DAT 0x14050024, gpa2 DAT 0x14050064**, pressed = bit 0. On Exynos
  sweet_keys.c maps the alive block and polls both every 20 ms (read only),
  reporting power, volume up, volume down, Bixby = Menu to sweet_hid_key, and
  logs every change with the raw values.
- **RAM (0104-exynos9810-ram-holes-from-adt):** pmap_is_bad_ram (0091) takes
  its holes from ADT /chosen/sweet-ram-holes (u64 <start end> pairs, up to
  16) when present, else 0091's SDM845 list. Plan: span 0x92000000-0xF0000000
  (bank 2 from 0xF0000000 up is modem/audio/debug carve-outs) with holes for
  removed-dma-pool 0xAF800000 (32M), 0xB8000000 (2M), the bank-0/bank-1 gap +
  seclog 0xC0000000 (512K), fb 0xCC000000, the bank-1/bank-2 gap +
  secure_camera 0xE1900000 (30M): about 1.3 GB instead of 472 MB. Needs the
  exact bank ends (live /proc/iomem or /sys/firmware/fdt in TWRP) before
  mkadt.py gets them: the physmap also maps the holes.
- **Live RAM map (TWRP, /proc/device-tree memory nodes + /proc/iomem):**
  0x80000000+0x3C800000 (to 0xBC800000), 0xC0000000+0x20000000 (to
  0xE0000000), 0xE1900000+0x1E700000 (to 4G; System RAM to 0xFF7FFFFF),
  0x880000000+0x80000000. mkadt.py now: span 0x92000000-0xF0000000
  (dram-size 0x5DF00000, panic log 0xEFF00000), holes 0xAF800000-0xB1800000,
  0xB8000000-0xB8200000, 0xBC800000-0xC0080000, 0xCC000000-0xCD100000,
  0xE0000000-0xE3700000 in /chosen/sweet-ram-holes: **1340 MB** usable
  (`--small` = the old 472 MB window). The bank gaps are inside the
  physmap; if that faults, the next step is a span that stops at 0xBC800000.
- **Kernel with 0103 + 0104 built** (xbxarchivr/s9 run 40725524440, success;
  only style warnings in sweet_keys.c of the kinds the S9 code already has),
  `kernel.development.sweet` 17265784 bytes, sha256 dea2acb1b54a254b...,
  in `out/exynos/xnu2/`.
- **X4.3** (boot.img sha256 271a9b48..., copy `out/exynos/boot-x4.3.img`,
  flashed 2026-10-28 with verified readback; result: booted to init, see below): that kernel + the 1340 MB ADT + sweet-init RAM disk, cmdline
  `debug=0xc serial=3 cpus=4 keepsyms=1 -v rd=md0 sweettouch=0 sweetusb=0
  sweetbattery=0 sweetcpu=0` (keys on). Expected: same path to "SWEET init:
  still alive"; "SWEET KEYS: Exynos gpa0=... gpa2=..." and a key=N down=1/0
  line per press; more memory in the VM/zone lines. A panic early in VM init
  would point at the bank gaps in the physmap.
- Total RAM is ~4015 MB (968 + 512 + 487 + 2048); the 2 GB bank at
  0x880000000 needs multi-range support in the pmap (one span cannot reach it
  without ~30 GB of hole), so it stays unused for now.

- **X4.3 result on the G960F (reported by the user, 2026-10-28):** booted to
  init ("SWEET init: still alive") with the 1340 MB ADT. Not recorded: whether
  the key presses logged `SWEET KEYS`, and the exact usable-memory line.

## Handoff: UFS X5 (2026-10-28, end of session)

- Repo note: Windows git rejects `s9/shim/con.c` (CON is a reserved name), which
  made the index look like 251 deleted files. Fixed locally with
  `git config core.protectNTFS false` and `git reset` (no files changed). A fresh
  Windows clone needs the same setting.
- **X5 (full UFS host re-init) written and cross-compiled, NOT flashed.**
  `exynos/x5.c` (new) + the `x5` case in `exynos/build.sh`.
  `./exynos/build.sh x5` builds clean: no warnings, no ABS64 relocations, Image
  131072 bytes, `out/exynos/boot-x5.img` repacked from the stock backup (header
  v0+dt 1820672, page 2048).
- What X5 does, in order (from ufs-exynos.c / ufs-cal-9810.c / ufshcd.c,
  lineage-17.1), after a read-only UFSP protector dump first (the old untested
  lead, now covered): PHY isolation bypass + io-coherency sysreg;
  FMPSECURITY + SMU_INIT firmware calls; host SW reset (vendor HCI_SW_RST 0x50
  = 0x3, poll to 0); init_host vendor registers; device reset_n (GPIO_OUT 0
  then 1); HCE enable; discover MCLK from PA_DBG_CLK_PERIOD 0x9514 (fallback
  166.5 MHz); available lanes; apply init_cfg; DME_LINKSTARTUP (result read from
  UniPro 0x7854); hibern8 calibration + post_init_cfg; point the request lists
  at ours and run-stop; NOP OUT until NOP IN; SET_FLAG fDeviceInit then poll
  READ_FLAG until cleared. Then the X3 read probe (TUR/SSU wake-up, one 4 KB
  READ(10) of LBA 1 into a 0xA5 buffer = the SBFES test, GPT, partitions, BOOT
  self-check). Every UTRD carries command type 1 (UFS storage, bit 28); SCSI
  sets the NEXUS bit, dev-mgmt clears it. No SCSI writes anywhere.
- Decisions recorded while writing (from the source, not tested): no
  UFSHCD_QUIRK_USE_OF_HCE (controller enabled via HCE); device reset = GPIO_OUT
  0 then 1; link-startup result from UniPro 0x7854 (GET_GENERRCODE_DIRECT);
  query response code at resp+6, flag value = bit 0 of BE32 at resp+20..23.
- Next: flash `out/exynos/boot-x5.img` with `exynos/twrp-boot.sh flash`
  (verified readback), photograph. The pass criterion is the READ(10) LBA 1
  buffer going from 0xA5 to "EFI PART" with IS.SBFES gone, then the GPT /
  partition list / BOOT self-check completing.

- **X4.3 re-flashed** (2026-10-28, after the user could not get it to boot again; boot.img sha256 271a9b48..., readback matched, adb from PowerShell). Result pending.

- **Keys did not react on X4.3 (user, 2026-10-28): cause found.** sweet_keys_init() and sweet_touch_init() are only called at the end of a successful attach in bsd/dev/sweet_ufs.c (0060/0072); the Exynos ADT has no /sweet-ufs node, so sweet_ufs_init returns ENXIO early and the 0103 GPIO polling never starts. **0105-exynos9810-start-input-without-ufs** calls both from that early exit too (both are idempotent; touch returns early on Exynos). Patch made on a Windows-side tree (scratchpad xnuw, xnu-8796.141.3 + 0001-0104), applies cleanly. Not built yet.

- **X5 flashed** (2026-10-28, boot.img `out/exynos/boot-x5.img` sha256 c4ca330b..., TWRP dd to BOOT sda10 from PowerShell, readback matched, phone rebooted into it). Result pending: pass = READ(10) LBA 1 buffer 0xA5 -> "EFI PART", IS.SBFES gone, GPT/partition list/BOOT self-check printed.

- **X5 result (photo, 2026-10-28): UFS host re-init WORKS up to the device.** UFSP 000=0 008=4 010=EFE26492 01C=00040000, 100=0 108=4 110=0; phy-pmu 00010001 coh 00000311; FMPSECURITY 0, SMU_INIT 0; HOST RESET sw_rst 00; HCE 1 hcs 00000008; MCLK ~166.5 MHz period 0A; rx 2 lanes 2; init_cfg rc 0; **LINK STARTUP gen_err 0** (hcs 0000000F); hibern8 rc 0; post_init_cfg rc 0; utrl 0880FC000 ucd 0880FF000; **NOP OUT rc 0 ocs 00** (NOP IN came back). Stopped at **FDEV INIT rc 4 ocs 00 st FE** = "SET_FLAG fDeviceInit failed": transport OK, query response 0xFE = Invalid OPCODE.
- **Cause: query function was in UPIU header byte 6 (the response byte) instead of byte 5** (Linux `UPIU_HEADER_DWORD(0, query_func, 0, 0)`). Fixed in `x5.c` query_flag; rebuilt `out/exynos/boot-x5.img` sha256 3ab5c190..., flashed 2026-10-28 with verified readback (TWRP dd, PowerShell); result below.

- **X5 (query fix) result (photo, 2026-10-28): DEVICE FULLY UP, DATA STILL BLOCKED.** Load addr 0x8013C000. MCLK from PA_DBG_CLK_PERIOD = 10 ns (100 MHz; 1US_TO_CNT 0x64). Link startup gen_err 0, hibern8/post_init_cfg rc 0, utrl 0x80144000 ucd 0x80147000, NOP OUT ok, **FDEV INIT SET rc 0 st 00, READ_FLAG fDeviceInit flag 0** (device init done). VS AFTER: 000=0C (TXPRDT prefetch bit not kept?), 004=0C, 00C=64, 060=0A, 06C=98000003, 070=1, 0B0=0000?218, 0B4=0x900, 0FC=0, 100=0, 040=FFF8. UFS ver 0x210 cap 1303FF0F, hcs 0x30F. Wake-up: TUR 6/29/00 (power-on reset UA, expected after our reset), SSU ACTIVE 6/29/00, TUR GOOD. **READ(10) LBA 1: rc 0 ocs 0 GOOD, prdt 128, res 0, buffer unchanged (0xA5)**, AFTER is 0x00020001 (SBFES), **vendor IS 0x00100000 and SMU_ABORT_MATCH_INFO nonzero** (value partly hidden by the screen crack; X3.9/X3.10 had both 0).
- Compared with Linux (universal9810 ufs-exynos.c/ufshcd.c, downloaded to the session scratchpad): Linux also does FMPSECURITY+SMU_INIT once in ->init, BEFORE ->host_reset (HCI_SW_RST) in ufshcd_hba_enable, so the order was not the bug. Differences found in X5: it CLEARED UFS_ACG_DISABLE bit 0 (= HW ACG on; Linux sets it), and after the SW reset FORCE_HCS was 0x900 / CLKSTOP had stop bits, while Linux's link_startup PRE does ctrl_clk(on) FORCE_HCS |= 0xDE0 (incl. UFSP_DRCG_EN, the FMP clock) and gate_clk(off) CLKSTOP &= ~0x17. On command errors Linux calls SMC SMU(SMU_ABORT=2, SMU_EMBEDDED), which returns ACCESS_CONTROL_ABORT 0x14 when the SMU blocked the access.
- **X5.1** (boot.img sha256 f0eab717..., copy `out/exynos/boot-x5.1.img`, built clean, flashed 2026-10-28 with verified readback; result below): ACG off + FORCE_HCS |= 0xDE0 + CLKSTOP &= ~0x17 after init_host; "PRE DATA" line (vendor IS + abort info before any data command); after the read an "SMU" line (abort info, FSM) and, on failure, the SMU_ABORT firmware call result (0x14 = SMU access control blocked the DMA).
- **X5.1 result (photo, 2026-10-28): clocks and SMU ruled out.** VS AFTER now as Linux (FORCE_HCS 0xDE0, UFS_ACG 1; TXPRDT reads back 0x0C, the prefetch bit 31 does not stick). **PRE DATA: vendor IS 0x00100000 already before any data command, abort_info 0** (so the X5 "nonzero smu_abort" was a misread through the screen crack). Wake-up as X5, READ(10) LBA 1 GOOD, buffer unchanged, IS 0x00020001 (SBFES), vendor IS 0x00100000, **SMU abort_info 0, FSM 0, SMC SMU_ABORT -> 0 (not ACCESS_CONTROL_ABORT 0x14)**. utrl 0x8013C000 ucd 0x8013F000.
- Remaining difference between the response DMA (works, even into the same RAM, X3.10) and the data DMA (SBFES): data-in goes through the FMP inline-crypto engine with CFG_DESCTYPE_3 (128-byte entries).
- **X5.2** (boot.img sha256 4d8593e8..., copy `out/exynos/boot-x5.2.img`, built clean, flashed 2026-10-28 with verified readback; result below): FMPSECURITY with **CFG_DESCTYPE_0** (what Samsung kernels without CONFIG_EXYNOS_FMP use) and 16-byte PRDT entries (prdt length field 16); "VS BEFORE" dump dropped (known); after a failed read a "DIAG" dump of vendor 0x10/14/18/20/24 (invalid UPIU ctrl/addr, invalid UTR/DIN offset), 0x38, 0x5C (RX UPIU match error), 0x64, 0x74, 0xC0-0xD4 (FSM, PRDT hit, DMA0/1 monitors), 0xF8 (AXI DMA IF ctrl).
- **X5.2 result (photo, 2026-10-28): descriptor type 0 did not help** (FMPSECURITY desc 0 -> 0, READ(10) prdt 16 GOOD, buffer unchanged, IS 0x00020001 SBFES, vendor IS 0x00100000, abort 0, SMU_ABORT -> 0). TXPRDT read back 0x8000000C this time. **DIAG after the read** (some digits behind the crack): 010 (INVALID_UPIU_CTRL)=0, 014 (INVALID_UPIU_BADDR)=0000000?, **018 (INVALID_UPIU_UBADDR)=DEADCAFE** (looks like), 020 (INVALID_UTR_OFFSET)=0x8000, 024 (INVALID_DIN_OFFSET)=0x10000, 038=0x00100000, 05C=00??0000 (hidden), 064=0, 074 (WRITE_DMA_CTRL)=3, 0C0=0, 0C4=000?0001, 0C8=01010101, 0CC=0, 0D0=01010101, 0D4=?0000000, 0F8 (AXI_DMA_IF_CTRL)=0x30.
- **Root-cause hypothesis (X5.3):** every probe stage used task tag `++tag` (1,2,3...) while the slots rotate 0,1,2... (tag != slot); ufshcd always uses tag = slot. All slots point at the SAME UCD, so a response UPIU routed by tag to the wrong slot still lands in our buffer and looks fine, but a DATA IN UPIU routed to the wrong slot finds no PRDT there -> treated as invalid UPIU -> DMA to the never-programmed invalid-UPIU sink (UBADDR 0xDEADCAFE) -> IS.SBFES. Explains every X3/X5 data failure. (Not verified.)
- **X5.3** (boot.img sha256 861add58..., copy `out/exynos/boot-x5.3.img`, built clean, flashed 2026-10-28 with verified readback; result below): task tag = slot in scsi_lun and devman (NOP/query); otherwise = X5.2 (desc type 0, 16-byte PRDT, DIAG dump). Pass = READ 1 buffer "EFI PART", then GPT/partitions/BOOT self-check.
- **X5.3 result (photo, 2026-10-28): tag = slot changed nothing** - same READ GOOD / buffer unchanged / IS 0x00020001 / vendor IS 0x00100000 / SMU abort 0, and the DIAG values are identical to X5.2 (so 018 "DEADCAFE"-like, 020=0x8000, 024=0x10000 are static register contents, not error captures). Tag hypothesis disproven. Ruled out so far: address/window (X3.10), FMPSECURITY/SMU_INIT, full host re-init + link startup + fDeviceInit, clocks/ACG, SMU access control, FMP descriptor type 3 vs 0, task tag.
- **Next: read the WORKING register state from TWRP** (Linux 4.9 + Samsung ufs-exynos has UFS running): vendor HCI 0x11121100 (0x00-0x160), std HCI 0x11120000, UFSP 0x11130000, sysreg coherency 0x11010700, phy-pmu 0x14060724, plus TWRP's own UTRD/UCD/PRDT of a live read, and diff against X5.3. Read-only.
## Live Linux UFS state from TWRP (2026-10-28, `exynos/twrp-ufsdump/`)

TWRP 3.7.0 kernel (4.9.118-g36e6ddff22b9) has no /dev/mem and no UFS debugfs, but loads unsigned modules (MODVERSIONS, no force-load). `twrp-ufsdump/ufsdump.c` is a read-only module (ioremap + readl, prints "UFSDUMP" lines, init returns -ENODEV so it never stays loaded). Built in WSL against **Samsung OSRC 4.9.118** (`import G965FXXU7DTAA OSRC`, LineageOS universal9810 commit 2b92eefa; env ANDROID_MAJOR_VERSION=q, HOSTCFLAGS=-fcommon) with TWRP's /proc/config.gz; that tree reproduces TWRP's module_layout CRC 0x599efa6b (lineage-17.1 HEAD 4.9.218 does not). CRCs come from TWRP's own Image (`kcrc2.py`: KASLR image, every __ksymtab pointer and __kcrctab CRC is an R_AARCH64_RELATIVE addend; ksymtab 0xffffff8009594510, kcrctab 0xffffff80095b9050, 9396 entries): printk 0x27e1a049, __ioremap 0xf24b3dfe, __iounmap 0x45a55ec8, memstart_addr 0xfadf2436. Run: `run.sh` (one direct 4 KB read, insmod, grep dmesg). Raw dump: `backup/twrp-ufsdump-2026-10-28.txt` (git-ignored).

Linux's working state (vs X5.3):
- **UTRD dword0 = 0x04000000 for SCSI data reads: command type 0 (SCSI), DD 2, no I bit** (interrupt aggregation). S-Boot: 0x01000000 (CT 0). **X3..X5.3 used CT 1 (0x15000000) for every data command** - the only structural difference found. (Source says UFS_STORAGE for ver 0x210, but the live descriptors carry 0.)
- dw6 0x04000400 (resp offset/len 0x400 bytes, same as ours), dw7 = prdt offset 0x800 << 16 | prdt length in bytes (0x80 per 128-byte FMP entry; 0x1000 = 32 entries), entry {addr lo, addr hi, 0, len-1} - same as our type-3 layout. Command UPIU flags 0x44 (ours 0x40), tag = slot. UTRL/UCD/data above 4 GB (0x8_fce02000, data 0x8_ebb64000).
- VS: 000=8000000C, 004=C, 00C=0xA6 (Linux mclk 166 MHz; X5 derives 100 MHz from PA_DBG_CLK_PERIOD 10 -> 1US_TO_CNT 0x64, init_cfg period 10), 018=0xBEADCAFE (static), 020=0x8000, 024=0x10000, 038=0x00100000 (normal), 040=FFFF, 044=F, 054=0x60000, 060=A, 06C=98000003, 070=1, 074=3, 07C=80002020, 080=80000007, 084=80000017, 08C=00010001, 0B0=8, 0B4=DE0, 0F8=0x30, 0FC=1, 110=1, 140=2. HCI: IE 0x30EF5, HCS 0x10F, UTRIACR 0x80000F01, RSR 1. COH 0x311, PHY-PMU 0x10001 (same as ours). UFSP 010 reads 0xEFFA6492 (varies; secure). PHY window reads 0xFF everywhere from Linux too.
- **X5.4** (boot.img sha256 f2cc8064..., copy `out/exynos/boot-x5.4.img`, built clean, flashed 2026-10-28 with verified readback; result below): SCSI UTRDs with CT 0 (dw0 = DD<<25 | I), descriptor type 3 / 128-byte PRDT entries again (as Linux). Devman (NOP/query) unchanged (CT 1 works for them).
- **X5.4 result (photo, 2026-10-28): CT 0 did not help** - identical to X5.3 (READ GOOD prdt 128, buffer unchanged, IS 0x00020001, vendor IS 0x00100000, SMU abort 0, DIAG unchanged). Command type ruled out.
- Remaining structural difference vs live Linux: **buffer location**. Linux's UTRL, UCD and data buffers are all in the 2 GB bank above 4 GB (0x8_fce02000, 0x8_ebb64000); X3..X5.4 used 0x80xxxxxx. The response UPIU (descriptor engine) lands fine low in RAM, but data-in passes the FMP engine, possibly another AXI master with its own allowed window. (X3.10 only moved the buffer within the same low region.)
- **X5.5** (boot.img sha256 f01f2ef4..., copy `out/exynos/boot-x5.5.img`, built clean, flashed 2026-10-28 with verified readback; result below): = X5.4 with the 4 KB data buffer at 0x8_C000_0000 (CPU fills it with 0xA5 first, MMU off; a fault there would show the SHIM PANIC screen).
- **X5.5 result (photo, 2026-10-28): buffer above 4 GB did not help** - CPU filled 0x8_C000_0000 fine (no panic), READ GOOD, buffer unchanged, IS 0x00020001, same DIAG. Buffer location ruled out (for data; UTRL/UCD still low, and those DMAs work).
- Samsung FMP driver (fmp_dev.c / fmp_lib.c) makes only DISK_KEY_STORED/CLEAR SMCs, nothing at probe: no missing FMP init call.
- Unexplored differences from live Linux: **link in HS fast mode** (Linux's last DME_GET in HCI 0x94/0x9C: PA_PWRMODE 0x1571 = 0x11; X5 stays in PWM after link startup), UniPro mclk 166 MHz (VS 00C 0xA6) vs X5's 100 MHz, UPIU flags 0x44, VS 07C/080/084 error-enable values, VS 08C bit 0, VS 0A0/0A4 (0x00484B4B / 0x004D534B), VS 110 = 1, HCI IE 0x30EF5 / UTRIACR 0x80000F01.
- **Next (instrument, not guess):** have the shim copy its full failing host state (HCI 0x00-0xFF, VS 0x000-0x15F, UFSP, UTRD/UCD/PRDT) into a reserved RAM region that survives a warm reset, then read it in TWRP with ufsdump.ko (ioremap) and diff against the Linux dump automatically.
- **ROOT CAUSE FOUND (2026-10-28, from the TWRP dump, not yet phone-verified): UTRD dword 7 halves swapped.** UFSHCI / Linux struct utp_transfer_req_desc: dw6 = resp offset [31:16] | resp length [15:0], dw7 = **PRDT offset [31:16] | PRDT length [15:0]** (live Linux 0x08000080 = offset 0x800, length 0x80). x3.c/x5.c wrote `prdt_len << 16 | PRDT_FIELD` = 0x00800800 -> the host took its PRDT from ucd+0x80 (zeroed) with length 0x800, i.e. entry {addr 0, len 0}, and DMA'd the data to physical 0 -> IS.SBFES. dw6 is symmetric (0x04000400), which is why responses always arrived. Explains every data failure X3.5-X5.5; the clocks/SMU/FMP/tag/CT/buffer experiments were chasing this. The mailbox plan (persist the shim's state in no-map fimc_is_lib RAM 0xFFF00000, read in TWRP) is shelved unless this does not fix it.
- **X5.6** (boot.img sha256 62f142ef..., copy `out/exynos/boot-x5.6.img`, built clean, flashed 2026-10-28 with verified readback; result below): = X5.5 (CT 0, desc type 3, data buffer 0x8_C000_0000) with dw7 = PRDT_FIELD << 16 | length. Pass = READ 1 "EFI PART", GPT, partitions, BOOT self-check. x3.c has the same swap (line 189) but is superseded.
## UFS DATA PATH WORKS (X5.6, 2026-10-28, photo)

**"EXYNOS X5.6: OK".** Full host re-init (SW reset, init_host, HCE, link startup gen_err 0, hibern8 + post_init_cfg, NOP, fDeviceInit) then: TUR UA 6/29/00, SSU ACTIVE UA, TUR GOOD; **READ(10) LBA 1 GOOD, prdt 128, buffer changed 4096 of 4096 bytes from offset 0, "45464920 50415254" = "EFI PART"**, IS 0x00000001 (UTRCS only, no SBFES), vendor IS 0x00100000, SMU abort 0. READ 0: MBR sig AA55. GPT partitions: BOOT lba 0x4C02 55 MiB, RECOVERY 0x8302 64 MiB, SYSTEM 0x10C80 4400 MiB, VENDOR 0x123C80 650 MiB, ODM 0x14C680 64? MiB, CACHE 0x174C80 600 MiB ext4, USERDATA 0x19E580 54364 MiB (encrypted). **BOOT self-check: kernel 131072 = this shim.** Data buffer at 0x8_C000_0000, CT 0, FMP desc type 3, 128-byte PRDT entries, UniPro still PWM / mclk 100 MHz.

The single bug behind every data failure since X3.5 was the swapped UTRD dw7 (PRDT offset belongs in [31:16]). Things the X5.x experiments changed that turned out NOT to be needed are unknown individually; the working combination is X5.6 as committed. To carry over to the XNU Exynos UFS driver (sweet_ufs.c is Qualcomm-only): full re-init as x5.c reinit(), query function in UPIU byte 5, tag = slot, UTRD dw6/dw7 = offset<<16 | length (bytes, Exynos PRDT_BYTE_GRAN), 128-byte PRDT entries with FMPSECURITY desc type 3 (or 16-byte with type 0 - not separately tested now), UCD: cmd 0 / response 0x400 / PRDT 0x800.

**Next:** (1) quick X5.7 checks to minimise the recipe: data buffer back in low RAM, CT 1 vs 0, desc type 0, so the XNU driver can use any RAM; (2) Exynos UFS driver for XNU (patch 0106) from x5.c, read-only first, root on the existing USERDATA partition per the 2026-10-15 approval.
## XNU Exynos UFS driver: patch 0106 (2026-10-28, written, NOT compiled)

`xnu/patches/0106-exynos9810-ufs-host-bringup.patch` (bsd/dev/sweet_ufs.c, on top of 0001-0105; `git apply --check` OK on the Windows-side tree). One driver, two hosts: `/sweet-ufs` compatible `samsung,exynos9810-ufs` selects the Exynos path, the Qualcomm path is byte-for-byte the same requests as before (only `bzero/dma_sync` sizes now come from `su.ucd_size`).
- Exynos bring-up = x5.c reinit(): PHY-PMU isolation bypass, io-coherency 0x300, SMC FMP_SECURITY(0,0,desc 3) + SMU_INIT, HCI_SW_RST, init_host + Linux clock/ACG values, GPIO_OUT reset_n, HCE, MCLK from PA_DBG_CLK_PERIOD (fallback 166.5 MHz), lanes, init_cfg, DME_LINK_STARTUP (gen_err from UniPro 0x7854, HCS.DP required), hibern8 calib, post_init_cfg; then ufs_take_over_host (also UTMRL on Exynos), NOP OUT -> NOP IN, SET_FLAG/READ_FLAG fDeviceInit (CT 1, NEXUS bit cleared for dev-mgmt); wake = TUR, SSU ACTIVE to W-LUN 0xD0 after the 2nd failure, up to 6 TURs.
- Exynos requests: CT 0, tag 0 (= slot 0), resp 0x400<<16|0x400, PRDT at 0x800, 128-byte entries of 4 KiB (256 for the 1 MiB bounce), dw7 = 0x800<<16 | n*128. DMA layout: UTRL 0, UTMRL 0x800, UCD 0x1000 (0x8800), bounce 0xC000. ufscache forced off. Default partition "USERDATA"; read-only unless ufsrw=1 (unchanged). Failures print the sense key/ASC/ASCQ. On an Exynos attach error the keys/touch drivers are still started.
- `exynos/mkadt.py` now emits /sweet-ufs {compatible samsung,exynos9810-ufs, reg <0x11120000 0x8000>}.
- **Not yet verified:** compiles (needs the macOS CI); DMA buffers come from kmem_alloc_contig in the 0x92000000 window (low RAM) - X5.6 proved data DMA only into 0x8_C000_0000, low RAM failed before only because of the dw7 bug (likely fine, unproven).
- **First phone test (read-only):** payload with this kernel + new ADT, cmdline as X4.3 plus `rd=ufsroot0 ufsroot=CACHE` (or USERDATA): expected "SWEET UFS: Exynos link up: mclk 100000000 Hz, 2 lanes, hcs 0xf" and "SWEET UFS: "CACHE" on LUN 0: first block 0x174c80, 600 MiB, read-only, DMA at 0x9...", then a root mount failure (ext4/encrypted, no HFS image yet). Next after that: an iOS HFS root image into USERDATA (approved 2026-10-15; confirm before writing).- **0106 compiled** (pushed main 01c371d..9cac44c to xbx, run 40774833689, success; only pre-existing style warnings in sweet_ufs.c): `out/exynos/xnu3/kernel.development.sweet` 17266040 bytes, sha256 363bf8b8d1d19066... ADT `out/exynos/exynos-ufs.adt` (7232 bytes, with /sweet-ufs). Payload `out/exynos/payload-ufs1.bin`, cmdline `debug=0xc serial=3 cpus=4 keepsyms=1 -v rd=ufsroot0 ufsroot=CACHE sweettouch=0 sweetusb=0 sweetbattery=0 sweetcpu=0` (no RAM disk). **Boot image `out/exynos/boot-ufs1.img`** (x4 shim, 42543104 bytes, sha256 cdd9c4c9...), flashed 2026-10-29 with verified readback; result below. Read-only test: expect the "Exynos link up" and "\"CACHE\" on LUN 0" lines, then a root mount failure (CACHE is ext4).
## XNU READS THE S9's UFS (boot-ufs1, 2026-10-29, photo)

0106 works on the first boot. Console: 4/4 CPUs, MAC/AMFI/Sandbox, "SWEET HFS: hfs-627.100.6 registered", dlil, then **"SWEET UFS: Exynos link up: mclk 100000000 Hz, 2 lanes, hcs 0xf"**, "SWEET UFS: command 0x00 failed: ocs 0x7 upiu 0x21 response 0x1 status 0x2 sense 6/29/00" (the expected power-on UA on the first TUR; retry passed), **"SWEET UFS: \"CACHE\" on LUN 0: first block 0x174c80, 600 MiB, read-only, DMA at 0x95724000"** (DMA in low RAM works: the X5.6 caveat is gone), "SWEET KEYS: Exynos gpa0=0x5e gpa2=0x14 (keys are active ...)", "SWEET TOUCH: disabled", "BSD root: ufsroot0, major 1, minor 0", "hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 96", "hfs_mount: hfs_mountfs returned error=22", "cannot mount root, errno = 19", mount(N) retry loop - as expected: HFS read CACHE's sector at 1024 through the driver and found ext4. "SWEET KEYS: Exynos GPIO polling on cpu 0" also seen.

**Next: an iOS 16 HFS root on USERDATA** (approved 2026-10-15: iOS root in the existing userdata partition, Android may be deleted, no GPT change; confirm the destructive step first). USERDATA = lba 0x19E580, 54364 MiB (4 KiB blocks), currently Android's encrypted f2fs/ext4. Flash path from TWRP: `dd` of the raw HFS image (or simg2img-expanded) to /dev/block/sda25 (by-name USERDATA), or `fastboot`-less: TWRP adb push in chunks. Image to use: the sweet's v15 root (`ios16-root-20H392-v15.img`, 16 GiB, lives on the Linux machine, not on this PC) - it carries sweet-specific plist edits (CommCenter removed, Setup skipped, activation seeded, network prefs) that are device-neutral enough to try. Then boot with `rd=ufsroot0` (default ufsroot=USERDATA), first read-only (`rootrw=0`, no `ufsrw`), then `ufsrw=1 rootrw=1` for launchd.
## iOS 16 root image for the S9, built on Windows/WSL2 (2026-10-29)

- IPSW members (iPhone10,3/10,6 16.7.16 20H392, `tools/remotezip.py`, CRC-32 ok) in `F:\ios-s9-ipsw\20H392`: 087-86698-044.dmg (OS, 4730814044), 087-86593-047.dmg (SystemOS, 2743074816), 087-86715-047.dmg (AppOS, 10485760). Not in git.
- WSL2 kernel 6.18.54.1-microsoft-standard-WSL2 has no HFS+: `hfsplus.ko` built from microsoft/WSL2-Linux-Kernel tag linux-msft-wsl-6.18.54.1 with the running /proc/config.gz + CONFIG_HFSPLUS_FS=m (`LOCALVERSION=` set so the shallow clone gets no "+"; full vmlinux for the MODVERSIONS CRCs, then in-tree `make modules`; `M=fs/hfsplus` does not work in 6.18). `~/wslk/k/fs/hfsplus/hfsplus.ko`, vermagic matches. apfs-fuse built from sgan81/apfs-fuse into ~/.local/bin (not packaged on Ubuntu 24.04); apt: hfsprogs rsync + kernel build deps (flex bison bc libelf-dev libssl-dev dwarves cpio) + apfs-fuse deps.
- `s9/exynos/mkroot-wsl.sh` (insmod + tools/mkiosroot.sh + tools/sweet-root-tweaks.py + fsck + sha256). Building on /mnt/f (9p) is slow (NTFS zero-fills the 16 GiB file, disk at 100%) but works.
- **Result: `F:\ios-s9-ipsw\ios16-root-20H392-s9.img`**, 16 GiB case-sensitive journaled HFS+ "iOSRoot", 12 GB used, fsck OK, **sha256 0b9c631bbc1277a4890f2aad7669066255883e7ffad7c1ff5e944c46019b7ebb**. Tweaks: 7 _PanicOnCrash renamed, 2 CommCenter jobs removed, containers dirs, VirtualAudio off, StartRecorderServer off (D22/D221/Default), migration/SpringBoard/purplebuddy/managedconfiguration prefs. **Warning: no mobileactivationd system container in a fresh image -> data ark skipped** (the sweet's v14 had one from a booted phone; activation itself comes from the ADT /chosen security properties). Fresh image = no MobileGestalt cache, no prior containers.
- Next: write it to USERDATA (sda25, by-name USERDATA; DESTRUCTIVE, confirm with the user), then boot 0106 with `rd=ufsroot0` (default ufsroot=USERDATA), read-only first.- **USERDATA now holds the iOS root (2026-10-29, user-approved; Android user data erased).** `s9/exynos/write-userdata.py` (TWRP adb exec-in is NOT binary-safe: a 1 MiB random test came back with another sha256; so 256 MiB chunks: adb push to /tmp, dd seek into sda25, per-chunk readback sha256), 64/64 chunks ok, **whole-range readback of sda25 = 0b9c631bbc1277a4890f2aad7669066255883e7ffad7c1ff5e944c46019b7ebb = the image**. sda25 = 57004785664 bytes; only its first 16 GiB are the HFS+ volume.
- **Boot image `out/exynos/boot-ios1.img`** (sha256 29bf8f93...): 0106 kernel (`xnu3`) + `exynos-ufs.adt` + cmdline `debug=0xc serial=3 cpus=4 keepsyms=1 -v rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweettouch=0 sweetusb=0 sweetbattery=0 sweetcpu=0` (read-only: no ufsrw/rootrw). Flashed 2026-10-29 with verified readback; result below. Expected: "\"USERDATA\" on LUN 0 ... read-only", HFS mount of iOSRoot, launchd starts (read-only root: daemons that write /var will fail; next boot adds `ufsrw=1 rootrw=1`).- **boot-ios1 result (photo, 2026-10-29): HFS recognises the iOS volume** (no "unknown Volume Signature" any more) but the mount loops on "hfs: Initializing the journal (joffset 0x82000 sz 0x800000)... jnl: do_jnl_io: strategy err 0x1e (EROFS from sweet_ufs, read-only) -> write_journal_header: error writing the journal header -> hfs_late_journal_init returned 22 -> cannot mount root, errno = 19". Linux mkfs.hfsplus -J leaves the journal needing initialisation, so XNU must write it even for a read-only root; the sweet always booted with ufsrw=1 rootrw=1.
- **boot-ios2** (`out/exynos/boot-ios2.img`, sha256 ee53a3e5..., flashed 2026-10-29 with verified readback; result below): boot-ios1 + `ufsrw=1 rootrw=1`. Writes are bounded by sweet_ufs to USERDATA (the iOS volume + the sweetlog area in the partition's last 4100 KiB, readable from TWRP with tools/sweet-log-pull.sh adapted to sda25).- **boot-ios2 result (2026-10-29): iOS ROOT MOUNTED READ-WRITE, launchd runs its boot tasks.** Screen: "created HFBT on iOSRoot", sampler "SMP -- sample 0". Persistent log pulled from TWRP (sda25 last 1025 blocks via dd to /tmp + adb pull; `s9/logs/boot-ios2.txt`, git-ignored; 20191 bytes, flush #10): mount_hfs, "/dev/ufsroot0 on / (hfs, local, journaled, noatime)", BindFS lookups of /private/preboot/<hash>/usr/standalone/firmware, /private/var/hardware/Pearl/..., /private/var/hardware/FactoryData/... fail (error 2, harmless), launchd boot tasks data-protection ("No SEP present"), finish-obliteration ("No obliteration needed"), detect-installed-roots, commit-boot-mode, restore-datapartition (optional, absent), mount-phase-2 ("data volume missing, but not required"), init-with-data-volume, MSUEarlyBootTask - last line at launchd time 00:07:54 + "SMP --- sample 3" (no SpringBoard/backboardd yet). No panic, no crash loop. **Very slow**: 15-30 s per boot task (each exec pages in the shared cache over the polled UFS path). User rebooted to TWRP after ~10 min. Next: let it run 25+ min, then pull the log again; afterwards look at UFS throughput (1 MiB bounce, 4 KiB PRDT entries, polled; sweet's 0058-style ufscache/ufswait knobs are Qualcomm-only).- **boot-ios2, second run (2026-10-29, log `s9/logs/boot-ios2b.txt`, 49206 bytes, flush #35): iOS USERLAND UP TO backboardd + SpringBoard, 0 CRASH lines, no panic.** Early boot tasks done by launchd time ~00:03:40 (2x faster than the first boot, which also initialised the journal). Then cc_fips_test, keybagd, usermanagerd, dirs_cleaner, xpcroleaccountd, init_featureflags, auearlyboot, tzinit, FinishRestoreFromBackup, sysstatuscheck, prng_seedctl, launchd_cache_loader, ~60 xpcproxy, and daemons incl. logd, configd, lockdownd, notifyd, runningboardd, containermanagerd, mediaserverd, amfid, locationd, bluetoothd, fairplayd.H2, tccd, thermalmonitord, IOMFB_FDR_Loader, healthd, sharingd, **backboardd[57]** (slide 0x12b60000, mh 0x104e1c000) and **SpringBoard[27]** (mh 0x104ab8000), timed, misd, powerd. Sampler: SpringBoard t0 pc 0x1b7173d08 (same pc/bt in samples 2 and 3: blocked, probably mach_msg waiting on backboardd); backboardd t0 busy in 0x192c2xxxx (sample 3). No "SWEET FB: backboardd" / IOMFB selector lines yet -> display not opened yet, panel shows only the console. Keys reach the kernel (SWEET KEYS key=2/0/3 = vol-, power, Bixby; SWEET HID "0 clients": backboardd had not opened HID yet). The user rebooted at sample 3 (~7.5 min). Next: a longer run (20-30 min, no key presses), then pull the log; symbolicate the SMP addresses against the 20H392 dyld cache (slide 0x12b60000) to see what backboardd/SpringBoard wait for.
## UFS speed: patch 0107 HS-G3 (2026-10-29, written, not compiled)

Why boots are slow: after link startup the link stays in **PWM-G1** (a few Mbit/s per lane); Linux switches to HS. TWRP's live HCI showed its last DME_GET PA_PWRMODE (0x1571) = 0x11 (FAST both ways). exynos9810.dtsi: ufs,pmd-attr-gear 3, lane 2; ufs-exynos.c exynos_ufs_get_pwr_mode: FAST_MODE, PA_HS_MODE_B. Sequence (ufshcd_change_power_mode + exynos_ufs_pwr_change_notify + ufs-cal-9810.c):
- pre_pmc `calib_of_hs_rate_b`: DME 0x2041=8064 0x2042=28224 0x2043=20160 0x15B0=12000 0x15B1=32000 0x15B2=16000; same values to UniPro regs 0x7888/0x788C/0x7890/0x78B8/0x78BC/0x78C0 (UNIPRO_DBG_APB = plain register writes); per-lane PMA 0xC8=0xBC 0xF0=0x7F 0x120=0xC0, G3: 0x128=0x00 0x134=0x63.
- DME_SET PA_RXGEAR 0x1583, PA_ACTIVERXDATALANES 0x1580, PA_RXTERMINATION 0x1584=1, PA_TXGEAR 0x1568, PA_ACTIVETXDATALANES 0x1560, PA_TXTERMINATION 0x1569=1, PA_HSSERIES 0x156A=2; then DME_SET PA_PWRMODE=0x11, wait IS.UPMS (bit 4), UPMCRS from UniPro 0x78EC (GET_UPMCRS_DIRECT) must be PWR_LOCAL (1).
- post_pmc: per lane, PMA 0x1FC bit 0x40 (CDR/AFC lock), up to 100 x (40 us, check, kick 0xF0=0x7F then 0xFF).
`xnu/patches/0107-exynos9810-ufs-hs-gear3.patch` (on 0106): exy_power_mode() after the partition is found; gear = min(local PA_MAXRXHSGEAR, peer PA_MAXRXHSGEAR, 3); boot-arg ufshs=0 skips it; ufsbench (default 1) prints "SWEET UFS: PWM-G1: read 4 MiB in N ms (X KiB/s)" before and "HS: read 64 MiB ..." after; a TUR afterwards decides whether the root attach continues. Work tree note: the old session's xnuw/.git/refs had vanished (temp cleanup) - recreated, HEAD d9958c8 (0105) -> 3a6e015 (0106) -> d743ce9 (0107).
- **0107 compiled** (push 9cac44c..2f07ccc to xbx, run 40792590208): `out/exynos/xnu4/kernel.development.sweet` sha256 47846a1f816116363bfd... **Boot image `out/exynos/boot-ios3.img`** (sha256 20a5bad8...) = boot-ios2 cmdline + 0107 kernel. Flashed 2026-10-29 02:24 with verified readback; result below. Expect 'SWEET UFS: PWM-G1: read 4 MiB ...', 'SWEET UFS: power mode HS-G3 rate B, 2 lanes (PA_PWRMODE 0x11)', 'SWEET UFS: HS: read 64 MiB ...'.

- **boot-ios2, 25-min run (log `s9/logs/boot-ios2c.txt`, 41438 bytes, flush #41): 0 CRASH, no panic; backboardd + SpringBoard alive through sampler passes 0-6.** backboardd's stack changes every sample (working: e.g. 0x1e9e9bce0, 0x1e9e48cd8, 0x1c58f7180 - this boot's cache slide differs from ios2b), SpringBoard waits at the same pc 0x1e9e43d08 from sample 2 to 4, then in 0x1c58f7180 like backboardd at 5. Still no 'SWEET FB: backboardd' (display not opened). Next: boot-ios3 (0107 HS-G3) to cut the I/O time, then symbolicate.
- **boot-ios3 result (2026-10-29, log `s9/logs/boot-ios3.txt`, 127235 bytes, flush #56): HS-G3 WORKS, ~1470x faster.** "SWEET UFS: PWM-G1: read 4 MiB in 8133 ms (503 KiB/s)", "SWEET UFS: power mode HS-G3 rate B, 2 lanes (PA_PWRMODE 0x11)", "SWEET UFS: HS: read 64 MiB in 88 ms (740625 KiB/s)" (~723 MB/s), then "\"USERDATA\" on LUN 0: first block 0x19e580, 54364 MiB, read-write, DMA at 0x95724000". **Apple logo on the panel**: backboardd opens AppleCLCD (sels 57,15,22,29,33,79,74,12,6,27,17,84,50,68; "SWEET FB: display on"), IOSurface create id 2 1125x2436 BGRA bpr 4608, "SWEET FB: swap 1 layer 0 surface 2 1125x2436" - the screen is **com.apple.datamigrator**'s first-boot data migration (as on the sweet; backboardd holds SpringBoard until it ends). Only crash: driverkitd sig 9 (harmless). SpringBoard[26] multi-threaded waiting; HID now has 1 client ("SWEET HID: key 0x30 down (1 clients)": keys reach backboardd). Loop of xpcproxy/bluetoothd/fairplayd.H2 relaunches (no BT hardware / no SEP, as on the sweet). User rebooted after ~10 min of logo. Next: a long run (30+ min) to let the one-time migration finish, then expect SpringBoard UI (Setup is pre-skipped).- **boot-ios3, 2nd run (log `s9/logs/boot-ios3b.txt`): the data migration FINISHED; SpringBoard now launches and aborts (8x, sig 6), PosterBoard crashes 4x (sig 5).** SpringBoard stderr (/dev/null tee) + heap scan: `*** Assertion failure in -[_UIScreenComplexBoundingPathUtilities _loadBitmapForScreen:type:], _UIScreenComplexBoundingPathUtilities.m:326` / `NSInternalInconsistencyException, reason: 'Failed to load bounding path bitmap data from the asset manager for asset name: BoundingPathBitmap-1125x2436-375x812-3.00x'` (the iPhone X notch/corner outline). Cause: the S9 ADT had **no /product node** (mkadt warned `research/ipsw-x/d22ap.adt missing`), so MobileGestalt has no artwork traits (device subtype 2436 etc.) for the asset lookup.
- Fix: `Firmware/all_flash/DeviceTree.d22ap.im4p` (33719 bytes, CRC ok, remotezip) -> IM4P payload is LZFSE (`bvx2` at offset 59) -> decoded with an `lzfse` CLI built from apfs-fuse's bundled 3rdparty/lzfse into ~/.local/bin -> `s9/research/ipsw-x/d22ap.adt` (186084 bytes, git-ignored). `s9/tools/im4p-payload.py` extracts uncompressed IM4P payloads (refuses bvx2). ADT now 9916 bytes with /product: product-name 'iPhone X', artwork-device-subtype 0x984, artwork-scale-factor 3, artwork-device-idiom phone, artwork-display-gamut P3, graphics-featureset-class APPLE4. **S9 ECID** /chosen/unique-chip-id = 0x5357454554009810 (was the sweet's ...0001): new IOPlatformUUID so MobileGestalt's on-disk cache (keyed by build + UUID) is recomputed with the new traits.
- **boot-ios4** (`out/exynos/boot-ios4.img`, sha256 36861ce0...): 0107 kernel (xnu4) + that ADT + boot-ios3 cmdline. Flashed 2026-10-29 02:56 with verified readback; result below.
## iOS 16 LOCK SCREEN ON THE GALAXY S9 EXYNOS (boot-ios4, 2026-10-29 ~03:00)

**MILESTONE: SpringBoard reached the lock screen** (reported by the user; no photo yet because the kernel console is drawn over it). The D22AP /product node + new ECID fixed the BoundingPathBitmap assertion. Known gaps: **no touch** (sweettouch=0; sweet_touch.c is the sweet's Goodix GT9896 on Qualcomm GENI I2C and returns early on Exynos - the S9 needs its own driver for its touch controller on Exynos I2C), console text over the GUI (sweetfb_noconsole=1 pauses it at the first swap, as on the sweet). Keys reach backboardd (power/volume/Bixby).
- **boot-ios5** (`out/exynos/boot-ios5.img`, sha256 125deb8f...): boot-ios4 + `sweetfb_noconsole=1` for a clean photo. Flashed 2026-10-29 03:01 with verified readback.
- Next: photo; then the S9 touch driver (identify the controller and its I2C bus/GPIOs from the stock rev26 DT in backup/exynos-boot-stock.img; LineageOS universal9810 drivers/input/touchscreen); battery/USB later.- **PHOTO (boot-ios5, 2026-10-29 ~03:05): clean iOS 16 lock screen on the Galaxy S9** (`s9/logs/MILESTONE-s9-ios16-lockscreen-2026-10-29.webp`, git-ignored): "Wednesday, December 31" / "4:02" (no RTC/network time: clock counts from the 1970 epoch, shown in a US timezone), charging battery icon (SweetBattery static fallback), camera button, home indicator (iPhone X layout from the D22AP /product traits). White text with a red outline - to check whether that is the lock-screen style or a colour-channel/format issue in the AppleCLCD -> DECON panel copy (compare with a known-colour UI). boot-ios4 log: only crash driverkitd (SpringBoard/PosterBoard no longer crash).
- **Next (in order):** (1) S9 touchscreen driver (stock rev26 DT in backup/exynos-boot-stock.img: controller, I2C bus, IRQ GPIO, power; LineageOS universal9810 drivers/input/touchscreen) feeding sweet_hid_touch; (2) RTC / time; (3) colour check; (4) battery (MAX77705 fuel gauge), USB.
## Touchscreen (2026-10-29): S6SY761 on HSI2C, probe stage X6

From the stock rev26 DT (`s9/tools/fdtgrep.py` on the dt section of backup/exynos-boot-stock.img: 12 DTBs, rev16/17/18/20/23/26 x2; ours = rev26) and LineageOS universal9810 drivers (i2c-exynos5.c, sec_ts/sec_ts.c/.h, pinctrl-exynos.c):
- **Controller: Samsung LSI S6SY761** (`/hsi2c@104B0000/touchscreen@48`, compatible sec,sec_ts, firmware tsp_sec/y761_star1.fw, project "Star1 / G960"), **I2C 0x48**, sec,max_coords <4096 4096>, irq_type 0x2008 (trigger low, oneshot).
- **Bus: HSI2C 0x104B0000** (samsung,exynos5-hsi2c, usi-i2c-v2, default-clk 200 MHz, reset-before-trans, stop-after-trans, scl-clk-stretching); pins: **WRONG at first** - I took hsi2c26-bus (GPG3-3/4, PERIC1, another bus) by name; the node's own pinctrl-0 (phandle 0x68) is **hsi2c10-bus = GPP1-4/5 function 2, pud 0, drv 0** in PERIC0 pinctrl 0x10430000 (gpp1 bank +0x020: CON 0x10430020, DAT +4, PUD +8); gpio_scl/sda = gpp1 4/5. Fixed in X6.3.
- **IRQ: GPA1-0** in ALIVE pinctrl 0x14050000 (gpa1 bank +0x040: CON 0x14050040, DAT 0x14050044 bit 0), active low.
- **Power: S2MPS18 LDO35 tsp_io 1.8 V, LDO43 tsp_avdd 3.0 V**, both regulator-boot-on (PMIC via SPEEDY 0x141C0000 - avoided for now).
- Protocol (sec_ts.c): reg read = write 1 byte cmd, read N (stop between, fine with stop-after-trans). 0x60 READ_ONE_EVENT (8 bytes), 0x61 all, 0x62 CLEAR_EVENT_STACK, 0x52 READ_ID, 0x55 BOOT_STATUS (0x20 app mode), 0x22 DEVICE_ID, 0x10 SENSE_ON. Event byte0: eid bits0-1 (0 coord, 1 status, 2 gesture, 3 empty), tid bits2-5, tchsta bits6-7; X = b1<<4 | b3>>4, Y = b2<<4 | (b3 & 0xF); action 1 press, 2 move, 3 release; b7 bits0-5 = events left. sec_ts_wait_for_ready waits for a status event (INFO/VENDOR) with ACK 0x00 (boot complete).
- HSI2C (polling mode, i2c-exynos5.c): USI_CON(0xC4)=0 releases USI reset; CTL SW_RST pulse; FS timing from ipclk (div = ipclk/(op*15) in FS3[23:16], FS2/FS1 masks); CTL=MASTER, TRAILIG=0xffffff, CONF|=AUTO_MODE; per message: FIFO_CTL, CTL RX/TXCHON, AUTO_CONF READ_WRITE(16)/STOP(17)/len, ADDR[16:10]=addr, MASTER_RUN(31); poll FIFO_STATUS RX_EMPTY(24)/TX level, INT_STATUS XFER_DONE(7)/NODEV(10), TRANS_STATUS NO_DEV(3)/NO_DEV_ACK(2).
- **X6** (`exynos/x6.c`, `build.sh x6`, boot.img `out/exynos/boot-x6.img` sha256 de3f3f19..., built clean, flashed 2026-10-29 03:11 with verified readback; result below): dumps GPG3/GPA1/HSI2C as S-Boot leaves them (a gated HSI2C clock would show SHIM PANIC with FAR 0x104B00xx), sets GPG3-3/4 func 2, resets/programs HSI2C at 100 kHz (assuming 200 MHz ipclk), reads event/ID/boot status/device ID, CLEAR + SENSE_ON, then shows live events/touches for 60 s (polls GPA1-0 every 0.5 ms). Unknowns it answers: HSI2C clock gate, touch rails on after S-Boot, coordinate range.- **X6 result (photo, 2026-10-29):** no panic (HSI2C registers readable). Before: GPG3 CON 0x01000000 PUD 0x00111111, GPA1 CON 0 DAT 0x10 (**touch IRQ already low**: the chip is powered and has events pending), HSI2C CTL 0 CONF 0 **USI_CON 1** FIFO/TRANS 0. After setup: GPG3 CON 0x01022000 (pin write stuck) but **CTL and CONF still 0: HSI2C ignored the writes**; every transfer rc 2 (timeout) with INT/TRANS status 0; 575 attention polls in 40 s, irq stays 0.
- Cause (USI-v2, Documentation usi_v2.txt): each USI port is UART/SPI/HSI2C and takes its protocol from a SYSREG word (I2C_SW_CONF = 1<<2, SPI 1<<1, UART 1<<0). spi/uart/hsi2c@104B0000 = PERIC0 USI port 3 -> stock DT **usi@1041101C usi_v2_mode = "i2c"** (usi_N at 0x10411004 + 8N: 0x10450000 N=0 ... 0x104B0000 N=3). S-Boot does not use this bus, so the mode is never set.
- **X6.1** (`out/exynos/boot-x6.1.img`): prints SYSREG 0x1041101C, writes 4 (I2C), then the X6 sequence. Flashed 2026-10-29 03:14 with verified readback; result below.- **X6.1 result (photo):** SYSREG 0x1041101C **0x00000000 -> 0x00000004**; now the HSI2C takes its setup (CTL 0x00000008 MASTER, CONF 0x980120FF, FS3 0x00850000) but every transfer still times out: INT_STATUS 0, **TRANS_STATUS 0x000A0003** (bit 17 MASTER_BUSY, master state stuck) -> register (APB) clock runs, the USI03 operating clock does not. IRQ (GPA1-0) stays low.
- USI03 clocks (DT clocks <0xe0 0xcd> = USI03 (CLK_PERIC0_BASE 200 + 24) and GATE_USI03 (200 + 5); clk-exynos9810.c: USI03 = DIV_CLK_PERIC0_USI03_USI, GATE_USI03 = HWACG gate on USI03_USI_QCH). Chain (cmucal-node.c) MUX_CLKCMU_PERIC0_IP_USER -> GATE_CLK_PERIC0_USI03_USI -> DIV_CLK_PERIC0_USI03_USI -> GOUT_..._USI03_USI_IPCLKPORT_IPCLK (+ RSTnSYNC gate). CMU_PERIC0 = 0x10400000 (cmucal-sfr.c): MUX IP_USER +0x120 (bit 4 sel), DIV +0x1810 (bits 0-3), GATE +0x2018, RSTnSYNC gate +0x206c, IPCLK gate +0x20dc (gates: bit 20 MANUAL, 21 CG_VAL, 28 auto), QCH_CON_USI03_USI_QCH +0x3038 (0 ENABLE, 1 CLOCK_REQ, 2 IGNORE_FORCE_PM_EN).
- **X6.2** (`out/exynos/boot-x6.2.img`, sha256 b4ed7597...): prints those 6 CMU registers, forces the 3 gates on (MANUAL|CG_VAL, auto off), retries READ_ID ("TRY 1"); if that fails also QCH ENABLE=0, CLOCK_REQ=1, IGNORE_FORCE_PM=1 and retries ("TRY 2"); then the X6 sequence. Flashed 2026-10-29 03:17 with verified readback; result below.- **X6.2 result (photo):** CMU as S-Boot left it: MUX IP_USER 0x00000010 (sel = PLL user clock), DIV 0x00000000, GATE / RSTnSYNC / IPCLK 0x00200000 (CG_VAL already 1), QCH 0x003F0002 (ENABLE 0, CLOCK_REQ 1) -> **clocks were already on**; forcing gates (TRY 1) and QCH 0x003F0006 (TRY 2) changed nothing, still MASTER_BUSY 0x000A0003. Real cause: **wrong pins** (see the corrected pins entry above): the stage had muxed GPG3-3/4 (hsi2c26) instead of the controller's GPP1-4/5 (hsi2c10), so SCL/SDA were not connected. (GPG3 PUD 0x00111111 = pull-downs on that unrelated bank; X6..X6.2's GPG3 writes were not persistent.)
- **X6.3** (`out/exynos/boot-x6.3.img`, sha256 a895dc29...): GPP1-4/5 -> function 2, PUD cleared for both, GPG3 untouched; CMU registers printed only. Flashed 2026-10-29 03:20 with verified readback; result below.- **X6.3 result (photo):** GPP1 CON 0x00000000 -> **0x00220000**, PUD 0x11111111 -> **0x11001111** (right pins now muxed, pulls off), USI mode 4, CTL/CONF set, clocks as before - and still every transfer MASTER_BUSY (TRANS_STATUS 0x000A0003, INT_STATUS 0); IRQ still low.
- **X6.4** (`out/exynos/boot-x6.4.img`, sha256 4738a7be...): measures instead of guessing: (1) **LINES** = SCL/SDA (GPP1-4/5 as inputs) with no pull and with pull-up - low means the bus is held/unpowered, (2) **BITBANG** = open-drain GPIO I2C (~50 kHz) READ_ID 0x52 and BOOT_STATUS 0x55 from 0x48 (rc 1 = no ACK on address; ACK + bytes = chip and wiring fine, only HSI2C at fault; bit-banging is also a usable fallback for the XNU driver), then (3) HSI2C again with the right pins **and** the CMU gates/QCH forced on. Flashed 2026-10-29 03:24 with verified readback; result below.- **X6.4 result (photo): THE TOUCH CHIP IS UNPOWERED.** LINES "scl/sda no pull 0/0, pull-up 0/0": SCL and SDA read low even with the SoC pull-ups on; bit-bang "rc 0 00 00 00" is meaningless (SDA stuck low = every ACK/data bit 0); HSI2C with right pins + forced clocks still MASTER_BUSY; touch IRQ (GPA1-0) low since boot. An unpowered I2C slave clamps SCL/SDA/IRQ to ground through its ESD diodes -> **S2MPS18 LDO35 (tsp_io 1.8 V) / LDO43 (tsp_avdd 3.0 V) are off after S-Boot** despite regulator-boot-on. All X6 boots came from TWRP via `adb reboot` (Linux had the rails on), so the rails do not survive the reboot through S-Boot.
- PMIC access: stock DT `speedy@141C0000/s2mps18mfd@00` (acpm-ipc-channel 2, i2c-speedy-address). s2mps18_core.c reads/writes **every** PMIC register through ACPM IPC (exynos_acpm_read_reg/write_reg -> APM firmware does the SPEEDY transfer). Registers (s2mps18-private.h): PMIC page = SPEEDY address 0x01; **L35CTRL 0x6C, L43CTRL 0x74**, VSEL bits 0-5 (S2MPS18_LDO_VSEL_MASK 0x3F), enable bits 6-7 (ENABLE_SHIFT 6, mask 0xC0). i2c-speedy.c: SPEEDY regs CTRL 0x000, FIFO_CTRL 0x004, CMD 0x008 (BURST_LENGTH, ADDRESS(x)<<7, RANDOM 1<<19, WRITE 1<<20), INT_STATUS 0x010 (TRANSFER_DONE bit 0, timeouts 1-3), FIFO_STATUS 0x030, TX 0x034, RX 0x038; plus an **IP batcher with a hardware APB semaphore shared with the APM** (IPBATCHER_CON 0x500 ... BATCHER_OPCODE 0x600, payload 0x1000-0x1060, SEMA_REL 0x200) - the AP must take the semaphore / use the batcher so it never collides with the APM's own PMIC (DVFS) traffic.
- **Decision pending (user):** powering touch needs PMIC writes (LDO35/LDO43 VSEL + enable). Plan: X6.5 read-only over SPEEDY via the batcher semaphore (PMIC ID reg 0x00 on page 0x01 as a protocol check, L35CTRL/L43CTRL), then, with the user's OK, write exact VSEL values (from s2mps18.c regulator descs) + enable, then re-run the touch probe.- **SPEEDY read-only probe: design (not written yet, 2026-10-29).** Linux's i2c-speedy.c transfer starts with `return 0;` (all real PMIC traffic goes through ACPM), but its batcher code is the AP-side path that arbitrates with the APM through the hardware semaphore. One register read = set_batcher_idle (IPBATCHER_CON 0x500 = 0); set_batcher_enable (|= BATCHER_ENABLE | DEDICATED_BATCHER_APB); batcher table entries via write_batcher (opcode byte = SPEEDY register offset, packed 4 per word at BATCHER_OPCODE 0x600 + 4*n, initial word 0xFFFFFFFF; payload at 0x1000 + 4*k): SPEEDY_CTRL 0x30051, FIFO_CTRL (FIFO_RESET | RX_TRIG 1 | TX_TRIG 1), INT_STATUS 0xFFFFFFFF, INT_ENABLE (timeouts cmd/standby/data | RX_ALMOST_FULL | RX_TRAILER | modebit/glitch/endbit errs), CMD (ADDRESS(a)<<7 | RANDOM 1<<19 | READ), RX_DATA (payload 0x77, its slot receives the byte), finalize (opcode 0xFF payload 0); IPBATCHER_INT_EN 0x508 = 1; CON |= START_BATCHER (1<<4); poll IPBATCHER_STATE 0x504 for OPERATION_COMPLETE (bit 0) / UNEXPECTED_IP_INTR (bit 1), IP_INT_STATUS 0x520 for errors, 500 ms; write bit 0 back to clear; read the RX payload slot; CON = 0. Probe once: batcher_swreset (CON SW_RESET 1<<31 pulse) + program_batcher_fsm (FSM_UNEXPEN 0x50C, FSM_TXEN 0x510 = DONE|TX_ALMOST_EMPTY, FSM_RXFIFO 0x514 = FIFO_EMPTY, FSM_CON 0x518 = DISABLE_STOP_CMD). SPEEDY addresses are 12-bit (i2c-core I2C_CLIENT_SPEEDY); assumed = (PMIC page << 8) | reg, i.e. PMIC ID 0x100, L1CTRL 0x149 (should read enabled, a check of the encoding), **L35CTRL 0x16C, L43CTRL 0x174** - verify with the ID/L1 reads before trusting it. Next: X6.5 = these reads only (no PMIC writes), then ask the user before writing VSEL+enable.
- **State at stop:** BOOT partition holds X6.4 (touch probe); restore iOS with `out/exynos/boot-ios5.img` (lock screen, no touch). USERDATA = iOS root (unchanged).- **X6.5** (`out/exynos/boot-x6.5.img`, sha256 3a639af1...): adds a **read-only** PMIC section before the touch steps: prints SPEEDY_CTRL / IPBATCHER_CON / IPBATCHER_STATE as S-Boot left them, does the probe-time batcher reset + FSM programming, then single batcher reads of 0x100 (PMIC ID), 0x149 (L1CTRL, expect enable bits 6-7 set), 0x16C (L35CTRL), 0x174 (L43CTRL); a failed read prints timeout/error with IPBATCHER_STATE and IP_INT_STATUS. No PMIC writes. The X6.4 LINES/BITBANG/HSI2C steps follow unchanged. Flashed 2026-10-29 03:41 with verified readback; result below.- **X6.5 result (photo): SHIM PANIC before the first PMIC line - ESR 0x96000010 (data abort, synchronous external abort), FAR 0x141C0500 = SPEEDY + IPBATCHER_CON.** The AP cannot even read the SPEEDY IP batcher (secure-only / not routed to the AP), consistent with the vendor kernel never using it (i2c-speedy.c xfer returns 0) and sending every PMIC access through **ACPM IPC** to the APM firmware (s2mps18_core.c exynos_acpm_read_reg/write_reg, DT acpm-ipc-channel 2). No PMIC access happened.
- Options for powering touch: (A) **ACPM IPC** like Linux (drivers/soc/samsung/acpm: mailbox + shared-memory queues to the APM; requires the APM firmware to be running after S-Boot - to be checked), the sanctioned path; (B) raw SPEEDY registers (CTRL/CMD/FIFO at 0x141C0000 + 0x000..0x104) without the semaphore - not tested whether even those are AP-accessible, and it could collide with APM PMIC traffic (CPU/GPU voltages) if the APM firmware is active. Recommendation: A, read-only first (PMIC ID / LDO1 / LDO35 / LDO43 via ACPM).- **ACPM IPC (drivers/soc/samsung/acpm, read 2026-10-29):** stock DT `acpm_ipc` reg = **mailbox 0x14100000** (INTGR0 0x08, INTCR1 0x20, INTSR1 0x28) + **APM SRAM 0x02039000 (0x30000)**, `initdata-base 0x7F00`; s2mps18 `acpm-ipc-channel 2` (also acpm 4, dvfs 5, s2d 0xb, tmu 0xc). struct acpm_framework (AP view, all u32): plugins 0, num_plugins 4, **ipc_channels 8** (SRAM offset), num_ipc_channels 0xC, pid_framework 0x10, pid_max 0x14, **ipc_ap_max 0x18**, ... fw_size 0x58. struct ipc_channel 72 bytes: id, field, owner, type, ch{rx_rear 16, rx_front 20, rx_base 24, rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size, tx_rear 40, tx_front 44, tx_base 48, q_len 52, q_elem_size 56, credit, mbox_addr}, ap_poll 68. AP TX = channel's rx_* queue, AP RX = tx_* queue. Send: wait not-full, seq (1..63) in cmd[0] bits 16-21, copy q_elem_size bytes to base + size*front, front++, mailbox INTGR0 = (1 << id) << 16. Polling reply: INTSR1 bit id, scan rx rear..front for the seq, copy, compact, rear++, INTCR1 = 1 << id when empty. MFD (acpm_mfd.c/h): cmd[0] = type(4b) << 8 | reg, cmd[1] = func (READ 0, WRITE 1 | val << 8, UPDATE 2 | val << 8 | mask << 16, BULK 3/4); reply cmd[1]: DEST bits 8-15, RETURN bits 24-31 (0 ok, 1 speedy_rx fail, 2 speedy_tx fail).
- **X6.6** (`out/exynos/boot-x6.6.img`, sha256 4552cb5b...): replaces the X6.5 batcher reads with ACPM IPC FUNC_READ of PMIC page 1 regs 0x00 (ID), 0x49 (L1CTRL), 0x6C (L35CTRL), 0x74 (L43CTRL); prints the framework descriptor (tbl, nch, ap_max, fw_size) and channel 2's queue geometry first; stops with "no usable channel 2" if the APM firmware is absent. Read-only towards the PMIC. Then the X6.4 touch steps. Flashed 2026-10-29 03:45 with verified readback; result below.- **X6.6 result (photo): ACPM IPC WORKS - the APM firmware is running after S-Boot.** ACPM: tbl 0x00005BFC, nch 18, ap_max 13, fw_size 0x00FF003F; channel 2: qlen 5, esz 16, poll 1, tx f/r 0/0, rx f/r 0/0. PMIC page 1 reads: **id 0x00, L1CTRL 0xC4 (enable bits set: LDO1 on - confirms the encoding), L35CTRL 0x2C, L43CTRL 0x30: both touch rails OFF** (enable bits 0), voltage selectors already right: LDO35 = LDO_MIN1 700 mV + 44 x 25 mV (STEP2) = **1.8 V**, LDO43 = LDO_MIN3 1800 mV + 48 x 25 mV = **3.0 V** (s2mps18.c LDO_DESC, matching the DT 1800000/3000000). LINES still 0/0, HSI2C MASTER_BUSY (unpowered chip).
- sec_ts_power(on): regulator_enable(dvdd = tsp_io), 1 ms, regulator_enable(avdd = tsp_avdd); probe then waits 70 ms.
- **X6.7** (`out/exynos/boot-x6.7.img`, sha256 cf435718...): after the X6.6 reads, **ACPM FUNC_UPDATE (2) value 0xC0 mask 0xC0 on L35CTRL, 1 ms, same on L43CTRL, 70 ms** (enable bits only; VSEL untouched; no other register), reads both back ("POWER ... now l35 EC l43 F0" expected), then LINES / BITBANG / HSI2C / touch loop. **First PMIC write - approved by the user 2026-10-29; flashed 03:48 with verified readback; result below.**- **X6.7 result (photo): TOUCH CHIP POWERED AND ANSWERING.** POWER "enable l35 rc 0 l43 rc 0 now l35 EC l43 F0" (both LDOs on at 1.8/3.0 V); **LINES 1/1 1/1** (bus pulled up); **BITBANG id rc 0 = AC 37 61** (S6SY761 ID), **boot rc 0 = 0x20 (APP mode, firmware running)**. HSI2C now completes transfers (INT_STATUS 0x80 XFER_DONE) but gets **NACK on the address** (rc 3, TRANS_STATUS 0x000A000D = done | NO_DEV_ACK | NO_DEV) although bit-bang on the same pins works -> HSI2C configuration left (timing from the guessed 200 MHz ipclk is the main suspect). IRQ still low (events pending, not read).
- **X6.8** (`out/exynos/boot-x6.8.img`): bit-bang at ~150 kHz (half period 3 us); one HSI2C ID try kept for the record; then over bit-bang: drain events, device ID, CLEAR_EVENT_STACK, SENSE_ON, and the 60 s touch loop (GPA1-0 low -> READ_ONE_EVENT, coordinate events shown as "touch id st x y left"). Flashed 2026-10-29 03:51 with verified readback; result below.- **X6.8 result (photo): TOUCH WORKS (bare metal).** HSI2C still NACK (rc 3, TRANS 0x000A000D). Bit-bang (~150 kHz): drained events (first `00 10 00 00 00 00 00 00`, then empty), **DEVICE_ID 53 45 37 61 08**, CLEAR + SENSE_ON rc 0, then **481 touch events in ~14 s**, e.g. "touch id 1 st 3 x 4040 y 15 left 0" - coordinates in the chip's 0..4095 range (max_coords 4096); IRQ (GPA1-0) back high after reading. Orientation/mapping not calibrated yet (one corner seen: x 4040 y 15).
- **Recipe for the XNU driver:** (1) ACPM FUNC_UPDATE 0xC0/0xC0 on PMIC page 1 L35CTRL (0x6C), 1 ms, L43CTRL (0x74), 70 ms; (2) GPP1-4 SCL / GPP1-5 SDA (PERIC0 0x10430020) as open-drain GPIO bit-bang, pull-ups external; (3) S6SY761 at 0x48: CLEAR_EVENT_STACK 0x62, SENSE_ON 0x10; (4) poll GPA1-0 (ALIVE 0x14050044 bit 0) low -> READ_ONE_EVENT 0x60 (8 bytes) until high; coordinate event eid 0: tid = b0>>2&0xF, x = b1<<4|b3>>4, y = b2<<4|(b3&0xF), action (tchsta) in b0 bits 6-7, events left b7&0x3F.- **Patch 0108** (`xnu/patches/0108-exynos9810-touch-s6sy761.patch`, on 0107, written, not compiled): bsd/dev/sweet_touch.c (the SDM845 S9's S6SY761 driver: bit-banged I2C, sec_ts events, burst reads, SETFUNC 0x61, SENSE_ON, 1440x2960 scaling, IRQ poll every 5 ms bound to cpu 1) gets an Exynos branch instead of its early return: pins GPP1-4 SCL / GPP1-5 SDA (PERIC0 0x10430020, open drain via the CON nibble, pulls off), IRQ GPA1-0 (ALIVE 0x14050044 bit 0); power = exy_touch_power(): ACPM IPC on channel 2 (mailbox 0x14100000, SRAM 0x02039000 + initdata 0x7F00), reads L35CTRL/L43CTRL, refuses unless vsel = 0x2C / 0x30 (1.8 / 3.0 V), FUNC_UPDATE 0xC0/0xC0 on L35, 1 ms, L43, 70 ms, reads back. Needs boot-arg sweettouch=1 (default 0); starts after sweettouchdelay (default 45 s).
- **0108 compiled** (push 2f07ccc..c5c29cc to xbx, run 40803292103, success): `out/exynos/xnu5/kernel.development.sweet` sha256 4d582ff7... **Boot image `out/exynos/boot-ios6.img`** (sha256 4324496c...) = boot-ios5 cmdline with **sweettouch=1** (sweetfb_noconsole=1 kept). Flashed 2026-10-29 04:02 with verified readback; result below. Expect in the log after ~45 s: 'SWEET TOUCH: S2MPS18 l35ctrl=0x2c l43ctrl=0x30', '... touch rails on', 'lines sda=1 scl=1', 'sec_ts 0x48 boot=0x20 ...', 'sec_ts sensing'.

## TOUCH WORKS IN iOS ON THE GALAXY S9 EXYNOS (boot-ios6, 2026-10-29 ~04:10)

User: "it worked perfectly" - lock screen unlocks with a swipe, touches land correctly (the 0..4095 -> 1440x2960 scaling and orientation from the SDM845 driver are right for the Exynos panel too). Patch 0108 (S6SY761 over bit-banged GPP1-4/5, ACPM LDO35/LDO43 power). Remaining complaints: **UI very slow**, and it "looks very bad because of no shaders" (no GPU: CoreAnimation software path). User asked to look at https://github.com/ry-diffusion/Chlorophyll.
## 0109: CPU DVFS through ACPM (written 2026-10-29, NOT built yet)

Slowness analysis (boot-ios6 log, observed): present copy+scale 1125x2436 -> 1440x2960 takes 10-19 ms, 2.2-4.9 fps at a 10 Hz vsync (sweetfb_hz default 10). The CPU is the bottleneck: CoreAnimation renders on 4 A55 cores at whatever clock S-Boot left; no DVFS on Exynos yet, M3 big cores not started.
Patch 0109 (xnuw commit 6e132cf): sweet_touch.c ACPM code generalized per channel (exy_acpm_xfer); new exy_cpu_dvfs() on DVFS channel 5 (acpm_dvfs.c format: cmd[0] domain = cal-id & 0xff, cmd[1] kHz, cmd[2] FREQ_REQ 0 / FREQ_GET 1). Domains from DT cal-ids: 0 MIF, 1 INT, 2 CPUCL0 (cal_id 0xb040002), 3 CPUCL1 (0xb040003), 4 G3D. Always logs `SWEET DVFS: <dom> <kHz>` (FREQ_GET, read-only); boot-args sweetcpufreq=<kHz> (clamped to 1794000, cl0 max from DT cpufreq-ufc) and sweetmiffreq=<kHz> (clamped 2093000) send FREQ_REQ. Runs at thread start on Exynos before the touch delay, also without sweettouch=1.
Next: push -> CI build -> boot-ios7 = boot-ios6 cmdline + sweetcpufreq=1794000; read the SWEET DVFS lines from the persistent log.
Chlorophyll (ry-diffusion): Metal on Mesa freedreno for Adreno 630 (iOS 18 arm64e, Bergamota kernel/libdrm); S9 Exynos is Mali-G72 MP18 (Bifrost), no XNU GPU driver at all -> would need a Panfrost-based backend plus an XNU Mali kernel driver (job manager, MMU, G3D power/DVFS). Not usable as-is; deferred.

- **0109 compiled** (push c5c29cc..0dd24d4 to xbx, run 40805355829, success): `out/exynos/xnu6/kernel.development.sweet` sha256 42cda508... **Boot image `out/exynos/boot-ios7.img`** (sha256 f2651ded...) = boot-ios6 cmdline + **sweetcpufreq=1794000**. Not flashed. Expect in the log early: 'SWEET DVFS: mif/int/cl0/cl1/g3d <kHz>' then 'SWEET DVFS: cl0 -> 1794000: now 1794000 kHz'. Fallback: boot-ios6.

- boot-ios7 flashed 2026-10-29 with verified readback (sha256 f2651ded...), rebooted into iOS. Result pending.


## boot-ios7 result + 0110 (2026-10-29)

- boot-ios7 log (s9/logs/boot-ios7.txt, observed): every `SWEET DVFS` FREQ_GET returned -1 (timeout) on ACPM channel 5, so the cl0 -> 1794000 request did nothing. Touch still fine.
- TWRP dmesg (observed): `acpm_dvfs_init fail ret = -2` -> stock Linux never gets channel 5 (the node passed via exynos_acpm_set_device has no acpm-ipc-channel), so acpm_dvfs.ch_num stays 0 and all DVFS requests go to acpm_ipc channel[0]. cal-if domain ids confirmed by dmesg: dvfs_mif 0, int 1, cpucl0 2, cpucl1 3, g3d 4. TWRP runs cl0 at 1690000 (available 455000..1794000), cl1 at 962000.
- Patch 0110 (xnuw HEAD): DVFS channel = table entry 0 by index (like channel[ch_num]), boot-arg sweetdvfsch=<index> overrides; logs `SWEET DVFS: channel[n] id .. qlen .. esz .. tx f/r .. rx f/r ..`.
- **0110 compiled** (push 0dd24d4..4c103ec... to xbx, run 40806999625, success): `out/exynos/xnu7/kernel.development.sweet`. **Boot image `out/exynos/boot-ios8.img`** (sha256 948693e0...) = boot-ios7 cmdline (sweetcpufreq=1794000), DVFS now on channel[0]. Not flashed.

- boot-ios8 flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **boot-ios8 result (observed, s9/logs/boot-ios8.txt):** channel[0] exists (id 0, qlen 15, esz 16, queues idle 0/0) and ANSWERS (no timeouts, unlike channel 5), but every FREQ_GET reply has cmd[1] = 0, and after FREQ_REQ cl0 1794000 the GET still says 0. So it is unknown whether the clock changed. Present stats did improve: avg 6.9-8.4 ms per present, 7-15.6 fps (boot-ios6: 10.5-19.4 ms, 2.2-4.9 fps) - consistent with a faster A55 but not proof (different workload/session).
- Next idea: measure the real A55 clock in-kernel (PMU cycle counter PMCCNTR_EL0 over a fixed mach_absolute_time interval) before and after FREQ_REQ; log raw reply words cmd[0..3].
- Patch 0111 (xnuw HEAD): measures the A55 clock without the PMU (Samsung EL2 may trap PMU regs): 20000 x 64 dependent adds (1/cycle on A55), interrupts off, best of 3 -> 'SWEET DVFS: A55 measured N MHz' before the request and 'cl0 -> 1794000: now .. kHz, measured N MHz' 20 ms after; logs raw cl0 reply words 'cl0 req R K kHz reply c0 c1 c2 c3'. Not built.


## Bold Text crash loop + USERDATA rewrite (2026-10-29)

- User turned on Settings > Accessibility > Bold Text in iOS (boot-ios8); SpringBoard then never came back.
- Log `s9/logs/boot-ios8-bold.txt` (observed, from the next boot): SpringBoard crash-loops with the same signature every launch: `sig 11 pc 0x0 lr 0x19e6c858c far 0x0` (call through a NULL function pointer), bt 0x19e6c8924 0x19e571884 0x19e533114 0x19e532f10 0x1a8a93760 0x197728780 (dispatch) ...; slide 0x9b90000. PosterBoard (lock screen wallpaper) also crash-loops: `sig 5` (SIGTRAP, Swift/assert trap) pc 0x1cb308268, bt 0x1cb30e304 0x1cb2f4c5c 0x1cb2f4644 0x1cb376204 0x1ad249318 0x1a554372c ...
- Not symbolicated: `ipsw` (used by tools/spin-sym.py) is not installed in this environment. Guess, NOT verified: the bold-text font/legibility path reaches a GPU/Metal-only routine that is missing without a GPU.
- Fix chosen by the user: rewrite USERDATA with the clean image F:\ios-s9-ipsw\ios16-root-20H392-s9.img (sha256 0b9c631b...) via write-userdata.py. Avoid Bold Text (and probably other text-legibility settings) until symbolicated.
- USERDATA rewritten 2026-10-29: 64/64 chunks verified, whole range sha256 0b9c631b... = image. Rebooted into iOS with boot-ios8 on BOOT.

- **0111 compiled** (run 40808819701, success): `out/exynos/xnu8/kernel.development.sweet` sha256 aaa5a91d... **Boot image `out/exynos/boot-ios9.img`** (sha256 926504d4...) = boot-ios8 cmdline; adds 'SWEET DVFS: A55 measured N MHz' before/after the cl0 request and raw cl0 reply words. Not flashed.

- boot-ios9 flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **boot-ios9 result (observed, s9/logs/boot-ios9.txt): CPU DVFS WORKS.** A55 measured **1052 MHz** as S-Boot left it, **1793 MHz** 20 ms after FREQ_REQ cl0 1794000 on ACPM channel[0]. Raw replies: FREQ_GET -> `0x30000 0 0 0` (only the seq survives; GET is not answered with a rate on this channel), FREQ_REQ -> `0x60000 0x1b5fd0 0 0` (rate echoed). So: trust the add-chain measurement, not FREQ_GET. Present avg 6.4 ms, 10.6 fps in the second window (first window 2.4 fps = first boot after the USERDATA rewrite, caches being rebuilt). Crashes: driverkitd x1, PosterBoard x1 (first boot), no SpringBoard loop.
- Next speed ideas: sweetmiffreq (MIF up to 2093000; effect only visible in present copy time since GET reads 0), sweetfb_hz above 10 now that the CPU is 1.7x faster, M3 big cores (CPU_ON never arrived, see X1.x).
- **Boot image `out/exynos/boot-ios10.img`** (sha256 4fb91220...) = 0111 kernel (xnu8) + boot-ios9 cmdline + **sweetmiffreq=2093000 sweetfb_hz=20**. Not flashed. Compare present avg/fps with boot-ios9 (6.4 ms, 10.6 fps).

- boot-ios10 flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **boot-ios10 result: GREEN SCREEN (user), no log.** The log area after boot-ios10 is byte-identical to the boot-ios9 dump (sha256 E70D114C...; total 171158, flush #29), so boot-ios10 never reached its first log flush. Only differences from boot-ios9: sweetmiffreq=2093000 and sweetfb_hz=20. Guess (strong, not proven): the MIF FREQ_REQ to 2093000 through ACPM channel[0] broke DRAM/display (stock Linux raises MIF through devfreq with its own sequence; the APM alone may not handle a jump from S-Boot's MIF clock). sweetfb_hz only changes a timer period. Do NOT use sweetmiffreq until understood.
- **boot-ios10b** (`out/exynos/boot-ios10b.img`, sha256 69ce9778...) = boot-ios9 cmdline + sweetfb_hz=20 only (no sweetmiffreq). Flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **boot-ios10b result (observed, s9/logs/boot-ios10b.txt): boots normally** -> the boot-ios10 green screen came from sweetmiffreq=2093000 (the only other difference). A55 again 1052 -> 1793 MHz. Present windows: 5.4 fps (13.0 ms avg, 214 full swaps), 9.9 fps (9.0 ms), 7.3 fps (7.5 ms) vs boot-ios9 10.6 fps (6.4 ms): **sweetfb_hz=20 gives no measurable gain** - frames are limited by CoreAnimation's CPU rendering and the full-screen copies, not the 10 Hz vsync. Crashes: driverkitd x1, PosterBoard x1, assetsd x2 (none looping).
- Remaining speed levers: start the 4 M3 big cores (CPU_ON never arrived in X1.x), cheaper present (zero-copy / partial copies of the dirty region instead of 1125x2436 -> 1440x2960 scaling), GPU (Mali-G72, no driver).

## X1.6: M3 big cores with Samsung's CPUCL1 pmucal_lpm_init writes (built 2026-10-29, NOT flashed)

Implements exynos/M3-POWER.md section 6 A+B in x1.c: before the first /cpus reg 0x1xx CPU_ON, read CLUSTER1_CPU0-3/NONCPU_STATUS (0x14062004/0C/14/1C/44), RESET_SEQUENCER_CONFIGURATION (0x14060500), MAILBOX_APM2AP (0x14100010); then the six RMW writes (0x14061088 top byte 0x8D; 0x14062114/2180/21EC/2258/2384 bits 1:0 = 2), each read back. After all CPU_ONs (+2 s) the status registers are read again. Final screen adds "M3 PMU" (before -> after), "M3 STAT" (before -> after CPU_ONs), "M3 MISC". PMU registers only, reset by the next boot. `out/exynos/boot-x1.img` sha256 429dbe30... Expected if it works: slots 4-7 green, MIDR 0x531F0020, MPIDR 0x800001nn, and the M3's MMFR0 (16K?).
- X1.6 flashed 2026-10-29 with verified readback (sha256 429dbe30...), rebooted. Result pending (photo). Afterwards restore boot-ios10b.

- **X1.6 RESULT (photo s9/logs/MILESTONE-s9-m3-cores-x1.6-2026-10-29.webp): ALL 8 CORES UP. The M3s support 16K.** CPU4-7 (/cpus 100-103): MIDR 531F0020, **MMFR0 00101122 (16K YES, same as the A55)**, EL1, MPIDR 80000100-80000103, arrived in 0 ms. VERDICT "16K on every core: XNU can run", SLOTS -UUUUUUU.
  - M3 PMU (before -> after, as M3-POWER.md predicted): 1088 C7150000 -> 8D150000; 2114/2180/21EC/2258/2384 00000001 -> 00000002.
  - M3 STAT (before writes -> after CPU_ONs): CLUSTER1_CPU0-3_STATUS 00880000 -> 00100F0F, NONCPU 00880000 -> 0010000F (powered).
  - M3 MISC: RESET_SEQUENCER_CONFIGURATION 0, MAILBOX_APM2AP 0.
  - So the six Samsung pmucal_lpm_init CPUCL1 writes are all that was missing. Next: XNU - same writes in SweetCPU before the CPU_ON of reg 0x1xx, ADT cpu nodes for the M3s (psci ids 0x100-0x103, MPIDR 0x800001nn, non-MT), cpus=8; check how the sweet scheduler/topology treats a second cluster (AMP: E = A55, P = M3).
- boot-ios10b restored on BOOT 2026-10-29 with verified readback, rebooted into iOS.


## X1.7: M3 instruction-set check before the 8-core XNU patch (built 2026-10-29, NOT flashed)

Blocker found while planning 0112 (observation from the build files): the SWEET kernel is built with `-march=armv8.2-a+crypto` (makedefs/MakeInc.def) and `__ARM_PAN_AVAILABLE__` (pexpert/arm64/SWEET.h), and there is no undefined-instruction emulation. TWRP's Linux hwcaps (system-wide) lack atomics, i.e. at least one core (the M3) has no LSE; v8.0 also means no PAN. If so, the kernel's LSE atomics / `msr PAN` would UNDEF on an M3, and A11-targeted iOS userland may use LSE too. Also found: start.s matches per-CPU data by `MPIDR & 0xFFFF` - M3 core 0 (0x80000100) collides with A55 core 1 (0x81000100); planned fix: key non-MT cores as 0x8100-0x8103.
X1.7 = X1.6 + ISAR0/ISAR1 on every core; final screen shows for CPU0 (A55) and CPU4 (M3): raw ISAR0/ISAR1/MMFR1/PFR0 and decoded LSE (ISAR0[23:20], 2 = LSE), PAN (MMFR1[23:20]), RCPC (ISAR1[23:20]), FP (PFR0[19:16], 1 = fp16). PMU/status block cut to one line each. `out/exynos/boot-x1.img` sha256 b56da2e6...
- X1.7 flashed 2026-10-29 with verified readback, rebooted. Result pending (photo). Afterwards restore boot-ios10b.

- **X1.7 RESULT (photo s9/logs/s9-x1.7-id-regs-2026-10-29.webp, observed):** all 8 cores up again (SLOTS -UUUUUUU).
  - A55 (CPU0): ISAR0 10211120, ISAR1 00100001, MMFR1 10212122, PFR0 10112222 -> LSE 2, PAN 2, RCPC 1, FP 1 (fp16), DPB 1.
  - **M3 (CPU4): ISAR0 00011120, ISAR1 00000000, MMFR1 00000000, PFR0 00002222 -> LSE 0, PAN 0, RCPC 0, FP 0 (no fp16), no RDM, no VH, no DPB. Pure ARMv8.0 + crypto/CRC.**
  - Consequence: the current kernel (-march=armv8.2-a, PAN) cannot run on an M3. iOS 16 userland is built for the A11 (v8.2: LSE atomics, fp16), so user threads on an M3 would likely SIGILL - not verified (would need disassembly of the shared cache).
  - Options: (A) kernel rebuilt for armv8-a without PAN (or PAN toggled at runtime only on A55s) and M3s used only for bound kernel work, e.g. the sweet.fb.present copy/scale (6-13 ms per frame); (B) trap-and-emulate LSE/fp16 on the M3s (slow, large); (C) leave the M3s off. A is the only one with a clear payoff.
- boot-ios10b restored on BOOT 2026-10-29 after X1.7 (verified readback), rebooted into iOS.


## Plan: M3s as kernel-only cores (researched 2026-10-29, NOT started)

Feasibility (read from the xnuw sources, observed):
- **Scheduler:** a processor with is_recommended = FALSE runs only threads bound to it (sched_prim.c ~2557 "Bound threads are dispatched to a processor without going through choose_processor()"; choose_processor drops non-recommended hints ~5124). sweet_bind_current_thread(cpu) (sched_prim.c 183) already binds (used by the vsync thread, "vsync thread on cpu 3").
- **Instruction set:** v8.2/PAN use is all compile-time and has v8.0 fallbacks: PAN in 11 files behind __ARM_PAN_AVAILABLE__ (pexpert/arm64/SWEET.h:36); __ARM_ARCH_8_2__ only gates UAO save/restore (cswitch.s), the ticket-lock casa (machine_routines_asm.s ~759, LL/SC fallback) and OS_ATOMIC_USE_LLSC (libkern/os/atomic_private.h:182); no fp16, no dc cvap. KDK arm64e libs are only inspected by CI, not linked.
- **Identity:** start.s matches cpu data by MPIDR & 0xFFFF; M3 core 0 (0x80000100) collides with A55 core 1 (0x81000100).

Steps:
1. Kernel for both cores: MACHINE_FLAGS_ARM64_SWEET -march=armv8.2-a+crypto -> armv8-a+crypto+crc; drop __ARM_PAN_AVAILABLE__ from SWEET.h. Cost: no PAN, LL/SC atomics; it is one kernel for SDM845 and Exynos, so the SDM845 build changes too (or add a separate SWEET_EXYNOS machine config + CI job).
2. Identity: SWEET start.s key = MPIDR & 0xFFFF, plus 0x8000 when MPIDR.MT (bit 24) is clear; same fold in ml_get_cpu_number; ADT reg for M3s = 0x8100-0x8103; SweetCPU::start range check allows 0x81nn on Exynos.
3. Power: the six pmucal_lpm_init writes (X1.6) in SweetCPU before the first M3 CPU_ON; psci-id 0x100-0x103 in the ADT (mkadt.py), cpus=8.
4. Keep users off: M3s must be non-recommended BEFORE processor_start (else the scheduler may dispatch an A11-built user thread there -> SIGILL); needs a SWEET hook into sched_update_recommended_cores / perfcontrol recommended mask, and a check that the perfcontrol failsafe never re-recommends them.
5. Work to move: bind sweet.fb.present (and optionally the touch thread, bit-banged I2C) to cpu 4+.

**Expected gain is small** (computed from boot-ios10b.txt): the present copy is busy 5.6-8.9% of ONE A55 (7.5-13 ms x 300 frames per 30-55 s), i.e. ~2% of the 4-core budget. The slowness is CoreAnimation software rendering in backboardd/SpringBoard (user space), which cannot run on the M3s. So steps 1-5 buy little for their risk. Higher-value alternatives: zero-copy present on DECON (point the window at the IOSurface; removes the copy instead of moving it, also frees memory bandwidth), lower render resolution (@2x ADT, --scale2: 46% fewer pixels for CoreAnimation), or a GPU driver (Mali-G72, long-term).
- **Better lever found (from s9/NOTES.md 385-400 and README):** the SDM845 port renders iOS as an iPhone XR: `mkadt.py --scale2` = canvas 828x1792 @2x + /product artwork-device-subtype 1792, artwork-scale-factor 2 (1.48 Mpx, -46% vs @3x 1125x2436), reaching 30+ fps there. The Exynos ADT (out/exynos/exynos-ufs.adt) is still @3x; exynos/mkadt.py already accepts --scale2. Cheap test: @2x Exynos ADT + boot-ios10b cmdline. Possible snag (guess): MobileGestalt's cache is keyed by build + IOPlatformUUID (from the ECID), so a screen-shape change may need a new synthetic ECID to force a recompute, as for the D22AP /product change.
- **boot-ios11** (`out/exynos/boot-ios11.img`, sha256 5dfdc191...) = boot-ios10b (0111 kernel xnu8, same cmdline) with the **@2x iPhone XR ADT** `out/exynos/exynos-ufs-xr2.adt` (`exynos/mkadt.py --scale2`, sha256 9b6c03fd...): canvas 828x1792, display-scale 2, /product artwork-device-subtype 1792 + artwork-scale-factor 2, and a new synthetic ECID 0x5357454554029810 (S9_ECID_2X) so MobileGestalt recomputes its screen answers. The @3x tree is unchanged apart from the per-build random-seed. Not flashed. Compare present/fps with boot-ios10b (5.4-9.9 fps); watch for SpringBoard asserts like the old BoundingPathBitmap one.

- boot-ios11 flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **boot-ios11 result (observed, s9/logs/boot-ios11.txt): @2x works and is faster.** No SpringBoard crash; crashes only driverkitd x1, PosterBoard x1 (both seen on every boot), MobileSMS x1. Dirty-region coordinates now in 828-px space (e.g. 446,580 128x128). Present windows: 5.5 (first boot, caches), 11.7, 16.2, 7.4, 13.5, **21.7, 20.5 fps**; present avg 2.0-6.7 ms (max 12-14 ms) vs boot-ios10b 7.5-13 ms (max 17 ms) and 5.4-9.9 fps. The best windows sit at the 20 Hz cap (sweetfb_hz=20), so the cap now limits: next try sweetfb_hz=30. Layout (status bar / notch) not yet confirmed by the user.
- **boot-ios12** (`out/exynos/boot-ios12.img`, sha256 dc398700...) = boot-ios11 (@2x ADT, 0111 kernel) with **sweetfb_hz=30**. Flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **boot-ios12 result (observed, s9/logs/boot-ios12.txt):** sweetfb_hz=30 in effect; present windows 7.2, 7.0, 8.6, 7.8, 11.4 fps (avg 3.8-10.6 ms). Not comparable with boot-ios11: this session was dominated by full-screen updates (198 and 220 of 300 swaps "full" vs 15-55 in boot-ios11's 20+ fps windows) and Messages was opened and crashed 5 times. So 30 Hz vs 20 Hz is undecided; needs the same actions on both (e.g. home-screen page swipes for 60 s).
- **Messages (MobileSMS) crash = the Bold Text SpringBoard crash.** Unslid (addr - slide): boot-ios12 MobileSMS slide 0x30040000, lr 0x1c4b7858c -> 0x194B3858C, bt 0x1c4b78924 -> 0x194B38924, 0x1c49e3114 -> 0x1949A3114. boot-ios8 SpringBoard (Bold Text) slide 0x9b90000, lr 0x19e6c858c -> 0x194B3858C, bt 0x19e6c8924 -> 0x194B38924, 0x19e533114 -> 0x1949A3114. Same shared-cache routine calls a NULL function pointer (pc 0). Messages also crashed once in boot-ios11 (same lr after unsliding: 0x1bf95458c - 0x2ae1c000 = 0x194B3858C). Symbolicating 0x194B3858C (needs `ipsw dyld a2s` on the 20H392 arm64 cache) names the missing function behind both crashes.

## Bold Text / Messages crash SYMBOLICATED (2026-10-29)

Tools: SystemOS cryptex `F:\ios-s9-ipsw\20H392\087-86593-047.dmg` mounted read-only in WSL with apfs-fuse at /tmp/sysos (cache: /tmp/sysos/root/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64); `ipsw` 3.1.739 linux_x86_64 (GitHub release, sha256 8124bdba... matches its checksums.txt) in WSL ~/ipsw-3.1.739, linked from ~/.local/bin/ipsw; `ipsw dyld a2s -s 0 DSC <unslid addr>`. Scripts: scratchpad sym.sh / sym2.sh. Unslid = crash address - the process's slide from the CRASH line.

- Crash site (both): `CI::GLContext::init() + 204` (0x194B3858C, CoreImage) calls a NULL function pointer, from `CI::GLContext::GLContext(ShareContextInfo, CGColorSpace*, CGColorSpace*, PixelFormat, bool, unsigned long, bool, bool) + 500` <- `+[CIContext(Internal) internalContextWithEAGLContext:options:] + 580` <- `-[CIContext initWithOptions:] + 504` <- `+[CIContext contextWithOptions:] + 36`.
- SpringBoard + Bold Text: `-[UIImage _imageWithStylePresets:tintColor:traitCollection:]` -> `-[CUICatalog imageByStylingImage:stylePresetName:...]` -> `+[CUIShapeEffectStack sharedCIContext]` (dispatch_once) -> CIContext. (Bold Text makes UIKit style glyph images through CoreUI's shape effects, which render with CoreImage.)
- Messages: app-icon scaling `____createApplicationIconFromImage_block_invoke` -> `__LICGImageCreateScaled` -> `__LICGImageCreateCoreImageLanczosScaled` -> `+[CIContext(IconServicesAdditions) _MI_sharedIconCompositorContext]` -> CIContext.
- Cause: with no Metal device, CIContext falls back to an OpenGL ES (EAGL) context; iOS 16's GLES runs on Metal (AppleMetalGLRenderer, see s9/ESTUDO-2026-10-25-gpu.md), so the GL context is not functional and CI::GLContext::init calls a function pointer that was never filled. Any process creating a default CIContext will crash the same way.
- Fix direction: make CIContext use CoreImage's CPU renderer (kCIContextUseSoftwareRenderer = YES) when there is no Metal device - e.g. an ElleKit tweak hooking -[CIContext initWithOptions:] (and/or +internalContextWithEAGLContext:options:), injected into SpringBoard and apps. The SDM845 root has ElleKit + DYLD_INSERT_LIBRARIES machinery (s9/tools/jb-launchd.py, s9/tools/prep-ios6.sh); the Exynos root (exynos/mkroot-wsl.sh) does not yet. Building the dylib needs an iOS SDK toolchain (macOS CI runner).
- **S9CISoft tweak written (2026-10-29, not built yet):** `s9/ios-tools/S9CISoft.m` + `S9CISoft.plist` (filter Bundles com.apple.CoreImage), build step added to `.github/workflows/s9-ios-tools.yml` (-fno-objc-arc, ad-hoc signed). Hooks -[CIContext initWithOptions:] with method_setImplementation (no ElleKit API) and adds "software_renderer" = YES (kCIContextUseSoftwareRenderer) when MTLCreateSystemDefaultDevice() (dlsym, checked once on the first CIContext) is nil. Kill switch /var/mobile/Library/S9CISoft/disabled. Checked first that no CI_* variable forces the CPU renderer (92 CI_* strings in CoreImage: none; "software_renderer" is only the option key).
- **Injection on the Exynos root is missing:** exynos/mkroot-wsl.sh runs only tools/mkiosroot.sh + tools/sweet-root-tweaks.py (no ElleKit). The SDM845 root gets it from s9/tools/prep-ios6.sh: ElleKit 1.2 deb (s9/jailbreak/fetch-jailbreak.sh, from ellekit.space) unpacked into the root, tweaks in /usr/lib/TweakInject (+ filter plist), and s9/tools/jb-launchd.py adds the jobs to /System/Library/xpc/launchd.plist (launchd_unsecure_cache=1 reads only that file): com.ios-s9.ellekit (/usr/libexec/ellekit/loader at boot), SpringBoard DYLD_INSERT_LIBRARIES=/usr/lib/TweakInject.dylib, com.ios-s9.respring (one respring per boot after the loader). Minimal Exynos version: only those three pieces (no bootstrap, dpkg, sshd, s9net), applied to the existing image in WSL (sudo mount by the user), then a USERDATA rewrite.
- S9CISoft built: s9-ios-tools run 40861382013 (success, -Werror clean), artifact in `s9/out/ios-tools-cisoft/` (S9CISoft.dylib 85808 bytes, sha256 2171a612..., ad-hoc signed; S9CISoft.plist). Not installed: the Exynos root has no ElleKit yet.

- **ElleKit for the Exynos root (2026-10-29): `s9/exynos/add-ellekit-wsl.sh` written, NOT run yet (needs the user's sudo).** Copies the clean image to `/mnt/f/ios-s9-ipsw/ios16-root-20H392-s9-ellekit.img` (SRC untouched), mounts it rw in WSL, unpacks ElleKit 1.2 (`s9/jailbreak/jb/debs/0-ellekit.deb`, from ellekit.space as fetch-jailbreak.sh, sha256 3ae1e482...; ./etc excluded), installs S9CISoft.{dylib,plist} into usr/lib/TweakInject, creates private/var/ios-s9 (loader log), runs `tools/jb-launchd.py MNT com.ios-s9.ellekit` (new: optional label filter; tested on a fake root), fsck, sha256.
  - Checked: ElleKit binaries link only system libraries + /usr/lib/swift (in the iOS 16 cache), no Procursus. The root has no /bin/sh, sleep, grep, killall (only /bin/df, /bin/ps), so the SDM845 respring job (sh script) is left out; Messages starts after the loader anyway, and a crash-looping SpringBoard is restarted by launchd. /etc -> private/etc and /var -> private/var are relative links; /Library, /usr are dirs.
- **add-ellekit-wsl.sh run by the user (2026-10-29):** `F:\ios-s9-ipsw\ios16-root-20H392-s9-ellekit.img` (17179869184 bytes) sha256 **18b1bd55f9510ca0833538fe58c3d9740bc40dabe2bdf992c60283af6295e046**; fsck OK (both passes). Contents seen in the script's listing: /usr/libexec/ellekit/loader 739984, /usr/lib/TweakInject/S9CISoft.dylib 85808 + .plist 286, /usr/lib/TweakInject.dylib -> /usr/lib/ellekit/libinjector.dylib. (`ls -la` prints 'No data available' for . and .. on Linux hfsplus: harmless.) Clean image unchanged (0b9c631b...). Not on the phone yet.

- USERDATA rewritten 2026-10-29 with the ElleKit + S9CISoft image: 64/64 chunks verified, whole range sha256 18b1bd55... = image. Rebooted into iOS with boot-ios12 (@2x, 30 Hz) on BOOT. Result pending: Messages and Bold Text should no longer crash; ElleKit loader log /private/var/ios-s9/ellekit.log (not readable from TWRP: no HFS+ there).

- **First ElleKit boot (observed, s9/logs/boot-ios12-ellekit.txt, flush #91):** it booted; the ElleKit loader ran (`exec loader`), the kernel's AMFI dyld policy allows DYLD_INSERT_LIBRARIES. SpringBoard started once, no SpringBoard crash. **MobileSMS still crashed 4x with the same CoreImage NULL call** (slide 0x22760000, lr 0x1b729858c -> 0x194B3858C) and no S9CISoft frame in its backtrace (initWithOptions+504 is called directly by _MI_sharedIconCompositorContext), so the hook was not active in Messages.
  - Likely cause (guess): the filter is `Bundles: com.apple.CoreImage`; TweakInject matches filters when it runs at process start, and Messages loads CoreImage later (IconServices), so S9CISoft never loaded there. Not excluded: pspawn injection into apps not working at all. ElleKit's own log is on the iOS volume (not readable from TWRP).
  - Fix plan: filter on com.apple.UIKit (loaded at start in every app), and install the hook when CoreImage appears (objc_addLoadImageFunc, which also covers already-loaded images); log to /var/mobile/Library/S9CISoft/log.txt where the sandbox allows (SpringBoard). Update the ellekit image in place and write only the 256 MiB chunks that changed.
- **S9CISoft v2 (written, not built):** filter Bundles com.apple.UIKit + com.apple.CoreImage; the constructor only registers `_dyld_register_func_for_add_image`, and the callback hooks -[CIContext initWithOptions:] once objc_getClass("CIContext") exists (dyld calls it for loaded and later images, after objc registered the classes and released runtimeLock; objc_addLoadImageFunc was rejected because it calls back with runtimeLock held). Logs "hooked ..." and the Metal verdict to /var/mobile/Library/S9CISoft/log.txt where writable.
- New: `s9/exynos/update-tweaks-wsl.sh` (in-place tweak swap in the ElleKit image + mobile-owned log dir, fsck, sha256) and `write-userdata.py IMG --changed` (hashes each 256 MiB chunk on the phone, pushes only differing chunks; same per-chunk readback and final whole-range check).
- S9CISoft v2 built: s9-ios-tools run 40870066799 (success), `s9/out/ios-tools-cisoft2/S9CISoft.dylib` sha256 eefb3143..., staged in s9/jailbreak/jb/ for update-tweaks-wsl.sh. Not in the image yet.

- **Bold Text on the ElleKit root with S9CISoft v1 (observed, s9/logs/boot-ios12-ellekit-bold.txt):** SpringBoard[26] (exec at log line 491, before `exec loader` at 892) and the relaunched SpringBoards 251/257/261 (after the loader) all crash with the same CoreImage NULL call (unslid 0x194B3858C; slide 0x15860000) and no tweak frame (+contextWithOptions: -> initWithOptions+504 directly). SpringBoard 265 printed "Hello world, SpringBoard!" - that string is in ElleKit's pspawn.dylib (launchd payload), so ElleKit's spawn injection works - and was still running at the end of the log. PosterBoard x4 (sig 5, as on every boot).
  - Reading: injection works; v1's filter (Bundles com.apple.CoreImage) is evaluated by libinjector at process start, before CoreImage is loaded, so S9CISoft was skipped (same as Messages). S9CISoft v2 (UIKit filter + hook on CoreImage load) addresses exactly this. Next: update-tweaks-wsl.sh (user, sudo) + write-userdata.py --changed.
- update-tweaks-wsl.sh run by the user: ElleKit image now has S9CISoft v2; sha256 **a6e34fed38efbdf5cb7810170f5238cefb480acb5486818b3fbc8d847e9ddb5c** (was 18b1bd55... on USERDATA). Not written to the phone yet.

- USERDATA = S9CISoft v2 ElleKit image (2026-10-29) via `write-userdata.py --changed`: 21 chunks rewritten (1, 3, 4, 18, 27, 47-62; iOS had modified many since the v1 write), 43 identical, whole range sha256 a6e34fed... = image. Rebooted into iOS (boot-ios12). Test pending: Messages, Bold Text; /var/mobile/Library/S9CISoft/log.txt (SpringBoard).

- **S9CISoft v2 result (observed, s9/logs/boot-ios12-cisoft2.txt):** Bold Text showed bold for a moment, then SpringBoard crashed 8x, same stack (+contextWithOptions: -> initWithOptions+504 -> GL -> NULL call; unslid 0x194B3858C, slide 0x389b4000). No hook frame, but v2's replacement ends in a tail call, so that does not prove the hook was absent.
- **Why software_renderer cannot work (from the disassembly, observed):** -[CIContext initWithOptions:] (0x1949A2F1C) calls _CIGLIsUsable, objectForKey:/dictionaryWithDictionary:/removeObjectForKey:/setValue:forKey: on the options, CI_ENABLE_METAL_GPU, CIMetalCopyDefaultDevice + CIMetalDeviceIsSupported -> +internalContextWithMTLDevice:options:, else +internalGLContextWithOptions: (call at 0x1949A3110). There is no CPU-renderer branch. After the GL call: `cbz x0, 0x1949A317C` -> release self, x20 = 0, return nil (clean). The returned value is an internal CI::Context* (passed to _initWithInternalRepresentation:, then CI::Object::unref).
- **S9CISoft v3 (written, not built):** without Metal, +internalGLContextWithOptions: and +internalContextWithEAGLContext:options: return NULL (class-method hooks via method_setImplementation, installed from the dyld add-image callback), so CIContext creation returns nil instead of crashing. Logs each refusal + the hook to /var/mobile/Library/S9CISoft/log.txt. Risk: callers that assume a non-nil CIContext.
- A raw grep of USERDATA (dd | grep -a for the tweak's log lines) was started from TWRP to see whether v2 ran in SpringBoard; it outlived the 10-minute tool timeout and kept running in the background.
- S9CISoft v3 built: s9-ios-tools run 40877629220 (success), `s9/out/ios-tools-cisoft3/S9CISoft.dylib` sha256 f55ef728..., staged in s9/jailbreak/jb/. Not in the image yet.

- update-tweaks-wsl.sh run by the user with S9CISoft v3: ElleKit image sha256 **6b408b593b061fc60413423d6fc31c158bec4fbad923596e27b17cf3acffeb0c**. Not written to the phone yet.
- **Phone hot (user, 2026-10-29):** cause = the raw USERDATA grep (dd 16 GB | grep -a on the phone's CPU) left running ~40 min in TWRP. Stopped (TaskStop + pkill); afterwards TWRP thermal zones BIG 49 C, LITTLE 51 C, G3D 49 C, battery 44.8 C, cpu0 598 MHz, cpu4 962 MHz. Do not run long dd|grep jobs on the phone again; search a pulled copy instead.
  - Separate concern (not measured): under iOS the kernel sets the A55s to 1794 MHz (sweetcpufreq) with no thermal management of its own; protection then relies on the APM firmware / hardware trips. If iOS runs hot, try sweetcpufreq=1456000.
- USERDATA = S9CISoft v3 ElleKit image (2026-10-29) via --changed: 24 chunks rewritten (1, 3, 4, 18, 27, 35, 47-64), whole range sha256 6b408b59... = image. Rebooted into iOS (boot-ios12). Test pending: Messages, Bold Text.

- **S9CISoft v3 WORKS (user, 2026-10-29): Messages opens and Bold Text works, no SpringBoard crash.** Fix = refuse CoreImage's GL context without Metal (+internalGLContextWithOptions: / +internalContextWithEAGLContext:options: return NULL -> CIContext nil), installed from a dyld add-image callback, loaded by ElleKit (UIKit filter). Current phone state: BOOT = boot-ios12 (0111 kernel, @2x XR canvas, sweetfb_hz=30, sweetcpufreq=1794000); USERDATA = ElleKit image 6b408b59... (F:\ios-s9-ipsw\ios16-root-20H392-s9-ellekit.img; clean image 0b9c631b... kept). Not checked: other CoreImage users getting nil (effects that silently do nothing), S9CISoft log.txt contents.

## Next features requested (2026-10-29): RAM 4 GB, Wi-Fi, brightness, audio, accurate battery - assessment

- **Battery (smallest):** SweetBattery.cpp + bsd/dev/sweet_max77705.c already read the MAX77705 fuel gauge (0x36: RepSOC 0x06, VCell 0x09, Current 0x0A, ...) on the SDM845 S9 via bit-banged GPIO33/34. Exynos stock DT: MAX77705 at hsi2c@14360000 (USI-v2 HSI2C, CMGP) addr 0x66; pins `hsi2c40-bus` = **gpm14-0 (SCL), gpm15-0 (SDA)** in pinctrl@14220000 (CMGP); IRQ gpa1-6. Port = Exynos pin backend for sweet_max77705 (like sweet_touch's GPP bit-bang) + CMGP bank offsets for gpm14/gpm15 (to take from Linux pinctrl-exynos9810 CMGP bank table) + enable SweetBattery on Exynos (boot-ios cmdline has sweetbattery=0). Read only first.
- **Brightness (medium):** nothing exists on either S9 build. Needs DCS commands to the S6E3HA8 panel over the Exynos DSIM (command FIFO) and a brightness interface iOS uses (backlight/IOMobileFramebuffer brightness property).
- **RAM 4 GB (medium-large, risky):** XNU takes one span (dram-base/dram-size) with holes (patch 0104). Today 0x92000000-0xF0000000 (~1.3 GB usable). The other 2 GB is the bank at 0x880000000; a span to it covers ~34 GB with a ~30 GB hole: pv/page tables sized by the span, physmap mapping non-existent PA (speculative access risk). Needs a look at 0104 and pmap before deciding.
- **Audio (large):** Exynos ABOX DSP + Cirrus codec + speaker amp; nothing exists (SDM845 only has the network S9Audio HAL driver).
- **Wi-Fi (very large):** Broadcom BCM4361 on PCIe: Exynos PCIe host driver + a Broadcom FullMAC driver + firmware + an iOS networking interface; nothing exists.
- **Patch 0112 (battery, written, not built):** sweet_max77705.c gets an Exynos backend: CMGP pinctrl 0x14220000, SCL = gpm14 (bank 0x180), SDA = gpm15 (bank 0x1A0), from Linux pinctrl-exynos.c exynos9810_pin_banks3 (bank_type_7: CON +0 4-bit, DAT +4, PUD +8). Open drain: CON 1 + DAT 0 = low, CON 0 = released. Logs the pins' original CON/DAT/PUD (expect function 2 = HSI2C) and the idle line levels (expect sda=1 scl=1). SweetBattery no longer forces the static fallback on Exynos (only sweetbattery=1, the Redmi's PM6150, is mapped to 0). Read-only: charging writes still need the sweet_charge sysctl. Boot image must drop sweetbattery=0.
- xnuw .git/refs vanished again (the work tree is under %TEMP%; empty dirs get cleaned). Recreated .git/refs/{heads,tags}; HEAD is detached, history intact (0111 = 001b8c0, 0112 on top). The first 0112 commit (5739800) carried an EMPTY patch file; fixed in the next commit. Consider moving xnuw out of %TEMP%.

- **0112 compiled** (run 40885884684, success): `out/exynos/xnu9/kernel.development.sweet` sha256 ae9c245a... **Boot image `out/exynos/boot-ios13.img`** (sha256 b1d46993...) = boot-ios12 (@2x ADT, 30 Hz, 1794 MHz) with the 0112 kernel and WITHOUT sweetbattery=0 (MAX77705 gauge). Not flashed. Expect: 'SWEET MAX77705: Exynos CMGP gpm14/gpm15 CON 0x2/0x2 ...', 'lines sda=1 scl=1', 'SWEET BATTERY: MAX77705 fuel gauge answers'. Compare % with TWRP's /sys/class/power_supply/battery/capacity.

- boot-ios13 flashed 2026-10-29 with verified readback, rebooted into iOS. TWRP reference just before: capacity 50 %, voltage_now 3787000 uV, current_now -167, status Charging (Fast), temp 34.8 C.

- **Battery WORKS on the Exynos S9 (user, boot-ios13, 2026-10-29): iOS shows about 50 %, matching TWRP's 50 % just before.** MAX77705 fuel gauge over bit-banged CMGP gpm14/gpm15 (0112). Not yet checked in the log: original pin CON values, charging indicator, current/voltage values iOS publishes. Next requested feature: brightness.


## Brightness: patch 0113 (written 2026-10-29, not built)

Research (observed in sources): panel lcdtype 9503811 = 0x910443 = Samsung's `s6e3ha8_star_a3_s0` (LineageOS universal9810 drivers/video/fbdev/exynos/panel/s6e3ha8/). Vendor set-bl = level2 key + gamma + AOR + ELVSS + ... + F7 03; mainline panel-samsung-s6e3ha8.c has no brightness control (fixed: CA gamma, B1 00 0C = AOR 0 %). s0 AOR table: 0x0B76 (dimmest) .. 0x000C (full). DSIM 0x16080000 (DT dsim@0x16080000): INTSRC 0x50 (bit 28 SFR_PH_FIFO_EMPTY, write-1-clear), PKTHDR 0x58 (DI | data0 << 8 | data1 << 16), PAYLOAD 0x5C (LE words) - dpu_9810 dsim_drv.c dsim_write_data/dsim_long_data_wr. iOS side: CoreBrightness strings name "AppleARMBacklight", "IODisplayParameters", "brightness", "backlight-control", "backlight-level", "IOMFBBrightnessLevel"; exact call shape not traced.
0113 (SweetFramebuffer.cpp, Exynos only): AOR-only dimming (gamma as S-Boot left it), DCS long writes F0 5A 5A / B1 hi lo / F7 03 / F0 A5 A5 through the DSIM FIFO (20 ms timeout, disables itself on failure); level 0..1000 -> lum = level^2/1000 -> AOR. AppleARMBacklight nub attached under AppleCLCD with IODisplayParameters/brightness {min 0, max 1000, value} + backlight-control; setProperties logs the XML it receives ("SWEET BL: setProperties ...") and takes brightness at top level or in IODisplayParameters (number or {value}). Boot-arg sweetbl=N sets a level at boot (panel path test without iOS).
- **0113 compiled** (run 40889078097, success): `out/exynos/xnu10/kernel.development.sweet` sha256 218e5395... **Boot image `out/exynos/boot-ios14.img`** (sha256 5f125852...) = boot-ios13 cmdline + **sweetbl=200** (lum 40/1000 -> AOR ~0x0B05): the screen should dim at the framebuffer start. Not flashed. Look for 'SWEET BL: DSIM INTSRC', 'SWEET BL: level 200 -> AOR 0x.... (0/0)', 'SWEET BL: setProperties ...'.

- **Display Zoom -> dark screen (user, boot-ios13; log s9/logs/boot-ios13-zoom.txt, observed):** no crash. With Zoom on, backboardd renders 750x1624 (zoomed canvas) instead of 828x1792 and keeps creating 750x1624 surfaces while `IOServiceMatchingfailed for: AppleM2ScalerCSCDriver` repeats (the hardware scaler it would use to fit 750x1624 onto the 828x1792 display does not exist here), so nothing is presented. Fix today: restore USERDATA to the ElleKit image (Zoom off). Real fix later: offer a 750x1624 mode / scale in SweetFramebuffer, or an AppleM2ScalerCSCDriver stand-in. Avoid Display Zoom meanwhile.
- Same log: battery driver live data `SWEET BATTERY: soc=47.4% vcell=3708mV avg=-223mA now=-685mA rep=1408/2967mAh design=2967mAh cycles=0.86 chgin=0 ...` (unplugged).
- 2026-10-29: boot-ios14 flashed (verified readback 5f125852...) and USERDATA restored to the ElleKit v3 image via --changed (18 chunks, whole range 6b408b59... = image; Display Zoom off again). Rebooted into iOS. Test pending: screen dimmer at boot (sweetbl=200), brightness slider, 'SWEET BL:' log lines.

- **Brightness WORKS on the Exynos S9 (user, boot-ios14, 2026-10-29): the screen came up dimmer (sweetbl=200 at framebuffer start) and the Control Center brightness slider works.** AOR-only dimming over the DSIM command FIFO + AppleARMBacklight nub (0113). Not yet read from the log: the exact setProperties shape iOS sends (SWEET BL: setProperties ...). Boot images can drop sweetbl (iOS sets the level itself). Remaining requests: RAM 4 GB, audio, Wi-Fi.


## RAM 4 GB: patch 0114 + `exynos/mkadt.py --ram4g` (written 2026-10-29, not built)

- How it worked (0104, observed): one XNU span [dram-base, +dram-size), ADT /chosen/sweet-ram-holes kept off the free list by pmap_is_bad_ram; the vm_page array must be contiguous (arm64 derives a page's PA from its index), so hole pages keep their vm_page entries.
- --ram4g ADT (`out/exynos/exynos-ufs-xr2-4g.adt`, @2x): span 0x92000000-0x8fff00000 (33.72 GiB of address space) with an extra hole 0xf0000000-0x880000000; RAM for XNU 3.31 GiB (was ~1.3). Cost: 2,209,728 16K pages -> ~101 MiB vm_page + 17 MiB pv_head. Live DT (TWRP): 2 GB bank 0x880000000+0x80000000, no reserved-memory in it.
- 0114 (arm_vm_init.c, SWEET only): (1) arm_vm_physmap_init splits "rest of physmem" around ADT holes >= 1 GB so the ~30 GB with no RAM is never mapped (a speculative access to Normal memory with nothing behind it can SError); (2) sweet_ram_hole_bytes() subtracted from mem_actual (-> max_mem_actual / hw.memsize), max_mem and sane_size, so iOS sees ~3.3 GB, not ~34 GB. mem_size/gPhysSize stay the span (physmap, pv tables). DT is available there (PE_init_platform(FALSE) runs before arm_vm_init).
- Known residual risk: start.s's bootstrap V=P/KVA mappings cover memSize (whole span incl. the hole) with 32 MB L2 blocks until arm_vm_init switches tables (one L2 page per mapping: 64 GB per L1 with 16K pages, so no table overflow).
- **0114 compiled** (run 40896591093, success): `out/exynos/xnu11/kernel.development.sweet` sha256 1acca22f... **Boot image `out/exynos/boot-ios15.img`** (sha256 9c90a31b...) = 0114 kernel + **4 GB ADT** (exynos-ufs-xr2-4g.adt) + boot-ios14 cmdline without sweetbl. Not flashed. Fallback: boot-ios14 (1.3 GB). Check: boots at all; 'pmap_startup() ... pages', hw.memsize ~3.3 GB.

- boot-ios15 (4 GB) flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **4 GB WORKS (boot-ios15, log s9/logs/boot-ios15.txt, observed):** booted to the lock screen (user); `vm_page_bootstrap: 205359 free pages, 11505 wired pages` = 3.31 GiB managed / 3.13 GiB free (1.3 GB layout, boot-ios12/13: 83030 free + 13162 wired = 1.47 / 1.27 GiB). pmap_startup 300 ms (was 16 ms) for the larger vm_page array. Crashes: driverkitd x1 only (as on every boot). No panic/SError. Not yet checked: hw.memsize as iOS reports it (should be ~3.3 GB), long use touching the high bank.

## Audio: hardware map and staged plan (researched 2026-10-29, nothing built)

Stock DT (observed):
- ABOX `/abox@0x17C50000` (samsung,abox): sfr 0x17C50000, sysreg 0x17C10000, sram 0x17D00000 (0x31000); 8 RDMA (0x17C51000+0x100*n), 5 WDMA (0x17C52000+), 4 UAIF (0x17C50500/510/520/530), DSIF; own GIC `abox_gic@0x17CF0000` (ABOX has its own CPU running Samsung firmware, ext_bin@0/1); IPC in SRAM (tx 0x30000, rx 0x30300, acks 0x302FC/0x305FC); power domain, clocks pll/cpu/audif/dmic/bus, `iommus` (SysMMU in front of its DMA); reserved abox_rmem 0xFB200000 (40 MB). In Samsung's driver PCM buffers are configured through IPC to that firmware.
- Codec Cirrus **CS47L93** on `spi@10470000` (cs47l93@0, own ADSP + firmware, accessory detect, micbias); reset pin pinctrl@10430000 codec-reset, IRQ codec-irq (ALIVE). Clock: `/sound` clocks xclkout (PMU clock out), fll1-refclk 26 MHz, sysclk 98.304 MHz.
- Speaker amp **MAX98512** on `hsi2c@108C0000` addr 0x39 (pins `max98512-i2c` in pinctrl@10830000), sysclk 12.288 MHz, mono.
- Card `/sound` (samsung,star-madera): uaif@0 -> codec AIF1, uaif@1 -> MAX98512 (routing `SPEAKER <- HiFi Playback`), uaif@2 -> codec AIF3, uaif@3 -> BT; RECEIVER <- HPOUT3L/R, HEADPHONE <- HPOUT2L/R. So: loudspeaker = ABOX UAIF1 -> amp directly; earpiece/headphones = codec.
- iOS side exists: s9/ios-tools/S9Audio.c (AudioServerPlugIn, 48 kHz stereo; today UDP to the PC). For the phone: write the mixed buffers to a kernel PCM device instead.

Staged plan:
1. Probe, read-only, kernel boot-arg: MAX98512 revision over bit-banged I2C, CS47L93 device ID over SPI (USI SPI or bit-bang), ABOX power-domain status/version register.
2. First sound without ABOX: CS47L93 tone generator -> HPOUT3 (earpiece) or HPOUT2 (headphones); needs xclkout 26 MHz + codec init.
3. ABOX: power domain + PLL_AUD + firmware (calliope, from the vendor partition) + IPC + SysMMU, RDMA -> UAIF1 -> MAX98512 -> loudspeaker.
4. iOS: kernel PCM ring buffer + S9Audio writing to it.
Stages 1-2 are days; 3 is the big one (weeks, firmware protocol).
- **Audio stage 1: patch 0115 (written, not built):** exy_audio_probe() in sweet_touch.c, boot-arg sweetaudio=1, runs right after the DVFS step. Reads: PMU pd-aud 0x14064000 config/status; S2MPS18 LDO33/LDO34 ctrl (0x6A/0x6B, assumed from LDO35 = 0x6C, LDO43 = 0x74) over ACPM; codec reset GPG1-0 (PERIC0 bank 0xA0) con/dat; MAX98512 (0x39) 16-bit regs 0x0402/0x0600 over bit-banged I2C on PERIC1 GPP4-4 SCL / GPP4-5 SDA (bank 0x000), pins restored; CS47L93 register 0 (expect 0x6371) over bit-banged SPI on PERIC0 GPP0-6 CLK / GPP0-4 MOSI / GPP0-5 MISO / GPP0-7 CS ONLY if both LDOs are enabled (bits 7:6) and reset is an output driven high. Sources: Linux pinctrl-exynos.c banks 6/7, madera-core.c (CS47L92_SILICON_ID 0x6371 covers CS47L93), max98512.h (R0402_REV_ID, B: R0600). Log lines "SWEET AUDIO: ...".
- **0115 compiled** (run 40900163015, success): `out/exynos/xnu12/kernel.development.sweet` sha256 c5b03a96... **Boot image `out/exynos/boot-ios16.img`** (sha256 716aff30...) = boot-ios15 (4 GB) + **sweetaudio=1**. Not flashed.

- boot-ios16 (audio probe) flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **Audio stage 1 RESULT (boot-ios16, log s9/logs/boot-ios16.txt, observed):** `pd-aud config 0xf status 0x10000f` (AUD power domain ON after S-Boot); `codec LDO33 0xec LDO34 0xe8` (both enabled, bits 7:6), `reset GPG1-0 con 0x1 dat 0` (CS47L93 held in reset, active low -> SPI read skipped by design); `MAX98512 bus con 0 idle scl=1 sda=1, rev 0x0402=65 0x0600=0` -> **MAX98512 answers at 0x39, REV_ID 0x41 (not the B variant)** over bit-banged I2C on GPP4-4/5. Boot otherwise normal (driverkitd x1).
  - Next (stage 1b): release the codec reset (GPG1-0 high; Linux madera does this after enabling the supplies) and read register 0 over SPI (expect 0x6371). Then stage 2 (tone to the earpiece) needs MCLK: codec FLL ref = xclkout 26 MHz on AUD pin gpb0-0 (AUD domain is on).
- **Patch 0116 (stage 1b, written):** exy_audio_probe releases the CS47L93 reset (GPG1-0 high) when the supplies are on and the pin is an output driven low, waits 20 ms, reads registers 0 (silicon ID, expect 0x6371) and 1 (hardware revision) over bit-banged SPI, then drives reset low again (as S-Boot left it). Still no codec register written.

- **0116 compiled** (run 40902283550, success): `out/exynos/xnu13/kernel.development.sweet` sha256 1fb42d21... **Boot image `out/exynos/boot-ios17.img`** (sha256 27b10fdc...) = boot-ios16 cmdline (4 GB, sweetaudio=1) with the 0116 kernel. Not flashed.

- boot-ios17 flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **Stage 1b RESULT (boot-ios17, log s9/logs/boot-ios17.txt, observed):** same pd-aud/LDO/MAX98512 lines as boot-ios16; `CS47L93 SPI con 0, reset released for the read, register 0 = 0 (expect 0x6371), revision 0`. GPP0 con 0 = USI2 pins were plain inputs (S-Boot never set SPI function). MISO sampled 0 throughout. Leading suspect: pin roles (CLK/MISO/MOSI = GPP0-6/5/4 was inferred from Samsung's DT ordering, not a datasheet). Next: try the 6 CLK/MOSI/MISO permutations of GPP0-4/5/6 (CS fixed GPP0-7), log MISO idle level and each read; worst case of a wrong permutation is a garbled transfer (floating MOSI ~ write of 0 to register 0 = soft reset).
- **Patch 0117 (written):** the CS47L93 SPI step tries all 6 CLK/MOSI/MISO orders of GPP0-4/5/6 (CS GPP0-7), logs the idle levels of GPP0-4/5/6 and register 0 for each order ('<-- MATCH' at 0x6371); codec reset released only for the reads.

- **0117 compiled** (run 40904382918, success): `out/exynos/xnu14/kernel.development.sweet` sha256 ccfd2f5b... **Boot image `out/exynos/boot-ios18.img`** (sha256 13a8a6d6...) = boot-ios17 cmdline with the 0117 kernel. Not flashed.

- boot-ios18 flashed 2026-10-29 with verified readback, rebooted into iOS. Result pending.

- **CS47L93 ANSWERS (boot-ios18, log s9/logs/boot-ios18.txt, observed): SPI wiring = CLK GPP0-4, MOSI GPP0-5, MISO GPP0-6, CS GPP0-7; register 0 = 0x6371** (CS47L92/93 silicon ID) with Madera framing over bit-banged SPI. The DT's spi2-bus "gpp0-6, gpp0-5, gpp0-4" is MISO/MOSI/CLK. Other 5 orders read 0. Idle gpp0-4/5/6 = 0/0/0 (PUD 0x1111 on gpp0-4..7 = pull-down). Audio stage 1 complete: AUD domain on, MAX98512 rev 0x41 on I2C, CS47L93 on SPI.
- Same boot: one SpringBoard crash = the CoreImage GL crash (unslid lr 0x194B3858C) in SpringBoard[26], the first SpringBoard, which starts before ElleKit's loader; launchd restarts it and the next one has S9CISoft. Happens once per boot while Bold Text is on (no respring job on this root: no shell).
- Next, stage 2: earpiece tone - codec init over SPI (clocking: FLL1 from MCLK1 = xclkout 26 MHz on AUD gpb0-0, SYSCLK 98.304 MHz), tone generator -> HPOUT3 (receiver). Needs the Madera/CS47L92 register sequences from madera-core.c / cs47l92.c.

## Audio stage 2: earpiece tone - patch 0118 (written 2026-10-29, not built)

- MCLK1 = OSC_AUD = VCLK_CLKOUT0 (dt-bindings clock 921 = CLK_CLKOUT_BASE + 1; /sound clocks xclkout) = **PMU + 0xA00**: SEL [13:8] = 1 (TCXO 26 MHz), bit 0 = DISABLE (Linux cal-if ra.c ra_enable_clkout: set sel, clear bit 0). pinctrl aud-codec-mclk (AUD gpb0-0) is defined but referenced by nothing.
- Madera registers (registers.h): TONE_GENERATOR_1 0x20 (TONE1_ENA 1), SYSTEM_CLOCK_1 0x101 (SRC [3:0], ENA 0x40, FREQ [10:8]), SAMPLE_RATE_1 0x102 (48 kHz = 3), OUTPUT_ENABLES_1 0x400 (OUT3L_ENA 0x20, EP_SEL 0x8000), OUTPUT_PATH_CONFIG_3L 0x420 (OUT3_MONO 0x1000), DAC_DIGITAL_VOLUME_3L 0x421 (VU 0x200, MUTE 0x100, vol 0x80 = 0 dB), OUT3LMIX_INPUT_1_SOURCE/VOLUME 0x6A0/0x6A1 (Tone Generator 1 = 0x04; vol code 0x20..0x50 << 1), IRQ1_RAW_STATUS_1 0x1880 (BOOT_DONE 0x80). cs47l92.c: OUT3 demux EP_SEL with outputs off; madera_hp_ev just sets the enable bit (clamp/short only with jack detect); madera_out_ev: 6 ms up delay on CS47L92/93.
- 0118: probe uses the proven SPI wiring + madera_rd/madera_wr; sweetaudio=2 -> exy_audio_tone(): CLKOUT0 on, poll BOOT_DONE, SYSCLK = MCLK1 declared 24.576 MHz (26 MHz real: +5.8 % pitch), 48 kHz, TONE1 -> OUT3L mix -12 dB, DAC3L -12 dB, OUT3 mono, EP_SEL 0, OUT3L_ENA, 40 ms; logs the registers; plays sweetaudiotone s (default 3), then OUT3L/TONE/SYSCLK off, CLKOUT0 restored, codec back in reset. Not applied: the CS47L92 register patch from madera-core, the FLL, DRE/EDRE.
- 0118 first CI run 40907515020 FAILED (redefinition of rev: the MAX98512 probe already uses it); renamed to crev. **0118 compiled** (run 40908337260): `out/exynos/xnu15/kernel.development.sweet` sha256 ab6ddd95... **Boot image `out/exynos/boot-ios19.img`** (sha256 98d682bc...) = boot-ios18 cmdline with **sweetaudio=2** (earpiece tone, 3 s, during early boot). Not flashed.

- boot-ios19 (earpiece tone) flashed 2026-10-29 with verified readback, rebooted. Result pending.

- **boot-ios19 RESULT: silent (user).** Log (observed): `CS47L93 register 0 = 0x6371, revision 0x1`; `CLKOUT0 0 -> 0x100, codec boot done after 0 ms`; `tone on: SYSCLK1 0x240 RATE1 0x3 TONE1 0x1 MIX 0x8004/0x68 DAC3L 0x68 PATH3L 0x1080 OUTEN 0x20 IRQRAW 0x80` - every write took (mixer bit 15 set = input active, guess), so the digital path runs and the problem is the output stage.
- **Patch 0119 (written):** suspect = the receiver is across HPOUT3L and HPOUT3R (DT routing lists both; OUT3 mono is differential) and only OUT3L was on. Now: Linux's cs47l92_reva_16_patch (38 writes, revision 0x1 = rev A) after boot done, tone into OUT3L and OUT3R mixers (0x6A0/0x6A8), DAC 3L/3R unmuted -12 dB (VU on the R write), OUT3L_ENA | OUT3R_ENA; logs HP_CTRL_3L (0x229) and IRQ1 raw status 1-6 (0x1880-0x1885); 5 s.
- **0119 compiled** (run 40910290875, success): `out/exynos/xnu16/kernel.development.sweet` sha256 8ef32dd6... **Boot image `out/exynos/boot-ios20.img`** (sha256 739a4ef6...) = boot-ios19 cmdline (sweetaudio=2) with the 0119 kernel. Not flashed.

- boot-ios20 (tone on both OUT3 halves + rev A patch) flashed 2026-10-29 with verified readback, rebooted. Result pending.

- **boot-ios20 RESULT: still silent (user).** Log (observed): all writes read back (MIX 0x8004/0x68 both halves, DAC3 0x68/0x68, PATH3L 0x1080, OUTEN 0x30, HP_CTRL_3L 0x1a0e); IRQ1 raw status 1-6 = 0x80 0x20 0 0 0 0 -> BOOT_DONE + MICD_CLAMP_FALL_STS1 (jack-detect clamp state).
  - DT accdet `acc@1 reg = <2>`: jack detection is on OUT2 (HEADPHONE <- HPOUT2), so the clamp is the jack's, not OUT3's.
  - **New reading (guess, to verify):** the MAX98512 node has `maxim,sub_reg = <0x3a>`, `spk-gain-rcv`, `digital-gain-rcv`, `mono_stereo` -> a second MAX98512 at I2C 0x3A drives the EARPIECE (S9 stereo speakers: bottom + earpiece), both fed by ABOX UAIF1. Then the codec's RECEIVER/HPOUT3 route is unused on this model and the codec's audible output is the 3.5 mm jack (HPOUT2). Next: probe I2C 0x3A (rev 0x0402) and send the tone to OUT2 (HPOUT2L/R, stereo) with wired headphones plugged in.
- **Patch 0120 (written):** tone to the HEADPHONE jack: OUT2L/R mixers 0x690/0x698 = Tone 1 at -12 dB, DAC 2L/2R (0x419/0x41D) unmuted -24 dB (about -36 dBFS), OUT2_MONO cleared (0x418), OUT2L_ENA|OUT2R_ENA (0x0C); logs HP_CTRL_2L (0x227) and MICD_CLAMP_CONTROL (0x2C6), not changed. Amp probe also reads I2C 0x3A (rev 0x0402) for the second (earpiece) MAX98512.

- **0120 compiled** (run 40912670850, success): `out/exynos/xnu17/kernel.development.sweet` sha256 4fcbff34... **Boot image `out/exynos/boot-ios21.img`** (sha256 c96fffaa...) = boot-ios20 cmdline (sweetaudio=2) with the 0120 kernel: tone to the headphone jack. Not flashed.

- boot-ios21 (headphone tone) flashed 2026-10-29 with verified readback, rebooted. Result pending.

- **FIRST SOUND (boot-ios21, user heard the tone in wired headphones; log s9/logs/boot-ios21.txt):** CS47L93 Tone Generator 1 -> OUT2L/R (HPOUT2, headphone jack), SYSCLK from MCLK1 = PMU CLKOUT0 26 MHz. Log: `tone on: SYSCLK1 0x240 RATE1 0x3 TONE1 0x1 MIX 0x8004/0x68 0x8004/0x68 DAC2 0x50/0x50 PATH2L 0x80 OUTEN 0xc HPCTRL2L 0x1a0e CLAMP 0x210`; IRQ raw 0x80 0x20 (clamp flag present, did not block).
- **Second MAX98512 confirmed at I2C 0x3A** (`second amp at 0x3a: rev 0x0402=65` = 0x41): the earpiece is the stereo-speaker amp, so the codec's OUT3/RECEIVER tries (boot-ios19/20) could not be heard. Speakers (bottom 0x39 + earpiece 0x3A) = ABOX UAIF1 -> MAX98512 x2.
- Audio stages 1 and 2 done. Next: stage 3 ABOX (power/clocks are on, firmware + IPC + SysMMU) -> UAIF1 -> both amps; or a quicker step first: PCM to the headphones through the codec's AIF from ABOX UAIF0. Either way ABOX is next.

## Audio stage 3: ABOX (started 2026-10-29)

- Firmware (vendor partition sda19, mounted read-only from TWRP, copied to git-ignored `s9/out/firmware/`): calliope_sram.bin 34676 B (sha256 15e44376...), calliope_dram.bin 2209916 B (6fb029b8...), calliope_iva.bin 57728 B (2455175a...), plus cs47l92-dsp1-trace.wmfw, dsm.bin, dsm_tune.bin. Samsung's; never in git.
- Driver (LineageOS universal9810 sound/soc/samsung/abox/abox.c, 9810 defines): boot = [if secure GIC: PMU ABOX_MAGIC 0x814 := 0, SMC 0x82000501, wait 0xAB0CAB0C, cpu off] -> sysreg MISC_CON (0x32C) = 1 -> clk cpu + audif -> ABOX GIC init -> copy SRAM fw to 0x17D00000 (0x31000), DRAM fw to dram_base (DRAM_FIRMWARE_SIZE 18 MB) mapped at IOVA 0x80000000, IVA fw at IOVA 0x90000000 -> PMU ABOX_CPU_CONFIGURATION 0x415C bit 0, ABOX_CPU_OPTION 0x4164 bit 28, poll ABOX_CPU_STATUS 0x4160 bit 0 -> firmware IPC boot done ("Calliope is ready to sing").
- IOVA layout (abox.h): DRAM fw 0x80000000, IVA 0x90000000, RDMA buffers 0x91000000 + n MB, WDMA 0x92000000, compr 0x93000000, VDMA 0x94000000, VSS 0xA0000000, dump 0xD0000000. SysMMU `/sysmmu@17E00000` (port Abox, no-suspend; exynos-iommu.h: MMU_CTRL 0x0 = 0x5 enabled / 0 bypass, VERSION 0x34).
- Idea: if the SysMMU stays in bypass, IOVA = PA, and 0x80000000-0x91FFFFFF is low RAM XNU never uses (span starts 0x92000000; holds only the shim/DTB, sec_debug_magic 0x80000000 and zswap_debug 0x91300000 pages) -> DRAM firmware + playback buffers without SysMMU page tables. (WDMA 0x92000000 would hit the kernel: no capture that way.)
- **Patch 0121 (written): sweetaudio=3 -> exy_abox_probe()**, read only: PMU 0x814/0x415C/0x4160/0x4164, then ABOX SFR (IP_INDEX/VERSION/SYSPOWER), sysreg MISC_CON/L2_CACHE_CON, SysMMU CTRL/CFG/STATUS/VERSION; logs and sleeps 11 s (one 10 s sweetlog flush) before each of those three blocks, so a bus hang leaves the block's name on UFS.
- **0121 compiled** (run 40915193774, success): `out/exynos/xnu18/kernel.development.sweet` sha256 3b6491ed... **Boot image `out/exynos/boot-ios22.img`** (sha256 2f755ca7...) = boot-ios21 cmdline with **sweetaudio=3** (ABOX probe; no tone). Not flashed. Fallback if it hangs: boot-ios15 (4 GB, no audio probe).

- boot-ios22 (ABOX probe) flashed 2026-10-29 with verified readback, rebooted. Result pending.

- **ABOX probe RESULT (boot-ios22, log s9/logs/boot-ios22.txt, observed):** `PMU magic 0x3100 cpu_config 0x1 cpu_status 0x20000 cpu_option 0` (ABOX CPU locally powered but not enabled/running; magic != 0xAB0CAB0C); `SFR ip_index 0x41424f58 ("ABOX") version 0x1010100 syspower ctrl 0 status 0` (ABOX SFR clocked and readable from the AP); then `reading sysreg 0x17c10000...` and the system FROZE (log ends after flush #2 with backboardd lines from the 11 s wait). SysMMU not reached.
  - Cause (strong guess): the ABOX sysreg block is TrustZone-protected: DT pd-aud has `need_smc = <0x17c10204>` (inside 0x17C10000), i.e. Samsung's kernel accesses it through the secure monitor (exynos_smc register read/write), not directly. Never read 0x17C10000 directly again.
  - boot-ios22 freezes during boot: do not leave it on BOOT; fall back to boot-ios15/21.
  - Next: SysMMU (0x17E00000) read on its own; ABOX sysreg via SMC (find Samsung's SMC_CMD_REG / SMC_REG_ID_SFR_R/W ids in include/soc/samsung/exynos-smc.h of the S9 kernel).
  - User: boot-ios22 **kernel panicked** (not just froze) at that read - consistent with an SError from a TrustZone-protected access. Panic text not in the sweetlog (last flush was before the read).

- boot-ios15 (4 GB, battery, brightness, no audio probe) restored on BOOT 2026-10-29 after the boot-ios22 panic (verified readback 9c90a31b...), rebooted into iOS.

- **need_smc = DTZPC, not register access (observed in sources):** exynos-pd.c reads need_smc -> cal_pd_set_smc_id -> pmucal_local.c: after a PD power-on, `exynos_pd_tz_restore(need_smc)`; exynos-el3_mon.c: `exynos_smc(SMC_CMD_PREAPRE_PD_ONOFF = 0x82000410, EXYNOS_WAKEUP_PD_DOWN = 1, addr, 0)` (save = EXYNOS_GET_IN_PD_DOWN 0 before power-off). S-Boot powered AUD on but never asked EL3 to restore the DTZPC, so the ABOX sysreg is secure-only -> the boot-ios22 panic.
- **Patch 0122 (written):** sweetaudio=3 issues smc(0x82000410, 1, 0x17c10204, 0) (flushed log line first), logs the return, then the flushed sysreg and SysMMU reads as before.
- **0122 compiled** (run 40919984471, success): `out/exynos/xnu19/kernel.development.sweet` sha256 bbbb488c... **Boot image `out/exynos/boot-ios23.img`** (sha256 e1981f5a...) = boot-ios22 cmdline (sweetaudio=3) with the 0122 kernel (DTZPC restore before the sysreg read). Not flashed. Fallback: boot-ios15.

- boot-ios23 (DTZPC restore + ABOX probe) flashed 2026-10-29 with verified readback, rebooted. Result pending.

- **boot-ios23 RESULT (photo + log s9/logs/boot-ios23.txt, observed):** `DTZPC restore smc(0x82000410, 1, 0x17c10204) = 0` (EL3 accepted), then `reading sysreg 0x17c10000...` -> panic "Kernel data abort" on CPU 3, esr 0x96000010 (EC 0x25 data abort same EL, DFSC 0x10 synchronous external abort). So the restore alone does not open the ABOX sysreg.
  - Hypotheses (not verified): (a) the restore only reapplies what a prior save (EXYNOS_GET_IN_PD_DOWN 0, before power-off) stored; S-Boot powered AUD on without that cycle, so mimic Linux: save -> AUD PD off -> on -> restore; (b) the sysreg's PCLK gate in CMU_AUD is off (ABOX SFR at 0x17C50000 is readable, sysreg may be gated separately) -> find CMU_AUD gate registers (cmucal-sfr.c "SYSREG_AUD").
  - boot-ios23 panics on every boot: put boot-ios15 back.
- boot-ios15 restored on BOOT 2026-10-29 after the boot-ios23 panic (verified readback 9c90a31b...), rebooted into iOS.

- **Clock path (sources, observed):** CMU_AUD = 0x17C00000 (cmucal-sfr.c SFR_BLOCK(AUD, 0x17c00000, 0x8000)). SYSREG_AUD has no plain gate, only a Q-channel: QCH_CON_SYSREG_AUD_QCH 0x3064 (also SYSMMU_AUD 0x3060, ABOX ACLK 0x3024, GPIO_AUD 0x304C, ABOX BCLK0-3 0x3028-0x3034); fields ENABLE bit 0, CLOCK_REQ bit 1, IGNORE_FORCE_PM_EN bit 2, EXPIRE_VAL [25:16].
- **Patch 0123 (written):** after the DTZPC restore, read the four Q-channels (flushed line first), then set CLOCK_REQ=1/ENABLE=0 on SYSREG_AUD and SYSMMU_AUD (manual clock on), log, then the flushed sysreg and SysMMU reads.
- **0123 compiled** (run 40922996377, success): `out/exynos/xnu20/kernel.development.sweet` sha256 fe647c60... **Boot image `out/exynos/boot-ios24.img`** (sha256 c3af3ae2...) = boot-ios23 cmdline (sweetaudio=3) with the 0123 kernel (Q-channel clock request before the sysreg read). Not flashed. Fallback: boot-ios15.

- **boot-ios24 flashed** to BOOT (sda10), readback sha256 c3af3ae2... matches. Rebooted. Fallback boot-ios15.

- **boot-ios24 result** (log s9/logs/boot-ios24.txt, not in git if ignored): DTZPC restore smc = 0; Q-channels read **0x3f0002** for sysreg/sysmmu/abox_aclk/gpio *before* any write (ENABLE=0 so no HW gating, CLOCK_REQ=1 already) -> clocks were already on; clock-gating hypothesis (b) **ruled out**. Log stops right after `reading sysreg 0x17c10000...` (same as ios23: the read still aborts/hangs; no panic text reaches sweetlog). Next: hypothesis (a) DTZPC save -> AUD PD off/on -> restore, or the access is blocked by TZPC for non-secure regardless (then do sysreg setup via SMC or skip the sysreg).

- **Why the ABOX sysreg read aborts (analysis 2026-10-29, sources):** esr 0x96000010 = synchronous external abort (the slave/TZPC refused the AP), not an MMU fault; AUD is on (pd-aud status 0x10000f), CMU_AUD and the ABOX SFR answer, Q-channels 0x3f0002. abox.c on 9810 does no secure call before writing SYSREG MISC_CON (is_secure_gic() is 8895-only), and pmucal aud_save reads SYSREG_AUD 0x104/0x300 *before* the first DTZPC save, so on Android the sysreg answers with AUD as S-Boot left it. What differs on our boot is unknown -> measure instead of guess.
- **Patch 0124 (written, not built):** sleh.c handle_kernel_abort sends a FSC_SYNC_EXT_ABORT at `sweet_probe_ld` to `sweet_probe_fail` (bus-error-safe MMIO read `sweet_probe_rd32`, sweet_touch.c). sweetaudio=3 now scans QCH/sysreg(0,0x104,0x204,0x300,0x328,0x32c)/SysMMU logging ABORT instead of panicking, then GPIO_AUD/SRAM, then tries (A) DTZPC save+restore, (B) ABOX CPU off + DTZPC save + AUD PD off/on (PMU 0x4000/0x4004) + restore, (C) smc 0x82000701 (abox.c MANUAL_SECURITY_CHANGE), rescanning after each; stops at the first that opens the sysreg.
- **0124 compiled** (run 40958223112, success): `out/exynos/xnu21/kernel.development.sweet` sha256 4dabb8a6... **Boot image `out/exynos/boot-ios25.img`** (sha256 3f8c5d6c...) = boot-ios24 cmdline (sweetaudio=3) with the 0124 kernel (safe ABOX probe + A/B/C). Not flashed. Fallback: boot-ios15.

- **boot-ios25 flashed** to BOOT (sda10), readback sha256 3f8c5d6c... matches. Rebooted. Fallback boot-ios15.

- **boot-ios25 RESULT (log s9/logs/boot-ios25.txt, observed): booted to iOS, no panic. The ABOX sysreg is NOT TrustZone-locked as a block.** As booted (no SMC, no power cycle): sysreg +0 = 0, +0x104 = 0, +0x300 = 0, **+0x32c MISC_CON = 0 (readable)**; only **+0x204 (the DTZPC word) and +0x328 (L2_CACHE_CON) ABORT**. boot-ios22-24 crashed on the L2_CACHE_CON read (abox.c touches it only with ABOX_QUIRK_BIT_SHARE_VTS_SRAM, so it is not for the 9810 AP); the DTZPC/Q-channel theories were wrong. Steps A/B/C were skipped (probe stops at the first open scan), AUD never power-cycled.
- **SysMMU_AUD 0x17E00000 readable:** CTRL 0x4 (bit 0 clear = translation off, so ABOX DMA currently sees physical addresses), CFG 0x20000, STATUS 0, VERSION 0x7040000f (SysMMU v7.4).
- Rule from now on: never touch SYSREG_AUD 0x204/0x328; sweet_probe_rd32 (patch 0124) for any unknown register.
- Next for audio: ABOX firmware boot (calliope_sram -> SRAM 0x17D00000, DRAM firmware at ABOX IOVA 0x80000000: with the SysMMU off that IOVA is PA 0x80000000, which holds sec_debug/shim/DTB, so either program SysMMU v7 page tables or find another layout), then PMU ABOX CPU enable and IPC.
- **Patch 0125 (written, not built): ABOX firmware boot, boot-arg sweetaudio=4** (exy_abox_boot, sweet_touch.c), after abox.c abox_enable() for the 9810 (no secure-GIC path; ABOX GIC is GIC_IS_SECURE_FREE = direct writes):
  - Firmware reaches the kernel with **`tools/mkpayload.py --blob Calliope-SRAM=... --blob Calliope-DRAM=... --blob Calliope-IVA=...`** (new option): loaded after the kernel like --ramdisk, /chosen/memory-map NAME = <pa, exact size>; read with SecureDTGetProperty + ml_static_ptovirt. Blobs stay out of git/CI (s9/out/firmware). Test pack: blobs at 0x944dc000.., payload +2.3 MB; BOOT is 57671680 bytes.
  - IOVA -> PA via a SysMMU_AUD v7 1 MB-section table (entry = pa>>4|2, PT_BASE_PPN 0xC = pt>>12, CTRL 0x7 block -> FLUSH 0x10 = 1 -> CTRL 0x5) into Android's **abox_rmem 0xfb200000 + 40 MB** (RAM hole, unused by XNU): DRAM fw IOVA 0x80000000 (18 MB) -> +0; IVA 0x90000000 -> +0x1200000; RDMA 0x91000000 (8 MB) -> +0x1300000; VSS 0xA0000000 (6 MB) and 0xA1000000 (1 MB) -> zeroed +0x1b00000/+0x2100000; table at +0x2200000; 0x10000000 one to one. Not mapping 0x80000000 one to one because PA 0x80000000 is sec_debug_magic.
  - Sequence: safe-read rmem/GICD/SRAM/SysMMU/MISC_CON (stop on ABORT); log CMU_AUD PLL_CON0 0x120, MUX_CPU 0x1000, DIV_CPU_ACLK 0x180c, MUX_CPU_USER 0x100 and PMU ABOX regs; SysMMU on; MISC_CON 0x32C = 1; GIC: GICC 0x17cf2004 = 0xff, GICD CTRL = 3, PRI x40 = 0x10101010, GICC CTRL = 3; copy SRAM (0x31000, zero tail), DRAM (zero to 18 MB), IVA; SFR SYSPOWER_CTRL 0x10 = 1; PMU 0x415C |= 1, 0x4164 |= 0x10000000, poll 0x4160 bit 0; pad retention off (0x4170 |= 0x10000000, 0x1424 |= 1); poll 3 s for the boot-done IPC in SRAM 0x30300 (ipcid 1 IPC_SYSTEM, +8 msgtype 3 ABOX_BOOT_DONE, +0x14 version), logging ABOX_CPU_PC (SFR 0x2C3C, safe read) and SysMMU INT_STATUS 0x60 / fault addrs 0x70/0x80.
  - Unknown: whether the ABOX CPU clock (PLL_AUD) runs as S-Boot left it; the log shows it.
- **0125 compiled** (run 40961166687, success): `out/exynos/xnu22/kernel.development.sweet` sha256 4df872ed... **Boot image `out/exynos/boot-ios26.img`** (sha256 054da497...) = boot-ios25 cmdline with **sweetaudio=4** + `--blob Calliope-SRAM/-DRAM/-IVA` (s9/out/firmware). Not flashed. Fallback: boot-ios25 (boots iOS) / boot-ios15.

- **boot-ios26 flashed** to BOOT (sda10), readback sha256 054da497... matches. Rebooted. Fallback boot-ios25 / boot-ios15.

- **boot-ios26 RESULT:** panicked (user report). The sweetlog on sda25 is unchanged from boot-ios25 (total 150197 bytes, flush #7), so the panic came before the first 10 s log flush of this boot. Suspects: (a) early boot rejecting the payload blobs / /chosen/memory-map entries (Calliope-*), or (b) exy_abox_boot before its first touch_wait (abox_blob, ml_io_map of the 40 MB rmem, safe peeks of rmem 0xfb200000 / GICD 0x17cf1000, which may raise an SError rather than a synchronous abort). Waiting for the panic text (photo).

- **boot-ios26 panic (photo, observed):** `kmem_alloc(..., 41943040, 0x100021) failed with 3 @vm_kern.c:178` on cpu 1, right after `SWEET ABOX: Calliope-SRAM at 0x944dc000, 34676 bytes / -DRAM at 0x944e8000, 2209916 / -IVA at 0x94704000, 57728` -> the --blob path works (firmware found, right sizes); the panic was exy_abox_boot's single 40 MB ml_io_map of abox_rmem (io_map panics when kernel_map has no 40 MB hole). Panic came before the first sweetlog flush, so sda25 kept the ios25 log.
- **Patch 0126 (written, not built):** abox_rmem is written through 1 MB ml_io_map windows (abox_rm_write, ml_io_unmap after each); the SysMMU table is built in a static 16 KB array and copied; dram[0] read back next to the file's first word.
- **0126 compiled** (run 40965666652, success; run 40964677851 failed: ml_io_unmap undeclared in BSD, fixed by an extern next to ml_io_map): `out/exynos/xnu23/kernel.development.sweet` sha256 2ce99221... **Boot image `out/exynos/boot-ios27.img`** (sha256 fff9251a...) = boot-ios26 cmdline (sweetaudio=4, Calliope blobs) with the 0126 kernel. Not flashed. Fallback: boot-ios25 / boot-ios15.

- **boot-ios27 flashed** to BOOT (sda10), readback sha256 fff9251a... matches. Rebooted. Fallback boot-ios25 / boot-ios15.

- **boot-ios27 panic (photo, observed):** got past the blobs and `SWEET ABOX: rmem 0xfb200000 = 0` (abox_rmem readable by the AP), then `vm_map_delete(...): Attempting to remove permanent VM map entry ... @vm_map.c:7940` from ml_io_unmap: on this XNU ml_io_map allocates KMA_PERMANENT in io_submap and can never be unmapped.
- **Patch 0127 (written, not built):** the abox_rmem windows use ml_io_map_unmappable(pa, 1 MB, VM_WIMG_IO 0x7) (KMA_DATA in kernel_map, which ml_io_unmap frees); declared extern in sweet_touch.c since BSD does not see it. Rule: any mapping that is unmapped later must come from ml_io_map_unmappable.
- **0127 compiled** (run 40967406672, success): `out/exynos/xnu24/kernel.development.sweet` sha256 787051a5... **Boot image `out/exynos/boot-ios28.img`** (sha256 fae18868...) = boot-ios27 cmdline (sweetaudio=4, Calliope blobs) with the 0127 kernel. Not flashed. Fallback: boot-ios25 / boot-ios15.

- **boot-ios28 flashed** to BOOT (sda10), readback sha256 fae18868... matches. Rebooted. Fallback boot-ios25 / boot-ios15.

- **boot-ios28 panic (photo, observed):** iOS was up (OS version 20H392 in the panic, user PCs on cores 0/2/3). Kernel data abort, esr 0x96000010 (synchronous external abort), far 0xfffffe100a43c000, registers x19 0x30300 / x23 0x30308 / x24 0x3030c / x26 0x30314 / x27 0xdead = the boot-done poll loop in exy_abox_boot: a plain touch_rd of ABOX SRAM ipc_rx 0x30300 bus-errored after the ABOX CPU was enabled. So rmem windows, SysMMU, GIC, firmware copy and CPU start got through without a panic; whether the CPU ran is in the sweetlog (pull from TWRP).

- **boot-ios28 log (s9/logs/boot-ios28.txt, observed; flush #2, panic ~10 s after the last line):** rmem 0xfb200000 = 0, GICD = 0, SRAM = 0xa83e60c0, SysMMU 0x4, MISC_CON 0 all readable; **CMU_AUD PLL_CON0 0xa05b0210 (PLL_AUD enabled, locked)**, MUX_CPU 0, DIV_CPU_ACLK 1, MUX_CPU_USER 0x10; PMU cpu_config 1, cpu_status 0x20000, cpu_option 0, pad_ret 0, gpio_mode 0xae4e0000. After the writes: **SysMMU ctrl 0x5 cfg 0x20000 pt_base 0xfd400 (= 0xfd400000, correct) status 0**; **sram[0] 0xe59ff018 (ARM reset vector `ldr pc,[pc,#24]`), dram[0] 0xe3c00007 = file**. Then `starting the ABOX CPU...`; the lines after it were lost with the panic (photo: plain read of SRAM +0x30300 bus error after ENABLE_CPU).
- **Patch 0128 (written, not built):** the boot-done poll uses only safe reads (abox_sp: value or 0xabababab), runs 10 s (abox.c BOOT_DONE_TIMEOUT_MS) with touch_wait(100), and logs ABOX_CPU_PC, PMU cpu_status, SYSPOWER_STATUS, SRAM +0 / +0x30000, the rx IPC words and SysMMU INT_STATUS/fault addresses.
- **0128 compiled** (run 40969223321, success): `out/exynos/xnu25/kernel.development.sweet` sha256 7fc5a73b... **Boot image `out/exynos/boot-ios29.img`** (sha256 a054c140...) = boot-ios28 cmdline (sweetaudio=4, Calliope blobs) with the 0128 kernel. Not flashed. Fallback: boot-ios25 / boot-ios15.

- **boot-ios29 flashed** to BOOT (sda10), readback sha256 a054c140... matches. Rebooted. Fallback boot-ios25 / boot-ios15.

- **boot-ios29 RESULT (log s9/logs/boot-ios29.txt, observed): booted to iOS; Calliope RUNS.** After ENABLE_CPU: cpu_status 0x1 at once; ABOX_CPU_PC 0x2a80 at t+0, then **0x2768** from t+100 ms to 10 s; SYSPOWER_STATUS 1; SRAM +0 went 0xe59ff018 -> 0x84a4 (firmware rewrote it); SRAM IPC area readable (the boot-ios28 bus error did not recur); SysMMU INT_STATUS 0, AR fault addr 0 (AW reg 0x80 refuses the read). **The firmware sent IPC_SYSTEM msg 0xFA = ABOX_REPORT_FAULT, param1 1 = data abort, param3 0x1cd9f0** (abox_ipc.h / abox.c), not ABOX_BOOT_DONE, then parked at 0x2768.
- **Patch 0129 (written, not built):** on 0xFA, abox_fault_dump logs param1-3, the bundle words, the CA7 R0-R14+PC dump at bundle.param_s32[0] (SRAM 0x30318; address < 0x80000000 = SRAM, 0x80000000.. = DRAM firmware window) and the live SFR registers ABOX_CPU_R 0x2C00-0x2C3C and L2C_STATUS 0x2C40.
- **0129 compiled** (run 40970813626, success): `out/exynos/xnu26/kernel.development.sweet` sha256 f123302a... **Boot image `out/exynos/boot-ios30.img`** (sha256 5bc33fa3...) = boot-ios29 cmdline (sweetaudio=4, Calliope blobs) with the 0129 kernel. Not flashed. Fallback: boot-ios29 (boots iOS) / boot-ios25 / boot-ios15.

- **boot-ios30 flashed** to BOOT (sda10), readback sha256 5bc33fa3... matches. Rebooted. Fallback boot-ios29 / boot-ios25.

- **boot-ios30 RESULT (log s9/logs/boot-ios30.txt, observed): booted to iOS.** Calliope PC 0x2aac -> 0x2768 again; ABOX_REPORT_FAULT: **param1 1 (data abort), param2 0x5 (ARMv7 DFSR: translation fault, level 1), param3 0xa200067** (boot-ios29: 0x1cd9f0, so the faulting address changes per boot = a garbage pointer); dump pointer 0x2d000 (SRAM) holds zeros; live CA7 regs: R1 0x17cf1f00 (its GICD_SGIR), R4 5, R5 0x0a200067, R6 0x0a200023, R13 0x1e000, R14 0x5503, PC 0x2768; L2C_STATUS 0.
- **Cause (likely):** the DT ext_bin files abox.c loads into the DRAM firmware area were missing. Vendor DTB (dtb5) ext_bin@0-6, area 1, offsets: dsm.bin 0x502000, AP_AUDIO_SLSI.bin 0x7e0000, APBargeIn_AUDIO_SLSI.bin 0x7dc000, SoundBoosterParam.bin 0x4fc000, APDV_AUDIO_SLSI.bin 0x4d0000, APBiBF_AUDIO_SLSI.bin 0x7df000, dsm_tune.bin 0x601000 (@7 dummy.bin disabled). All seven are in VENDOR (sda19) /firmware; the five new ones copied to s9/out/firmware (gitignored).
- **Patch 0130 (written, not built):** after the DRAM/IVA copy, each ext file is written at its offset if present as a --blob of the same name; abox_rm_write maps page-aligned windows (offsets need not be page aligned).
- **0130 compiled** (run 40972250889, success): `out/exynos/xnu27/kernel.development.sweet` sha256 93e0a5ed... **Boot image `out/exynos/boot-ios31.img`** (sha256 3258a62f...) = sweetaudio=4 + Calliope-SRAM/-DRAM/-IVA + the seven ext_bin blobs (--blob dsm.bin=out\firmware\dsm.bin etc.). Not flashed. Fallback: boot-ios30/29 (boot iOS) / boot-ios25.

- **boot-ios31 flashed** to BOOT (sda10), readback sha256 3258a62f... matches. Rebooted. Fallback boot-ios30 / boot-ios25.

- **boot-ios31 RESULT (log s9/logs/boot-ios31.txt, observed): booted to iOS.** All seven ext_bin blobs found and written. Calliope still faults ~100 ms after start: ABOX_REPORT_FAULT data abort, **param2 0x805** (DFSR: WnR write + FS 0x5 level-1 translation fault in the CA7's own MMU), **param3 0x0a200053** (ios30 0x0a200067, R6 0x0a200023 both boots) -> a write walking through 0x0A2000xx, not a random pointer. SysMMU INT_STATUS 0 (so not an IOVA fault). Same live regs otherwise (R1 GICD_SGIR, SP 0x1e000, LR 0x5503, PC 0x2768).
- Firmware strings (calliope_sram.bin): "VSS task was inited", "[MAIN] WARNING VSS init timed out", "CP Magic Code is ready, VHM_initialize done MC=0x%X", "ABox system was inited: %s", "Main init completed! Go processing!": boot waits for the modem's VSS shared memory (IOVA 0xA0000000, zeros here; on Android CP shm 0xFEE00000). No static L1 page table in the images (built at run time).
- **Patch 0131 (written, not built):** after the fault, abox_postmortem scans SRAM (past the image, below 0x30000) and the 18 MB DRAM area for 16 KB-aligned ARMv7 L1 tables (prints the first 48 non-zero entries = the CA7 VA map) and for printable text the firmware wrote outside its images (its log), up to 60 lines; safe reads throughout.
- **0131 compiled** (run 40973865259, success): `out/exynos/xnu28/kernel.development.sweet` sha256 1203a22f... **Boot image `out/exynos/boot-ios32.img`** (sha256 ff2b58c8...) = boot-ios31 blobs/cmdline with the 0131 kernel. Not flashed. Fallback: boot-ios31/30 (boot iOS) / boot-ios25.

- **boot-ios32 flashed** to BOOT (sda10), readback sha256 ff2b58c8... matches. Rebooted. Fallback boot-ios31 / boot-ios25.

- **boot-ios32 RESULT (log s9/logs/boot-ios32.txt, observed): booted to iOS.** Same fault (data abort, DFSR 0x805, DFAR 0x0a200053). **Calliope's runtime L1 table is in SRAM at +0x24000** (84 entries): VA 0 -> coarse table SRAM 0x28000 (0x00028001); identity sections 0x10000000, 0x10c00000, 0x17c00000, 0x17d00000 (attr 0xc12 device), 0x80000000-0x811xxxxx (0x5c16 normal, 0x80800000/0x80b00000/0x81100000 0xc12), 0x90000000 -> coarse 0x28400, 0x91000000-0x927xxxxx 0x4c12 ... (48 printed). **Nothing maps 0x0A2xxxxx.** No printable text found outside the images. DFAR bytes `xx 00 20 0a` look like text (char, NUL, ' ', '\n').
- **Disassembly (capstone, calliope_sram.bin, ARM):** vectors reset 0x2960, undef 0x2af4, svc 0x73c, pabort 0x2b68, **dabort 0x2bfc**, irq 0x2c94. dabort: lr -= 8; r0-r14 stored into the block at *(VA 0x955c) (+0x38 = faulting PC); then blx 0x2708 (Thumb) with r0 DFSR, r1 DFAR, r2 PC; 0x2708 stores APSR, PC at VA 0x954c/0x9550 (strd to 0x9544+8), sends the fault IPC (bl 0x3c1a), cpsid i; wfi (0x2762) = where PC parks.
- Android also maps IOVA_DUMP_BUFFER 0xD0000000 -> abox_rmem (abox_dbg.c) - the region we use for the DRAM firmware; to sort out once Calliope boots.
- **Patch 0132 (written, not built):** fault dump adds the coarse-table entries for VA 0-0x34000, the faulting PC + CPSR (VA 0x9550/0x954c) and the saved r0-r14 context (block at *(VA 0x955c)), read through the coarse table (abox_va_sram); L1 print limit 96.
- **0132 compiled** (run 40978964403, success): `out/exynos/xnu29/kernel.development.sweet` sha256 814ff057... **Boot image `out/exynos/boot-ios33.img`** (sha256 ca91a1a4...) = boot-ios32 blobs/cmdline with the 0132 kernel. Not flashed. Fallback: boot-ios32/31 (boot iOS) / boot-ios25.

- **boot-ios33 flashed** to BOOT (sda10), readback sha256 ca91a1a4... matches. Rebooted. Fallback boot-ios32 / boot-ios25.

- **boot-ios33 RESULT (log s9/logs/boot-ios33.txt, observed): booted to iOS.** Coarse table (SRAM 0x28000): VA 0x0-0x8fff -> PA 0x0.. attr 0x3ba (RO code), 0x9000-0x1ffff 0x177 (RW), 0x20000-0x23fff 0x133, 0x24000-0x28fff 0x037 (page tables), 0x29000-0x30fff 0x133 (0x2d000 0x177), identity throughout. **FAULT PC 0x8004fcbc**, CPSR (abort mode) 0x800001d7. The context block pointer at VA 0x955c is 0 -> the block is at VA 0 (why SRAM+0 reads 0x84a4 = saved R0): R0 0x84a4, R1 0x9000, R2 0x93c, R3 0x8004fcbd, R4 0x800e6cd8, R5 0x800e6cf8, R6 0x0a200023, R12 0xc0000000, SP_abt 0x1e6fc, LR 0x8004fcbc.
- **Disassembly:** 0x8004fcbc (DRAM file +0x4fcbc, Thumb) = `push {r4,r5,r6,lr}`, entry of an LZ-style decompressor (ldrb [r0]! / strb [r1]!). DRAM file +0xe6cd8 is the C-runtime scatter-load table: {0x84a4 -> 0x9000, 0x93c, routine 0x8004fcbc} (SRAM .data), {0x8021b87c -> 0x80b00000, 0xffc0c, routine 0x5ae4} (DRAM .bss zero-init). So Calliope dies at the very first scatter-load call, and the push faulting at 0x0a2000xx means the current mode's SP was ~0x0a200063. Reset (0x2960): bl 0x2ecc compares SRAM 0x30ffc with 0x504e4943 ("PNIC") -> resume/panic path at 0x26f1 if equal (we zero SRAM, so not taken); then SCTLR/ACTLR setup, VBAR = 0, stacks UND 0x1e900, ABT 0x1e700, FIQ 0x1e500, IRQ 0x1e500, SVC 0x1e200, SYS 0x1e200; blx 0x17a8 (Thumb: builds the MMU tables via 0x1594), then caches.
- **Patch 0133 (written, not built):** the fault dump adds the abort frame from srsdb (LR_abt / SPSR_abt at 0x1e6f8 / 0x1e6fc = faulting mode), the top 16 words of each mode stack (0x1e200/0x1e500/0x1e700/0x1e900) and SRAM 0x9000 vs 0x84a4 (did the decompress run).
- **Reset path, continued (capstone):** after the cache loop, SCTLR |= M (MMU on, A cleared), then blx 0x2689 -> 0x2688 b.w 0x104c (Thumb: bl 0xe94 / b 0xfc2 = Calliope's own GIC distributor/CPU-interface init at 0x17cf1000/0x17cf2000, not __main), then blx 0x3c (ARM: sp = [0x60] = 0x1e000; blx 0x54f0, 0x680c, 0x6b74, 0x6858 = runtime + main). Where the bogus SP comes from is still open; boot-ios34's abort frame/stack dump should say.

- **0133 compiled** (run 40980307678, success): `out/exynos/xnu30/kernel.development.sweet` sha256 dc872caa... **Boot image `out/exynos/boot-ios34.img`** (sha256 59f79fda...) = boot-ios33 blobs/cmdline with the 0133 kernel. Not flashed. Fallback: boot-ios33 (boots iOS) / boot-ios25.

