/*
 * Galaxy S9 Exynos milestone X2: interrupts on the A55 cluster.
 *
 * X1 showed the GIC is a GICv2 (DT arm,cortex-a15-gic at 0x10100000, no
 * GICv3 system registers), so XNU's GICv3 path from the SDM845 port cannot
 * be reused. This stage proves the pieces a GICv2 backend needs, on every
 * A55 (the M3s do not start yet, see NOTES.md):
 *   - distributor and CPU interface reachable from non-secure EL1;
 *   - the virtual timer PPI (from the DT /timer node) fires every 10 ms;
 *   - a core can send itself an SGI (GICD_SGIR, filter "self");
 *   - CPU0 can send an SGI to each other core (target = that core's CPU
 *     interface bit, read from its banked GICD_ITARGETSR0).
 * Only banked SGI/PPI state is touched; no SPI is enabled. GICv2 layout:
 * ARM IHI 0048B.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "xplat.h"

#define GICD_CTLR	0x000
#define GICD_TYPER	0x004
#define GICD_IIDR	0x008
#define GICD_IGROUPR0	0x080
#define GICD_ISENABLER0	0x100
#define GICD_IPRIORITYR	0x400
#define GICD_ITARGETSR	0x800
#define GICD_SGIR	0xf00
#define GICC_CTLR	0x00
#define GICC_PMR	0x04
#define GICC_BPR	0x08
#define GICC_IAR	0x0c
#define GICC_EOIR	0x10
#define GICC_IIDR	0xfc

#define SGI_SELF	1
#define SGI_CROSS	2
#define SGI_WAKE	3	/* uncounted: ends a waiting core's WFI */
#define SGIR_SELF	(2u << 24)	/* TargetListFilter: only the sender */
#define SELF_SGIS	10
#define CROSS_SGIS	5
#define WAIT_MS		5000
#define RUN_SECONDS	60

#define REP_READY	0x58325244U	/* "X2RD": own tests done, waiting for SGIs */
#define REP_FAULT	0x58324641U

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

struct report {
	uint64_t magic;
	uint32_t ifc, igroup0, isen_before, ctlr_before, ctlr_after;
	uint32_t timer, sgi, cross, other, last;
	uint64_t esr, elr;
} __attribute__((aligned(64)));

static volatile struct report rep[XP_NR_CPUS];
static volatile uint32_t sgi_done;	/* CPU0 -> secondaries: stop waiting */
static uint32_t vtimer;			/* INTID */
static volatile int in_exc;

/* ---- interrupts ---- */

static void irq(void)
{
	volatile struct report *r = &rep[rsr("tpidr_el1") & 7];
	uint32_t iar = rd32(xp_gicc() + GICC_IAR), id = iar & 0x3ff;

	if (id >= 1020)
		return;	/* spurious */
	if (id == vtimer) {
		r->timer++;
		wsr("cntv_tval_el0", xp.cntfrq / 100);
	} else if (id == SGI_SELF) {
		r->sgi++;
	} else if (id == SGI_CROSS) {
		r->cross++;
	} else if (id != SGI_WAKE) {
		r->other++;
		r->last = id;
	}
	wr32(xp_gicc() + GICC_EOIR, iar);
}

static void prio(uintptr_t d, uint32_t id)
{
	uintptr_t a = d + GICD_IPRIORITYR + (id & ~3u);
	uint32_t sh = 8 * (id & 3);

	wr32(a, (rd32(a) & ~(0xffu << sh)) | (0xa0u << sh));
}

/* Everything one core can test by itself; results in its report slot. */
static void gic_cpu(int s)
{
	volatile struct report *r = &rep[s];
	uintptr_t d = xp_gicd(), c = xp_gicc();
	uint64_t end;

	r->ifc = rd32(d + GICD_ITARGETSR) & 0xff;	/* banked: own interface bit */
	r->igroup0 = rd32(d + GICD_IGROUPR0);
	r->isen_before = rd32(d + GICD_ISENABLER0);
	prio(d, SGI_SELF);
	prio(d, SGI_CROSS);
	prio(d, SGI_WAKE);
	prio(d, vtimer);
	wr32(d + GICD_ISENABLER0, 1u << SGI_SELF | 1u << SGI_CROSS | 1u << SGI_WAKE | 1u << vtimer);
	r->ctlr_before = rd32(c + GICC_CTLR);
	wr32(c + GICC_PMR, 0xf0);
	wr32(c + GICC_BPR, 0);
	wr32(c + GICC_CTLR, r->ctlr_before | 1);	/* NS view: EnableGrp1 */
	r->ctlr_after = rd32(c + GICC_CTLR);

	/* virtual timer: 10 ms period for 300 ms -> ~30 */
	wsr("cntv_tval_el0", xp.cntfrq / 100);
	wsr("cntv_ctl_el0", 1);
	isb();
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	end = xp_now() + xp.cntfrq * 3 / 10;
	while (xp_now() < end)
		;
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	isb();

	/* SGIs to itself */
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	for (int i = 0; i < SELF_SGIS; i++) {
		wr32(d + GICD_SGIR, SGIR_SELF | SGI_SELF);
		xp_delay_ms(5);
	}
	xp_delay_ms(20);
	__asm__ volatile("msr daifset, #2" ::: "memory");
	xp_flush(r, sizeof(*r));
}

void secondary_main(uint64_t slot)
{
	int s = slot & 7;
	volatile struct report *r = &rep[s];

	wsr("tpidr_el1", 0x100 | s);
	gic_cpu(s);
	r->magic = REP_READY;
	xp_flush(r, sizeof(*r));
	/* take CPU0's SGIs until it says it is done */
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	while (!sgi_done)
		__asm__ volatile("wfi");
	__asm__ volatile("msr daifset, #2" ::: "memory");
	xp_flush(r, sizeof(*r));
	xp_square(s, 0xff00ff00);
	for (;;)
		__asm__ volatile("wfe");
}

/* regs: x0-x28, a pad word, then the x29/x30 the vector stashed */
void exc_handler(uint64_t slot, uint64_t *regs)
{
	uint64_t t = rsr("tpidr_el1");

	if (slot == 5) {	/* IRQ, current EL, SPx */
		irq();
		return;
	}
	if (t & 0x100) {	/* a secondary: report and park */
		volatile struct report *r = &rep[t & 7];
		r->esr = rsr("esr_el1");
		r->elr = rsr("elr_el1");
		r->magic = REP_FAULT;
		xp_flush(r, sizeof(*r));
		xp_square(t & 7, 0xffff0000);
		for (;;)
			__asm__ volatile("wfe");
	}
	if (in_exc++ || !xp.fb_ok)
		xp_reset();
	char b[48];
	con_clear(PANICBG);
	con_title("SHIM PANIC");
	put_dec(put(b, "slot "), slot);
	con_str("EXC", b);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("LR", regs[31]);
	xp_delay_ms(60000);
	xp_reset();
}

/* ---- screen ---- */

static int ok_timer(uint32_t n)
{
	return n >= 25 && n <= 35;
}

static void row(int i, int started)
{
	volatile struct report *r = &rep[i];
	char label[] = "CPU0", b[80], *p;

	label[3] = '0' + i;
	p = put_hex(b, xp.cpu_reg[i], 3);
	if (!started) {
		put(p, " (M3, not started)");
	} else if (r->magic == REP_FAULT) {
		p = put_hex(put(p, " FAULT ESR "), r->esr, 8);
		put_hex(put(p, " ELR "), r->elr, 9);
	} else if (r->magic != REP_READY && i) {
		put(p, " never reported");
	} else {
		p = put_hex(put(p, " if "), r->ifc, 2);
		p = put_dec(put(p, " tmr "), r->timer);
		p = put_dec(put(p, " sgi "), r->sgi);
		p = put_dec(put(p, " x "), r->cross);
		p = put_dec(put(p, " oth "), r->other);
		if (r->other)
			p = put_dec(put(p, "/"), r->last);
		put_hex(put(p, " grp "), r->igroup0, 8);
	}
	con_str(label, b);
}

void shim_main(uint64_t dtb)
{
	char b[96], *p;
	int started[XP_NR_CPUS] = { 0 };

	wsr("tpidr_el1", 0);
	xp_init(dtb);
	vtimer = 16 + (xp.ntimer >= 3 ? xp.timer_ppi[2] : 11);
	uintptr_t d = xp_gicd(), c = xp_gicc();

	if (xp.fb_ok) {
		con_clear(GREY);
		con_title("EXYNOS X2 RUNS");
		con_str("MODEL", xp.model[0] ? xp.model : "(no DTB)");
		p = put_hex(put(b, "D "), d, 9);
		p = put_hex(put(p, " C "), c, 9);
		put(p, xp.ngic_reg >= 2 ? " (DT reg)" : " (default)");
		con_str("GIC", b);
		con_str("", xp.gic_compat[0] ? xp.gic_compat : "no GIC node");
		p = put(b, "ppi");
		for (int i = 0; i < xp.ntimer; i++)
			p = put_dec(put(p, " "), xp.timer_ppi[i]);
		put_dec(put(p, " -> vtimer INTID "), vtimer);
		con_str("TIMER", b);
	}

	/* Distributor: Group 1 forwarding on (NS view of GICD_CTLR, bit 0). */
	uint32_t dctlr = rd32(d + GICD_CTLR);
	wr32(d + GICD_CTLR, dctlr | 1);
	if (xp.fb_ok) {
		p = put_hex(put(b, "ctlr "), dctlr, 2);
		p = put_hex(put(p, " -> "), rd32(d + GICD_CTLR), 2);
		p = put_hex(put(p, " typer "), rd32(d + GICD_TYPER), 8);
		put_hex(put(p, " iidr "), rd32(d + GICD_IIDR), 8);
		con_str("GICD", b);
		p = put_hex(put(b, "iidr "), rd32(c + GICC_IIDR), 8);
		put(p, "  (GIC-400: 0202143B)");
		con_str("GICC", b);
		con_skip();
	}

	/* CPU0 first, then each A55 (/cpus reg below 0x100) in turn. */
	gic_cpu(0);
	started[0] = 1;
	int row_y = con_row();
	if (xp.fb_ok)
		row(0, 1);
	for (int i = 1; i < xp.ncpu; i++) {
		if (xp.cpu_reg[i] >= 0x100)
			continue;
		rep[i].magic = 0;
		xp_flush(&rep[i], sizeof(rep[i]));
		started[i] = 1;
		xp_cpu_on(i);
		for (int t = 0; t < WAIT_MS; t++) {
			xp_flush(&rep[i], sizeof(rep[i]));
			if (rep[i].magic == REP_READY || rep[i].magic == REP_FAULT)
				break;
			xp_delay_ms(1);
		}
	}

	/* CPU0 -> each waiting core, by its CPU interface bit */
	for (int i = 1; i < xp.ncpu; i++) {
		if (!started[i] || rep[i].magic != REP_READY || !rep[i].ifc)
			continue;
		for (int k = 0; k < CROSS_SGIS; k++) {
			wr32(d + GICD_SGIR, rep[i].ifc << 16 | SGI_CROSS);
			xp_delay_ms(10);
		}
	}
	xp_delay_ms(50);
	sgi_done = 1;
	xp_flush(&sgi_done, sizeof(sgi_done));
	for (int i = 1; i < xp.ncpu; i++)
		if (started[i] && rep[i].magic == REP_READY && rep[i].ifc)
			wr32(d + GICD_SGIR, rep[i].ifc << 16 | SGI_WAKE);
	xp_delay_ms(50);

	int pass = 1, n = 0;
	for (int i = 0; i < xp.ncpu; i++) {
		if (!started[i])
			continue;
		xp_flush(&rep[i], sizeof(rep[i]));
		n++;
		if ((i && rep[i].magic != REP_READY) || !ok_timer(rep[i].timer) ||
		    rep[i].sgi != SELF_SGIS || (i && rep[i].cross != CROSS_SGIS))
			pass = 0;
	}

	if (xp.fb_ok) {
		con_set_row(24);
		con_title(pass ? "EXYNOS X2: OK  " : "EXYNOS X2: FAIL");
		con_set_row(row_y);
		for (int i = 0; i < xp.ncpu; i++)
			row(i, started[i]);
		p = put_dec(b, n);
		put(p, pass ? " cores: timer, SGI self+cross OK" : " cores: see rows (tmr~30 sgi 10 x 5)");
		con_str("VERDICT", b);
		con_skip();
	}
	int tick_y = con_row();
	uint64_t t0 = xp_now();
	for (unsigned s = 0; s <= RUN_SECONDS; s++) {
		while (xp_now() - t0 < xp.cntfrq * s)
			;
		if (xp.fb_ok) {
			p = put(put_dec(b, s), "s  reboot in ");
			put(put_dec(p, RUN_SECONDS - s), "s ");
			con_set_row(tick_y);
			con_str("TIME", b);
			con_str("", "Take a photo of this screen.");
		}
	}
	xp_reset();
}
