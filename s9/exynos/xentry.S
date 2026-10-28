/*
 * Exynos secondary-core entry. PSCI 0.1 CPU_ON (what the G960F firmware
 * speaks: PSCI_VERSION is NOT_SUPPORTED) takes no context id, so x0 is
 * whatever the firmware left. MPIDR is no help either: on the G960F the
 * cores' real MPIDR does not match the /cpus reg values (X1.3: two cores
 * landed in slot 0, CPU0's own). CPU0 starts one core at a time and stores
 * its slot (1..7) in x_next_slot just before CPU_ON; the core reads it here.
 * With the MMU off on both sides that word is read straight from memory.
 *
 * X1.4: the A55s came up, the Mongoose M3s never did, although Linux starts
 * them with the same CPU_ON. Linux's entry first puts the core into a known
 * state (drop from EL2, fixed SCTLR_EL1, VBAR, SP); a core fresh out of reset
 * may not inherit what the A55s did. Do the same, then paint a blue bar into
 * the core's square before any C runs: blue alone = entered but died early.
 */
#define SCTLR_EL1_MMU_OFF	0x30d00800	/* RES1 bits, LE, MMU/caches off */
#define FB_BASE			0xcc000000
#define PANEL_W			1440
#define SQ			40		/* x1.c square() */

	.text
	.globl x_secondary_entry
x_secondary_entry:
	msr	daifset, #0xf
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	b.ne	1f
	/* EL2 (no hypervisor in the way): run EL1 in AArch64, timers open */
	mov	x1, #(1 << 31)			/* HCR_EL2.RW */
	msr	hcr_el2, x1
	mrs	x1, cnthctl_el2
	orr	x1, x1, #3			/* EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mov	x1, #0x3c5			/* EL1h, DAIF masked */
	msr	spsr_el2, x1
	adr	x1, 1f
	msr	elr_el2, x1
	eret
1:	movz	x1, #(SCTLR_EL1_MMU_OFF & 0xffff)
	movk	x1, #(SCTLR_EL1_MMU_OFF >> 16), lsl #16
	msr	sctlr_el1, x1
	isb
	adr	x1, vectors
	msr	vbar_el1, x1
	isb

	adrp	x0, x_next_slot
	ldr	w0, [x0, :lo12:x_next_slot]
	and	x0, x0, #7

	/* blue bar: top 10 rows of the square, a8r8g8b8 0xff0000ff */
	mov	x1, #(PANEL_W - 8 * (SQ + 16))
	mov	x2, #(SQ + 16)
	madd	x1, x0, x2, x1			/* x of the square */
	movz	x2, #(FB_BASE >> 16), lsl #16
	movz	x3, #(40 * PANEL_W * 4 & 0xffff)
	movk	x3, #(40 * PANEL_W * 4 >> 16), lsl #16
	add	x2, x2, x3
	add	x2, x2, x1, lsl #2		/* first pixel */
	movz	w3, #0x00ff
	movk	w3, #0xff00, lsl #16
	mov	x4, #10				/* rows */
	mov	x7, #(PANEL_W * 4)		/* stride */
2:	mov	x5, #SQ
	mov	x6, x2
3:	str	w3, [x6], #4
	subs	x5, x5, #1
	b.ne	3b
	add	x2, x2, x7
	subs	x4, x4, #1
	b.ne	2b
	dsb	sy

	b	secondary_entry
