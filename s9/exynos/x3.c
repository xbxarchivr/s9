/*
 * Galaxy S9 Exynos milestone X3 (rev 1.1): read-only probe of the internal UFS that
 * S-Boot loaded us from. CPU0 only. Adapted from shim/ufs.c (SDM845 M3).
 *
 * Stock DT (rev26, from the boot.img dt section): ufs@0x11120000
 * "samsung,exynos-ufs", reg = hci 0x11120000 (0x200), vendor hci 0x11121100
 * (0x200), unipro 0x11110000, ufsp/SMU 0x11130000; dma-coherent, io-coherency
 * bits 0x300 in 0x11010700. TWRP: BOOT = sda10 (57671680 bytes).
 *
 * Samsung's host differs from plain UFSHCI in ways we cannot test without
 * the phone, so the probe is built to find out safely:
 *   - Linux's exynos driver uses UFSHCD_QUIRK_PRDT_BYTE_GRAN: response and
 *     PRDT offsets/lengths in the UTRD in bytes, not dwords. The UTRD fields
 *     are chosen so both readings land inside our own 16 KB descriptor
 *     buffer (response at 512 or 2048, PRDT at 1024 or 4096, a copy of the
 *     PRDT entry at both), and where the response UPIU shows up says which.
 *   - Each PRDT entry moves at most 4 KB (HCI_TXPRDT/RXPRDT_ENTRY_SIZE =
 *     PRDT_SET_SIZE(12) in Linux), so one 4 KB block per command.
 *   - The slot's bit in HCI_UTRL_NEXUS_TYPE (vendor +0x40) marks a SCSI
 *     command; Linux sets it before every doorbell.
 * No SCSI command here writes: TEST UNIT READY, READ CAPACITY(10), READ(10).
 * MMU off: all accesses are Device, the DMA needs no cache maintenance.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "xplat.h"

#define UFS_HCI		0x11120000UL
#define UFS_VS		0x11121100UL
#define SYSREG_COH	0x11010700UL	/* ufs-io-coherency, mask 0x300 */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define VS_TXPRDT	0x00
#define VS_RXPRDT	0x04
#define VS_NEXUS	0x40

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u
#define RESP_FIELD	512u	/* bytes -> 512, dwords -> 2048 */
#define PRDT_FIELD	1024u	/* bytes -> 1024, dwords -> 4096 */
#define RUN_SECONDS	60

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

static inline uint32_t hci(uint32_t o) { return rd32(UFS_HCI + o); }
static inline void hci_wr(uint32_t o, uint32_t v) { wr32(UFS_HCI + o, v); }

struct utrd {
	uint32_t dw0, dw1, dw2, dw3, ucdba, ucdbau;
	uint32_t resp;		/* [31:16] response length, [15:0] offset */
	uint32_t prdt;		/* [31:16] PRDT length, [15:0] offset */
};

static volatile struct utrd utrl[32] __attribute__((aligned(4096)));
static volatile uint8_t ucd[16384] __attribute__((aligned(4096)));
static volatile uint8_t data[BLOCK] __attribute__((aligned(4096)));
static volatile int in_exc;

/* found by the first TEST UNIT READY */
static int dword_gran = -1;	/* 0: offsets in bytes (Exynos quirk), 1: standard */
static uint32_t prdt_len;	/* value of the PRDT length field that moves data */
static uint32_t resp_at;	/* where the response UPIU landed */

static void zero(volatile uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		p[i] = 0;
}

static uint32_t be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t le64(const volatile uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static void prdt_entry(uint32_t off, uintptr_t buf, uint32_t len)
{
	volatile uint32_t *e = (volatile uint32_t *)(ucd + off);
	e[0] = (uint32_t)buf;
	e[1] = (uint32_t)(buf >> 32);
	e[2] = 0;
	e[3] = len - 1;		/* data byte count - 1 */
}

/*
 * One SCSI command in slot 0. 0 = good status; else 1 timeout, 2 OCS error,
 * 3 no response UPIU found, 4 SCSI status not GOOD.
 */
static uint32_t scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len, uint32_t prdt_field_len,
		     uint32_t *ocs, uint32_t *st)
{
	static uint8_t tag;
	volatile struct utrd *u = &utrl[0];

	zero((volatile uint8_t *)u, sizeof(*u));
	zero(ucd, sizeof(ucd));
	ucd[0] = 0x01;			/* COMMAND UPIU */
	ucd[1] = len ? 0x40 : 0x00;	/* R: data in */
	ucd[2] = 0;			/* LUN 0 */
	ucd[3] = ++tag;
	ucd[12] = len >> 24;
	ucd[13] = len >> 16;
	ucd[14] = len >> 8;
	ucd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd[16 + i] = cdb[i];
	if (len) {	/* the entry where either reading of the offset looks */
		prdt_entry(PRDT_FIELD, (uintptr_t)data, len);
		prdt_entry(PRDT_FIELD * 4, (uintptr_t)data, len);
	}

	u->dw0 = 1u << 28 | (len ? 2u : 0u) << 25 | 1u << 24;	/* UFS storage, dev->host, I */
	u->dw2 = 0x0f;			/* OCS: invalid until the host writes it */
	u->ucdba = (uint32_t)(uintptr_t)ucd;
	u->ucdbau = (uint32_t)((uintptr_t)ucd >> 32);
	u->resp = RESP_FIELD << 16 | RESP_FIELD;
	u->prdt = (len ? prdt_field_len : 0) << 16 | PRDT_FIELD;
	dsb();

	wr32(UFS_VS + VS_NEXUS, rd32(UFS_VS + VS_NEXUS) | 1);	/* slot 0: SCSI command */
	hci_wr(REG_UTRLDBR, 1);
	uint64_t end = xp_now() + xp.cntfrq;	/* 1 s */
	while (hci(REG_UTRLDBR) & 1)
		if (xp_now() > end) {
			hci_wr(REG_UTRLCLR, ~1u);	/* write 0 to clear slot 0 */
			return 1;
		}
	dsb();
	*ocs = u->dw2 & 0xff;
	resp_at = ucd[RESP_FIELD] == 0x21 ? RESP_FIELD : ucd[RESP_FIELD * 4] == 0x21 ? RESP_FIELD * 4 : 0;
	*st = resp_at ? ucd[resp_at + 7] : 0xff;
	if (*ocs)
		return 2;
	if (!resp_at)
		return 3;
	return *st ? 4 : 0;
}

static uint32_t read_block(uint32_t lba, uint32_t *ocs, uint32_t *st)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, 1, 0 };

	zero(data, sizeof(data));
	return scsi(cdb, sizeof(cdb), BLOCK, prdt_len, ocs, st);
}

static char *put_rc(char *p, uint32_t rc, uint32_t ocs, uint32_t st)
{
	p = put_dec(put(p, "rc "), rc);
	p = put_hex(put(p, " ocs "), ocs, 2);
	return put_hex(put(p, " st "), st, 2);
}

static int name_is(const char *n, const char *want)
{
	while (*n && *n == *want)
		n++, want++;
	return *n == *want;
}

static const char *fs_type(uint32_t lba)
{
	uint32_t ocs, st;

	if (read_block(lba, &ocs, &st))
		return "read error";
	const volatile uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other (encrypted?)";
}

/* GPT entries, one block (32 entries) at a time; big ones and BOOT. */
static void list_partitions(uint32_t entry_lba, uint32_t nent, uint32_t *boot_lba)
{
	static char names[128][17];
	static uint64_t first[128], last[128];
	uint32_t ocs, st, n = 0;
	int shown = 0;
	char b[80], *p;

	for (uint32_t blk = 0; blk * 32 < nent && blk < 4; blk++) {
		if (read_block(entry_lba + blk, &ocs, &st)) {
			con_str("PART", "read error");
			return;
		}
		for (uint32_t i = 0; i < 32 && blk * 32 + i < nent; i++, n++) {
			const volatile uint8_t *e = data + i * 128;
			first[n] = le64(e + 32);
			last[n] = le64(e + 40);
			for (int c = 0; c < 16; c++) {
				uint8_t ch = e[56 + 2 * c];
				names[n][c] = ch >= 32 && ch < 127 ? ch : ch ? '?' : 0;
			}
			names[n][16] = 0;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		if (!first[i])
			continue;
		uint32_t mib = (uint32_t)((last[i] - first[i] + 1) >> 8);
		int boot = name_is(names[i], "BOOT");
		if (boot)
			*boot_lba = (uint32_t)first[i];
		if (mib < 64 && !boot && !name_is(names[i], "RECOVERY"))
			continue;
		p = put(b, names[i]);
		while (p < b + 11)
			*p++ = ' ';
		p = put_hex(put(p, "lba "), (uint32_t)first[i], 8);
		p = put_dec(put(p, " MiB "), mib);
		if (name_is(names[i], "USERDATA") || name_is(names[i], "CACHE"))
			put(put(p, " "), fs_type((uint32_t)first[i]));
		con_str(shown++ ? "" : "PART", b);
	}
}

static int probe(void)
{
	char b[96], *p;
	uint32_t ocs = 0, st = 0, rc = 0;

	uint32_t cap = hci(REG_CAP), hcs = hci(REG_HCS), hce = hci(REG_HCE);
	p = put_hex(put(b, "ver "), hci(REG_VER), 8);
	p = put_hex(put(p, " cap "), cap, 8);
	put_dec(put(p, " slots "), (cap & 0x1f) + 1);
	con_str("UFS", b);
	p = put_hex(put(b, "hce "), hce, 1);
	p = put_hex(put(p, " hcs "), hcs, 8);
	p = put_hex(put(p, " is "), hci(REG_IS), 8);
	put_hex(put(p, " rsr "), hci(REG_UTRLRSR), 1);
	con_str("", b);
	p = put_hex(put(b, "utrl "), (uint64_t)hci(REG_UTRLBAU) << 32 | hci(REG_UTRLBA), 9);
	put_hex(put(p, " dbr "), hci(REG_UTRLDBR), 8);
	con_str("", b);
	p = put_hex(put(b, "txprdt "), rd32(UFS_VS + VS_TXPRDT), 8);
	p = put_hex(put(p, " rxprdt "), rd32(UFS_VS + VS_RXPRDT), 8);
	put_hex(put(p, " nexus "), rd32(UFS_VS + VS_NEXUS), 8);
	con_str("VENDOR", b);
	put_hex(put(b, "io-coherency "), rd32(SYSREG_COH), 8);
	con_str("SYSREG", b);

	/*
	 * X3 on the G960F: HCE read 0 while HCS was 0x10F and S-Boot's list was
	 * running (Linux: UFSHCI_QUIRK_BROKEN_HCE for Exynos), so HCE says
	 * nothing here. Ready = device present, both lists and UIC ready.
	 */
	if (!(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY) || !(hcs & HCS_UCRDY)) {
		con_str("STOP", "host not ready");
		return 0;
	}
	if (hci(REG_UTRLDBR)) {
		con_str("STOP", "requests still pending");
		return 0;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	hci_wr(REG_UTRLRSR, 0);
	hci_wr(REG_UTRLBA, (uint32_t)(uintptr_t)utrl);
	hci_wr(REG_UTRLBAU, (uint32_t)((uintptr_t)utrl >> 32));
	hci_wr(REG_UTRLRSR, 1);
	/* Never ring a doorbell unless the host really uses our list. */
	if (hci(REG_UTRLBA) != (uint32_t)(uintptr_t)utrl || !(hci(REG_UTRLRSR) & 1)) {
		con_str("STOP", "host did not take our request list");
		return 0;
	}
	p = put_hex(put(b, "ours "), (uintptr_t)utrl, 9);
	put_hex(put(p, " ucd "), (uintptr_t)ucd, 9);
	con_str("LIST", b);

	/* TEST UNIT READY: no data, so only the response offset matters. */
	static const uint8_t tur[6] = { 0x00 };
	for (int i = 0; i < 3; i++) {
		rc = scsi(tur, sizeof(tur), 0, 0, &ocs, &st);
		if (resp_at)
			break;
	}
	p = put_rc(b, rc, ocs, st);
	if (resp_at) {
		dword_gran = resp_at == RESP_FIELD * 4;
		p = put_dec(put(p, " resp @"), resp_at);
		put(p, dword_gran ? " = dwords (standard)" : " = bytes (Exynos)");
	} else {
		put(p, " no response UPIU");
	}
	con_str("TUR", b);
	if (!resp_at)
		return 0;

	/*
	 * READ CAPACITY(10), 8 bytes. PRDT length: 1 entry (standard), or in
	 * bytes: 16 (plain entry) / 128 (Samsung FMP-extended entry).
	 */
	static const uint8_t cap10[10] = { 0x25 };
	static const uint32_t tries_b[2] = { 16, 128 };
	int ntries = dword_gran ? 1 : 2;
	for (int i = 0; i < ntries; i++) {
		prdt_len = dword_gran ? 1 : tries_b[i];
		zero(data, 8);
		rc = scsi(cap10, sizeof(cap10), 8, prdt_len, &ocs, &st);
		p = put_rc(b, rc, ocs, st);
		p = put_dec(put(p, " prdt "), prdt_len);
		p = put_hex(put(p, " last "), be32(data), 8);
		put_dec(put(p, " blk "), be32(data + 4));
		con_str(i ? "" : "CAPACITY", b);
		if (!rc && be32(data + 4) == BLOCK)
			break;
	}
	if (rc || be32(data + 4) != BLOCK)
		return 0;
	p = put_dec(b, (uint32_t)(((uint64_t)be32(data) + 1) >> 8));
	put(p, " MiB on LUN 0");
	con_str("", b);

	/* LBA 0: protective MBR; LBA 1: GPT header. */
	rc = read_block(0, &ocs, &st);
	p = put_rc(b, rc, ocs, st);
	put_hex(put(p, " MBR sig "), data[510] | data[511] << 8, 4);
	con_str("READ 0", b);
	if (rc)
		return 0;
	rc = read_block(1, &ocs, &st);
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = data[i] >= 32 && data[i] < 127 ? data[i] : '.';
	sig[8] = 0;
	p = put_rc(b, rc, ocs, st);
	put(put(p, " "), sig);
	con_str("READ 1", b);
	if (rc || !name_is(sig, "EFI PART"))
		return 0;
	uint32_t nent = data[80] | data[81] << 8, esz = data[84] | data[85] << 8;
	uint32_t elba = data[72] | data[73] << 8;
	if (esz != 128 || nent > 128)
		return 0;
	uint32_t boot_lba = 0;
	list_partitions(elba, nent, &boot_lba);

	/* End to end: BOOT must start with the image we flashed (this shim). */
	if (!boot_lba)
		return 0;
	rc = read_block(boot_lba, &ocs, &st);
	uint32_t ksize = data[8] | data[9] << 8 | data[10] << 16 | (uint32_t)data[11] << 24;
	int ours = !rc && data[0] == 'A' && data[1] == 'N' && data[2] == 'D' && data[7] == '!' &&
		   ksize == (uint32_t)(xp.image_end - xp.image);
	p = put_rc(b, rc, ocs, st);
	p = put_dec(put(p, " kernel "), ksize);
	put(p, ours ? " = this shim" : " ?");
	con_str("BOOT", b);
	return ours;
}

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
	char b[48], *p;

	wsr("tpidr_el1", 0);
	xp_init(dtb);
	if (!xp.fb_ok)
		xp_reset();
	con_clear(GREY);
	con_title("EXYNOS X3.1 RUNS");
	con_str("MODEL", xp.model[0] ? xp.model : "(no DTB)");
	p = put_hex(put(b, "IMG "), xp.image, 9);
	put_dec(put(p, " size "), (uint32_t)(xp.image_end - xp.image));
	con_str("SHIM", b);
	con_skip();

	int ok = probe();

	int y = con_row();
	con_set_row(24);
	con_title(ok ? "EXYNOS X3.1: OK  " : "EXYNOS X3.1: STOP");
	con_set_row(y);
	con_skip();
	int tick_y = con_row();
	uint64_t t0 = xp_now();
	for (unsigned s = 0; s <= RUN_SECONDS; s++) {
		while (xp_now() - t0 < xp.cntfrq * s)
			;
		p = put(put_dec(b, s), "s  reboot in ");
		put(put_dec(p, RUN_SECONDS - s), "s ");
		con_set_row(tick_y);
		con_str("TIME", b);
		con_str("", "Take a photo of this screen.");
	}
	xp_reset();
}
