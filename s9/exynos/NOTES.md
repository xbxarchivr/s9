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
- **Decision pending (user):** powering touch needs PMIC writes (LDO35/LDO43 VSEL + enable). Plan: X6.5 read-only over SPEEDY via the batcher semaphore (PMIC ID reg 0x00 on page 0x01 as a protocol check, L35CTRL/L43CTRL), then, with the user's OK, write exact VSEL values (from s2mps18.c regulator descs) + enable, then re-run the touch probe.