/*
 * Read-only dump of the Exynos 9810 UFS host as TWRP's Linux leaves it, for
 * comparison with the X5 bare-metal re-init. Prints to dmesg ("UFSDUMP") and
 * refuses to stay loaded (init returns -ENODEV). Writes nothing anywhere.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>

#define UFS_HCI		0x11120000
#define UFS_VS		0x11121100
#define UFS_PHY		0x11124000
#define UFS_UFSP	0x11130000
#define SYSREG_COH	0x11010700
#define PHY_PMU		0x14060724

static void dump(const char *tag, phys_addr_t pa, unsigned int len)
{
	void __iomem *b = ioremap(pa, len < 16 ? 16 : len);
	unsigned int i;

	if (!b) {
		pr_info("UFSDUMP %s ioremap failed\n", tag);
		return;
	}
	if (len < 16)
		pr_info("UFSDUMP %s %08llx: %08x\n", tag, (u64)pa, readl(b));
	else
		for (i = 0; i < len; i += 16)
			pr_info("UFSDUMP %s %08llx: %08x %08x %08x %08x\n", tag, (u64)pa + i,
				readl(b + i), readl(b + i + 4), readl(b + i + 8), readl(b + i + 12));
	iounmap(b);
}

static void dump_ram(const char *tag, u64 pa, unsigned int len)
{
	const u32 *p = (const u32 *)__va(pa);
	unsigned int i;

	for (i = 0; i < len / 4; i += 4)
		pr_info("UFSDUMP %s %08llx: %08x %08x %08x %08x\n", tag, pa + 4 * i,
			p[i], p[i + 1], p[i + 2], p[i + 3]);
}

static int __init ufsdump_init(void)
{
	void __iomem *h;
	u64 utrl, ucd;
	u32 dw[8];
	int slot, shown = 0, i;

	dump("HCI", UFS_HCI, 0x100);
	dump("VS", UFS_VS, 0x160);
	dump("COH", SYSREG_COH, 4);
	dump("PMU", PHY_PMU, 4);
	dump("PHY", UFS_PHY, 0x800);

	/* the request list Linux uses right now (RAM, linear map) */
	h = ioremap(UFS_HCI, 0x100);
	if (!h)
		return -ENODEV;
	utrl = (u64)readl(h + 0x54) << 32 | readl(h + 0x50);
	pr_info("UFSDUMP UTRL %016llx DBR %08x\n", utrl, readl(h + 0x58));
	iounmap(h);
	for (slot = 0; slot < 32 && utrl; slot++) {
		const u32 *u = (const u32 *)__va(utrl + 32 * slot);

		for (i = 0; i < 8; i++)
			dw[i] = u[i];
		pr_info("UFSDUMP UTRD %2d: %08x %08x %08x %08x %08x %08x %08x %08x\n", slot,
			dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
		/* a data command: show its command UPIU and PRDT */
		if (shown < 3 && (dw[7] >> 16)) {
			ucd = (u64)dw[5] << 32 | (dw[4] & ~0x7fu);
			dump_ram("UPIU", ucd, 32);
			dump_ram("PRDT", ucd + (dw[7] & 0xffff), 128);
			shown++;
		}
	}

	/* UFSP last: only the offsets X5 already reads from the normal world */
	dump("UFSP", UFS_UFSP, 0x20);
	dump("UFSPW", UFS_UFSP + 0x100, 0x20);
	pr_info("UFSDUMP done\n");
	return -ENODEV;
}
module_init(ufsdump_init);
MODULE_LICENSE("GPL");
