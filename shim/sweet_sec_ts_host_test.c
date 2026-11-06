/* Host unit test for the pure sec_ts protocol layer of the gs101 touch driver (xnu patch 0146, bsd/dev/sweet_sec_ts.h).
 * shim/build.sh extracts that header from the patch, so the kernel and this test always see the same bytes. Build (also run by
 * shim/build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -Iout -o out/sweet_sec_ts_host_test sweet_sec_ts_host_test.c
 * Events are encoded through a copy of the downstream bitfield struct (sec_ts.h:832-845, GCC lays the first field in the low bits),
 * so the header's hand-written shifts are checked against the compiler's layout, not against themselves. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sweet_sec_ts.h"

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

struct coord {          /* struct sec_ts_event_coordinate, sec_ts.h:832-845 */
	uint8_t eid:2;
	uint8_t tid:4;
	uint8_t tchsta:2;
	uint8_t x_11_4;
	uint8_t y_11_4;
	uint8_t y_3_0:4;
	uint8_t x_3_0:4;
	uint8_t major;
	uint8_t minor;
	uint8_t z:6;
	uint8_t ttype_3_2:2;
	uint8_t left_event:6;
	uint8_t ttype_1_0:2;
} __attribute__((packed));

static void
mk(uint8_t *e, unsigned tid, unsigned action, int x, int y, unsigned ttype, unsigned left)
{
	struct coord c;

	_Static_assert(sizeof(c) == SEC_TS_EVENT_SIZE, "event size");
	memset(&c, 0, sizeof(c));
	c.eid = SEC_TS_EID_COORDINATE;
	c.tid = tid & 0xfu;
	c.tchsta = action & 3u;
	c.x_11_4 = (uint8_t)(x >> 4);
	c.x_3_0 = x & 0xf;
	c.y_11_4 = (uint8_t)(y >> 4);
	c.y_3_0 = y & 0xf;
	c.major = 10;
	c.minor = 8;
	c.z = 0x20;
	c.ttype_3_2 = (ttype >> 2) & 3u;
	c.ttype_1_0 = ttype & 3u;
	c.left_event = left & 0x3fu;
	memcpy(e, &c, sizeof(c));
}

int
main(void)
{
	uint8_t f[16], e[SEC_TS_EVENT_SIZE], r[16];
	struct sec_ts_ev v;
	unsigned n, px, py;

	/* read request: the bytes the shim sent for BOOT_STATUS (touch-run1, accepted by the chip) */
	sec_ts_read_req(f, SEC_TS_READ_BOOT_STATUS, 1);
	EXPECT(f[0] == 0xaa && f[1] == 0 && f[2] == 1 && f[3] == 0 && f[4] == 1 && f[5] == 0x55);
	EXPECT(f[6] == (uint8_t)(0xaa + 1 + 1 + 0x55) && f[7] == 0);
	sec_ts_read_req(f, SEC_TS_READ_ALL_EVENT, 31u * 8u);
	EXPECT(f[3] == 0 && f[4] == 248 && f[5] == 0x61);

	/* reply length: header 7 + data + checksum 1, multiple of 4 */
	EXPECT(sec_ts_reply_len(1) == 12 && sec_ts_reply_len(5) == 16 && sec_ts_reply_len(8) == 16 && sec_ts_reply_len(9) == 20);
	EXPECT(sec_ts_reply_len(248) == 256);

	/* observed reply (touch-run1): BOOT_STATUS = aa 00 03 00 00 55 00 20, checksum = byte sum */
	memset(r, 0, sizeof(r));
	memcpy(r, "\xaa\x00\x03\x00\x00\x55\x00\x20", 8);
	r[8] = (uint8_t)(0xaa + 0x03 + 0x55 + 0x20);
	EXPECT(sec_ts_reply_ok(r, SEC_TS_READ_BOOT_STATUS, 1));
	EXPECT(r[SEC_TS_RD_HDR] == SEC_TS_STATUS_APP_MODE);
	EXPECT(!sec_ts_reply_ok(r, SEC_TS_READ_ID, 1));          /* wrong register echo */
	r[8] ^= 1;
	EXPECT(!sec_ts_reply_ok(r, SEC_TS_READ_BOOT_STATUS, 1)); /* checksum */
	r[8] ^= 1;
	r[0] = 0xab;
	r[8] = (uint8_t)(r[8] + 1);                              /* checksum consistent with the wrong sync byte */
	EXPECT(!sec_ts_reply_ok(r, SEC_TS_READ_BOOT_STATUS, 1)); /* sync alone */
	memset(r, 0, sizeof(r));
	EXPECT(!sec_ts_reply_ok(r, SEC_TS_READ_BOOT_STATUS, 1)); /* MISO stuck low: sync fails even though the sum matches */

	/* write frames: SENSE_ON (no data) and SET_TOUCHFUNCTION 0x0061 little-endian, as sec_ts_write builds them */
	memset(f, 0xee, sizeof(f));
	n = sec_ts_write_frame(f, SEC_TS_CMD_SENSE_ON, NULL, 0);
	EXPECT(n == 8);
	EXPECT(f[0] == 0xaa && f[1] == 0 && f[2] == 1 && f[3] == 0 && f[4] == 0 && f[5] == 0x10);
	EXPECT(f[6] == (uint8_t)(0xaa + 1 + 0x10) && f[7] == 0);
	{
		const uint8_t d[2] = { 0x61, 0x00 };
		memset(f, 0xee, sizeof(f));
		n = sec_ts_write_frame(f, SEC_TS_CMD_SET_TOUCHFUNCTION, d, 2);
		EXPECT(n == 12);
		EXPECT(f[1] == 0 && f[2] == 3 && f[5] == 0x30 && f[6] == 0x61 && f[7] == 0x00);
		EXPECT(f[8] == (uint8_t)(0xaa + 3 + 0x30 + 0x61) && f[9] == 0 && f[10] == 0 && f[11] == 0);
	}
	{
		const uint8_t d[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
		EXPECT(sec_ts_write_frame(f, SEC_TS_CMD_SENSE_ON, d, 9) == 0);   /* too long: refused */
		n = sec_ts_write_frame(f, SEC_TS_CMD_SENSE_ON, d, 8);            /* the longest legal frame: 6 + 8 + 1 -> 16 */
		EXPECT(n == 16 && f[2] == 9 && f[13] == 8 && f[14] == (uint8_t)(0xaa + 9 + 0x10 + 36) && f[15] == 0);
	}

	/* boot-complete event observed on raven (touch-run1): 09 00 10 00 00 00 00 00 */
	memcpy(e, "\x09\x00\x10\x00\x00\x00\x00\x00", 8);
	EXPECT(sec_ts_boot_complete(e));
	EXPECT(!sec_ts_watchdog_reset(e));                         /* status_data_1 0x10, not 0x20 */
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_STATUS);
	e[2] = SEC_TS_BOOT_WATCHDOG;
	EXPECT(sec_ts_watchdog_reset(e));
	e[0] = 0x08;                                               /* same type bits, but eid 0: a coordinate event */
	EXPECT(!sec_ts_watchdog_reset(e) && !sec_ts_queue_full(e));
	e[0] = 0x09;
	e[0] = (uint8_t)(1u | (SEC_TS_STYPE_VENDOR_INFO << 2));
	e[2] = 0;
	EXPECT(sec_ts_boot_complete(e) && !sec_ts_watchdog_reset(e));
	e[1] = 0x40;                                               /* a vendor ack, not boot complete */
	EXPECT(!sec_ts_boot_complete(e));
	memset(e, 0, sizeof(e));
	EXPECT(!sec_ts_boot_complete(e) && sec_ts_parse_event(e).kind == SEC_TS_NONE);

	/* review 1 F3: ERR status, id QUEUE_FULL (sec_ts.c:3232-3240); not ESD, not INFO, not a coordinate event */
	memcpy(e, "\x05\x01\x00\x00\x00\x00\x00\x00", 8);   /* eid 1, stype 1 */
	EXPECT(sec_ts_queue_full(e) && sec_ts_parse_event(e).kind == SEC_TS_STATUS && !sec_ts_watchdog_reset(e));
	e[1] = 0x02;                                                 /* SEC_TS_ERR_EVENT_ESD */
	EXPECT(!sec_ts_queue_full(e));
	e[1] = 0x01;
	e[0] = 0x09;                                                 /* INFO, id 1 = SEC_TS_ACK_WET_MODE */
	EXPECT(!sec_ts_queue_full(e));
	e[0] = 0x04;                                                 /* eid 0 = coordinate */
	EXPECT(!sec_ts_queue_full(e));

	/* coordinate events through the compiler's bitfield layout */
	mk(e, 1, SEC_TS_ACTION_PRESS, 0, 0, SEC_TS_TT_NORMAL, 0);
	v = sec_ts_parse_event(e);
	EXPECT(v.kind == SEC_TS_DOWN && v.id == 0 && v.x == 0 && v.y == 0 && v.ttype == 0);
	mk(e, 10, SEC_TS_ACTION_MOVE, 1439, 3119, SEC_TS_TT_NORMAL, 3);
	v = sec_ts_parse_event(e);
	EXPECT(v.kind == SEC_TS_DOWN && v.id == 9 && v.x == 1439 && v.y == 3119);
	EXPECT(sec_ts_events_left(e) == 3);
	mk(e, 3, SEC_TS_ACTION_MOVE, 0xabc, 0x5d3, SEC_TS_TT_WET, 0);
	v = sec_ts_parse_event(e);
	EXPECT(v.kind == SEC_TS_DOWN && v.id == 2 && v.x == 0xabc && v.y == 0x5d3 && v.ttype == SEC_TS_TT_WET);
	mk(e, 3, SEC_TS_ACTION_PRESS, 100, 200, SEC_TS_TT_GLOVE, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_DOWN);
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, SEC_TS_TT_PALM, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_UP);           /* release whatever the type */
	mk(e, 3, SEC_TS_ACTION_PRESS, 100, 200, SEC_TS_TT_PALM, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_CANCEL);
	mk(e, 3, SEC_TS_ACTION_MOVE, 100, 200, SEC_TS_TT_GRIP, 0);
	v = sec_ts_parse_event(e);
	EXPECT(v.kind == SEC_TS_CANCEL && v.ttype == SEC_TS_TT_GRIP);
	mk(e, 3, SEC_TS_ACTION_PRESS, 100, 200, 1 /* hover */, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);
	/* review 1 F2: an unreported type is ignored for every action, its release included (sec_ts.c:2059-2068) */
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, 1 /* hover */, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, 4 /* stylus */, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, 15 /* unknown */, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, SEC_TS_TT_GRIP, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_UP);
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, SEC_TS_TT_WET, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_UP);
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, SEC_TS_TT_GLOVE, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_UP);
	mk(e, 3, SEC_TS_ACTION_RELEASE, 100, 200, SEC_TS_TT_NORMAL, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_UP);
	mk(e, 3, 0 /* ACTION_NONE */, 100, 200, SEC_TS_TT_NORMAL, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);
	mk(e, 0, SEC_TS_ACTION_PRESS, 100, 200, SEC_TS_TT_NORMAL, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);      /* tid 0 -> t_id 255, outside the table */
	mk(e, 11, SEC_TS_ACTION_PRESS, 100, 200, SEC_TS_TT_NORMAL, 0);
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);      /* t_id 10 >= MAX_SUPPORT_TOUCH_COUNT */
	mk(e, 3, SEC_TS_ACTION_PRESS, 100, 200, SEC_TS_TT_NORMAL, 0);   /* a valid finger ... */
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_DOWN);
	e[0] = (uint8_t)((e[0] & ~3u) | 2u);                       /* ... with the gesture event id */
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);
	e[0] = (uint8_t)(e[0] | 3u);                               /* empty event id */
	EXPECT(sec_ts_parse_event(e).kind == SEC_TS_IGNORED);

	/* left-event count: 6 bits; above MAX_EVENT_COUNT - 1 is an overflow */
	mk(e, 1, SEC_TS_ACTION_MOVE, 1, 1, 0, 31);
	EXPECT(sec_ts_events_left(e) == 31);
	mk(e, 1, SEC_TS_ACTION_MOVE, 1, 1, 0, 32);
	EXPECT(sec_ts_events_left(e) == SEC_TS_MAX_EVENTS);
	mk(e, 1, SEC_TS_ACTION_MOVE, 1, 1, SEC_TS_TT_GRIP, 63);     /* ttype bits share byte 7: must not leak into the count */
	EXPECT(sec_ts_events_left(e) == SEC_TS_MAX_EVENTS);
	mk(e, 1, SEC_TS_ACTION_MOVE, 1, 1, 3, 2);
	EXPECT(sec_ts_events_left(e) == 2);

	/* panel info: x = b0<<8 | b1, y = b2<<8 | b3 */
	memcpy(r, "\x05\xa0\x0c\x30", 4);
	sec_ts_panel_size(r, &px, &py);
	EXPECT(px == 1440 && py == 3120);

	printf("sweet_sec_ts_host_test: all checks passed\n");
	return 0;
}
