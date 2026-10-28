/*
 * Galaxy S9 Exynos milestone X5: full UFS host re-init, then re-run the X3
 * read-only probe. CPU0 only.
 *
 * X3.9 found the data path broken from S-Boot's hand-off: every data-in DMA
 * ends in IS.SBFES (system bus fatal error) even though the host's response
 * UPIU DMA into our descriptor buffer works. S-Boot powers the device down
 * before jumping to us and leaves the host clocks gated. This stage does what
 * Linux's exynos driver does from scratch -- the UFSP/SMU firmware setup, the
 * host software reset + host init, a device reset_n toggle, the HCE enable,
 * the full UniPro link startup with the Samsung calibration tables, and the
 * fDeviceInit handshake -- then tries to read the GPT again.
 *
 * Reference: LineageOS android_kernel_samsung_universal9810 (lineage-17.1),
 * drivers/scsi/ufs/{ufs-exynos.c,ufs-cal-9810.c,ufshcd.c}. The host is
 * UFSHCI 2.1 (UFSHCD_QUIRK_PRDT_BYTE_GRAN: response/PRDT offsets are bytes),
 * link-startup result is read from UniPro 0x7854 (GET_GENERRCODE_DIRECT),
 * the device is reset by HCI_GPIO_OUT 0 then 1, and on UFS 2.x every UTRD
 * carries command type 1 (UFS storage). No SCSI command here writes.
 * MMU off: all accesses are Device, DMA needs no cache maintenance.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "xplat.h"

#define UFS_HCI		0x11120000UL	/* standard HCI */
#define UFS_VS		0x11121100UL	/* vendor HCI */
#define UFS_UNIPRO	0x11110000UL	/* UniPro (DME result 0x7854 lives here) */
#define UFS_UFSP	0x11130000UL	/* UFS protector / SMU */
#define UFS_PMA		0x11124000UL	/* M-PHY PMA (+0x140 per lane) */
#define PHY_PMU		0x14060724UL	/* phy-sys: isolation bypass bit 0 */
#define SYSREG_COH	0x11010700UL	/* ufs-io-coherency, mask/bits 0x300 */

/* standard UFSHCI */
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
#define REG_UTMRLBA	0x70
#define REG_UTMRLBAU	0x74
#define REG_UTMRLRSR	0x80
#define REG_UIC_CMD	0x90
#define REG_UIC_ARG1	0x94
#define REG_UIC_ARG2	0x98
#define REG_UIC_ARG3	0x9c

/* vendor HCI (ufs-exynos.h) */
#define VS_TXPRDT	0x00
#define VS_RXPRDT	0x04
#define VS_1US_TO_CNT	0x0c
#define VS_IS		0x38
#define VS_NEXUS	0x40
#define VS_TMNEXUS	0x44
#define VS_SW_RST	0x50
#define VS_REORDER	0x60
#define VS_BURST	0x6c
#define VS_GPIO_OUT	0x70
#define VS_V2P1_CTRL	0x8c
#define VS_CLKSTOP	0xb0
#define VS_FORCE_HCS	0xb4
#define VS_FSM		0xc0
#define VS_DMA0		0xc8
#define VS_DMA1		0xd0
#define VS_UFS_ACG	0xfc
#define VS_IOP_ACG	0x100
#define VS_SMU_ABORT	0x10c

/* HCS bits */
#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UTMRLRDY	(1u << 2)
#define HCS_UCRDY	(1u << 3)

/* UFSP protector registers (ufs-exynos.h) */
#define UFSPRCTRL	0x000
#define UFSPRSTAT	0x008
#define UFSPRSECURITY	0x010
#define UFSPVERSION	0x01c
#define UFSPWCTRL	0x100
#define UFSPWSTAT	0x108
#define UFSPWSECURITY	0x110

/* include/linux/smc.h, include/crypto/smu.h (as in X3) */
#define SMC_CMD_FMP_SECURITY	0xc2001810UL
#define SMC_CMD_SMU		0xc2001850UL
#define SMU_EMBEDDED		0
#define SMU_INIT		0
#define CFG_DESCTYPE_3		3	/* 128-byte FMP PRDT entries */

#define BLOCK		4096u
#define RESP_FIELD	1024u	/* Exynos quirk: UTRD response offset in bytes */
#define PRDT_FIELD	2048u	/* UTRD PRDT offset in bytes */
#define DATA_OFF	8192u	/* data area in the last 8 KB of ucd */

#define RUN_SECONDS	60

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

/* UIC command opcodes (ufshci.h) */
#define UIC_CMD_DME_GET		0x01
#define UIC_CMD_DME_SET		0x02
#define UIC_CMD_DME_PEER_GET	0x03
#define UIC_CMD_DME_PEER_SET	0x04
#define UIC_CMD_DME_LINK_STARTUP 0x16
#define UIC_COMMAND_COMPL	(1u << 10)	/* REG_IS bit 10 */

/* UniPro DME result register for link startup (GET_GENERRCODE_DIRECT) */
#define UNIP_LINKSTARTUP_RESULT	0x7854

/* UniPro attribute ids (unipro.h) */
#define PA_AVAILTXDATALANES	0x1520
#define PA_AVAILRXDATALANES	0x1540
#define PA_HIBERN8TIME		0x15a7
#define PA_TACTIVATE		0x15a8
#define RX_MIN_ACTIVATETIME_CAP	0x008f

/* PA_DBG_CLK_PERIOD: S-Boot stores the UniPro MCLK period (ns) here. */
#define PA_DBG_CLK_PERIOD	0x9514
#define MCLK_FALLBACK_HZ	166500000UL

#define UIC_ARG_MIB(attr)		(((uint32_t)((attr) & 0xffff) << 16))
#define UIC_ARG_MIB_SEL(attr, sel)	(UIC_ARG_MIB(attr) | ((sel) & 0xffff))

#define TX_LANE_0	0
#define RX_LANE_0	4

/* UTRD/UPIU */
#define UTP_UFS_STORAGE		(1u << 28)	/* command type 1, UFS 2.x */
#define UTP_INTR		(1u << 24)	/* I: interrupt on completion */

/* layer ids for the cal tables (ufs-cal-9810.c) */
enum {
	L_PCS_COMN = 1,
	L_PCS_RX,
	L_PCS_TX,
	L_PCS_RX_PRD,
	L_PCS_TX_PRD,
	L_UNIPRO_STD,
	L_UNIPRO_DBG,
	L_UNIPRO_DBG_PRD,
	L_PMA_COMN,
	L_PMA_TRSV,
	L_COMMON_WAIT,
};

struct calcfg { uint16_t addr; uint32_t val; uint8_t lyr; };

/* init_cfg (ufs-cal-9810.c lines 135-169); val 0 means "MCLK period" */
static const struct calcfg init_cfg[] = {
	{ 0x9514, 0x00000000, L_UNIPRO_DBG_PRD },
	{ 0x0200, 0x40,       L_PCS_COMN },
	{ 0x0012, 0x00,       L_PCS_RX_PRD },
	{ 0x00aa, 0x00,       L_PCS_TX_PRD },
	{ 0x005c, 0x38,       L_PCS_RX },
	{ 0x000f, 0x00,       L_PCS_RX },
	{ 0x0065, 0x01,       L_PCS_RX },
	{ 0x0069, 0x01,       L_PCS_RX },
	{ 0x0021, 0x00,       L_PCS_RX },
	{ 0x0022, 0x00,       L_PCS_RX },
	{ 0x0084, 0x01,       L_PCS_RX },
	{ 0x0004, 0x01,       L_PCS_TX },
	{ 0x008f, 0x3e,       L_PCS_TX },
	{ 0x0200, 0x00,       L_PCS_COMN },
	{ 0x9536, 0x4e20,     L_UNIPRO_DBG },
	{ 0x9564, 0x2e820183, L_UNIPRO_DBG },
	{ 0x155e, 0x00,       L_UNIPRO_STD },
	{ 0x3000, 0x00,       L_UNIPRO_STD },
	{ 0x3001, 0x01,       L_UNIPRO_STD },
	{ 0x4021, 0x01,       L_UNIPRO_STD },
	{ 0x4020, 0x01,       L_UNIPRO_STD },
	{ 0x008c, 0x80,       L_PMA_COMN },
	{ 0x0074, 0x10,       L_PMA_COMN },
	{ 0x0110, 0xb5,       L_PMA_TRSV },
	{ 0x0134, 0x43,       L_PMA_TRSV },
	{ 0x016c, 0x20,       L_PMA_TRSV },
	{ 0x0178, 0xc0,       L_PMA_TRSV },
	{ 0x00e0, 0x12,       L_PMA_TRSV },
	{ 0x0164, 0x58,       L_PMA_TRSV },
	{ 0x01b0, 0x18,       L_PMA_TRSV },
	{ 0x008c, 0xc0,       L_PMA_COMN },
	{ 0x008c, 0x00,       L_PMA_COMN },
	{ 0x0000, 0xc8,       L_COMMON_WAIT },
};
#define N_INIT_CFG	(sizeof(init_cfg) / sizeof(init_cfg[0]))

/* post_init_cfg (ufs-cal-9810.c lines 172-183) */
static const struct calcfg post_init_cfg[] = {
	{ 0x9529, 0x01, L_UNIPRO_DBG },
	{ 0x15a4, 0xfa, L_UNIPRO_STD },
	{ 0x9529, 0x00, L_UNIPRO_DBG },
	{ 0x0200, 0x40, L_PCS_COMN },
	{ 0x0035, 0x05, L_PCS_RX },
	{ 0x0073, 0x01, L_PCS_RX },
	{ 0x0041, 0x02, L_PCS_RX },
	{ 0x0042, 0xac, L_PCS_RX },
	{ 0x0200, 0x00, L_PCS_COMN },
};
#define N_POST_INIT_CFG (sizeof(post_init_cfg) / sizeof(post_init_cfg[0]))

static inline uint32_t hci(uint32_t o) { return rd32(UFS_HCI + o); }
static inline void hci_wr(uint32_t o, uint32_t v) { wr32(UFS_HCI + o, v); }

struct utrd {
	uint32_t dw0, dw1, dw2, dw3, ucdba, ucdbau;
	uint32_t resp;		/* [31:16] response length, [15:0] offset */
	uint32_t prdt;		/* [31:16] PRDT length, [15:0] offset */
};

static volatile struct utrd utrl[32] __attribute__((aligned(4096)));
static volatile uint8_t utmrl[2048] __attribute__((aligned(4096)));
static volatile uint8_t ucd[16384] __attribute__((aligned(4096)));
static volatile uint8_t *data;
static volatile int in_exc;

static int dword_gran = -1;	/* 0: offsets in bytes (Exynos quirk), 1: standard */
static uint32_t prdt_len;
static uint32_t resp_at;
static int slot;
static int last_slot;
static uint32_t resid;
static uint32_t last_is;
static uint8_t sense[3];
static uint8_t tag;

static uint32_t uic_res, uic_get;	/* last UIC result code and arg3 value */
static uint32_t mclk_rate;		/* discovered UniPro MCLK */
static uint32_t num_lanes;		/* discovered data lanes */

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

static void be32_put(volatile uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void prdt_entry(uint32_t off, uintptr_t buf, uint32_t len)
{
	volatile uint32_t *e = (volatile uint32_t *)(ucd + off);
	e[0] = (uint32_t)buf;
	e[1] = (uint32_t)(buf >> 32);
	e[2] = 0;
	e[3] = len - 1;		/* data byte count - 1 */
}

static void udelay(unsigned us)
{
	uint64_t end = xp_now() + xp.cntfrq / 1000000 * us;
	while (xp_now() < end)
		;
}

/* ---- UIC / DME ---- */

/*
 * Issue one UIC command and wait for UIC_COMMAND_COMPL. Returns 0 on
 * completion (result code in uic_res, arg3 in uic_get), -1 if the controller
 * is not ready, -2 on timeout.
 */
static int uic(uint32_t opcode, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
	uint64_t end;

	if (!(hci(REG_HCS) & HCS_UCRDY))
		return -1;
	hci_wr(REG_IS, hci(REG_IS));	/* W1C, also clears UIC_COMMAND_COMPL */
	hci_wr(REG_UIC_ARG1, arg1);
	hci_wr(REG_UIC_ARG2, arg2);
	hci_wr(REG_UIC_ARG3, arg3);
	dsb();
	udelay(1000);			/* ~1 ms before the DME (cal safety) */
	hci_wr(REG_UIC_CMD, opcode & 0xff);
	end = xp_now() + xp.cntfrq;	/* 1 s */
	while (!(hci(REG_IS) & UIC_COMMAND_COMPL))
		if (xp_now() > end)
			return -2;
	hci_wr(REG_IS, UIC_COMMAND_COMPL);
	uic_res = hci(REG_UIC_ARG2) & 0xff;
	uic_get = hci(REG_UIC_ARG3);
	return 0;
}

static int dme_set(uint32_t attr_sel, uint32_t val)
{
	if (uic(UIC_CMD_DME_SET, attr_sel, 0, val))
		return -1;
	return (int)uic_res;
}

static int dme_get(uint32_t attr_sel, uint32_t *val)
{
	if (uic(UIC_CMD_DME_GET, attr_sel, 0, 0))
		return -1;
	*val = uic_get;
	return (int)uic_res;
}

static int dme_peer_set(uint32_t attr_sel, uint32_t val)
{
	if (uic(UIC_CMD_DME_PEER_SET, attr_sel, 0, val))
		return -1;
	return (int)uic_res;
}

/* result read from UniPro 0x7854, as Linux does with GET_GENERRCODE_DIRECT */
static int dme_link_startup(uint32_t *gen_err)
{
	if (uic(UIC_CMD_DME_LINK_STARTUP, 0, 0, 0))
		return -1;
	*gen_err = rd32(UFS_UNIPRO + UNIP_LINKSTARTUP_RESULT);
	return 0;
}

/* M-PHY PMA write, gating MPHY_APBCLK_STOP around it (phy_pma_writel) */
static void pma_write(uint32_t val, uint32_t reg)
{
	uint32_t c = rd32(UFS_VS + VS_CLKSTOP);
	wr32(UFS_VS + VS_CLKSTOP, c & ~(1u << 3));
	wr32(UFS_PMA + reg, val);
	wr32(UFS_VS + VS_CLKSTOP, c | (1u << 3));
}

static int apply_cfg(const struct calcfg *cfg, unsigned n, uint32_t mclk_period)
{
	for (unsigned j = 0; j < n; j++) {
		uint32_t a = cfg[j].addr, v = cfg[j].val;
		for (uint32_t i = 0; i < num_lanes; i++) {
			switch (cfg[j].lyr) {
			case L_PCS_COMN:
			case L_UNIPRO_STD:
			case L_UNIPRO_DBG:
				if (i == 0 && dme_set(UIC_ARG_MIB(a), v))
					return 1;
				break;
			case L_UNIPRO_DBG_PRD:
				if (i == 0 && dme_set(UIC_ARG_MIB(a), mclk_period))
					return 2;
				break;
			case L_PCS_RX:
				if (dme_set(UIC_ARG_MIB_SEL(a, RX_LANE_0 + i), v))
					return 3;
				break;
			case L_PCS_TX:
				if (dme_set(UIC_ARG_MIB_SEL(a, TX_LANE_0 + i), v))
					return 4;
				break;
			case L_PCS_RX_PRD:
				if (dme_set(UIC_ARG_MIB_SEL(a, RX_LANE_0 + i), mclk_period))
					return 5;
				break;
			case L_PCS_TX_PRD:
				if (dme_set(UIC_ARG_MIB_SEL(a, TX_LANE_0 + i), mclk_period))
					return 6;
				break;
			case L_PMA_COMN:
				if (i == 0)
					pma_write(v, a);
				break;
			case L_PMA_TRSV:
				pma_write(v, a + 0x140 * i);
				break;
			case L_COMMON_WAIT:
				if (i == 0)
					udelay(v);
				break;
			}
		}
	}
	return 0;
}

/* ufs_cal_calib_hibern8_values */
static int calib_hibern8(void)
{
	uint32_t hw_min_tact, peer_tact, h8_time;

	if (dme_get(UIC_ARG_MIB_SEL(RX_MIN_ACTIVATETIME_CAP, RX_LANE_0), &hw_min_tact))
		return -1;
	if (dme_get(UIC_ARG_MIB(PA_TACTIVATE), &peer_tact))
		return -2;
	if (dme_get(UIC_ARG_MIB(PA_HIBERN8TIME), &h8_time))
		return -3;
	if (peer_tact >= hw_min_tact)
		if (dme_peer_set(UIC_ARG_MIB(PA_TACTIVATE), peer_tact + 1))
			return -4;
	if (dme_set(UIC_ARG_MIB(PA_HIBERN8TIME), h8_time + 1))
		return -5;
	return 0;
}

/* ---- transfer request machinery (from X3, proven on the G960F) ---- */

static uint32_t scsi_lun(uint8_t lun, unsigned timeout_ms, const uint8_t *cdb, unsigned cdb_len,
			 uint32_t len, uint32_t prdt_field_len, uint32_t *ocs, uint32_t *st)
{
	int k = 0;
	while (k < 16 && (hci(REG_UTRLDBR) & 1u << ((slot + k) & 15)))
		k++;
	if (k == 16)
		return 5;
	last_slot = (slot + k) & 15;
	slot = (last_slot + 1) & 15;
	volatile struct utrd *u = &utrl[last_slot];
	uint32_t bit = 1u << last_slot;

	zero((volatile uint8_t *)u, sizeof(*u));
	zero(ucd, DATA_OFF);
	ucd[0] = 0x01;			/* COMMAND UPIU */
	ucd[1] = len ? 0x40 : 0x00;	/* R: data in */
	ucd[2] = lun;
	ucd[3] = ++tag;
	ucd[12] = len >> 24;
	ucd[13] = len >> 16;
	ucd[14] = len >> 8;
	ucd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd[16 + i] = cdb[i];
	if (len)
		prdt_entry(PRDT_FIELD, (uintptr_t)data, len);

	u->dw0 = 1u << 28 | (len ? 2u : 0u) << 25 | 1u << 24;
	u->dw2 = 0x0f;
	u->ucdba = (uint32_t)(uintptr_t)ucd;
	u->ucdbau = (uint32_t)((uintptr_t)ucd >> 32);
	u->resp = RESP_FIELD << 16 | RESP_FIELD;
	u->prdt = (len ? prdt_field_len : 0) << 16 | PRDT_FIELD;
	dsb();

	hci_wr(REG_IS, hci(REG_IS));
	wr32(UFS_VS + VS_NEXUS, rd32(UFS_VS + VS_NEXUS) | bit);	/* SCSI: NEXUS bit set */
	hci_wr(REG_UTRLDBR, bit);
	uint64_t end = xp_now() + xp.cntfrq / 1000 * timeout_ms;
	while (hci(REG_UTRLDBR) & bit)
		if (xp_now() > end) {
			last_is = hci(REG_IS);
			hci_wr(REG_UTRLCLR, ~bit);
			hci_wr(REG_UTRLCLR, bit);
			xp_delay_ms(10);
			*ocs = u->dw2 & 0xff;
			*st = 0xff;
			return 1;
		}
	dsb();
	last_is = hci(REG_IS);
	*ocs = u->dw2 & 0xff;
	resp_at = ucd[RESP_FIELD] == 0x21 ? RESP_FIELD : ucd[RESP_FIELD * 4] == 0x21 ? RESP_FIELD * 4 : 0;
	*st = resp_at ? ucd[resp_at + 7] : 0xff;
	resid = resp_at ? be32(ucd + resp_at + 12) : 0;
	if (resp_at && *st == 2) {
		sense[0] = ucd[resp_at + 32 + 2 + 2] & 0xf;
		sense[1] = ucd[resp_at + 32 + 2 + 12];
		sense[2] = ucd[resp_at + 32 + 2 + 13];
	}
	int ocs_quirk = *ocs == 7 && resp_at && (ucd[resp_at + 6] || *st);
	if (*ocs && !ocs_quirk)
		return 2;
	if (!resp_at)
		return 3;
	return *st ? 4 : 0;
}

static uint32_t scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len, uint32_t prdt_field_len,
		     uint32_t *ocs, uint32_t *st)
{
	return scsi_lun(0, 1000, cdb, cdb_len, len, prdt_field_len, ocs, st);
}

/*
 * One device-management command (NOP OUT / query): no data phase, command
 * type 1 (UFS 2.x). Linux's exynos_ufs_set_nexus_t_xfer_req CLEARS the slot's
 * NEXUS bit for these (cmd == NULL), unlike SCSI.
 */
static uint32_t devman(unsigned timeout_ms, uint8_t resp_type, uint32_t *ocs, uint32_t *st)
{
	int k = 0;
	while (k < 16 && (hci(REG_UTRLDBR) & 1u << ((slot + k) & 15)))
		k++;
	if (k == 16)
		return 5;
	last_slot = (slot + k) & 15;
	slot = (last_slot + 1) & 15;
	volatile struct utrd *u = &utrl[last_slot];
	uint32_t bit = 1u << last_slot;

	zero((volatile uint8_t *)u, sizeof(*u));
	u->dw0 = UTP_UFS_STORAGE | UTP_INTR;
	u->dw2 = 0x0f;
	u->ucdba = (uint32_t)(uintptr_t)ucd;
	u->ucdbau = (uint32_t)((uintptr_t)ucd >> 32);
	u->resp = RESP_FIELD << 16 | RESP_FIELD;
	u->prdt = 0;
	dsb();

	hci_wr(REG_IS, hci(REG_IS));
	wr32(UFS_VS + VS_NEXUS, rd32(UFS_VS + VS_NEXUS) & ~bit);	/* dev mgmt: NEXUS clear */
	hci_wr(REG_UTRLDBR, bit);
	uint64_t end = xp_now() + xp.cntfrq / 1000 * timeout_ms;
	while (hci(REG_UTRLDBR) & bit)
		if (xp_now() > end) {
			last_is = hci(REG_IS);
			hci_wr(REG_UTRLCLR, ~bit);
			hci_wr(REG_UTRLCLR, bit);
			xp_delay_ms(10);
			*ocs = u->dw2 & 0xff;
			*st = 0xff;
			return 1;
		}
	dsb();
	last_is = hci(REG_IS);
	*ocs = u->dw2 & 0xff;
	resp_at = ucd[RESP_FIELD] == resp_type ? RESP_FIELD :
		  ucd[RESP_FIELD * 4] == resp_type ? RESP_FIELD * 4 : 0;
	*st = resp_at ? ucd[resp_at + 6] : 0xff;	/* response field (query) */
	if (*ocs && !(*ocs == 7 && resp_at))
		return 2;
	if (!resp_at)
		return 3;
	return 0;
}

/* NOP OUT: success = NOP IN (0x20) comes back. */
static uint32_t nop_out(uint32_t *ocs, uint32_t *st)
{
	zero(ucd, DATA_OFF);
	ucd[0] = 0x00;		/* NOP OUT */
	ucd[3] = ++tag;
	return devman(30, 0x20, ocs, st);
}

/*
 * fDeviceInit flag query. opcode 0x06 = SET_FLAG (write func, value 1),
 * 0x05 = READ_FLAG (read func). Query function goes in header byte 5 (Linux
 * UPIU_HEADER_DWORD(0, query_func, 0, 0)); with it in byte 6 the device
 * answered 0xFE (invalid opcode). Response code at resp+6, flag value is bit 0
 * of the BE32 value at resp+20..23.
 */
static uint32_t query_flag(uint8_t opcode, unsigned timeout_ms, uint32_t *ocs, uint32_t *st, uint32_t *flag)
{
	int write = opcode == 0x06;

	zero(ucd, DATA_OFF);
	ucd[0] = 0x16;			/* QUERY REQ */
	ucd[3] = ++tag;
	ucd[5] = write ? 0x81 : 0x01;	/* query func: write / read (byte 6 = response) */
	ucd[12] = opcode;
	ucd[13] = 0x01;			/* fDeviceInit */
	ucd[14] = 0;			/* index */
	ucd[15] = 0;			/* selector */
	if (write)
		be32_put(ucd + 20, 1);
	uint32_t rc = devman(timeout_ms, 0x36, ocs, st);
	if (rc)
		return rc;
	*st = resp_at ? ucd[resp_at + 6] : 0xff;	/* query response code */
	if (flag && resp_at)
		*flag = ucd[resp_at + 23] & 1;
	return resp_at && ucd[resp_at + 6] == 0 ? 0 : 4;
}

static uint32_t read_block(uint32_t lba, uint32_t *ocs, uint32_t *st)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, 1, 0 };

	zero(data, BLOCK);
	return scsi(cdb, sizeof(cdb), BLOCK, prdt_len, ocs, st);
}

static char *put_rc(char *p, uint32_t rc, uint32_t ocs, uint32_t st)
{
	p = put_dec(put(p, "rc "), rc);
	p = put_hex(put(p, " ocs "), ocs, 2);
	p = put_hex(put(p, " st "), st, 2);
	if (rc == 1) {
		p = put_hex(put(p, " is "), last_is, 8);
		p = put_hex(put(p, " dbr "), hci(REG_UTRLDBR), 4);
	} else if (rc == 2) {
		p = put_hex(put(p, " is "), last_is, 8);
	} else if (st == 2) {
		p = put_hex(put(p, " sense "), sense[0], 1);
		p = put_hex(put(p, "/"), sense[1], 2);
		p = put_hex(put(p, "/"), sense[2], 2);
	}
	return p;
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

static void vs_dump(const char *label)
{
	static const uint16_t regs[12] = {
		0x00, 0x04, 0x0c, 0x50, 0x60, 0x6c, 0x70, 0xb0, 0xb4, 0xfc, 0x100, 0x40
	};
	char b[128], *p = b;

	for (int i = 0; i < 12; i++) {
		if (i == 6) {
			con_str(label, b);
			p = b;
		}
		p = put_hex(put(put_hex(p, regs[i], 3), "="), rd32(UFS_VS + regs[i]), 8);
		*p++ = ' ';
		*p = 0;
	}
	con_str("", b);
}

static void ufsp_dump(void)
{
	static const uint16_t regs[7] = {
		UFSPRCTRL, UFSPRSTAT, UFSPRSECURITY, UFSPVERSION, UFSPWCTRL, UFSPWSTAT, UFSPWSECURITY
	};
	char b[128], *p = b;

	for (int i = 0; i < 7; i++) {
		if (i == 4) {
			con_str("UFSP", b);
			p = b;
		}
		p = put_hex(put(put_hex(p, regs[i], 3), "="), rd32(UFS_UFSP + regs[i]), 8);
		*p++ = ' ';
		*p = 0;
	}
	con_str("", b);
}

/* ---- the re-init ---- */

static int reinit(void)
{
	char b[128], *p;
	uint32_t r, get, gen_err;

	con_str("REINIT", "UFSP protector dump + host SW reset + link startup");
	ufsp_dump();

	/* PHY isolation bypass + io-coherency (exynos_ufs_init_system) */
	wr32(PHY_PMU, rd32(PHY_PMU) | 1u);
	wr32(SYSREG_COH, (rd32(SYSREG_COH) & ~0x300u) | 0x300u);
	p = put_hex(put(b, "phy-pmu "), rd32(PHY_PMU), 8);
	put_hex(put(p, " coh "), rd32(SYSREG_COH), 8);
	con_str("SYS", b);

	/* FMPSECURITY & SMU (exynos_ufs_init) */
	uint64_t r_sec = xp_psci(SMC_CMD_FMP_SECURITY, 0, SMU_EMBEDDED, CFG_DESCTYPE_3);
	uint64_t r_smu = xp_psci(SMC_CMD_SMU, SMU_INIT, SMU_EMBEDDED, 0);
	p = put_hex(put(b, "FMPSECURITY "), r_sec, 8);
	put_hex(put(p, " SMU_INIT "), r_smu, 8);
	con_str("FIRMWARE", b);

	vs_dump("VS BEFORE");

	/* host software reset (exynos_ufs_host_reset) */
	wr32(UFS_VS + VS_FORCE_HCS, rd32(UFS_VS + VS_FORCE_HCS) & ~(1u << 4));	/* auto HCI clk off */
	wr32(UFS_VS + VS_SW_RST, 0x3);		/* UFS_SW_RST_MASK: link | unipro */
	uint64_t end = xp_now() + xp.cntfrq / 1000;	/* 1 ms */
	while ((rd32(UFS_VS + VS_SW_RST) & 0x3) && xp_now() < end)
		;
	p = put_hex(put(b, "sw_rst "), rd32(UFS_VS + VS_SW_RST), 2);
	con_str("HOST RESET", b);
	if (rd32(UFS_VS + VS_SW_RST) & 0x3)
		return -1;

	/* init_host: host-side vendor registers (mclk-dependent ones deferred) */
	wr32(UFS_VS + VS_REORDER, 0xa);
	wr32(UFS_VS + VS_TXPRDT, 0x8000000c);	/* PRDT_PREFECT_EN | PRDT_SET_SIZE(12) */
	wr32(UFS_VS + VS_RXPRDT, 0x0c);
	wr32(UFS_VS + VS_NEXUS, 0xffffffff);
	wr32(UFS_VS + VS_TMNEXUS, 0xffffffff);
	wr32(UFS_VS + VS_BURST, 0x98000003);	/* WLU_EN | BURST_LEN(3) */
	wr32(UFS_VS + VS_IOP_ACG, rd32(UFS_VS + VS_IOP_ACG) & ~1u);
	wr32(UFS_VS + VS_UFS_ACG, rd32(UFS_VS + VS_UFS_ACG) & ~1u);
	dsb();

	/* device reset_n toggle (exynos_ufs_dev_hw_reset) */
	wr32(UFS_VS + VS_GPIO_OUT, 0);
	udelay(5);
	wr32(UFS_VS + VS_GPIO_OUT, 1);

	/* host controller enable (__ufshcd_hba_enable) */
	hci_wr(REG_HCE, 0);			/* force the reset state first */
	hci_wr(REG_HCE, 1);
	xp_delay_ms(1);
	end = xp_now() + xp.cntfrq;		/* 1 s */
	while (!(hci(REG_HCE) & 1) && xp_now() < end)
		;
	p = put_hex(put(b, "hce "), hci(REG_HCE), 1);
	put_hex(put(p, " hcs "), hci(REG_HCS), 8);
	con_str("HCE", b);
	if (!(hci(REG_HCE) & 1))
		return -2;

	/* wait for the controller to be ready */
	uint32_t want = HCS_DP | HCS_UTRLRDY | HCS_UTMRLRDY | HCS_UCRDY;
	end = xp_now() + xp.cntfrq;		/* 1 s */
	uint32_t hcs = 0;
	while ((hcs = hci(REG_HCS)) && (hcs & want) != want && xp_now() < end)
		;
	p = put_hex(put(b, "hcs "), hcs, 8);
	con_str("READY", b);

	/* discover the UniPro MCLK period S-Boot left in PA_DBG_CLK_PERIOD */
	mclk_rate = MCLK_FALLBACK_HZ;
	if (!dme_get(UIC_ARG_MIB(PA_DBG_CLK_PERIOD), &get) && get)
		mclk_rate = 1000000000UL / get;
	p = put_dec(put(b, "mclk "), mclk_rate);
	put_dec(put(p, " Hz  period "), mclk_rate ? 1000000000UL / mclk_rate : 0);
	con_str("MCLK", b);

	/* fit_aggr_timeout: 1us-to-cnt, IA tick sel (interrupt aggregation only) */
	wr32(UFS_VS + VS_V2P1_CTRL, rd32(UFS_VS + VS_V2P1_CTRL) | (1u << 16));
	wr32(UFS_VS + VS_1US_TO_CNT, (mclk_rate / 1000000) & 0x3ff);

	/* available lanes (link_startup_notify PRE) */
	num_lanes = 2;
	uint32_t rxl = 0, txl = 0;
	if (!dme_get(UIC_ARG_MIB(PA_AVAILRXDATALANES), &rxl) && rxl)
		num_lanes = rxl;
	if (!dme_get(UIC_ARG_MIB(PA_AVAILTXDATALANES), &txl) && txl && txl < num_lanes)
		num_lanes = txl;
	p = put_dec(put(b, "rx "), rxl);
	put_dec(put(p, " tx "), txl);
	put_dec(put(p, " lanes "), num_lanes);
	con_str("LANES", b);

	/* pre-link: init_cfg */
	uint32_t mclk_period = 1000000000UL / mclk_rate;
	r = apply_cfg(init_cfg, N_INIT_CFG, mclk_period);
	p = put_dec(put(b, "init_cfg rc "), r);
	put_hex(put(p, " period "), mclk_period, 4);
	con_str("PRE LINK", b);
	if (r)
		return -3;

	/* link startup */
	if (dme_link_startup(&gen_err)) {
		con_str("LINK", "DME_LINK_STARTUP timed out");
		return -4;
	}
	p = put_hex(put(b, "gen_err 0x"), gen_err, 8);
	put_hex(put(p, " hcs "), hci(REG_HCS), 8);
	con_str("LINK STARTUP", b);

	/* post-link: hibern8 calib + post_init_cfg */
	r = calib_hibern8();
	p = put_dec(put(b, "hibern8 rc "), r);
	con_str("POST LINK", b);
	if (r)
		return -5;
	r = apply_cfg(post_init_cfg, N_POST_INIT_CFG, mclk_period);
	p = put_dec(put(b, "post_init_cfg rc "), r);
	con_str("POST LINK", b);
	if (r)
		return -6;

	/* make operational: point the request lists at ours and run-stop */
	hci_wr(REG_UTRLRSR, 0);
	hci_wr(REG_UTMRLRSR, 0);
	hci_wr(REG_UTRLBA, (uint32_t)(uintptr_t)utrl);
	hci_wr(REG_UTRLBAU, (uint32_t)((uintptr_t)utrl >> 32));
	hci_wr(REG_UTMRLBA, (uint32_t)(uintptr_t)utmrl);
	hci_wr(REG_UTMRLBAU, (uint32_t)((uintptr_t)utmrl >> 32));
	dsb();
	hci_wr(REG_UTRLRSR, 1);
	hci_wr(REG_UTMRLRSR, 1);
	p = put_hex(put(b, "utrl "), (uintptr_t)utrl, 9);
	put_hex(put(p, " ucd "), (uintptr_t)ucd, 9);
	con_str("LIST", b);

	/* verify device init: NOP OUT until NOP IN */
	uint32_t ocs = 0, st = 0, rc = 0;
	int nop_ok = 0;
	for (int i = 0; i < 10 && !nop_ok; i++) {
		rc = nop_out(&ocs, &st);
		nop_ok = rc == 0;
		if (rc != 0 && rc != 3)		/* 3 = response present but not NOP IN */
			break;
	}
	p = put_rc(b, rc, ocs, st);
	put(p, nop_ok ? "" : " (no NOP IN)");
	con_str("NOP OUT", b);

	/* fDeviceInit: set flag, poll until cleared */
	uint32_t flag = 1;
	rc = query_flag(0x06, 600, &ocs, &st, &flag);
	p = put_rc(b, rc, ocs, st);
	con_str("FDEV INIT SET", b);
	if (rc) {
		con_str("STOP", "SET_FLAG fDeviceInit failed");
		return -7;
	}
	end = xp_now() + xp.cntfrq;		/* 1 s */
	do {
		rc = query_flag(0x05, 600, &ocs, &st, &flag);
		if (!flag)
			break;
		xp_delay_ms(1);
	} while (flag && xp_now() < end);
	p = put_rc(b, rc, ocs, st);
	put_dec(put(p, " flag "), flag);
	con_str("FDEV INIT", b);

	vs_dump("VS AFTER");
	return flag ? -8 : 0;
}

/* ---- post-reinit read probe (the X3 checks) ---- */

static int probe(void)
{
	char b[128], *p;
	uint32_t ocs = 0, st = 0, rc = 0;

	data = ucd + DATA_OFF;
	slot = 0;
	tag = 0;
	dword_gran = -1;

	uint32_t cap = hci(REG_CAP), hcs = hci(REG_HCS);
	p = put_hex(put(b, "ver "), hci(REG_VER), 8);
	p = put_hex(put(p, " cap "), cap, 8);
	put_dec(put(p, " slots "), (cap & 0x1f) + 1);
	con_str("UFS", b);
	p = put_hex(put(b, "hcs "), hcs, 8);
	put_hex(put(p, " is "), hci(REG_IS), 8);
	con_str("", b);

	/* wake the device S-Boot (and our reset) left powered down (as X3.2) */
	static const uint8_t tur[6] = { 0x00 };
	static const uint8_t ssu_active[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	static const uint8_t ssu_start[6] = { 0x1b, 0, 0, 0, 0x01, 0 };
	int good = 0;
	for (int step = 0; step < 6 && !good; step++) {
		const char *what;
		if (step == 1) {
			rc = scsi_lun(0xd0, 5000, ssu_active, 6, 0, 0, &ocs, &st);
			what = "SSU ACTIVE";
		} else if (step == 3) {
			rc = scsi_lun(0, 5000, ssu_start, 6, 0, 0, &ocs, &st);
			what = "SSU START";
		} else {
			rc = scsi(tur, sizeof(tur), 0, 0, &ocs, &st);
			what = "TUR";
			good = rc == 0;
		}
		p = put_rc(b, rc, ocs, st);
		if (resp_at && dword_gran < 0) {
			dword_gran = resp_at == RESP_FIELD * 4;
			put(p, dword_gran ? " (dwords)" : " (bytes)");
		}
		con_str(what, b);
	}
	if (!good)
		return 0;

	prdt_len = dword_gran ? 1 : 128;

	/* one 4 KB READ(10) of LBA 1 into a 0xA5-filled buffer: the SBFES test */
	for (unsigned i = 0; i < BLOCK; i++)
		data[i] = 0xa5;
	static const uint8_t rd_lba1[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
	rc = scsi(rd_lba1, sizeof(rd_lba1), BLOCK, prdt_len, &ocs, &st);
	unsigned changed = 0, first_ch = BLOCK;
	for (unsigned i = 0; i < BLOCK; i++)
		if (data[i] != 0xa5) {
			changed++;
			if (first_ch == BLOCK)
				first_ch = i;
		}
	p = put_rc(b, rc, ocs, st);
	p = put_dec(put(p, " prdt "), prdt_len);
	put_dec(put(p, " slot "), last_slot);
	put_dec(put(p, " res "), resid);
	con_str("READ 1", b);
	p = put_dec(put(b, "changed "), changed);
	p = put_dec(put(p, " from "), first_ch);
	p = put_hex(put(p, " bytes "), be32(data), 8);
	put_hex(put(p, " "), be32(data + 4), 8);
	con_str("BUFFER", b);
	p = put_hex(put(b, "is "), hci(REG_IS), 8);
	p = put_hex(put(p, " vs_is "), rd32(UFS_VS + VS_IS), 8);
	put_hex(put(p, " smu_abort "), rd32(UFS_VS + VS_SMU_ABORT), 8);
	con_str("AFTER", b);
	if (rc || !(data[0] == 'E' && data[1] == 'F' && data[2] == 'I' && data[3] == ' '))
		return 0;

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

void exc_handler(uint64_t slotx, uint64_t *regs)
{
	char b[48];

	if (in_exc++ || !xp.fb_ok)
		xp_reset();
	con_clear(PANICBG);
	con_title("SHIM PANIC");
	put_dec(put(b, "slot "), slotx);
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
	con_title("EXYNOS X5 RUNS");
	con_str("MODEL", xp.model[0] ? xp.model : "(no DTB)");
	p = put_hex(put(b, "IMG "), xp.image, 9);
	put_dec(put(p, " size "), (uint32_t)(xp.image_end - xp.image));
	con_str("SHIM", b);
	con_skip();

	int rr = reinit();
	p = put_dec(put(b, "reinit rc "), rr);
	con_str("REINIT", b);
	con_skip();

	int ok = 0;
	if (!rr)
		ok = probe();

	int y = con_row();
	con_set_row(24);
	con_title(ok ? "EXYNOS X5: OK  " : "EXYNOS X5: STOP");
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
