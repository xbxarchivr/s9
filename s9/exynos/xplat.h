#pragma once
#include <stdint.h>

/*
 * What every Exynos stage after X1 needs: the platform as the DT describes
 * it, PSCI (0.1 IDs from the DT), the panel S-Boot left on, and starting
 * cores one at a time through xentry.S. Facts behind the defaults: NOTES.md.
 */

#define XP_NR_CPUS	8
#define XP_FB_BASE	0xcc000000UL	/* X1: a8r8g8b8, 1440x2960 */
#define XP_DECON_BASE	0x16030000UL	/* X1: decon_f */
#define XP_GICD_BASE	0x10101000UL	/* used only if the DT gives no reg */
#define XP_GICC_BASE	0x10102000UL

struct xplat {
	uintptr_t dtb, image, image_end;
	uint32_t dtb_size;
	uint64_t cntfrq;		/* CPU0's; S-Boot leaves secondaries' at 0 */
	char model[48], psci_method[8], gic_compat[40];
	uint32_t psci_cpu_on, psci_cpu_off;
	int ncpu;
	uint64_t cpu_reg[XP_NR_CPUS];	/* /cpus reg: logical ids PSCI translates */
	int ngic_reg;
	uint64_t gic_reg[4][2];		/* dist, cpu, vctrl, vcpu: base, size */
	int ntimer;
	uint32_t timer_ppi[4];		/* secure phys, phys, virt, hyp */
	uint32_t trig_before, trig_after;
	int fb_ok;			/* fb does not overlap the image or DTB */
};

extern struct xplat xp;
extern volatile uint32_t x_next_slot;	/* xentry.S reads it */

void xp_init(uint64_t dtb);		/* DT, display; paints nothing */
uint64_t xp_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t xp_now(void);
void xp_delay_ms(unsigned ms);
void xp_flush(const volatile void *p, uint64_t n);
void xp_square(int slot, uint32_t argb);
int64_t xp_cpu_on(int i);		/* CPU_ON for /cpus entry i as slot i */
void __attribute__((noreturn)) xp_reset(void);
uintptr_t xp_gicd(void);
uintptr_t xp_gicc(void);
