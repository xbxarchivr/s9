# Powering the Mongoose M3 cluster (CPUCL1) before PSCI CPU_ON

Research note, 2026-10-28. No phone was used. Sources were read, nothing was run.
Answers the question from X1.4/X1.5: the four M3 cores (`/cpus` reg 0x100-0x103)
accept CPU_ON (it returns 0) but never run a single instruction of `xentry.S`.

**Short answer:** this is a known failure mode with a known fix. A mainline port
for the S9+ ran into exactly the same symptom ("CPU4: failed to come online",
CPU_ON returns 0). Its fix is **six read-modify-write stores into the PMU at
0x14060000, made before the first CPU_ON of an M3 core.** These are the CPUCL1
entries of Samsung's `pmucal_lpm_init`, which Samsung's own kernel runs from
its clock driver in `time_init`, long before SMP starts. Plain mainline does not
make these writes, and on its own it does **not** bring the M3s up.

Labels used below: **[C]** = confirmed in the cited source text. **[R]** = reported
by another project from its own device runs (we cannot check it, but it is
specific). **[G]** = our guess.

## 1. The fix: Samsung's CPUCL1 `pmucal_lpm_init` entries

Each write is `reg = (reg & ~mask) | value` [C] (`pmucal_rae_write`,
`drivers/soc/samsung/cal-if/pmucal_rae.c:116`).

| PA | name (Samsung) | mask | value | cold-boot value left by S-Boot [R] |
|---|---|---|---|---|
| 0x14061088 | CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG | 0xFF000000 | 0x8D000000 | 0xC7150000 (becomes 0x8D150000) |
| 0x14062114 | MEMORY_CLUSTER1_CPU0_OPTION | 0x3 | 0x2 | 1 |
| 0x14062180 | MEMORY_CLUSTER1_CPU1_OPTION | 0x3 | 0x2 | 1 |
| 0x140621EC | MEMORY_CLUSTER1_CPU2_OPTION | 0x3 | 0x2 | 1 |
| 0x14062258 | MEMORY_CLUSTER1_CPU3_OPTION | 0x3 | 0x2 | 1 |
| 0x14062384 | MEMORY_CLUSTER1_NONCPU_OPTION | 0x3 | 0x2 | 1 |

- [C] The offsets, masks and values are in Samsung's
  `drivers/soc/samsung/cal-if/exynos9810/flexpmu_cal_system_exynos9810.h`,
  array `pmucal_lpm_init[]` (lines 4 and 29-33 in LineageOS
  [android_kernel_samsung_universal9810](https://github.com/LineageOS/android_kernel_samsung_universal9810/blob/lineage-17.1/drivers/soc/samsung/cal-if/exynos9810/flexpmu_cal_system_exynos9810.h),
  branch lineage-17.1).
- [C] When Samsung runs them: `CLK_OF_DECLARE(exynos9810_clk, ...)` →
  `exynos9810_clk_init` → `cal_if_init()` → `pmucal_system_init()` →
  `pmucal_rae_handle_seq(pmucal_lpm_init, ...)`
  (`drivers/clk/samsung/clk-exynos9810.c:502,561`;
  `drivers/soc/samsung/cal-if/cal-if.c:389-408`;
  `drivers/soc/samsung/cal-if/pmucal_system.c:157-216`). `CLK_OF_DECLARE`
  drivers run from `of_clk_init()` in `time_init()`, before `smp_init()`. So
  the TWRP/Android kernel has made these writes before its first CPU_ON.
- [R] SheroAbi's [galaxy-s9plus-mainline-linux](https://github.com/SheroAbi/galaxy-s9plus-mainline-linux)
  (mainline 7.1 on the SM-G965F; ztOS uses it as a git submodule) carries them as a
  port patch in `scripts/build/patch_tree.py` (section "M3 cluster: vendor PMU
  defaults before PSCI CPU_ON", around line 664). Its comment says:
  without these six writes, CPU4-7 take CPU_ON (it returns 0) but never reach
  `secondary_start_kernel`, and they log "CPU4: failed to come online" and
  "failed in unknown state : 0x0". The writes are made in
  `cpu_psci_cpu_boot()` right before `psci_ops.cpu_on()`, only for
  `cpu_logical_map(cpu) & ~3 == 0x100`, and each one is read back and checked.
  The patch is enabled by the cmdline flag `s9p_vendor_m3_defaults`, together
  with `maxcpus=8`. `scripts/build/build71.sh:314` says "Verified star2lte
  eight-core startup: initialize the M3 PMU defaults before CPU_ON".
  `docs/06-known-issues.md:24` says "Initial SMP bring-up works with the six
  vendor M3 PMU defaults and `maxcpus=8`".
- [R] The same comment says the S-Boot values (0xC7150000 and 1) are also there
  after a warm reboot. So the writes are needed on every boot through S-Boot.
- [C] The rest of `pmucal_lpm_init` (other MEMORY_*_OPTION registers,
  PS_HOLD_CONTROL, TCXO/regulator durations, RESET_LPI_TIMEOUT) is not about
  CPUCL1. The S9+ port does not write it.
- [G] What the writes mean: the MEMORY_*_OPTION field (bits 1:0) appears to
  select how the PMU sequences the core's or cluster's SRAM power (1 vs 2,
  for example retention vs. full power gating). CPU_QCH_* is the Q-channel
  setup for the CPUCL1 clock handshake. With S-Boot's values, the FlexPMU
  power-up sequence for the big cluster probably stalls on the memory or
  Q-channel handshake, so the core never comes out of reset. EL3 has already
  returned 0 by then, because it only posts the request.

## 2. Does mainline do it? No (mainline 6.14+ = plain PSCI, nothing else)

- [C] Mainline `arch/arm64/boot/dts/exynos/exynos9810.dtsi` (Markuss Broks and
  Maksym Holovach, 2024) has `/cpus` reg 0x0-0x3 and 0x100-0x103, each with
  `enable-method = "psci"`. The PSCI 0.1 node has `method = "smc"`,
  `cpu_on = 0xc4000003`, `cpu_off = 0x84000002` and `cpu_suspend = 0xc4000001`.
  There are no clocks, CMU or power domains in it. The PMU appears only as a
  syscon at 0x14060000 (`samsung,exynos9810-pmu`, used for syscon-restart). The
  starlte DTS adds only simple-framebuffer, memory, gpio-keys and oscclk.
- [C] The upstream series (v2-v4 plus the Dec-2024 resend on LKML,
  [v2 cover](https://lkml.rescloud.iu.edu/2410.3/02429.html),
  [v4 09/10](https://lkml.rescloud.iu.edu/2411.0/01849.html)) only says
  "PSCI for bringing up secondary cores". Its firmware notes cover CNTFRQ_EL0
  (not set by S-Boot) and the DECON trigger (uniLoader). It says nothing about
  the M3 coming up.
- [C] Upstream uniLoader `board/samsung/board-starlte.c` (ivoszbg) writes only
  `DECON_F 0x16030000 + 0x70 = 0x1281`. It does nothing for secondaries. The
  S9+ port's copy of uniLoader is the same (its `soc/exynos/exynos9810.c` also
  only does the DECON write).
- [R] Taken together with the S9+ patch comment, plain mainline gets only the
  four A55s on this phone. The "8 cores" claims of the S9+ port and ztOS depend
  on the six PMU writes above.

## 3. What Samsung's kernel does around a CPUCL1 CPU_ON

The vendor kernel uses `CONFIG_FLEXPMU=y`, `CONFIG_PMUCAL=y` and
`CONFIG_EXYNOS9810_ACPM=y` [C] (`arch/arm64/configs/exynos9810-starlte_defconfig`).
Core and cluster power are switched by the APM firmware ("flexible PMU"). The
kernel only sets hints and arms wake-up lines.

**Boot, before SMP** [C]: `pmucal_lpm_init` (section 1).

**Hot-plug in, run by the incoming core *after* it is already running**
[C] (`exynos-powermode.c:693-704`, registered as `CPUHP_AP_EXYNOS_CPU_UP_POWER_CONTROL`,
which is an AP state):
`cluster_enable()` if it is the first core of its cluster, then `cpu_enable()`.
So these are **not** preconditions for CPU_ON. They are listed here because
they are the documented PMU interface:

- Core index = Samsung `linear_phycpu(mpidr)`: (cluster << 2) | core, using the MT
  layout for the A55s. A55 = 0-3, M3 = 4-7 [C] (`exynos-powermode.c:69-74`).
- Kernel-side `coreNN_on` for M3 core *n* (n = 0..3) [C]
  (`flexpmu_cal_cpu_exynos9810.h`, non-`ACPM_FRAMEWORK` half):
  - `0x14070200` GRP2_INTR_BID_ENABLE: clear bit *n*
  - `0x1407020C` GRP2_INTR_BID_CLEAR: write bit *n*, only if `0x14070208` (pending) has bit *n* set
  - `0x14067F08` PMU_EVENT_INTERRUPT_ENABLE_GRP2: clear bit *n*

  (The A55 cores use bits 4-7 of the same registers.) `coreNN_off` sets these
  bits again and clears GRP1 pending (`0x14070108` / `0x1407010C`).
- `cluster1_on` (kernel side): `0x14100010` MAILBOX_APM2AP bit 24 = 0.
  `cluster1_off` sets bit 24 = 1 (a cluster power-down request to the APM) [C].
- CPU_INFORM hints: `0x14060860 + 4*core` (core 4-7 → 0x870-0x87C) [C]
  (`flexpmu_cal_powermode_exynos9810.h`). ztOS writes 1 (C2) for a dying core
  and 2 for a cluster power-down (CPD) [R].
- Status registers (read-only, good for diagnosis) [C]: CLUSTER1_CPU*n*_STATUS =
  `0x14062004 + 8*n` (0x2004/0x200C/0x2014/0x201C), CLUSTER1_NONCPU_STATUS
  `0x14062044`, CLUSTER0_CPU*n*_STATUS `0x14062024 + 8*n`, CLUSTER0_NONCPU_STATUS
  `0x1406204C`. [R] ztOS measured CLUSTER1_CPU3_STATUS = `0x00880000` for a
  core that is off and `0x00100F0F` for one that is on; low nibble 0xF = powered.

**What the APM firmware does** (the `#else /* ACPM_FRAMEWORK */` half of the same
header; this is the firmware's own sequence, shown for reference only) [C]:

- `core10_on` (M3 core 0): CLUSTER1_CPU0_CONFIGURATION `0x14062000` bits 3:0 = 0xF
  (then wait), BISR_RETENTION_CLUSTER1_CPU0 `0x14061014` bit 0 = 0,
  OTP_CLUSTER1_CPU0 `0x1406100C` bit 0 = 1, GRP1_INTR_BID_ENABLE `0x14070100`
  bit 0 = 0. For core *n* the CONFIGURATION register is `0x14062000 + 8*n`.
- `cluster1_on`: CPUCL1_HCHGEN_CLKMUX `0x1D100850` bits 6:0 = 0,
  CLUSTER1_NONCPU_CONFIGURATION `0x14062040` = 0xF (then wait),
  BISR_RETENTION_CLUSTER1_NONCPU `0x140610B4` bit 0 = 0,
  OTP_CLUSTER1_NONCPU `0x14061098` bit 0 = 1, then the CLUSTER1_CPU*n*_ETC_RESET and
  NONCPU_ETC_RESET bits (`0x14063C0C/1C/2C/3C` bits 13:12 and 9:8,
  `0x14063C8C` bits 31, 25, 18). The reset writes are skipped when
  RESET_SEQUENCER_CONFIGURATION `0x14060500` bit 0 = 0.

[G] We should not write the firmware-side sequence from the AP. It belongs to
the APM. Doing it behind the firmware's back can desynchronise its state
machine. Use it only as a last-resort experiment (section 6, step D).

**`big_reset_control()`** [C] (`exynos-reboot.c:181`) changes CPU reset-disable
bits only when DFD dump is enabled (RESET_SEQUENCER_CONFIGURATION has
DFD_EDPCSR_DUMP_EN). It is not a power-up precondition.

## 4. Related facts that matter for the shim

- [R] Real MPIDR_EL1 values (ztOS `power/FIRMWARE-AUDIT.md`, live kernel):
  A55 = 0x81000000/0x81000100/0x81000200/0x81000300 (MT=1, core in Aff1; this
  matches our X1.4 result). **M3 = 0x80000100..0x80000103** (no MT bit, cluster
  in Aff1, core in Aff0). The PSCI target IDs stay `/cpus` reg 0x100-0x103.
  X1.5's MPIDR-free slot handoff is therefore still right. An M3 that arrives
  should report 0x800001nn.
- [R] Hot-plug trap, for later stages: under mainline a core that takes CPU_OFF
  without the CPU_INFORM hint and an armed wake line "stays powered, parked in
  the firmware", and a later CPU_ON returns 0 and does nothing (ztOS
  `power/README.md`, `power/hotplug/zrn_hotplug_main.c`). Before a core goes
  down: CPU_INFORM[core] = 1, set GRP2 enable bit, clear GRP1 pending, set
  PMU_EVENT_INTERRUPT_ENABLE_GRP2 bit. After it comes back: undo all of these.
  The writes must be made on the dying core with interrupts off, right before
  CPU_OFF. ztOS also never powers off the last core of a cluster.
  **Consequence for X1.x:** the X1/X1.1/X1.2 runs did CPU_OFF A55 cores
  without the hint. That is harmless within one boot, but it is the reason
  "park in WFE", not CPU_OFF, is right until XNU has a proper hot-plug path.
- [R] The S9+ port reports that the M3 lacks FEAT_LVA and FEAT_LPA2, so it uses
  48-bit VA/PA. This is irrelevant for XNU's 16K/39-bit layout, but it confirms
  the M3 is ARMv8.0-class.

## 5. M3 16K granule (ID_AA64MMFR0_EL1.TGran16)

- Nothing public found. There is no published MMFR0 value for MIDR 0x531F0020.
  It is not in the S9+ port, the ztOS docs, the upstream series, the vendor
  kernel, or a web search for "SANITY CHECK: Unexpected variation in
  SYS_ID_AA64MMFR0_EL1" together with Exynos 9810. (GitHub code search hit the
  API rate limit partway through the ztOS repository, so that check is not
  complete.)
- [C] Our A55 value is MMFR0 = 0x00101122: TGran16 (bits 23:20) = 1, so 16K
  is supported.
- [G] An indirect public check: mainline 4K kernels on this phone (the S9+ port,
  ztOS) run without Samsung's cpufeature silencing. Mainline treats TGRAN16
  as FTR_STRICT, so if the M3 differed from the A55 in MMFR0, their dmesg
  would print `SANITY CHECK: Unexpected variation in SYS_ID_AA64MMFR0_EL1 ...
  CPU4: 0x...` with the raw M3 value. Anyone holding such a dmesg has the
  answer. We could not find one posted.
- The direct way: once section 6 brings an M3 up, X1.5's per-core report
  already prints its MMFR0.

## 6. Proposed shim experiment (X1.6)

Goal: make one M3 run `xentry.S`, then report MIDR/MPIDR/**MMFR0**. This changes
only the order of events in `x1.c`, around the existing CPU_ON loop
(`x1.c:531`). Nothing persistent is written: every register is in the PMU,
which is reset by the next reboot.

**A. Read first, show it on screen (no writes)** before any CPU_ON:
`0x14061088`, `0x14062114`, `0x14062180`, `0x140621EC`, `0x14062258`,
`0x14062384`, plus status `0x14062004/0C/14/1C`, `0x14062044`, `0x1406204C`,
`0x14060500` and `0x14100010`. Expected [R]: 0xC71500xx (top byte 0xC7) and four
or five values with bits 1:0 = 1. If the values differ, photograph them anyway.

**B. The six writes**, after the A55 CPU_ONs and before the first M3 CPU_ON
(same as the S9+ patch), each read back and shown as `before -> after`:

```c
static const struct { uint32_t off, mask, val; } m3_lpm[] = {
	{ 0x1088, 0xFF000000u, 0x8D000000u },	/* CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG */
	{ 0x2114, 0x3, 0x2 },			/* MEMORY_CLUSTER1_CPU0_OPTION */
	{ 0x2180, 0x3, 0x2 },			/* MEMORY_CLUSTER1_CPU1_OPTION */
	{ 0x21EC, 0x3, 0x2 },			/* MEMORY_CLUSTER1_CPU2_OPTION */
	{ 0x2258, 0x3, 0x2 },			/* MEMORY_CLUSTER1_CPU3_OPTION */
	{ 0x2384, 0x3, 0x2 },			/* MEMORY_CLUSTER1_NONCPU_OPTION */
};
/* PMU = 0x14060000; v = rd32(PMU+off); wr32(PMU+off, (v & ~mask) | val); dsb sy */
```

Then CPU_ON 0x100..0x103 exactly as X1.5 does (5 s each, park in WFE, no
CPU_OFF), and re-read the CLUSTER1 status registers after each one. If the
writes are what is missing, expect the CLUSTER1_CPUn_STATUS low byte to go to
0x0F, a blue bar plus a green square in slots 4-7, MIDR 0x531F0020, MPIDR
0x800001nn, and the MMFR0 answer.

**C. If B is not enough** (CPU_ON still returns 0, status unchanged), also do
the kernel-side "core on" interrupt hygiene for M3 core *n* before its CPU_ON.
This is harmless: it is exactly what Samsung's kernel does for a core coming
up. Write CPU_INFORM `0x14060870 + 4*n` = 0, clear bit *n* of `0x14070200`,
write bit *n* to `0x1407020C` if `0x14070208` has it, clear bit *n* of
`0x14067F08`. Also clear bit 24 of `0x14100010` (MAILBOX_APM2AP, "cluster on").
Show all of them before and after.

**D. Last resort only (guess, not from any working port):** if STATUS shows
CLUSTER1_NONCPU still off (`0x14062044` low nibble != 0xF), try the
firmware-side `cluster1_on` head: `0x1D100850` bits 6:0 = 0, then
`0x14062040` bits 3:0 = 0xF, and poll `0x14062044` for up to 10 ms. Do not do
this before A-C have been photographed, and confirm the step with the user
first: it touches the APM firmware's own state.

Order of value: A+B is one boot and is the likely fix ([R], with the same
symptom on the same SoC). C and D only come in if B fails.
