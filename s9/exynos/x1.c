/*
 * Galaxy S9 Exynos (SM-G960F/G965F, Exynos 9810, starlte/star2lte)
 * milestone X1 (rev 1.1): prove the shim runs under Samsung's S-Boot and answer the
 * questions that decide whether this SoC can run XNU at all.
 *
 *   1. Does every core, Mongoose M3 included, support the 16K granule
 *      (ID_AA64MMFR0_EL1.TGran16)? XNU on arm64 needs it; the A55s have it,
 *      nobody has checked the M3s. Each secondary is started with PSCI
 *      CPU_ON and reports its own ID registers.
 *   2. What S-Boot hands over: EL, DTB, PSCI conduit, memory banks,
 *      reserved memory, the GIC (DT node and ID_AA64PFR0_EL1.GIC: 0 means
 *      no GICv3 system registers, i.e. a GICv2 GIC-400) and the DECON nodes.
 *   3. Can we draw on the panel S-Boot left on?
 *
 * Nothing below is verified on this phone yet. Starting points, from the
 * public Exynos 9810 bring-up work (uniLoader board-starlte, mainline
 * exynos9810-starlte.dts): framebuffer 1440x2960 a8r8g8b8 at 0xcc000000,
 * DECON-F at 0x16030000. The S6E3HA8 runs in command mode, so pixels only
 * reach the glass if DECON keeps triggering frames: uniLoader writes
 * 0x1281 to HW_SW_TRIG_CONTROL (+0x70) for that. The DT wins over these
 * defaults whenever it names a framebuffer region or a DECON node.
 *
 * No watchdog, no RAM log: the Qualcomm ones in m1.c do not exist here and
 * nothing is known yet about the Exynos ones. The screen is the log, and
 * the phone resets itself after RUN_SECONDS so a run with a dark screen
 * still says "the shim ran" by rebooting on time.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "fdt.h"
#include "con.h"

#define DEF_FB_BASE		0xcc000000UL
#define DEF_DECON_BASE		0x16030000UL
#define DECON_TRIG		0x70		/* HW_SW_TRIG_CONTROL */
#define DECON_TRIG_HW		0x1281		/* what uniLoader writes */

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define NR_CPUS			8
#define NR_MEM			4
#define NR_RESV			8
#define NR_NODE			3
#define RUN_SECONDS		90
#define PANIC_SECONDS		60
#define CPU_ROW_H		48	/* con.c rows: 16 px glyphs at scale 3 */
#define CPU_ROW_COLS		50

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

extern char _head[], _end[], x_secondary_entry[];

enum { CPU_ABSENT, CPU_CALL, CPU_WAIT, CPU_UP, CPU_FAULT, CPU_SELF };

struct cpu {
	uint64_t reg;			/* MPIDR affinity from the DT */
	char compat[28], enable[12];
	volatile int state;
	volatile uint64_t midr, mpidr, mmfr0, mmfr1, pfr0, el, esr, elr;
	int64_t ret;			/* PSCI CPU_ON */
};

struct node {
	char name[40], compat[40];
	uint64_t addr;			/* unit address from the node name */
};

struct region {
	char name[32];
	uint64_t base, size;
};

static struct {
	uintptr_t dtb, image, image_end;
	uint32_t dtb_size;
	char model[48], compat[48], bootloader[32], hardware[32], lcdtype[16];
	char psci_method[8], psci_compat[28];
	uint32_t psci_cpu_on, psci_cpu_off;	/* PSCI 0.1: function IDs from the DT */
	uint32_t root_ac, root_sc, rm_ac, rm_sc;
	int ncpu, nmem, nresv, ngic, ndecon;
	struct cpu cpu[NR_CPUS];
	struct region mem[NR_MEM], resv[NR_RESV];
	struct node gic[NR_NODE], decon[NR_NODE];
	uint64_t psci, cntfrq;
	uintptr_t fb_base, decon_base;
	int fb_src;			/* 0 default, 1 reserved-memory, -1 unsafe */
	uint32_t trig_before, trig_after;
} x;

static volatile int in_exc;

/* ---- small helpers ---- */

static uint64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	if (x.psci_method[0] == 'h')
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

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + x.cntfrq / 1000 * ms;
	while (now() < end)
		;
}

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

/* node name up to '@' equals b */
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

static uint64_t unit_addr(uintptr_t name)
{
	uint64_t v = 0;

	while (rd8(name) && rd8(name) != '@')
		name++;
	if (!rd8(name))
		return 0;
	if (rd8(name + 1) == '0' && (rd8(name + 2) == 'x' || rd8(name + 2) == 'X'))
		name += 2;
	for (name++;; name++) {
		uint8_t c = rd8(name);
		if (c >= '0' && c <= '9')
			v = v << 4 | (c - '0');
		else if (c >= 'a' && c <= 'f')
			v = v << 4 | (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v = v << 4 | (c - 'A' + 10);
		else
			return v;
	}
}

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
	return a < b + blen && b < a + alen;
}

/* ---- device tree ---- */

/* the word after key in the command line */
static void bootarg(uintptr_t v, uint32_t len, const char *key, char *out, int n)
{
	for (uint32_t i = 0; i < len; i++) {
		int m = 0;
		while (key[m] && i + m < len && rd8(v + i + m) == (uint8_t)key[m])
			m++;
		if (key[m])
			continue;
		int o = 0;
		for (uint32_t j = i + m; j < len && o < n - 1; j++) {
			uint8_t c = rd8(v + j);
			if (!c || c == ' ')
				break;
			out[o++] = c;
		}
		out[o] = 0;
		return;
	}
}

static struct cpu *cpu_slot(void)
{
	return x.ncpu ? &x.cpu[x.ncpu - 1] : 0;
}

static void dt_prop(void *ctx, int depth, const uintptr_t *names,
		    uintptr_t prop, uintptr_t val, uint32_t len)
{
	(void)ctx;
	if (depth == 1) {
		if (dt_eq(prop, "model"))
			dt_copy(x.model, sizeof(x.model), val, len);
		else if (dt_eq(prop, "compatible"))
			dt_copy(x.compat, sizeof(x.compat), val, len);
		else if (dt_eq(prop, "#address-cells") && len == 4)
			x.root_ac = be32(val);
		else if (dt_eq(prop, "#size-cells") && len == 4)
			x.root_sc = be32(val);
		return;
	}

	/* /cpus/cpu@N: properties come in node order, so a new "reg" opens a slot */
	if (depth == 3 && dt_eq(names[2], "cpus") && dt_base_eq(names[3], "cpu")) {
		if (dt_eq(prop, "reg") && x.ncpu < NR_CPUS && len >= 4) {
			struct cpu *c = &x.cpu[x.ncpu++];
			c->reg = be32(val + len - 4);	/* last cell: Aff2..Aff0 */
			c->state = CPU_ABSENT;
		} else if (dt_eq(prop, "compatible") && cpu_slot()) {
			dt_copy(cpu_slot()->compat, sizeof(cpu_slot()->compat), val, len);
		} else if (dt_eq(prop, "enable-method") && cpu_slot()) {
			dt_copy(cpu_slot()->enable, sizeof(cpu_slot()->enable), val, len);
		}
		return;
	}

	if (depth == 2 && dt_base_eq(names[2], "memory") && dt_eq(prop, "reg")) {
		uint32_t ac = x.root_ac ? x.root_ac : 2, sc = x.root_sc ? x.root_sc : 2;
		uint32_t step = 4 * (ac + sc);
		for (uint32_t o = 0; o + step <= len && x.nmem < NR_MEM; o += step) {
			x.mem[x.nmem].base = cells(val + o, ac);
			x.mem[x.nmem].size = cells(val + o + 4 * ac, sc);
			x.nmem++;
		}
		return;
	}

	/* the node's own cells come before its children in the blob */
	if (depth == 2 && dt_eq(names[2], "reserved-memory") && len == 4) {
		if (dt_eq(prop, "#address-cells"))
			x.rm_ac = be32(val);
		else if (dt_eq(prop, "#size-cells"))
			x.rm_sc = be32(val);
		return;
	}

	if (depth == 3 && dt_eq(names[2], "reserved-memory") && dt_eq(prop, "reg") &&
	    x.nresv < NR_RESV) {
		uint32_t ac = x.rm_ac ? x.rm_ac : x.root_ac ? x.root_ac : 2;
		uint32_t sc = x.rm_sc ? x.rm_sc : x.root_sc ? x.root_sc : 2;
		struct region *r = &x.resv[x.nresv++];
		dt_copy(r->name, sizeof(r->name), names[3], 64);
		if (len >= 4 * (ac + sc)) {
			r->base = cells(val, ac);
			r->size = cells(val + 4 * ac, sc);
		} else {
			r->base = unit_addr(names[3]);
		}
		return;
	}

	if (depth == 2 && dt_base_eq(names[2], "psci")) {
		if (dt_eq(prop, "method"))
			dt_copy(x.psci_method, sizeof(x.psci_method), val, len);
		else if (dt_eq(prop, "compatible"))
			dt_copy(x.psci_compat, sizeof(x.psci_compat), val, len);
		else if (dt_eq(prop, "cpu_on") && len == 4)
			x.psci_cpu_on = be32(val);
		else if (dt_eq(prop, "cpu_off") && len == 4)
			x.psci_cpu_off = be32(val);
		return;
	}

	if (depth == 2 && dt_eq(names[2], "chosen") && dt_eq(prop, "bootargs")) {
		bootarg(val, len, "androidboot.bootloader=", x.bootloader, sizeof(x.bootloader));
		bootarg(val, len, "androidboot.hardware=", x.hardware, sizeof(x.hardware));
		bootarg(val, len, "lcdtype=", x.lcdtype, sizeof(x.lcdtype));
		return;
	}

	if (dt_eq(prop, "compatible")) {
		struct node *n = 0;
		if (dt_has(val, len, "arm,gic") || dt_has(val, len, "arm,cortex-a15-gic")) {
			if (x.ngic < NR_NODE)
				n = &x.gic[x.ngic++];
		} else if (dt_has(val, len, "decon")) {
			if (x.ndecon < NR_NODE)
				n = &x.decon[x.ndecon++];
		}
		if (n) {
			dt_copy(n->name, sizeof(n->name), names[depth], 64);
			dt_copy(n->compat, sizeof(n->compat), val, len);
			n->addr = unit_addr(names[depth]);
		}
	}
}

static int name_has(const char *s, const char *needle)
{
	for (; *s; s++) {
		int k = 0;
		while (needle[k] && s[k] == needle[k])
			k++;
		if (!needle[k])
			return 1;
	}
	return 0;
}

/* A reserved region big enough for the panel whose name says framebuffer. */
static void pick_fb(void)
{
	uint64_t need = (uint64_t)PANEL_W * PANEL_H * 4;

	x.fb_base = DEF_FB_BASE;
	x.fb_src = 0;
	for (int i = 0; i < x.nresv; i++) {
		struct region *r = &x.resv[i];
		if (r->size < need)
			continue;
		if (name_has(r->name, "framebuffer") || name_has(r->name, "fb") ||
		    name_has(r->name, "splash") || name_has(r->name, "decon")) {
			x.fb_base = r->base;
			x.fb_src = 1;
			break;
		}
	}
	/* never paint over ourselves or the DTB */
	if (overlaps(x.fb_base, need, x.image, x.image_end - x.image) ||
	    (x.dtb_size && overlaps(x.fb_base, need, x.dtb, x.dtb_size)))
		x.fb_src = -1;
}

/* The lowest-addressed DECON node is DECON-F, the one driving the main panel. */
static void pick_decon(void)
{
	x.decon_base = DEF_DECON_BASE;
	uint64_t best = ~0ULL;
	for (int i = 0; i < x.ndecon; i++)
		if (x.decon[i].addr && x.decon[i].addr < best &&
		    !name_has(x.decon[i].name, "dsim"))
			best = x.decon[i].addr;
	if (best != ~0ULL)
		x.decon_base = best;
}

/* ---- CPUs ---- */

static void cpu_read(struct cpu *c)
{
	c->midr = rsr("midr_el1");
	c->mpidr = rsr("mpidr_el1");
	c->mmfr0 = rsr("id_aa64mmfr0_el1");
	c->mmfr1 = rsr("id_aa64mmfr1_el1");
	c->pfr0 = rsr("id_aa64pfr0_el1");
	c->el = (rsr("CurrentEL") >> 2) & 3;
}

/* slot (x0 from xentry.S) only picks the stack: find our DT entry by MPIDR */
void secondary_main(uint64_t slot)
{
	uint64_t me = rsr("mpidr_el1") & 0xffffff;
	int idx = -1;

	(void)slot;
	for (int i = 0; i < x.ncpu; i++)
		if ((x.cpu[i].reg & 0xffffff) == me)
			idx = i;
	wsr("tpidr_el1", 0x100 | (idx & 7));
	if (idx >= 0) {
		cpu_read(&x.cpu[idx]);
		dsb();
		x.cpu[idx].state = CPU_UP;
		dsb();
	}
	psci_call(x.psci_cpu_off ? x.psci_cpu_off : PSCI_CPU_OFF, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static void cpu_row(int i);

/*
 * X1 hung somewhere in here on the G960F (PSCI_VERSION was NOT_SUPPORTED):
 * each core's row goes on screen before its CPU_ON, so a hang names the call.
 */
static void probe_cpus(int row)
{
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	uint64_t on = x.psci_cpu_on ? x.psci_cpu_on : PSCI_CPU_ON;
	int found_self = 0;

	for (int i = 0; i < x.ncpu; i++) {
		struct cpu *c = &x.cpu[i];
		con_set_row(row + i * CPU_ROW_H);
		if ((c->reg & 0xffffff) == self) {
			cpu_read(c);
			c->state = CPU_SELF;
			found_self = 1;
			if (x.fb_src >= 0)
				cpu_row(i);
			continue;
		}
		c->state = CPU_CALL;
		if (x.fb_src >= 0)
			cpu_row(i);
		dsb();
		c->state = CPU_WAIT;
		c->ret = (int64_t)psci_call(on, c->reg, (uintptr_t)x_secondary_entry, i);
		if (c->ret != 0)
			c->state = CPU_ABSENT;
		for (int t = 0; t < 1000 && c->state == CPU_WAIT; t++)
			delay_ms(1);
		con_set_row(row + i * CPU_ROW_H);
		if (x.fb_src >= 0)
			cpu_row(i);
	}
	/* No /cpus in the DT: at least report the boot core. */
	if (!found_self && x.ncpu < NR_CPUS) {
		struct cpu *c = &x.cpu[x.ncpu++];
		c->reg = self;
		cpu_read(c);
		c->state = CPU_SELF;
	}
}

/* ---- screen ---- */

static char *put_size(char *p, uint64_t v)
{
	if (v >= (1ULL << 30) && !(v & ((1ULL << 20) - 1)))
		return put(put_dec(p, v >> 20), "M");
	if (v >= (1ULL << 20))
		return put(put_dec(p, v >> 20), "M+");
	return put(put_dec(p, v >> 10), "K");
}

/* one fixed-width row per core, so a later state overwrites an earlier one */
static void cpu_row(int i)
{
	struct cpu *c = &x.cpu[i];
	char label[] = "CPU0", b[80], *p;

	label[3] = '0' + i;
	p = put_hex(b, c->reg, 3);
	switch (c->state) {
	case CPU_UP:
	case CPU_SELF:
		p = put_hex(put(p, " "), c->midr, 8);
		p = put(put(p, " 16K "), (c->mmfr0 >> 20) & 0xf ? "YES" : "NO ");
		p = put_hex(put(p, " M0 "), c->mmfr0, 8);
		p = put_dec(put(p, " EL"), c->el);
		if (c->state == CPU_SELF)
			p = put(p, " boot");
		break;
	case CPU_CALL:
		p = put(put(put(p, " "), c->enable[0] ? c->enable : "?"), " CPU_ON...");
		break;
	case CPU_FAULT:
		p = put_hex(put(p, " FAULT ESR "), c->esr, 8);
		p = put_hex(put(p, " ELR "), c->elr, 9);
		break;
	case CPU_WAIT:
		p = put(p, " CPU_ON ok, never arrived");
		break;
	default:
		p = put_hex(put(p, " CPU_ON ret "), (uint64_t)c->ret, 8);
	}
	while (p < b + CPU_ROW_COLS)
		*p++ = ' ';
	*p = 0;
	con_str(label, b);
}

static void bands(void)
{
	int y = con_row();
	fb_rect(0, y, fb.width, 72, RED);
	fb_text(24, y + 12, 3, "RED", WHITE, RED);
	fb_rect(0, y + 72, fb.width, 72, GREEN);
	fb_text(24, y + 84, 3, "GREEN", BLACK, GREEN);
	fb_rect(0, y + 144, fb.width, 72, BLUE);
	fb_text(24, y + 156, 3, "BLUE", WHITE, BLUE);
	con_set_row(y + 232);
}

static int all_16k(void)
{
	int seen = 0;
	for (int i = 0; i < x.ncpu; i++) {
		struct cpu *c = &x.cpu[i];
		if (c->state != CPU_UP && c->state != CPU_SELF)
			continue;
		seen++;
		if (!((c->mmfr0 >> 20) & 0xf))
			return 0;
	}
	return seen == x.ncpu ? 1 : -1;
}

static void draw(int final)
{
	char b[96], *p;

	con_clear(GREY);
	con_title(final ? "EXYNOS X1.1: OK" : "EXYNOS X1.1 RUNS");
	bands();

	con_str("MODEL", x.model[0] ? x.model : "(no DTB)");
	con_str("COMPAT", x.compat[0] ? x.compat : "?");
	con_str("SBOOT", x.bootloader[0] ? x.bootloader : "?");
	p = put(put(b, x.hardware[0] ? x.hardware : "?"), "  lcd ");
	put(p, x.lcdtype[0] ? x.lcdtype : "?");
	con_str("HW", b);
	p = put_dec(put(b, "EL"), (rsr("CurrentEL") >> 2) & 3);
	p = put_dec(put(p, "  CNTFRQ "), (uint32_t)x.cntfrq);
	con_str("ENTRY", b);
	p = put_hex(put(b, "IMG "), x.image, 9);
	p = put_hex(put(p, " DTB "), x.dtb, 9);
	put_size(put(p, " "), x.dtb_size);
	con_str("WHERE", b);
	p = put_hex(b, x.psci, 8);
	p = put(put(p, " via "), x.psci_method[0] ? x.psci_method : "smc?");
	put(put(p, " "), x.psci_compat[0] ? x.psci_compat : "?");
	con_str("PSCI", b);
	p = put_hex(put(b, "CPU_ON "), x.psci_cpu_on, 8);
	put_hex(put(p, " CPU_OFF "), x.psci_cpu_off, 8);
	con_str("PSCI ID", x.psci_cpu_on ? b : "no cpu_on in DT: 0.2 IDs");
	for (int i = 0; i < x.nmem; i++) {
		p = put_hex(b, x.mem[i].base, 9);
		put_size(put(p, " "), x.mem[i].size);
		con_str(i ? "" : "MEMORY", b);
	}
	if (!x.nmem)
		con_str("MEMORY", "? (no memory node)");
	con_skip();

	for (int i = 0; i < x.ngic; i++) {
		p = put(put(put(b, x.gic[i].name), " "), x.gic[i].compat);
		con_str(i ? "" : "GIC DT", b);
	}
	if (!x.ngic)
		con_str("GIC DT", "no arm,gic node");
	{
		uint32_t g = (x.cpu[0].pfr0 >> 24) & 0xf;
		for (int i = 0; i < x.ncpu; i++)
			if (x.cpu[i].state == CPU_SELF)
				g = (x.cpu[i].pfr0 >> 24) & 0xf;
		con_str("GIC CPU", g ? "GICv3+ sysregs" : "no sysregs (GICv2)");
	}
	for (int i = 0; i < x.ndecon; i++) {
		p = put(put(put(b, x.decon[i].name), " "), x.decon[i].compat);
		con_str(i ? "" : "DECON DT", b);
	}
	p = put_hex(put(b, "@"), x.decon_base, 9);
	p = put_hex(put(p, " trig "), x.trig_before, 4);
	put_hex(put(p, " -> "), x.trig_after, 4);
	con_str("DECON", b);
	p = put_hex(b, x.fb_base, 9);
	put(p, x.fb_src > 0 ? " (reserved-memory)" : " (default)");
	con_str("FB", b);
	for (int i = 0; i < x.nresv; i++) {
		p = put(put(b, x.resv[i].name), " ");
		p = put_hex(p, x.resv[i].base, 9);
		put_size(put(p, " "), x.resv[i].size);
		con_str(i ? "" : "RESERVED", b);
	}
	con_skip();
	if (!final)
		return;

	for (int i = 0; i < x.ncpu; i++)
		cpu_row(i);
	{
		int v = all_16k();
		con_str("VERDICT", v > 0 ? "16K on every core: XNU can run" :
				   v == 0 ? "a core lacks 16K: XNU CANNOT run" :
					    "not every core reported");
	}
	con_skip();
}

static void tick(int row, unsigned secs, unsigned total)
{
	char b[48], *p;

	p = put(put_dec(b, secs), "s  reboot in ");
	put(put_dec(p, total - secs), "s ");
	con_set_row(row);
	con_str("TIME", b);
	con_str("", "Take a photo of this screen.");
}

static void __attribute__((noreturn)) reset(void)
{
	psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* regs: x0-x28, a pad word, then the x29/x30 the vector stashed */
void exc_handler(uint64_t slot, uint64_t *regs)
{
	uint64_t t = rsr("tpidr_el1");
	char b[48];

	if (t & 0x100) {	/* a secondary: report and park */
		struct cpu *c = &x.cpu[t & 7];
		c->esr = rsr("esr_el1");
		c->elr = rsr("elr_el1");
		dsb();
		c->state = CPU_FAULT;
		dsb();
		for (;;)
			__asm__ volatile("wfe");
	}
	if (in_exc++ || x.fb_src < 0)
		reset();
	con_clear(PANICBG);
	con_title("SHIM PANIC");
	put_dec(put(b, "slot "), slot);
	con_str("EXC", b);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("LR", regs[31]);
	con_hex("IMG", x.image);
	delay_ms(PANIC_SECONDS * 1000);
	reset();
}

void shim_main(uint64_t dtb)
{
	wsr("tpidr_el1", 0);
	x.dtb = dtb;
	x.image = (uintptr_t)_head;
	x.image_end = (uintptr_t)_end;
	x.cntfrq = rsr("cntfrq_el0");
	if (!x.cntfrq)
		x.cntfrq = 26000000;	/* Exynos 9810 system counter */

	/* 1. Read everything that needs no device register: DTB is plain RAM. */
	x.dtb_size = fdt_totalsize(dtb);
	fdt_walk(dtb, dt_prop, 0);
	pick_fb();
	pick_decon();

	/* 2. Proof of life. On a command-mode panel this may stay invisible until step 3. */
	fb.base = x.fb_base;
	fb.width = PANEL_W;
	fb.height = PANEL_H;
	fb.stride = PANEL_W * 4;
	fb.bpp = 4;
	fb.pos_b = 0;		/* a8r8g8b8: B, G, R, A in memory */
	fb.pos_g = 1;
	fb.pos_r = 2;
	fb.pos_a = 3;
	if (x.fb_src >= 0) {
		con_clear(GREY);
		con_title("EXYNOS X1.1 RUNS");
		bands();
	}

	/* 3. Let DECON send our frames: the first device register we touch. */
	x.trig_before = rd32(x.decon_base + DECON_TRIG);
	if (x.trig_before != DECON_TRIG_HW)
		wr32(x.decon_base + DECON_TRIG, DECON_TRIG_HW);
	dsb();
	x.trig_after = rd32(x.decon_base + DECON_TRIG);

	/* 4. PSCI and the other cores. */
	x.psci = psci_call(PSCI_VERSION, 0, 0, 0);
	if (x.fb_src >= 0)
		draw(0);
	probe_cpus(con_row());

	/* 5. Full report, then a counter: if it moves, frames keep reaching the glass. */
	if (x.fb_src >= 0)
		draw(1);
	int row = con_row();
	uint64_t t0 = now();
	for (unsigned s = 0; s <= RUN_SECONDS; s++) {
		while (now() - t0 < x.cntfrq * s)
			;
		if (x.fb_src >= 0)
			tick(row, s, RUN_SECONDS);
	}
	reset();
}
