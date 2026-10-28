/*
 * Exynos platform layer for the stages after X1 (X1 keeps its own copy of
 * all this, as it ran on the phone). See xplat.h.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "fdt.h"
#include "xplat.h"

#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define DECON_TRIG		0x70		/* HW_SW_TRIG_CONTROL */
#define DECON_TRIG_HW		0x1281		/* X1: makes our frames reach the glass */
#define SQ			40

extern char _head[], _end[], x_secondary_entry[];

struct xplat xp;
volatile uint32_t x_next_slot;

uint64_t xp_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	if (xp.psci_method[0] == 'h')
		__asm__ volatile("hvc #0"
				 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
				 :
				 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	else
		__asm__ volatile("smc #0"
				 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
				 :
				 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

uint64_t xp_now(void)
{
	isb();
	return rsr("cntpct_el0");
}

void xp_delay_ms(unsigned ms)
{
	uint64_t end = xp_now() + xp.cntfrq / 1000 * ms;
	while (xp_now() < end)
		;
}

void xp_flush(const volatile void *p, uint64_t n)
{
	dsb();
	for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + n; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	dsb();
}

/* Constants only, so a core that knows nothing else can still draw it. */
void xp_square(int slot, uint32_t argb)
{
	uintptr_t base = XP_FB_BASE + 40 * PANEL_W * 4 + (PANEL_W - (8 - slot) * (SQ + 16)) * 4;

	for (int y = 0; y < SQ; y++) {
		volatile uint32_t *q = (volatile uint32_t *)(base + y * PANEL_W * 4);
		for (int i = 0; i < SQ; i++)
			q[i] = argb;
		xp_flush(q, SQ * 4);
	}
}

int64_t xp_cpu_on(int i)
{
	x_next_slot = i;
	xp_flush(&x_next_slot, sizeof(x_next_slot));
	return (int64_t)xp_psci(xp.psci_cpu_on ? xp.psci_cpu_on : PSCI_CPU_ON,
				xp.cpu_reg[i], (uintptr_t)x_secondary_entry, 0);
}

void xp_reset(void)
{
	xp_psci(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

uintptr_t xp_gicd(void)
{
	return xp.ngic_reg >= 1 ? xp.gic_reg[0][0] : XP_GICD_BASE;
}

uintptr_t xp_gicc(void)
{
	return xp.ngic_reg >= 2 ? xp.gic_reg[1][0] : XP_GICC_BASE;
}

/* ---- device tree ---- */

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(rd32(a));
}

static uint64_t cells(uintptr_t v, uint32_t n)
{
	return n == 2 ? (uint64_t)be32(v) << 32 | be32(v + 4) : be32(v);
}

static int dt_eq(uintptr_t a, const char *b)
{
	while (rd8(a) && rd8(a) == (uint8_t)*b)
		a++, b++;
	return rd8(a) == (uint8_t)*b;
}

static int dt_base_eq(uintptr_t a, const char *b)
{
	while (rd8(a) && rd8(a) != '@' && rd8(a) == (uint8_t)*b)
		a++, b++;
	return (rd8(a) == 0 || rd8(a) == '@') && *b == 0;
}

static int dt_has(uintptr_t a, uint32_t len, const char *needle)
{
	for (uint32_t i = 0; i < len; i++) {
		int k = 0;
		while (needle[k] && i + k < len && rd8(a + i + k) == (uint8_t)needle[k])
			k++;
		if (!needle[k])
			return 1;
	}
	return 0;
}

static void dt_copy(char *d, int n, uintptr_t s, uint32_t len)
{
	int i = 0;

	while (i < n - 1 && (uint32_t)i < len && rd8(s + i)) {
		d[i] = rd8(s + i);
		i++;
	}
	d[i] = 0;
}

/*
 * #address-cells / #size-cells of the node at each depth. The walker only
 * reports properties, so a node is "new" when its name pointer changes;
 * a new node starts from the DT defaults (2, 1).
 */
#define MAXD 16
static uintptr_t seen[MAXD + 1];
static uint32_t ac[MAXD + 1], sc[MAXD + 1];
static uintptr_t gic_node, timer_node;

static void dt_prop(void *ctx, int depth, const uintptr_t *names,
		    uintptr_t prop, uintptr_t val, uint32_t len)
{
	(void)ctx;
	if (depth > MAXD)
		return;
	if (seen[depth] != names[depth]) {
		seen[depth] = names[depth];
		ac[depth] = 2;
		sc[depth] = 1;
	}
	if (dt_eq(prop, "#address-cells") && len == 4)
		ac[depth] = be32(val);
	else if (dt_eq(prop, "#size-cells") && len == 4)
		sc[depth] = be32(val);

	if (depth == 1 && dt_eq(prop, "model"))
		dt_copy(xp.model, sizeof(xp.model), val, len);
	if (depth == 3 && dt_eq(names[2], "cpus") && dt_base_eq(names[3], "cpu") &&
	    dt_eq(prop, "reg") && len >= 4 && xp.ncpu < XP_NR_CPUS)
		xp.cpu_reg[xp.ncpu++] = be32(val + len - 4);
	if (depth == 2 && dt_base_eq(names[2], "psci")) {
		if (dt_eq(prop, "method"))
			dt_copy(xp.psci_method, sizeof(xp.psci_method), val, len);
		else if (dt_eq(prop, "cpu_on") && len == 4)
			xp.psci_cpu_on = be32(val);
		else if (dt_eq(prop, "cpu_off") && len == 4)
			xp.psci_cpu_off = be32(val);
	}
	if (dt_eq(prop, "compatible")) {
		if (!gic_node && (dt_has(val, len, "arm,gic-400") ||
				  dt_has(val, len, "arm,cortex-a15-gic"))) {
			gic_node = names[depth];
			dt_copy(xp.gic_compat, sizeof(xp.gic_compat), val, len);
		} else if (!timer_node && dt_has(val, len, "arm,armv8-timer")) {
			timer_node = names[depth];
		}
	}
}

/*
 * Second pass, now that the GIC and timer nodes are known (by the address
 * of their name): their reg/interrupts may come before their compatible.
 */
static uintptr_t want_gic, want_timer;
static uint32_t gic_pac, gic_psc;

static void dt_prop2(void *ctx, int depth, const uintptr_t *names,
		     uintptr_t prop, uintptr_t val, uint32_t len)
{
	(void)ctx;
	if (depth > MAXD)
		return;
	if (seen[depth] != names[depth]) {
		seen[depth] = names[depth];
		ac[depth] = 2;
		sc[depth] = 1;
	}
	if (dt_eq(prop, "#address-cells") && len == 4)
		ac[depth] = be32(val);
	else if (dt_eq(prop, "#size-cells") && len == 4)
		sc[depth] = be32(val);
	if (names[depth] == want_gic && dt_eq(prop, "reg")) {
		gic_pac = ac[depth - 1];
		gic_psc = sc[depth - 1];
		uint32_t step = 4 * (gic_pac + gic_psc);
		xp.ngic_reg = 0;
		for (uint32_t o = 0; o + step <= len && xp.ngic_reg < 4; o += step) {
			xp.gic_reg[xp.ngic_reg][0] = cells(val + o, gic_pac);
			xp.gic_reg[xp.ngic_reg][1] = cells(val + o + 4 * gic_pac, gic_psc);
			xp.ngic_reg++;
		}
	}
	if (names[depth] == want_timer && dt_eq(prop, "interrupts")) {
		xp.ntimer = 0;
		for (uint32_t o = 0; o + 12 <= len && xp.ntimer < 4; o += 12)
			xp.timer_ppi[xp.ntimer++] = be32(val + o + 4);
	}
}

void xp_init(uint64_t dtb)
{
	xp.dtb = dtb;
	xp.image = (uintptr_t)_head;
	xp.image_end = (uintptr_t)_end;
	xp.cntfrq = rsr("cntfrq_el0");
	if (!xp.cntfrq)
		xp.cntfrq = 26000000;	/* X1: what S-Boot sets on CPU0 */
	xp.dtb_size = fdt_totalsize(dtb);

	fdt_walk(dtb, dt_prop, 0);
	want_gic = gic_node;
	want_timer = timer_node;
	for (int d = 0; d <= MAXD; d++)
		seen[d] = 0;
	if (want_gic || want_timer)
		fdt_walk(dtb, dt_prop2, 0);

	uint64_t need = (uint64_t)PANEL_W * PANEL_H * 4;
	xp.fb_ok = !(XP_FB_BASE < xp.image_end && xp.image < XP_FB_BASE + need) &&
		   !(xp.dtb_size && XP_FB_BASE < xp.dtb + xp.dtb_size && xp.dtb < XP_FB_BASE + need);
	fb.base = XP_FB_BASE;
	fb.width = PANEL_W;
	fb.height = PANEL_H;
	fb.stride = PANEL_W * 4;
	fb.bpp = 4;
	fb.pos_b = 0;
	fb.pos_g = 1;
	fb.pos_r = 2;
	fb.pos_a = 3;

	xp.trig_before = rd32(XP_DECON_BASE + DECON_TRIG);
	if (xp.trig_before != DECON_TRIG_HW)
		wr32(XP_DECON_BASE + DECON_TRIG, DECON_TRIG_HW);
	dsb();
	xp.trig_after = rd32(XP_DECON_BASE + DECON_TRIG);
}
