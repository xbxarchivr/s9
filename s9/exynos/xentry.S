/*
 * Exynos secondary-core entry. PSCI 0.1 CPU_ON (what the G960F firmware
 * speaks: PSCI_VERSION is NOT_SUPPORTED) takes no context id, so x0 is
 * whatever the firmware left. Derive the stack slot from MPIDR instead:
 * Exynos 9810 cores are 0x000-0x003 (A55) and 0x100-0x103 (M3), so
 * slot = Aff1 * 4 + Aff0, kept within the 8 stacks shim.ld reserves.
 */
	.text
	.globl x_secondary_entry
x_secondary_entry:
	mrs	x0, mpidr_el1
	ubfx	x1, x0, #8, #4		/* Aff1 */
	and	x0, x0, #0xf		/* Aff0 */
	add	x0, x0, x1, lsl #2
	and	x0, x0, #7
	b	secondary_entry
