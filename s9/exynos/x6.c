/*
 * Galaxy S9 Exynos milestone X6: touchscreen probe. CPU0 only, MMU off.
 *
 * The G960F's touch controller is a Samsung LSI S6SY761 ("sec,sec_ts",
 * firmware tsp_sec/y761_star1.fw) at I2C address 0x48 on the USI-v2 HSI2C
 * at 0x104B0000 (stock rev26 DT; its pinctrl-0 is hsi2c10-bus = GPP1-4/5
 * function 2, no pull, in PERIC0 pinctrl 0x10430000 + 0x20 - X6..X6.2 set
 * GPG3-3/4, another bus's pins), attention IRQ on GPA1-0 (ALIVE 0x14050040,
 * active low), supplies S2MPS18 LDO35 tsp_io 1.8 V and LDO43 tsp_avdd
 * 3.0 V, both regulator-boot-on. This stage finds out what S-Boot leaves
 * on: it reads the HSI2C block (a gated clock would fault: SHIM PANIC with
 * FAR), sets the pins, programs the controller as i2c-exynos5.c does in
 * polling mode, talks to the chip (sec_ts.c: event 0x60, ID 0x52, boot
 * status 0x55, device ID 0x22, SENSE_ON 0x10) and then shows live touch
 * coordinates for 60 s. Writes go only to the HSI2C/pinctrl registers and
 * to the touch chip's command interface.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "xplat.h"

#define RUN_SECONDS	60

#define HSI2C		0x104B0000UL
#define PERIC0_GPP1	0x10430020UL	/* CON +0, DAT +4, PUD +8 */
/*
 * USI-v2: PERIC0 USI port 3 (spi/uart/hsi2c@104B0000) takes its protocol
 * from this SYSREG word (stock DT usi@1041101C usi_v2_mode = "i2c"). Until
 * it says I2C the HSI2C registers ignore writes (X6: CTL/CONF stayed 0).
 */
#define SYSREG_USI03	0x1041101CUL
#define USI_I2C_SW_CONF	(1u << 2)

/*
 * X6.2: with the mode set, writes stick but every transfer stays
 * MASTER_BUSY (TRANS_STATUS 0x000A0003): the USI03 operating clock is not
 * running. CMU_PERIC0 0x10400000 (cmucal-sfr.c / cmucal-node.c, DT clocks
 * USI03 = DIV_CLK_PERIC0_USI03_USI and GATE_USI03 = USI03_USI_QCH):
 * MUX_CLKCMU_PERIC0_IP_USER -> GATE_CLK_PERIC0_USI03_USI -> DIV ->
 * GOUT USI03_USI_IPCLK (+ RSTnSYNC), Q-channel on top. Gates: bit 20
 * MANUAL, 21 CG_VAL, 28 auto gating; QCH: 0 ENABLE, 1 CLOCK_REQ,
 * 2 IGNORE_FORCE_PM_EN.
 */
#define CMU_PERIC0	0x10400000UL
#define CMU_MUX_IP_USER	(CMU_PERIC0 + 0x0120)	/* bit 4 MUX_SEL */
#define CMU_DIV_USI03	(CMU_PERIC0 + 0x1810)	/* bits 0-3 ratio, 16 busy */
#define CMU_GATE_USI03	(CMU_PERIC0 + 0x2018)
#define CMU_GOUT_RSTSYN	(CMU_PERIC0 + 0x206c)
#define CMU_GOUT_IPCLK	(CMU_PERIC0 + 0x20dc)
#define CMU_QCH_USI03	(CMU_PERIC0 + 0x3038)
#define GATE_MANUAL_ON	(1u << 20 | 1u << 21)
#define GATE_AUTO	(1u << 28)
#define PERIC1_GPG3	0x108300C0UL	/* CON +0, DAT +4, PUD +8 */
#define ALIVE_GPA1	0x14050040UL
#define TS_ADDR		0x48

/* i2c-exynos5.c */
#define CTL		0x00
#define FIFO_CTL	0x04
#define TRAILIG_CTL	0x08
#define INT_ENABLE	0x20
#define INT_STATUS	0x24
#define FIFO_STATUS	0x30
#define TX_DATA		0x34
#define RX_DATA		0x38
#define CONF		0x40
#define AUTO_CONF	0x44
#define TIMEOUT		0x48
#define TRANS_STATUS	0x50
#define TIMING_FS1	0x60
#define TIMING_FS2	0x64
#define TIMING_FS3	0x68
#define ADDR		0x70
#define USI_CON		0xC4

#define CTL_MASTER	(1u << 3)
#define CTL_RXCHON	(1u << 6)
#define CTL_TXCHON	(1u << 7)
#define CTL_SW_RST	(1u << 31)
#define CONF_AUTO_MODE	(1u << 31)
#define AUTO_READ	(1u << 16)
#define AUTO_STOP	(1u << 17)
#define AUTO_RUN	(1u << 31)
#define INT_XFER_DONE	(1u << 7)
#define INT_NODEV	(1u << 10)
#define FIFO_RX_EMPTY	(1u << 24)
#define FIFO_TX_EMPTY	(1u << 8)
#define TS_NO_DEV_ACK	(1u << 2)
#define TS_NO_DEV	(1u << 3)

/* sec_ts.h */
#define SEC_TS_CMD_SENSE_ON		0x10
#define SEC_TS_READ_DEVICE_ID		0x22
#define SEC_TS_READ_ID			0x52
#define SEC_TS_READ_BOOT_STATUS		0x55
#define SEC_TS_READ_ONE_EVENT		0x60
#define SEC_TS_CMD_CLEAR_EVENT_STACK	0x62

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

static volatile int in_exc;
static uint32_t last_int, last_trans;

static inline uint32_t hr(uint32_t o) { return rd32(HSI2C + o); }
static inline void hw(uint32_t o, uint32_t v) { wr32(HSI2C + o, v); }

static void udelay(unsigned us)
{
	uint64_t end = xp_now() + xp.cntfrq / 1000000 * us;
	while (xp_now() < end)
		;
}

/*
 * exynos5_i2c_set_timing(FAST) for op_clk, assuming the 200 MHz ipclk the
 * DT asks for (default-clk). If S-Boot left a slower source the bus is just
 * slower, which is fine for a probe.
 */
static void hsi2c_timing(uint32_t ipclk, uint32_t op_clk)
{
	uint32_t div = (ipclk / (op_clk * 15)) & 0xff;
	hw(TIMING_FS3, (hr(TIMING_FS3) & ~0x00ff0000u) | div << 16);
	uint32_t h = (9 * (ipclk / 1000000)) / ((div + 1) * 10);
	if (h > 7)
		h = 7;
	h = (0xffffffffu >> h << h) & 0xff;
	hw(TIMING_FS2, (hr(TIMING_FS2) & ~0xffu) | h);
	uint32_t s = (9 * (ipclk / 1000000)) / ((div + 1) * 10) - 1;
	if (s > 7)
		s = 7;
	s = (0xffffffffu >> s << s) & 0xff;
	hw(TIMING_FS1, (hr(TIMING_FS1) & ~0x00ff0000u) | s << 16);
}

static void hsi2c_reset(void)
{
	hw(USI_CON, 0);				/* exynos_usi_init: release USI reset */
	hw(CTL, hr(CTL) | CTL_SW_RST);
	hw(CTL, hr(CTL) & ~CTL_SW_RST);
	hsi2c_timing(200000000, 100000);	/* 100 kHz: slow and safe */
	hw(CTL, CTL_MASTER);
	hw(TRAILIG_CTL, 0xffffff);
	hw(CONF, hr(CONF) | CONF_AUTO_MODE);
}

/*
 * One polled message as exynos5_i2c_xfer_msg (HSI2C_POLLING) with stop
 * after it (samsung,stop-after-trans). Returns 0 or a nonzero code.
 */
static int hsi2c_msg(uint8_t addr, int rd, uint8_t *buf, unsigned len)
{
	uint32_t ctl = hr(CTL), auto_conf = hr(AUTO_CONF);
	unsigned n = 0;

	hw(TIMEOUT, hr(TIMEOUT) & ~(1u << 31));
	hw(FIFO_CTL, 3u | (len & 0x7f) << 4 | (len & 0x7f) << 16);	/* RX/TX FIFO en, triggers */
	if (rd) {
		ctl = (ctl & ~CTL_TXCHON) | CTL_RXCHON;
		auto_conf |= AUTO_READ;
	} else {
		ctl = (ctl & ~CTL_RXCHON) | CTL_TXCHON;
		auto_conf &= ~AUTO_READ;
	}
	auto_conf |= AUTO_STOP;
	hw(ADDR, (hr(ADDR) & ~((0x3ffu << 10) | 0x3ffu | (0xffu << 24))) | (uint32_t)(addr & 0x7f) << 10);
	hw(CTL, ctl);
	hw(INT_STATUS, hr(INT_STATUS));
	hw(INT_ENABLE, INT_XFER_DONE);
	hw(AUTO_CONF, (auto_conf & ~0xffffu) | len);
	hw(AUTO_CONF, hr(AUTO_CONF) | AUTO_RUN);

	uint64_t end = xp_now() + xp.cntfrq / 20;	/* 50 ms */
	if (rd) {
		while (n < len && xp_now() < end)
			if (!(hr(FIFO_STATUS) & FIFO_RX_EMPTY))
				buf[n++] = (uint8_t)hr(RX_DATA);
	} else {
		while (n < len && xp_now() < end)
			if ((hr(FIFO_STATUS) & 0x7f) < 64)
				hw(TX_DATA, buf[n++]);
	}
	int done = 0;
	while (xp_now() < end) {
		uint32_t is = hr(INT_STATUS);
		if ((is & INT_XFER_DONE) && (rd || (hr(FIFO_STATUS) & FIFO_TX_EMPTY))) {
			done = 1;
			break;
		}
		if (is & INT_NODEV)
			break;
	}
	last_int = hr(INT_STATUS);
	last_trans = hr(TRANS_STATUS);
	hw(INT_STATUS, last_int);
	if (last_trans & (TS_NO_DEV | TS_NO_DEV_ACK) || last_int & INT_NODEV) {
		hsi2c_reset();
		return 3;			/* no ACK: no device / unpowered */
	}
	if (n < len || !done) {
		hsi2c_reset();
		return 2;			/* timeout */
	}
	return 0;
}

/*
 * X6.5: read-only access to the S2MPS18 PMIC over SPEEDY (0x141C0000) through
 * its IP batcher, which takes the hardware semaphore shared with the APM
 * firmware, so these reads never collide with the APM's own PMIC traffic
 * (i2c-speedy.c exynos_speedy_xfer_batcher, read direction, single access).
 * The batcher runs a table: opcode byte = SPEEDY register offset, payload =
 * value; the RX_DATA entry's payload slot receives the byte read.
 * Addresses are 12 bit; assumed (page << 8) | reg with the PMIC page 0x01.
 */
#define SPEEDY			0x141C0000UL
#define SP_CTRL			0x000
#define SP_FIFO_CTRL		0x004
#define SP_CMD			0x008
#define SP_INT_ENABLE		0x00C
#define SP_INT_STATUS		0x010
#define SP_RX_DATA		0x038
#define IPB_CON			0x500
#define IPB_STATE		0x504
#define IPB_INT_EN		0x508
#define IPB_FSM_UNEXPEN		0x50C
#define IPB_FSM_TXEN		0x510
#define IPB_FSM_RXFIFO		0x514
#define IPB_FSM_CON		0x518
#define IP_INT_STATUS		0x520
#define IPB_OPCODE		0x600
#define IPB_PAYLOAD		0x1000
#define IPB_ENABLE_APB		(1u << 0 | 1u << 1)	/* BATCHER_ENABLE | DEDICATED_BATCHER_APB */
#define IPB_START		(1u << 4)
#define IPB_SW_RESET		(1u << 31)
#define IPB_DONE		(1u << 0)
#define IPB_UNEXPECTED		(1u << 1)
#define SP_ERRS			(0xeu | 0x7u << 16 | 0x3u << 20)	/* timeouts, rx/tx errors */

static uint32_t sp_cmdbuf, sp_cmdidx, sp_cmdptr, sp_desc;

static void sp_batch(uint32_t payload, uint32_t opcode)
{
	sp_cmdbuf &= ~(0xffu << (8 * sp_cmdidx));
	sp_cmdbuf |= opcode << (8 * sp_cmdidx);
	wr32(SPEEDY + IPB_OPCODE + 4 * sp_cmdptr, sp_cmdbuf);
	wr32(SPEEDY + IPB_PAYLOAD + 4 * sp_desc, payload);
	sp_cmdidx++;
	sp_desc++;
	if (sp_cmdidx == 4) {
		sp_cmdidx = 0;
		sp_cmdptr++;
		sp_cmdbuf = 0xffffffffu;
	}
}

/* exynos_speedy_probe: batcher reset + which IP bits it monitors */
static void sp_init(void)
{
	wr32(SPEEDY + IPB_CON, rd32(SPEEDY + IPB_CON) | IPB_SW_RESET);
	wr32(SPEEDY + IPB_CON, rd32(SPEEDY + IPB_CON) & ~IPB_SW_RESET);
	wr32(SPEEDY + IPB_FSM_UNEXPEN, SP_ERRS);
	wr32(SPEEDY + IPB_FSM_TXEN, 1u << 0 | 1u << 4);	/* TRANSFER_DONE, TX_ALMOST_EMPTY */
	wr32(SPEEDY + IPB_FSM_RXFIFO, 1u << 6);		/* FIFO_EMPTY */
	wr32(SPEEDY + IPB_FSM_CON, 1u << 0);		/* DISABLE_STOP_CMD */
}

static uint32_t sp_last_state, sp_last_int;

/* one byte from 12-bit SPEEDY address a; -1 on timeout, -2 on error */
static int sp_read(uint32_t a)
{
	sp_cmdbuf = 0xffffffffu;
	sp_cmdidx = sp_cmdptr = sp_desc = 0;
	wr32(SPEEDY + IPB_CON, 0);				/* idle */
	wr32(SPEEDY + IPB_CON, IPB_ENABLE_APB);		/* idle -> init */
	sp_batch(0x30051, SP_CTRL);				/* SPEEDY enable */
	sp_batch(1u << 31 | 1u << 8 | 1u, SP_FIFO_CTRL);	/* FIFO reset, TX/RX trigger 1 */
	sp_batch(0xffffffffu, SP_INT_STATUS);
	sp_batch(0xeu | 1u << 8 | 1u << 9 | 0x7u << 16, SP_INT_ENABLE);
	sp_batch((a & 0xfff) << 7 | 1u << 19, SP_CMD);	/* random access, read */
	uint32_t rx_slot = sp_desc;
	sp_batch(0x77, SP_RX_DATA);
	sp_batch(0, 0xff);					/* end of table */
	wr32(SPEEDY + IPB_INT_EN, 1);
	wr32(SPEEDY + IPB_CON, rd32(SPEEDY + IPB_CON) | IPB_START);
	int r = -1;
	uint64_t end = xp_now() + xp.cntfrq / 2;		/* 500 ms */
	while (xp_now() < end) {
		sp_last_state = rd32(SPEEDY + IPB_STATE);
		sp_last_int = rd32(SPEEDY + IP_INT_STATUS);
		if (sp_last_state & IPB_DONE) {
			r = (sp_last_int & SP_ERRS) ? -2 : 0;
			break;
		}
		if ((sp_last_state & IPB_UNEXPECTED) && (sp_last_int & SP_ERRS)) {
			r = -2;
			break;
		}
	}
	if (sp_last_state & IPB_DONE)
		wr32(SPEEDY + IPB_STATE, sp_last_state | IPB_DONE);
	wr32(SPEEDY + IPB_INT_EN, 0);
	int v = r ? r : (int)(rd32(SPEEDY + IPB_PAYLOAD + 4 * rx_slot) & 0xff);
	wr32(SPEEDY + IPB_CON, 0);
	return v;
}

static char *put_sp(char *p, const char *name, uint32_t a)
{
	int v = sp_read(a);
	p = put(put(p, name), " ");
	if (v < 0) {
		p = put(p, v == -1 ? "timeout st " : "error st ");
		p = put_hex(p, sp_last_state, 8);
		return put(put_hex(put(p, " is "), sp_last_int, 8), "  ");
	}
	return put(put_hex(p, (uint32_t)v, 2), "  ");
}

/*
 * X6.4: bit-banged I2C on GPP1-4 (SCL) / GPP1-5 (SDA), open drain: a line is
 * driven low as an output with DAT 0, released as an input (the bus has its
 * own pull-ups on tsp_io). ~50 kHz. Proves the chip and the wiring without
 * the HSI2C block.
 */
#define BB_SCL		4
#define BB_SDA		5
#define BB_HALF_US	10

static void bb_line(int pin, int high)
{
	uint32_t con = rd32(PERIC0_GPP1) & ~(0xfu << (pin * 4));
	if (!high) {
		wr32(PERIC0_GPP1 + 4, rd32(PERIC0_GPP1 + 4) & ~(1u << pin));
		con |= 1u << (pin * 4);		/* output, low */
	}
	wr32(PERIC0_GPP1, con);			/* input = released */
	udelay(BB_HALF_US);
}

static int bb_get(int pin)
{
	return rd32(PERIC0_GPP1 + 4) >> pin & 1;
}

static void bb_start(void)
{
	bb_line(BB_SDA, 1);
	bb_line(BB_SCL, 1);
	bb_line(BB_SDA, 0);
	bb_line(BB_SCL, 0);
}

static void bb_stop(void)
{
	bb_line(BB_SDA, 0);
	bb_line(BB_SCL, 1);
	bb_line(BB_SDA, 1);
}

/* returns 0 on ACK */
static int bb_write_byte(uint8_t v)
{
	for (int i = 7; i >= 0; i--) {
		bb_line(BB_SDA, v >> i & 1);
		bb_line(BB_SCL, 1);
		bb_line(BB_SCL, 0);
	}
	bb_line(BB_SDA, 1);
	bb_line(BB_SCL, 1);
	int nack = bb_get(BB_SDA);
	bb_line(BB_SCL, 0);
	return nack;
}

static uint8_t bb_read_byte(int ack)
{
	uint8_t v = 0;
	bb_line(BB_SDA, 1);
	for (int i = 0; i < 8; i++) {
		bb_line(BB_SCL, 1);
		v = (uint8_t)(v << 1 | bb_get(BB_SDA));
		bb_line(BB_SCL, 0);
	}
	bb_line(BB_SDA, !ack);
	bb_line(BB_SCL, 1);
	bb_line(BB_SCL, 0);
	bb_line(BB_SDA, 1);
	return v;
}

/* cmd write, stop, then read len bytes: 0 ok, 1 no ACK on address, 2 no ACK on cmd */
static int bb_ts_read(uint8_t cmd, uint8_t *buf, unsigned len)
{
	bb_start();
	if (bb_write_byte(TS_ADDR << 1)) {
		bb_stop();
		return 1;
	}
	if (bb_write_byte(cmd)) {
		bb_stop();
		return 2;
	}
	bb_stop();
	bb_start();
	if (bb_write_byte(TS_ADDR << 1 | 1)) {
		bb_stop();
		return 3;
	}
	for (unsigned i = 0; i < len; i++)
		buf[i] = bb_read_byte(i + 1 < len);
	bb_stop();
	return 0;
}

static int ts_read(uint8_t reg, uint8_t *buf, unsigned len)
{
	int r = hsi2c_msg(TS_ADDR, 0, &reg, 1);
	return r ? r : hsi2c_msg(TS_ADDR, 1, buf, len);
}

static int ts_cmd(uint8_t cmd)
{
	return hsi2c_msg(TS_ADDR, 0, &cmd, 1);
}

static char *put_bytes(char *p, const uint8_t *b, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		p = put_hex(p, b[i], 2);
		*p++ = ' ';
	}
	*p = 0;
	return p;
}

static char *put_rc(char *p, const char *what, int r)
{
	p = put(p, what);
	p = put_dec(put(p, " rc "), r);
	p = put_hex(put(p, " is "), last_int, 8);
	return put(put_hex(put(p, " ts "), last_trans, 8), " ");
}

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
	char b[128], *p;
	uint8_t d[8];
	int r;

	wsr("tpidr_el1", 0);
	xp_init(dtb);
	if (!xp.fb_ok)
		xp_reset();
	con_clear(GREY);
	con_title("EXYNOS X6.5 RUNS");
	con_str("MODEL", xp.model[0] ? xp.model : "(no DTB)");

	/* what S-Boot left: pins, IRQ level, and the HSI2C block itself */
	p = put_hex(put(b, "gpg3 con "), rd32(PERIC1_GPG3), 8);
	p = put_hex(put(p, " pud "), rd32(PERIC1_GPG3 + 8), 8);
	p = put_hex(put(p, "  gpa1 con "), rd32(ALIVE_GPA1), 8);
	put_hex(put(p, " dat "), rd32(ALIVE_GPA1 + 4), 2);
	con_str("PINS", b);
	p = put_hex(put(b, "ctl "), hr(CTL), 8);
	p = put_hex(put(p, " conf "), hr(CONF), 8);
	p = put_hex(put(p, " usi "), hr(USI_CON), 8);
	p = put_hex(put(p, " fifo "), hr(FIFO_STATUS), 8);
	put_hex(put(p, " trans "), hr(TRANS_STATUS), 8);
	con_str("HSI2C", b);

	/* X6.5: PMIC over SPEEDY, reads only: ID, LDO1 (expected on), LDO35, LDO43 */
	p = put_hex(put(b, "ctrl "), rd32(SPEEDY + SP_CTRL), 8);
	p = put_hex(put(p, " ipb con "), rd32(SPEEDY + IPB_CON), 8);
	put_hex(put(p, " state "), rd32(SPEEDY + IPB_STATE), 8);
	con_str("SPEEDY", b);
	sp_init();
	p = put_sp(b, "id", 0x100);
	put_sp(p, "l1ctrl", 0x149);
	con_str("PMIC", b);
	p = put_sp(b, "l35ctrl", 0x16c);
	put_sp(p, "l43ctrl", 0x174);
	con_str("", b);

	/* X6.1: select I2C on USI port 3, then the pins */
	uint32_t usi_before = rd32(SYSREG_USI03);
	wr32(SYSREG_USI03, USI_I2C_SW_CONF);
	p = put_hex(put(b, "sysreg usi03 "), usi_before, 8);
	put_hex(put(p, " -> "), rd32(SYSREG_USI03), 8);
	con_str("USI", b);

	/* X6.3: GPP1-4/5 -> function 2, no pull (hsi2c10-bus, the controller's own pinctrl-0) */
	p = put_hex(put(b, "gpp1 con "), rd32(PERIC0_GPP1), 8);
	put_hex(put(p, " pud "), rd32(PERIC0_GPP1 + 8), 8);
	con_str("GPP1", b);

	/* X6.4: bus levels with the pins as inputs (no pull, then pull-up), and a bit-banged ID read */
	wr32(PERIC0_GPP1, rd32(PERIC0_GPP1) & ~(0xffu << 16));
	wr32(PERIC0_GPP1 + 8, rd32(PERIC0_GPP1 + 8) & ~(0xffu << 16));
	udelay(100);
	uint32_t lv_none = rd32(PERIC0_GPP1 + 4) >> 4 & 3;
	wr32(PERIC0_GPP1 + 8, rd32(PERIC0_GPP1 + 8) | 0x33u << 16);
	udelay(100);
	uint32_t lv_up = rd32(PERIC0_GPP1 + 4) >> 4 & 3;
	wr32(PERIC0_GPP1 + 8, rd32(PERIC0_GPP1 + 8) & ~(0xffu << 16));
	p = put_dec(put(b, "scl/sda no pull "), lv_none & 1);
	p = put_dec(put(p, "/"), lv_none >> 1);
	p = put_dec(put(p, "  pull-up "), lv_up & 1);
	put_dec(put(p, "/"), lv_up >> 1);
	con_str("LINES", b);
	r = bb_ts_read(SEC_TS_READ_ID, d, 3);
	p = put_dec(put(b, "bitbang id rc "), r);
	put_bytes(put(p, "  "), d, r ? 0 : 3);
	con_str("BITBANG", b);
	r = bb_ts_read(SEC_TS_READ_BOOT_STATUS, d, 1);
	p = put_dec(put(b, "bitbang boot rc "), r);
	put_bytes(put(p, "  "), d, r ? 0 : 1);
	con_str("", b);
	uint32_t con = rd32(PERIC0_GPP1);
	con = (con & ~(0xffu << 16)) | 0x22u << 16;
	wr32(PERIC0_GPP1, con);
	wr32(PERIC0_GPP1 + 8, rd32(PERIC0_GPP1 + 8) & ~(0xffu << 16));
	hsi2c_reset();
	p = put_hex(put(b, "gpp1 con "), rd32(PERIC0_GPP1), 8);
	p = put_hex(put(p, " pud "), rd32(PERIC0_GPP1 + 8), 8);
	p = put_hex(put(p, " ctl "), hr(CTL), 8);
	p = put_hex(put(p, " conf "), hr(CONF), 8);
	put_hex(put(p, " fs3 "), hr(TIMING_FS3), 8);
	con_str("SETUP", b);
	con_skip();

	/* X6.2: the USI03 operating clock, as S-Boot left it, then forced on */
	p = put_hex(put(b, "mux "), rd32(CMU_MUX_IP_USER), 8);
	p = put_hex(put(p, " div "), rd32(CMU_DIV_USI03), 8);
	p = put_hex(put(p, " gate "), rd32(CMU_GATE_USI03), 8);
	con_str("CMU", b);
	p = put_hex(put(b, "rstsyn "), rd32(CMU_GOUT_RSTSYN), 8);
	p = put_hex(put(p, " ipclk "), rd32(CMU_GOUT_IPCLK), 8);
	put_hex(put(p, " qch "), rd32(CMU_QCH_USI03), 8);
	con_str("", b);
	/* X6.4: with the right pins, force the gates and the Q-channel too */
	const uintptr_t gates[3] = { CMU_GATE_USI03, CMU_GOUT_RSTSYN, CMU_GOUT_IPCLK };
	for (int i = 0; i < 3; i++)
		wr32(gates[i], (rd32(gates[i]) & ~GATE_AUTO) | GATE_MANUAL_ON);
	wr32(CMU_QCH_USI03, (rd32(CMU_QCH_USI03) & ~1u) | 1u << 1 | 1u << 2);
	hsi2c_reset();
	con_skip();

	/* the chip: pending event (boot status), IDs */
	for (int i = 0; i < 3; i++) {
		r = ts_read(SEC_TS_READ_ONE_EVENT, d, 8);
		put_bytes(put_rc(b, "event", r), d, r ? 0 : 8);
		con_str(i ? "" : "EVENT", b);
		if (r)
			break;
	}
	r = ts_read(SEC_TS_READ_ID, d, 3);
	put_bytes(put_rc(b, "id", r), d, r ? 0 : 3);
	con_str("ID", b);
	r = ts_read(SEC_TS_READ_BOOT_STATUS, d, 1);
	put_bytes(put_rc(b, "boot", r), d, r ? 0 : 1);
	con_str("BOOT", b);
	r = ts_read(SEC_TS_READ_DEVICE_ID, d, 5);
	put_bytes(put_rc(b, "devid", r), d, r ? 0 : 5);
	con_str("DEVID", b);
	r = ts_cmd(SEC_TS_CMD_CLEAR_EVENT_STACK);
	if (!r) {
		xp_delay_ms(10);
		r = ts_cmd(SEC_TS_CMD_SENSE_ON);
	}
	put_rc(b, "clear+sense_on", r);
	con_str("SENSE", b);
	con_skip();
	con_str("", "Touch the screen: last events below, 60 s.");
	int ev_y = con_row();
	con_skip();
	con_skip();
	con_skip();
	con_skip();
	con_skip();
	con_skip();

	int y = con_row();
	unsigned events = 0, touches = 0, row = 0;
	uint64_t t0 = xp_now();
	for (unsigned s = 0; s <= RUN_SECONDS;) {
		if (!(rd32(ALIVE_GPA1 + 4) & 1)) {	/* attention: active low */
			r = ts_read(SEC_TS_READ_ONE_EVENT, d, 8);
			events++;
			if (!r) {
				unsigned eid = d[0] & 3, tid = d[0] >> 2 & 0xf, st = d[0] >> 6;
				p = b;
				if (eid == 0) {
					unsigned x = (unsigned)d[1] << 4 | d[3] >> 4;
					unsigned yy = (unsigned)d[2] << 4 | (d[3] & 0xf);
					touches++;
					p = put_dec(put(p, "touch id "), tid);
					p = put_dec(put(p, " st "), st);
					p = put_dec(put(p, " x "), x);
					p = put_dec(put(p, " y "), yy);
					p = put_dec(put(p, " left "), d[7] & 0x3f);
				} else {
					p = put_dec(put(p, "event eid "), eid);
					p = put(p, "  ");
					put_bytes(p, d, 8);
				}
			} else {
				put_rc(b, "read", r);
			}
			con_set_row(ev_y + row);
			con_str("", "                                                            ");
			con_set_row(ev_y + row);
			con_str("", b);
			row = (row + 1) % 6;
		}
		if (xp_now() - t0 >= xp.cntfrq * s) {
			p = put(put_dec(b, s), "s  events ");
			p = put_dec(p, events);
			p = put_dec(put(p, " touches "), touches);
			put_hex(put(p, " irq "), rd32(ALIVE_GPA1 + 4) & 1, 1);
			con_set_row(y);
			con_str("TIME", b);
			s++;
		}
		udelay(500);
	}
	xp_reset();
}
