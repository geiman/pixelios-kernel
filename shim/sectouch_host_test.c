/* Host test for the sec_ts (S6SY79x, raven) path of touchprobe.c: the request/reply frame helpers, and whole probe runs
 * against a fake chip behind a fake SPI11 engine. The fake answers only well-formed sec_ts request frames, only while the
 * chip select pad is low and CH_CFG has CPOL|CPHA (SPI mode 3), and only after its boot delay, so a wrong frame, a wrong
 * mode or a missing chip select shows up as a failed identify. It #includes the .c (statics reachable) and uses the REAL
 * generated board macros with TOUCH_PROTO_SEC_TS (build.sh derives that header for non-sec_ts boards too). Build:
 *   gcc -O1 -Wall -Wextra -Werror -I. -Iout -include out/board_config_sec.h -o out/sectouch_host_test sectouch_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* a small read budget so the dead-counter test ends quickly; the delay logic is the production one */
#define TP_DELAY_POLLS_PER_US 4u
#include "touchprobe.c"

#if !(SOC_GS101 && defined(TOUCH_SPI_BASE) && defined(TOUCH_PROTO_SEC_TS))
#error "needs a gs101 board_config.h with the touch macros and TOUCH_PROTO_SEC_TS"
#endif

/* ---- fake hardware: registers ---- */
#define NREG 512
static struct { uintptr_t pa; uint32_t v; } reg[NREG];
static unsigned nreg;
static uint64_t fake_now;
static int frozen;			/* the counter does not advance (dead CNTPCT) */
static unsigned long accesses;
static uintptr_t wlog[200000];
static unsigned nw;
static char lines[2048][128];
static unsigned nlines;

/* ---- fake chip ---- */
static int spi_ok;			/* the engine moves data at all */
static int chip;			/* a chip is on the bus */
static int miso_high;			/* no chip: MISO floats high (0xff) instead of low */
static int corrupt;			/* the chip's checksum byte is off by one */
static unsigned boot_status;		/* what READ_BOOT_STATUS returns */
static uint64_t ready_at;		/* boot-complete event once fake_now >= this (0 = never) */
static uint64_t reset_high_at;		/* when the reset pad last went high */
static unsigned bad_mode, bad_cs;	/* TX bytes sent without mode 3 / with the chip select high */
static uint8_t txb[64];			/* bytes of the current burst */
static unsigned ntx, rx_idx;
static uint8_t reply[16];		/* reply to the last good request, clocked out over the next read bursts */
static unsigned reply_pos, reply_len, nreq, nreq_bad;
static unsigned last_reg;
static uint8_t req[64];			/* request bytes of the current chip-select window, processed when the chip select rises */
static unsigned nreqb;
static uint64_t first_req_at;		/* first request after the reset pulse */
static uint64_t req_end_at;		/* when the chip select rose after the last request */
static uint64_t min_gap;		/* shortest request-end to next chip-select-fall interval, us */
static unsigned ignore_reqs;		/* the chip ignores this many requests first (forces the A-E fallback) */
static unsigned stall_variants;		/* the engine stalls until this many fallback variants have timed out */
static unsigned variant_timeouts;

static uint32_t garbage(uintptr_t pa)
{
	uint64_t x = pa * 0x9e3779b97f4a7c15ull + 777;

	x ^= x >> 29;
	x *= 0xbf58476d1ce4e5b9ull;
	x ^= x >> 32;
	return (uint32_t)x;
}

static int in_spi(uintptr_t pa)
{
	return pa >= SPI && pa < SPI + 0x1000;
}

static uint32_t *slot(uintptr_t pa)
{
	for (unsigned i = 0; i < nreg; i++)
		if (reg[i].pa == pa)
			return &reg[i].v;
	assert(nreg < NREG);
	reg[nreg].pa = pa;
	reg[nreg].v = in_spi(pa) ? 0 : garbage(pa);
	return &reg[nreg++].v;
}

static int cs_low(void)
{
	/* pad on the SPI function (variant E): the engine drives nSS, asserted during a transfer when CS_AUTO
	 * (S3C64XX_SPI_SLAVE_AUTO BIT(1), spi-s3c64xx.c:80; set by enable_cs :654-659, kernel/gs 6e771b230c52) is set */
	if (((*slot(TOUCH_PIN_CS_BANK) >> (TOUCH_PIN_CS_NR * 4)) & 0xfu) == TOUCH_SPI_FUNC)
		return (*slot(SPI + TP_SPI_CS_REG) & (1u << 1)) != 0;
	return !((*slot(TOUCH_PIN_CS_BANK + 4) >> TOUCH_PIN_CS_NR) & 1u);
}

/* written from the source, not from touchprobe.h: spi-s3c64xx.c:54-56 (kernel/gs 6e771b230c52) CH_SLAVE BIT(4), CPOL_L BIT(3), CPHA_B BIT(2) */
static int mode3(void)
{
	uint32_t c = *slot(SPI + TP_SPI_CH_CFG);

	return (c & ((1u << 3) | (1u << 2))) == ((1u << 3) | (1u << 2)) && !(c & (1u << 4));
}

static int booted(void)
{
	return chip && reset_high_at && fake_now >= reset_high_at + 50000u;	/* answers frames 50 ms after reset */
}

/* An independent sec_ts responder (sec_ts.c:310-472 read framing, written from the source, not from touchprobe.h). */
static void chip_request(void)
{
	unsigned sum = 0, i, len, n = 0;
	uint8_t d[8];

	nreq++;
	if (reset_high_at && !first_req_at)
		first_req_at = fake_now;
	memcpy(txb, req, sizeof(txb));
	for (i = 0; i < 6; i++)
		sum += txb[i];
	reply_len = 0;
	if (nreqb != 8 || txb[0] != 0xAA || txb[1] != 0 || txb[2] != 1 || (uint8_t)sum != txb[6]) {
		nreq_bad++;	/* a malformed frame: never expected from the probe */
		return;
	}
	if (!booted())	/* in reset or still booting (variant F runs before the reset pulse): no answer */
		return;
	if (ignore_reqs) {
		ignore_reqs--;
		return;
	}
	len = ((unsigned)txb[3] << 8) | txb[4];
	last_reg = txb[5];
	memset(d, 0, sizeof(d));
	switch (txb[5]) {
	case 0x22: n = 5; d[0] = 0x53; d[1] = 0x79; d[2] = 0x11; d[3] = 0x22; d[4] = 0x33; break;	/* made-up id bytes */
	case 0x52: n = 3; d[0] = 0xAC; d[1] = 0x37; d[2] = 0x71; break;
	case 0x55: n = 1; d[0] = (uint8_t)boot_status; break;
	case 0x60: n = 8;
		if (ready_at && fake_now >= ready_at) {
			d[0] = (uint8_t)(2u << 2);	/* TYPE_STATUS_EVENT_INFO in bits 5:2 */
			d[1] = 0x00;			/* SEC_TS_ACK_BOOT_COMPLETE */
		}
		break;
	default: break;
	}
	assert(n == len);	/* the probe asks for each register's real length */
	memset(reply, 0, sizeof(reply));
	reply[0] = 0xAA;
	reply[5] = txb[5];
	reply[6] = 0;
	memcpy(reply + 7, d, n);
	sum = 0;
	for (i = 0; i < 7 + n; i++)
		sum += reply[i];
	reply[7 + n] = (uint8_t)(sum + (corrupt ? 1 : 0));
	reply_len = (7 + n + 1 + 3) & ~3u;
	reply_pos = 0;
}

static uint8_t rx_byte(void)
{
	unsigned i = rx_idx++;

	if (i < ntx && txb[0] == 0xAA) {	/* the request burst: collected now, acted on when the chip select rises */
		if (i + 1 == ntx && cs_low()) {
			memcpy(req, txb, sizeof(req));
			nreqb = ntx;
			reply_len = 0;	/* a new request in the same window as a reply would be a framing error */
		}
		return chip ? 0 : (miso_high ? 0xff : 0);
	}
	if (!chip)
		return miso_high ? 0xff : 0;
	if (!cs_low() || reply_pos >= reply_len)
		return 0;
	return reply[reply_pos++];
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	assert(++accesses < 80000000ul);
	if (pa == SPI + TP_SPI_STATUS) {
		/* RX level = bytes clocked in and not yet read, TX drained; stall: the TX FIFO never drains, RX stays empty */
		*v = spi_ok ? ((ntx > rx_idx ? ntx - rx_idx : 0u) << 15) : (ntx << 6);
		return 1;
	}
	if (pa == SPI + TP_SPI_RX_DATA) {
		*v = rx_byte();
		return 1;
	}
	*v = *slot(pa);
	return 1;
}

int try_write32(uintptr_t pa, uint32_t v)
{
	assert(nw < sizeof(wlog) / sizeof(wlog[0]));
	wlog[nw++] = pa;
	if (pa == SPI + TP_SPI_PACKET_CNT)
		rx_idx = ntx = 0;
	if (pa == SPI + TP_SPI_TX_DATA) {
		if (!mode3())
			bad_mode++;
		if (!cs_low())
			bad_cs++;
		if (ntx < sizeof(txb))
			txb[ntx++] = (uint8_t)v;
	}
	if (pa == TOUCH_PIN_CS_BANK + 4) {	/* chip select rising edge ends a message (one spi_sync) */
		uint32_t old = *slot(pa), bit = 1u << TOUCH_PIN_CS_NR;

		if (!(old & bit) && (v & bit) && nreqb) {
			chip_request();
			nreqb = 0;
			req_end_at = fake_now;
		}
		if ((old & bit) && !(v & bit) && req_end_at) {	/* next chip select falls: the request-to-reply gap */
			if (fake_now - req_end_at < min_gap)
				min_gap = fake_now - req_end_at;
			req_end_at = 0;
		}
	}
	if (pa == TOUCH_PIN_RESET_BANK + 4) {
		uint32_t old = *slot(pa), bit = 1u << TOUCH_PIN_RESET_NR;

		if (!(old & bit) && (v & bit))
			reset_high_at = fake_now;
	}
	*slot(pa) = v;
	return 1;
}

uint64_t counter_freq(void) { return 1000000; }	/* 1 tick = 1 us */
uint64_t touch_ticks(void) { fake_now += frozen ? 0 : 7; return fake_now; }
void wait_ticks(uint64_t t) { assert(!frozen); fake_now += t; }	/* the sec_ts path must not need it (a dead counter would hang it) */
void con_str(const char *label, const char *s)
{
	assert(strlen(label) + 1 + strlen(s) < RAMLOG_LINE_MAX);
	assert(nlines < 2048);
	snprintf(lines[nlines++], sizeof(lines[0]), "%s %s", label, s);
	if (stall_variants && strstr(s, "VARIANT ") && strstr(s, ": timeout") && ++variant_timeouts >= stall_variants)
		spi_ok = 1;	/* the next variant's burst completes */
}

/* say() splits long lines at spaces; search the text with the "SWEET TOUCH: " / "SWEET TOUCH RESULT: " prefixes removed
 * and the pieces joined by one space, so a needle may span a split */
static int has(const char *needle)
{
	static char all[2048 * 128];
	size_t p = 0;

	for (unsigned i = 0; i < nlines; i++) {
		const char *s = lines[i];

		if (getenv("TOUCH_V"))
			fprintf(stderr, "%s\n", s);
		if (!strncmp(s, "SWEET TOUCH RESULT: ", 20))
			s += 20;
		else if (!strncmp(s, "SWEET TOUCH: ", 13))
			s += 13;
		p += (size_t)snprintf(all + p, sizeof(all) - p, "%s ", s);
		assert(p < sizeof(all));
	}
	all[p] = 0;
	return strstr(all, needle) != NULL;
}

/* ABL handoff state as bluejay showed it (touch_host_test.c reset()): mux pad an input at 0, USI mode 0, CS_REG 1. */
static void reset(void)
{
	nreg = nw = nlines = 0;
	accesses = 0;
	fake_now = 1000;
	spi_ok = 1;
	chip = 1;
	miso_high = corrupt = 0;
	boot_status = 0x20;
	ready_at = 0;
	reset_high_at = 0;
	bad_mode = bad_cs = 0;
	ntx = rx_idx = reply_pos = reply_len = nreq = nreq_bad = last_reg = nreqb = 0;
	first_req_at = req_end_at = 0;
	min_gap = ~0ull;
	ignore_reqs = 0;
	stall_variants = variant_timeouts = 0;
	frozen = 0;
	*slot(TOUCH_PIN_MUX_BANK) &= ~(0xfu << (TOUCH_PIN_MUX_NR * 4));
	*slot(TOUCH_PIN_MUX_BANK + 4) &= ~(1u << TOUCH_PIN_MUX_NR);
	*slot(TOUCH_PIN_RESET_BANK + 4) &= ~(1u << TOUCH_PIN_RESET_NR);
	*slot(TOUCH_USI_MODE_REG) &= ~(uint32_t)TOUCH_USI_MODE_MASK;
	*slot(CMU + TOUCH_CMU_MUX_USI) &= ~((1u << TOUCH_CMU_BIT_MUX_BUSY) | (1u << TOUCH_CMU_BIT_MUX_SEL));
	*slot(CMU + TOUCH_CMU_DIV) &= ~(1u << TOUCH_CMU_BIT_MUX_BUSY);
	*slot(SPI + TP_SPI_CS_REG) = 1;
	*slot(USI + TP_USI_CON) = 1;
}

/* the bluejay recipe that worked (patch 0095): mux, ABL transfer, oscillator clock, reset pulse */
#define RECIPE	(TP_O_MUX | TP_O_CS | TP_O_OSC | TP_O_RST)

static void test_frames(void)
{
	static const uint8_t want[8] = { 0xAA, 0x00, 0x01, 0x00, 0x05, 0x22, 0xD2, 0x00 };	/* AA+01+05+22 = 0xD2 */
	uint8_t f[8];
	uint64_t w0, w1;

	tp_sec_req(f, 0x22, 5);
	assert(!memcmp(f, want, sizeof(f)));
	tp_sec_req(f, 0x60, 8);
	assert(f[4] == 8 && f[5] == 0x60 && f[6] == (uint8_t)(0xAA + 1 + 8 + 0x60));
	assert(tp_sec_reply_len(5) == 16 && tp_sec_reply_len(3) == 12 && tp_sec_reply_len(1) == 12 && tp_sec_reply_len(8) == 16);
	assert(tp_sec_reply_len(9) == 20);

	/* AA 00 00 00 00 55 00 20 | ck = AA+55+20 = 0x11f -> 0x1f */
	w0 = 0x20005500000000AAull;
	w1 = 0x1f;
	assert(tp_sec_reply_ok(w0, w1, 0x55, 1));
	assert(!tp_sec_reply_ok(w0, w1 ^ 1, 0x55, 1));		/* checksum */
	assert(!tp_sec_reply_ok(w0, w1, 0x22, 1));			/* echo */
	assert(!tp_sec_reply_ok(w0 ^ 0x01, w1, 0x55, 1));		/* sync */
	assert(!tp_sec_reply_ok(w0, w1, 0x55, 9));			/* over TP_SEC_MAX_LEN: the probe never reads it (refused, see test_length) */
	assert(!tp_sec_reply_ok(~0ull, ~0ull, 0x55, 1));		/* MISO high */
	assert(!tp_sec_reply_ok(0, 0, 0x55, 1));			/* MISO low: sync fails */

	assert(tp_sec_boot_complete(2u << 2, 0) && tp_sec_boot_complete(7u << 2, 0) && tp_sec_boot_complete((2u << 2) | 3u, 0));
	assert(!tp_sec_boot_complete(2u << 2, 1) && !tp_sec_boot_complete(3u << 2, 0) && !tp_sec_boot_complete(0, 0));
	printf("PASS sectouch frames: request bytes, reply length, acceptance test, boot-complete event\n");
}

static void test_good_run(void)
{
	reset();
	ready_at = 1;	/* ready as soon as it answers */
	touch_probe_run(RECIPE);
	assert(bad_mode == 0 && bad_cs == 0);
	assert(has("rst: boot-complete event seen"));
	assert(has("identify device-id reg 0x22: sync=aa echo=22 st=00 data=53 79 11 22 33 -> sec_ts frame ok"));
	assert(has("identify: boot status 0x20 = app"));
	assert(has("identify: 3/3 sec_ts frames ok -> S6SY79x answers"));
	assert(has("rst=ready id=sec-ts-ok-app"));
	assert(has("phase 1 result: chip answered"));
	assert(nreq_bad == 0);
	/* the reset pulse came before the first request, and the probe waited the sec_ts boot delay (sec_ts.c:4731-4733) first */
	assert(reset_high_at && nreq >= 4 && first_req_at >= reset_high_at + 80000u);
	/* sec_ts_spi_delay: >= 100 us between the request message and the reply message (sec_ts.c:61,95,401) */
	assert(min_gap != ~0ull && min_gap >= 100u);

	reset();
	ready_at = 1;
	boot_status = 0x10;
	touch_probe_run(RECIPE);
	assert(has("boot (bootloader") && has("id=sec-ts-ok-boot"));

	/* boot-complete arrives after a few 20 ms polls */
	reset();
	ready_at = 1000 + 300000;
	touch_probe_run(RECIPE);
	assert(has("rst: boot-complete event seen") && has("id=sec-ts-ok-app"));

	/* without rst there is no reset pulse: the chip (not booted from our reset) does not answer, which is reported */
	reset();
	touch_probe_run(TP_O_MUX | TP_O_CS | TP_O_OSC);
	assert(bad_mode == 0 && bad_cs == 0);
	assert(has("id=rx-no-chip"));
	printf("PASS sectouch good runs: mode 3 and chip select low on every byte, boot wait, three good frames, boot/app status\n");
}

static void test_failures(void)
{
	uint64_t t0;

	/* no boot-complete event ever: at most 100 tries, bounded in time, then the identify still runs */
	reset();
	t0 = fake_now;
	touch_probe_run(RECIPE);
	assert(has("no boot-complete event after 100 tries"));
	assert(fake_now - t0 < 5000000u);
	assert(has("id=sec-ts-ok-app"));	/* the chip answers reads even without the event */

	/* no chip, MISO high */
	reset();
	chip = 0;
	miso_high = 1;
	touch_probe_run(RECIPE);
	assert(has("no answer, MISO high") && has("0/3 sec_ts frames ok") && has("id=rx-no-chip"));
	assert(has("phase 1 result: chip did not answer"));

	/* no chip, MISO low */
	reset();
	chip = 0;
	touch_probe_run(RECIPE);
	assert(has("no answer, MISO low") && has("id=rx-no-chip"));

	/* bad checksum: never accepted */
	reset();
	ready_at = 1;
	corrupt = 1;
	touch_probe_run(RECIPE);
	assert(has("-> bad frame") && has("0/3 sec_ts frames ok") && !has("sec-ts-ok"));

	/* SPI engine stalls: the first transfer times out, nothing hangs */
	reset();
	spi_ok = 0;
	t0 = fake_now;
	touch_probe_run(RECIPE);
	assert(has("rst: event read timed out"));
	assert(has("SPI transfer timed out"));
	assert(has("id=stall") || has("id=rx-no-chip"));
	assert(fake_now - t0 < 5000000u);

	/* mux refused (bus not driven to the AP and no mux option): no SPI traffic at all */
	reset();
	*slot(TOUCH_PIN_MUX_BANK + 4) |= 1u << TOUCH_PIN_MUX_NR;
	touch_probe_run(TP_O_CS | TP_O_OSC | TP_O_RST);
	assert(nreq == 0 && has("mux=refused"));
	printf("PASS sectouch failures: no event (bounded), no chip (MISO high/low), bad checksum, SPI stall, mux refused\n");
}

/* plain run (no experiment options): the first identify fails, the A-E fallback re-inits the engine, which must keep mode 3
 * (review finding 3: s3c64xx_spi_hwinit wrote CH_CFG = 0) */
static void test_fallback(void)
{
	reset();
	reset_high_at = 1;	/* the chip booted at power-on (no rst option) */
	fake_now = 200000;	/* ... long enough ago */
	ready_at = 1;
	ignore_reqs = 3;	/* the three identify reads go unanswered once */
	*slot(TOUCH_PIN_MUX_BANK) = (*slot(TOUCH_PIN_MUX_BANK) & ~(0xfu << (TOUCH_PIN_MUX_NR * 4))) | (1u << (TOUCH_PIN_MUX_NR * 4));	/* AP drives mux low */
	touch_probe_run(0);
	assert(has("VARIANT A completes"));
	assert(bad_mode == 0);
	assert(has("3/3 sec_ts frames ok") && has("id=sec-ts-ok-app"));
	printf("PASS sectouch fallback: variant A re-init keeps SPI mode 3, the second identify answers\n");
}

/* every fallback variant re-inits the engine through spi_engine_init and must keep mode 3 and the chip select low:
 * the engine stalls until variants A..(n-1) have timed out, so variant n is the one that completes */
static void test_each_variant(void)
{
	for (unsigned n = 1; n < TP_NVARIANTS; n++) {
		char want[32];

		reset();
		reset_high_at = 1;
		fake_now = 200000;
		ready_at = 1;
		spi_ok = 0;
		stall_variants = n;
		*slot(TOUCH_PIN_MUX_BANK) = (*slot(TOUCH_PIN_MUX_BANK) & ~(0xfu << (TOUCH_PIN_MUX_NR * 4))) | (1u << (TOUCH_PIN_MUX_NR * 4));
		touch_probe_run(0);
		snprintf(want, sizeof(want), "VARIANT %c completes", (char)('A' + n));
		assert(variant_timeouts == n && has(want));
		assert(bad_mode == 0 && bad_cs == 0);
		assert(has("3/3 sec_ts frames ok") && has("id=sec-ts-ok-app"));
	}
	printf("PASS sectouch variants: B..E each complete with SPI mode 3 and the chip select low\n");
}

/* review finding 1: a dead counter must not hang any delay or poll of the sec_ts path */
static void test_dead_counter(void)
{
	reset();
	ready_at = 1;
	frozen = 1;
	touch_probe_run(RECIPE);
	assert(has("opts=0x27 mux="));	/* the summary line was reached */
	frozen = 0;
	printf("PASS sectouch dead counter: the run ends (bounded delays and polls)\n");
}

/* review finding 2: a read longer than the probe's buffer is refused before anything is sent */
static void test_length(void)
{
	struct tp t = { 0, 0, 0, 0, RECIPE };
	uint64_t w0 = 1, w1 = 1;
	unsigned before;

	reset();
	before = nw;
	assert(sec_read(&t, TP_SEC_READ_ONE_EVENT, TP_SEC_MAX_LEN + 1u, &w0, &w1) == -3);
	assert(nw == before && w0 == 1 && w1 == 1);
	assert(TP_SEC_MAX_LEN == 8u && tp_sec_reply_len(TP_SEC_MAX_LEN) == 16u);
	printf("PASS sectouch length: len > %u refused with nothing sent\n", TP_SEC_MAX_LEN);
}

/* every write of a recipe run goes through w32 and lands only where touch_host_test's allowlist expects (same list) */
static void test_writes(void)
{
	reset();
	ready_at = 1;
	touch_probe_run(RECIPE);
	for (unsigned i = 0; i < nw; i++) {
		uintptr_t pa = wlog[i];
		int ok = (pa >= SPI && pa < SPI + 0x30 && pa != SPI + TP_SPI_STATUS && pa != SPI + TP_SPI_RX_DATA) ||
			 pa == USI + TP_USI_CON || pa == USI + TP_USI_OPTION || pa == TOUCH_USI_MODE_REG ||
			 pa == CMU + TOUCH_CMU_MUX_USI || pa == CMU + TOUCH_CMU_DIV || pa == CMU + TOUCH_CMU_GATE_IPCLK ||
			 pa == CMU + TOUCH_CMU_GATE_PCLK || pa == CMU + TOUCH_CMU_GATE_RSTN || pa == TOP + TOUCH_CMUTOP_GATE ||
			 pa == TOUCH_PIN_CS_BANK || pa == TOUCH_PIN_CS_BANK + 4 || pa == TOUCH_PIN_CS_BANK + 8 ||
			 pa == TOUCH_PIN_MUX_BANK || pa == TOUCH_PIN_MUX_BANK + 4 || pa == TOUCH_PIN_MUX_BANK + 8;
		if (!ok)
			fprintf(stderr, "unexpected write pa=%#lx\n", (unsigned long)pa);
		assert(ok);
	}
	assert(!has("REFUSED"));
	printf("PASS sectouch writes: %u writes, all inside the touch allowlist, none refused\n", nw);
}

int main(void)
{
	test_frames();
	test_good_run();
	test_failures();
	test_fallback();
	test_each_variant();
	test_dead_counter();
	test_length();
	test_writes();
	printf("PASS sectouch_host_test\n");
	return 0;
}
