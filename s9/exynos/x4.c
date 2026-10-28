/*
 * Galaxy S9 Exynos milestone X4: boot XNU. As on the SDM845 (shim/m4.c),
 * the boot image's ramdisk holds the SWEETXNU payload (tools/mkpayload.py:
 * kernel + ADT from exynos/mkadt.py + boot-args), and shim/loader.c plays
 * iBoot: copies the kernel to the ADT's RAM window (0x92000000), places the
 * ADT and boot_args behind it and jumps with the MMU off, CPU0 only.
 * Before that, xp_init() switches the DECON-F HW trigger on (X1), so XNU's
 * framebuffer console reaches the glass.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "loader.h"
#include "xplat.h"

#define RUN_SECONDS	60

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

static volatile int in_exc;

/* regs: x0-x28, a pad word, then the x29/x30 the vector stashed */
void exc_handler(uint64_t slot, uint64_t *regs)
{
	char b[48];

	if (in_exc++ || !xp.fb_ok)
		xp_reset();
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

void shim_main(uint64_t dtb)
{
	char b[64], *p;

	wsr("tpidr_el1", 0);
	xp_init(dtb);
	if (!xp.fb_ok)
		xp_reset();
	con_clear(GREY);
	con_title("EXYNOS X4: XNU");
	con_str("MODEL", xp.model[0] ? xp.model : "(no DTB)");
	p = put_hex(put(b, "IMG "), xp.image, 9);
	put_hex(put(p, " DTB "), xp.dtb, 9);
	con_str("SHIM", b);
	p = put_hex(put(b, "trig "), xp.trig_before, 4);
	put_hex(put(p, " -> "), xp.trig_after, 4);
	con_str("DECON", b);

	const char *why = loader_boot(dtb);	/* returns only if it could not jump */
	con_str("LOADER", why);
	con_skip();
	con_str("", "Take a photo of this screen.");
	int y = con_row();
	uint64_t t0 = xp_now();
	for (unsigned s = 0; s <= RUN_SECONDS; s++) {
		while (xp_now() - t0 < xp.cntfrq * s)
			;
		p = put(put_dec(b, s), "s  reboot in ");
		put(put_dec(p, RUN_SECONDS - s), "s ");
		con_set_row(y);
		con_str("TIME", b);
	}
	xp_reset();
}
