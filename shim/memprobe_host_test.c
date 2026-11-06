/* Host test for memprobe.c (--mem-probe): a fake HSI2 S2MPU register window. try_write32 is deliberately NOT defined, so linking fails if the dump
 * ever references a writer. Uses the REAL generated board macros (raven or bluejay board_config.h). Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -I. -Iout -include out/board_config.h -o out/memprobe_host_test memprobe_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "memprobe.c"

#if !(SOC_GS101 && defined(MEM_S2MPU_HSI2))
#error "needs a gs101 board_config.h with the mem block"
#endif

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static char lines[256][RAMLOG_LINE_MAX + 8];
static unsigned nl;
static int ctrl0_readable, enabled, bad_vid = -1;
static uint32_t attr[8][64];
static unsigned reads, outside;

void con_str(const char *label, const char *s)
{
	assert(nl < 256);
	assert(strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", label, s);
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	uintptr_t off;

	reads++;
	if (pa < MEM_S2MPU_HSI2 || pa >= MEM_S2MPU_HSI2 + 0x10000) {
		outside++;
		return 0;
	}
	off = pa - MEM_S2MPU_HSI2;
	if (off == 0) {
		if (!ctrl0_readable)
			return 0;
		*v = enabled ? 0x1u : 0x6u;			/* ENABLE alone, or every bit but ENABLE */
		return 1;
	}
	if (off >= 0x4000 && off < 0x4000 + 8 * 0x200 && (off & 7) == 4) {
		if ((int)((off - 0x4000) / 0x200) == bad_vid)
			return 0;
		*v = attr[(off - 0x4000) / 0x200][((off - 0x4000) % 0x200) / 8];
		return 1;
	}
	*v = 0x10000000u + (uint32_t)off;			/* any other register: a recognisable value */
	return 1;
}

static int has(const char *want)
{
	for (unsigned i = 0; i < nl; i++)
		if (strcmp(lines[i], want) == 0)
			return 1;
	return 0;
}

static int has_prefix(const char *want)
{
	for (unsigned i = 0; i < nl; i++)
		if (strncmp(lines[i], want, strlen(want)) == 0)
			return 1;
	return 0;
}

static void run(void)
{
	nl = 0;
	reads = 0;
	outside = 0;
	mem_s2mpu_dump();
}

/* VID 0 the way Linux leaves it (s2mpu-lib.c:327-345): RD|WR on granules 0..3 and 34..63 */
static void linux_open(void)
{
	memset(attr, 0, sizeof(attr));
	for (unsigned m = 0; m < 64; m++)
		if (m < 4 || m >= 34)
			attr[0][m] = 6;
}

int main(void)
{
	const unsigned first = MEM_HIGH_GB_FIRST, last = MEM_HIGH_GB_LAST;
	char want[64];

	/* CTRL0 unreadable: one verdict, nothing else read */
	ctrl0_readable = 0;
	run();
	EXPECT(has("MEM VERDICT unreadable") && has("MEM CTRL0 unreadable") && reads == 1 && outside == 0);

	/* disabled: the masks are still printed, the verdict says there is no filter */
	ctrl0_readable = 1;
	enabled = 0;
	memset(attr, 0, sizeof(attr));
	run();
	EXPECT(has("MEM VERDICT OFF: no DMA filter") && has("MEM CTRL0 v=00000006") && outside == 0);
	EXPECT(has("MEM v0 rw=0000000000000000") && has("MEM v7 l2=0000000000000000 b=00"));
	EXPECT(reads == 1 + 7 + 8 * 64);			/* CTRL0, 7 other registers, 8 VIDs x 64 granules */

	/* enabled, VID 0 open as under Linux: the whole high bank is open */
	enabled = 1;
	linux_open();
	run();
	EXPECT(has("MEM VERDICT ON, v0 hi open") && has("MEM CTRL0 v=00000001"));
	EXPECT(has("MEM v0 rw=FFFFFFFC0000000F"));
	snprintf(want, sizeof(want), "MEM v0 hi rw=%04X l2=0000", (1u << (last - first + 1)) - 1);
	EXPECT(has(want));
	snprintf(want, sizeof(want), "MEM GB %02X-%02X", first, last);
	EXPECT(has(want));

	/* one high granule closed (the last one of the bank): not open */
	attr[0][last] = 0;
	run();
	EXPECT(has("MEM VERDICT ON, v0 hi NOT open"));
	snprintf(want, sizeof(want), "MEM v0 hi rw=%04X l2=0000", (1u << (last - first)) - 1);
	EXPECT(has(want));

	/* read-only granule: RD without WR does not count as open */
	linux_open();
	attr[0][first] = 2;
	run();
	EXPECT(has("MEM VERDICT ON, v0 hi NOT open"));

	/* a 2nd-level table on a high granule decides per page: not simply open */
	linux_open();
	attr[0][first] = 6 | 1;
	run();
	EXPECT(has("MEM VERDICT ON, v0 hi NOT open"));
	snprintf(want, sizeof(want), "MEM v0 hi rw=%04X l2=0001", (1u << (last - first + 1)) - 1);
	EXPECT(has(want));

	/* granules outside the high bank do not affect the verdict; other VIDs are reported but not judged */
	linux_open();
	attr[0][0] = 0;
	attr[0][63] = 0;
	attr[1][first] = 6;
	run();
	EXPECT(has("MEM VERDICT ON, v0 hi open") == (last < 63));
	EXPECT(has_prefix("MEM v1 rw=") && outside == 0);
	snprintf(want, sizeof(want), "MEM v1 rw=%016llX", 1ull << first);	/* exact per-VID mask: the VID stride is 0x200 */
	EXPECT(has(want));
	attr[7][63] = 6 | 1;
	run();
	EXPECT(has("MEM v7 rw=8000000000000000") && has("MEM v7 l2=8000000000000000 b=00"));

	/* unreadable L1 entries are counted per VID and never count as open */
	linux_open();
	bad_vid = 0;
	run();
	EXPECT(has("MEM v0 rw=0000000000000000") && has("MEM v0 l2=0000000000000000 b=40") && has("MEM VERDICT ON, v0 hi NOT open"));
	EXPECT(has("MEM v1 l2=0000000000000000 b=00"));
	bad_vid = -1;

	/* every line fits the panel's useful width (it clips at the right edge on raven) */
	for (unsigned i = 0; i < nl; i++)
		EXPECT(strlen(lines[i]) <= 4 + 28);

	printf("memprobe_host_test: all checks passed (granules %02X-%02X)\n", first, last);
	return 0;
}
