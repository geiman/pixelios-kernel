#pragma once
#include <stdint.h>

/*
 * touchprobe.h - Pixel 6a touchscreen SPI11 / USI11 bring-up experiment WITHOUT booting iOS
 * (build.sh --touch-probe[=run]).
 * --touch-probe=railsr / rails add the ACPM client of acpm.c (read / set the touch rails LDO25M, LDO26M). A register-level port of the XNU driver
 * xnu/patches/0088-sweet-gs101-touch.patch (+0089 CMU_PERIC1 clock chain, +0090 variants A-E, +0092 CMU_TOP gate)
 * as it runs with boot-args `sweettouchclk=3 sweettouchvar=1`.
 *
 * touch_probe_dump(): phase 1, READ-ONLY (CMU_PERIC1 + CMU_TOP, USI11 mode, pad banks, USI/SPI registers).
 * touch_probe_run(opts): phase 2, the writes of that boot-arg set plus the docs/research/21 experiments chosen by TP_O_*;
 *                     only called by --touch-probe=run[,...].
 * Every constant comes from the generated TOUCH_* board macros (tools/boards.py <- soc.json / board.json, the same
 * keys tools/mkadt.py puts into the /sweet-touch node). Output lines are the XNU driver's "SWEET TOUCH: ..." lines
 * (label "SWEET", text "TOUCH: ..."); long XNU lines are split at spaces to fit the 96-byte RAM-log line.
 *
 * Pure decoders live here so shim/touch_host_test.c can test them.
 */

/* SPI engine layout, spi-s3c64xx.c (0088 patch :235-:257) */
#define TP_SPI_CH_CFG		0x00u
#define TP_SPI_CLK_CFG		0x04u
#define TP_SPI_MODE_CFG		0x08u
#define TP_SPI_CS_REG		0x0cu
#define TP_SPI_INT_EN		0x10u
#define TP_SPI_STATUS		0x14u
#define TP_SPI_TX_DATA		0x18u
#define TP_SPI_RX_DATA		0x1cu
#define TP_SPI_PACKET_CNT	0x20u
#define TP_SPI_PENDING_CLR	0x24u
#define TP_SPI_SWAP_CFG		0x28u
#define TP_SPI_FB_CLK		0x2cu
#define TP_CH_SW_RST		(1u << 5)
#define TP_CH_RXCH_ON		(1u << 1)
#define TP_CH_TXCH_ON		(1u << 0)
#define TP_CH_HS_EN		(1u << 6)
#define TP_PKT_CNT_EN		(1u << 16)
#define TP_MODE_TRAILCNT	(0x3ffu << 19)
#define TP_PND_ERR_CLR		0x1eu
#define TP_CLK_ENCLK		(1u << 8)	/* 0090 variant A */
#define TP_CLK_PSR_MASK		0xffu
#define TP_CS_AUTO		(1u << 1)	/* 0090 variant E */
#define TP_CS_NSC_CNT_2		(2u << 4)
#define TP_USI_CON		0x04u
#define TP_USI_CON_RESET	(1u << 0)
#define TP_USI_OPTION		0x08u
#define TP_USI_OPT_CLKREQ	(1u << 1)
#define TP_USI_OPT_CLKSTOP	(1u << 2)
#define TP_SPI_FIFO_BYTES	64u
#define TP_NVARIANTS		5u		/* A..E, tried in this order */
#define TP_BURST_TIMEOUT_US	20000u		/* 0088 tg_spi_burst */
#define TP_MUX_BUSY_TIMEOUT_US	1000u		/* 0089 tg_cmu_enable */
#define TP_MODE_LOOPBACK	(1u << 3)	/* S3C64XX_SPI_MODE_SELF_LOOPBACK, docs/research/21 X3 */
#define TP_FTM5_CMD_FIFO_READ	0x87u		/* 0088 sweet_ftm5.h */
#define TP_FTM5_EVENT_SIZE	8u
#define TP_FTM5_EVT_CONTROLLER_READY 0x03u

/* Build options (build.sh --touch-probe=run,mux,cs,osc|top,loop,rst,misopu,railsr|rails), docs/research/21 section 3 */
#define TP_O_MUX	1u	/* drive the TS_SPI_SEL pad low if the AP does not (XNU sweettouchmux=1) */
#define TP_O_CS		2u	/* X1: ABL-exact manual chip select, variant F */
#define TP_O_OSC	4u	/* X2: USI11 clock from oscclk, divider 5 */
#define TP_O_TOP	8u	/* X6: USI11 clock from the TOP source, divider 9 */
#define TP_O_LOOP	16u	/* X3: self-loopback */
#define TP_O_RST	32u	/* X4: pull-ups off, reset pulse, controller-ready poll */
#define TP_O_MISOPU	64u	/* X5: MISO pull-up during the identify */
#define TP_O_RAILSR	128u	/* ACPM: READ the two touch rail enable registers (LDO25M/LDO26M, docs/research/23); no SPI writes */
#define TP_O_RAILS	256u	/* ACPM: UPDATE bit 7 of both (set, never clear), read back; implies railsr */
#define TP_O_EXP	(TP_O_CS | TP_O_OSC | TP_O_TOP | TP_O_LOOP | TP_O_RST | TP_O_MISOPU)

#define TP_MAX_POLLS		100000u		/* belt and braces: a poll loop ends even if the timer is dead */

#define TP_ST_RX_LVL(v)		(((v) >> 15) & 0x1ffu)
#define TP_ST_TX_LVL(v)		(((v) >> 6) & 0x1ffu)

/* FTM5 chip-id read, sweet_ftm5.h (0088 :32-:43) */
#define TP_FTM5_CMD_HW_REG_R	0xFBu
#define TP_FTM5_ADDR_DCHIP_ID	0x20000000u
#define TP_FTM5_DUMMY_BYTES	1u
#define TP_FTM5_SPI_DELAY_CS_US	10u

/* Samsung sec_ts (S6SY79x, raven; board.json touch_protocol "sec_ts"). Read from source: kernel/google-modules/touch/sec_touch
 * b23d1f8dfb9e (android-gs-raviole-5.10-android13-qpr3), sec_ts.h / sec_ts.c lines as cited. */
#define TP_SEC_SYNC		0xAAu		/* SEC_TS_SPI_SYNC_CODE, sec_ts.h:91 */
#define TP_SEC_REQ_LEN		8u		/* header 5 + reg 1 + checksum 1, padded to 4 (sec_ts.c:310-324) */
#define TP_SEC_RD_HDR		7u		/* SEC_TS_SPI_READ_HEADER_SIZE, sec_ts.h:93 */
#define TP_SEC_RD_DELAY_US	100u		/* sec_ts_spi_delay default for these registers, sec_ts.c:61,95 */
#define TP_SEC_READ_DEVICE_ID	0x22u		/* sec_ts.h:220, 5 bytes (sec_ts.c:4322) */
#define TP_SEC_READ_ID		0x52u		/* sec_ts.h:249, 3 bytes (sec_ts.c:4100) */
#define TP_SEC_READ_BOOT_STATUS	0x55u		/* sec_ts.h:250, 1 byte (sec_ts.c:4347) */
#define TP_SEC_READ_ONE_EVENT	0x60u		/* sec_ts.h:252, SEC_TS_EVENT_BUFF_SIZE 8 bytes (sec_ts.h:350) */
#define TP_SEC_EVENT_SIZE	8u
#define TP_SEC_STATUS_BOOT	0x10u		/* SEC_TS_STATUS_BOOT_MODE, sec_ts.h:338 */
#define TP_SEC_STATUS_APP	0x20u		/* SEC_TS_STATUS_APP_MODE, sec_ts.h:339 */
#define TP_SEC_EVT_INFO		2u		/* TYPE_STATUS_EVENT_INFO, sec_ts.h:121 */
#define TP_SEC_EVT_VENDOR_INFO	7u		/* TYPE_STATUS_EVENT_VENDOR_INFO, sec_ts.h:124 */
#define TP_SEC_ACK_BOOT_COMPLETE 0x00u		/* sec_ts.h:372 */
#define TP_SEC_READY_POLL_US	20000u		/* sec_ts_wait_for_ready: sec_ts_delay(20) per try, sec_ts.c:1112 */
#define TP_SEC_READY_TRIES	100u		/* SEC_TS_WAIT_RETRY_CNT, sec_ts.h:608 */
#define TP_SEC_BOOT_DELAY_US	80000u		/* after reset high: mdelay(10) + sec_ts_delay(70), sec_ts.c:4731-4733 */
/* s3c64xx CH_CFG clock polarity / phase (spi-cpol, spi-cpha = SPI mode 3): spi-s3c64xx.c:55-56, cleared at :780-782, set at :784-788,
 * written at :790 (kernel/gs 6e771b230c52) */
#define TP_CH_CPOL_L		(1u << 3)
#define TP_CH_CPHA_B		(1u << 2)
#define TP_CH_SLAVE		(1u << 4)	/* S3C64XX_SPI_CH_SLAVE, spi-s3c64xx.c:54, cleared with them at :780-782 */
/* CH_CFG mode bits the engine init writes: mode 3 for sec_ts; 0 (mode 0, master) for FTM5 as before */
#ifdef TOUCH_PROTO_SEC_TS
#define TP_CH_MODE		(TP_CH_CPOL_L | TP_CH_CPHA_B)
#else
#define TP_CH_MODE		0u
#endif

/* byte k of a <= 16-byte stream packed into two little-endian words (byte k in w0 for k < 8, else in w1) */
static inline unsigned tp_byte(uint64_t w0, uint64_t w1, unsigned k)
{
	return (unsigned)(((k < 8u ? w0 : w1) >> (8u * (k & 7u))) & 0xffu);
}

/* The probe reads at most 16 reply bytes (two 8-byte bursts into two words), so at most TP_SEC_MAX_LEN data bytes; sec_ts itself
 * allows longer reads (sec_ts.c:326-328 for any len up to io_burstmax). A longer request is refused as a caller error, never
 * sent and never reported as a bus result. Every register the probe reads is <= 8 bytes (DEVICE_ID 5, ID 3, BOOT_STATUS 1, ONE_EVENT 8). */
#define TP_SEC_MAX_LEN		8u

/* bytes clocked in for a read of len data bytes: header 7 + data + checksum 1, rounded up to 4 (sec_ts.c:326-328) */
static inline unsigned tp_sec_reply_len(unsigned len)
{
	return (TP_SEC_RD_HDR + len + 1u + 3u) & ~3u;
}

/* The read request: AA 00 01 len_hi len_lo reg checksum 00 (sec_ts.c:310-324; checksum = byte sum of the first six). */
static inline void tp_sec_req(uint8_t *f, unsigned reg, unsigned len)
{
	unsigned i, sum = 0;

	f[0] = TP_SEC_SYNC;
	f[1] = 0x00;
	f[2] = 0x01;
	f[3] = (uint8_t)(len >> 8);
	f[4] = (uint8_t)len;
	f[5] = (uint8_t)reg;
	for (i = 0; i < 6u; i++)
		sum += f[i];
	f[6] = (uint8_t)sum;
	f[7] = 0x00;
}

/* sec_ts_read_internal's acceptance test (sec_ts.c:411-435): sync byte, register echoed at byte 5, and the byte sum of the
 * header and data equal to the checksum byte after them. Byte 6 (command status) is not checked there either (:431). */
static inline int tp_sec_reply_ok(uint64_t w0, uint64_t w1, unsigned reg, unsigned len)
{
	unsigned i, sum = 0;

	if (len > TP_SEC_MAX_LEN)
		return 0;
	for (i = 0; i < TP_SEC_RD_HDR + len; i++)
		sum += tp_byte(w0, w1, i);
	return tp_byte(w0, w1, 0) == TP_SEC_SYNC && tp_byte(w0, w1, 5) == reg &&
	       (sum & 0xffu) == tp_byte(w0, w1, TP_SEC_RD_HDR + len);
}

/* sec_ts_wait_for_ready_with_count (sec_ts.c:1096-1110): the event type is bits 5:2 of byte 0, the ack is byte 1 */
static inline int tp_sec_boot_complete(unsigned e0, unsigned e1)
{
	unsigned type = (e0 >> 2) & 0xfu;

	return (type == TP_SEC_EVT_INFO || type == TP_SEC_EVT_VENDOR_INFO) && e1 == TP_SEC_ACK_BOOT_COMPLETE;
}

#ifdef TOUCH_CMU_BIT_GATE_MANUAL
/* 0089 tg_cmu_gate: manual gate open = MANUAL 1, CG_VAL 1, automatic gating 0. */
static inline uint32_t tp_gate_open(uint32_t v)
{
	return (v | (1u << TOUCH_CMU_BIT_GATE_MANUAL) | (1u << TOUCH_CMU_BIT_GATE_VAL)) &
	       ~(1u << TOUCH_CMU_BIT_GATE_AUTO);
}
#define TP_GATE_CHECK_MASK ((1u << TOUCH_CMU_BIT_GATE_MANUAL) | (1u << TOUCH_CMU_BIT_GATE_VAL) | \
			    (1u << TOUCH_CMU_BIT_GATE_AUTO))
#endif

/* ftm5_chip_id_match: 1 = same order, 2 = swapped, 0 = no match */
static inline int tp_chip_id_match(const uint8_t *got, const uint8_t *want)
{
	if (got[0] == want[0] && got[1] == want[1])
		return 1;
	if (got[0] == want[1] && got[1] == want[0])
		return 2;
	return 0;
}

/* Fault-guarded MMIO (shim.c; host tests substitute a fake window). */
int try_read32(uintptr_t address, uint32_t *value);
int try_write32(uintptr_t address, uint32_t value);
uint64_t touch_ticks(void);	/* CNTPCT (shim.c) */

void touch_probe_dump(void);	/* phase 1, read-only */
int touch_probe_dvfs(unsigned step);	/* ACPM DVFS (TOUCH_DVFS): 0 describe channel 1 (reads), 1 FREQ_GET all domains, 2 FREQ_REQ DVFS_KHZ0..2 + read back; 0 = ok */
int touch_probe_rails(unsigned step);	/* ACPM rails (TOUCH_RAILS): 0 describe (reads), 1 read both regs, 2 update + read back; 0 = ok */
void touch_probe_run(unsigned opts);	/* phase 2: the sweettouchclk=3 sweettouchvar=1 writes + the TP_O_* experiments */
