/*
 * Exynos secondary-core entry. PSCI 0.1 CPU_ON (what the G960F firmware
 * speaks: PSCI_VERSION is NOT_SUPPORTED) takes no context id, so x0 is
 * whatever the firmware left. MPIDR is no help either: on the G960F the
 * cores' real MPIDR does not match the /cpus reg values (X1.3: two cores
 * landed in slot 0, CPU0's own). CPU0 starts one core at a time and stores
 * its slot (1..7) in x_next_slot just before CPU_ON; the core reads it here.
 * With the MMU off on both sides that word is read straight from memory.
 */
	.text
	.globl x_secondary_entry
x_secondary_entry:
	adrp	x0, x_next_slot
	ldr	w0, [x0, :lo12:x_next_slot]
	and	x0, x0, #7
	b	secondary_entry
