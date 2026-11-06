/*
 * memprobe.c - read-only HSI2 S2MPU dump (see memprobe.h). Freestanding, no static state, no MMIO store of any kind (audited like gpus2mpu.o).
 *
 * Offsets [src: kernel/google-modules/soc/gs 20407875 (android-gs-raviole-6.1-android16) drivers/soc/google/s2mpu/s2mpu-regs.h, local copy
 * ~/git/pixel6/xnu-local/refs/soc-gs/]: CTRL0 0x00 (ENABLE bit0, :3,29), CTRL1 0x04 (:4), CFG 0x10 (:5), FAULT_STATUS 0x2000 (:22),
 * FAULT_PA_LOW(n) 0x2004 + n*0x20 (:23), FAULT_INFO(n) 0x2010 + n*0x20 (:25), L1ENTRY_L2TABLE_ADDR(n, m) 0x4000 + n*0x200 + m*8 and
 * L1ENTRY_ATTR(n, m) 0x4004 + n*0x200 + m*8 (:20-21; n = VID, m = gigabyte index = PA >> 30); attr bits L2TABLE_EN bit0, RD_ACCESS bit1,
 * WR_ACCESS bit2 (:47-64). 8 VIDs: s2mpu-lib.c init closes all of them (:314, :325); 64 granules (WHI_MAX_GB_GRANULES, s2mpu-lib.c:762).
 * Linux's own setup, for comparison (s2mpu-lib.c:327-345): with vid 1 (PCIe on HSI2) VID 0 is opened RD|WR for granules 0..3 and 34..63.
 * Lines are short on purpose: the panel clips long lines at its right edge (raven).
 */
#include <stdint.h>
#include "console.h"
#include "ramlog.h"
#include "memprobe.h"
#if SOC_GS101 && defined(MEM_S2MPU_HSI2)
#define S_CTRL0		0x000u
#define S_CTRL1		0x004u
#define S_CFG		0x010u
#define S_FAULT_STATUS	0x2000u
#define S_FAULT_PA_LOW(n)	(0x2004u + (n) * 0x20u)
#define S_FAULT_INFO(n)		(0x2010u + (n) * 0x20u)
#define S_L1_ATTR(n, m)		(0x4004u + (n) * 0x200u + (m) * 8u)
#define S_VIDS		8u
#define S_GRANULES	64u
#define A_L2EN		1u
#define A_RDWR		6u		/* RD_ACCESS | WR_ACCESS */

_Static_assert(MEM_HIGH_GB_FIRST <= MEM_HIGH_GB_LAST && MEM_HIGH_GB_LAST < S_GRANULES, "high bank granules");
_Static_assert(MEM_HIGH_GB_LAST - MEM_HIGH_GB_FIRST < 16u, "the per-bank mask is printed as 4 hex digits");

__attribute__((noipa)) static unsigned put_txt(char *dst, unsigned cap, unsigned pos, const char *s)
{
	while (*s && pos + 1 < cap)
		dst[pos++] = *s++;
	return pos;
}

__attribute__((noipa)) static unsigned put_hex(char *dst, unsigned cap, unsigned pos, uint64_t v, unsigned digits)
{
	for (unsigned i = 0; i < digits && pos + 1 < cap; i++) {
		unsigned d = (unsigned)((v >> (4 * (digits - 1 - i))) & 0xf);

		dst[pos++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
	}
	return pos;
}

static void emit(char *buf, unsigned pos)
{
	if (pos >= RAMLOG_LINE_MAX)
		pos = RAMLOG_LINE_MAX - 1;
	buf[pos] = 0;
	con_str("MEM", buf);
}

/* "<name> v=XXXXXXXX" or "<name> unreadable"; returns whether it was read */
static int rd(const char *name, uintptr_t pa, uint32_t *v)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, name);
	int ok = try_read32(pa, v);

	if (ok) {
		p = put_txt(line, sizeof(line), p, " v=");
		p = put_hex(line, sizeof(line), p, *v, 8);
	} else {
		p = put_txt(line, sizeof(line), p, " unreadable");
	}
	emit(line, p);
	return ok;
}

void mem_s2mpu_dump(void)
{
	const uintptr_t b = MEM_S2MPU_HSI2;
	const unsigned nhi = MEM_HIGH_GB_LAST - MEM_HIGH_GB_FIRST + 1u;
	const uint64_t himask = ((1ull << nhi) - 1) << MEM_HIGH_GB_FIRST;
	char line[RAMLOG_LINE_MAX];
	uint32_t ctrl0 = 0, v;
	uint64_t v0hi = 0, v0l2 = 1;		/* stays "not open" if VID 0 is never summarised */
	unsigned p;

	p = put_txt(line, sizeof(line), 0, "HSI2 S2MPU ");
	p = put_hex(line, sizeof(line), p, b, 8);
	emit(line, p);
	if (!rd("CTRL0", b + S_CTRL0, &ctrl0)) {
		p = put_txt(line, sizeof(line), 0, "VERDICT unreadable");
		emit(line, p);
		return;
	}
	rd("CTRL1", b + S_CTRL1, &v);
	rd("CFG", b + S_CFG, &v);
	rd("FAULT", b + S_FAULT_STATUS, &v);
	rd("v0 FINFO", b + S_FAULT_INFO(0), &v);
	rd("v0 FPA", b + S_FAULT_PA_LOW(0), &v);
	rd("v1 FINFO", b + S_FAULT_INFO(1), &v);
	rd("v1 FPA", b + S_FAULT_PA_LOW(1), &v);
	for (unsigned vid = 0; vid < S_VIDS; vid++) {
		uint64_t rdwr = 0, l2 = 0;
		unsigned bad = 0;

		for (unsigned m = 0; m < S_GRANULES; m++) {
			if (!try_read32(b + S_L1_ATTR(vid, m), &v)) {
				bad++;
				continue;
			}
			if ((v & A_RDWR) == A_RDWR)
				rdwr |= 1ull << m;
			if (v & A_L2EN)			/* a 2nd-level table decides: the granule is neither simply open nor closed */
				l2 |= 1ull << m;
		}
		p = put_txt(line, sizeof(line), 0, "v");
		p = put_hex(line, sizeof(line), p, vid, 1);
		p = put_txt(line, sizeof(line), p, " rw=");
		p = put_hex(line, sizeof(line), p, rdwr, 16);
		emit(line, p);
		p = put_txt(line, sizeof(line), 0, "v");
		p = put_hex(line, sizeof(line), p, vid, 1);
		p = put_txt(line, sizeof(line), p, " l2=");
		p = put_hex(line, sizeof(line), p, l2, 16);
		p = put_txt(line, sizeof(line), p, " b=");
		p = put_hex(line, sizeof(line), p, bad, 2);
		emit(line, p);
		if (vid == 0) {				/* the summary line for the high bank, VID 0 (open to everything but PCIe under Linux) */
			v0hi = (rdwr & himask) >> MEM_HIGH_GB_FIRST;
			v0l2 = (l2 & himask) >> MEM_HIGH_GB_FIRST;
			p = put_txt(line, sizeof(line), 0, "v0 hi rw=");
			p = put_hex(line, sizeof(line), p, v0hi, 4);
			p = put_txt(line, sizeof(line), p, " l2=");
			p = put_hex(line, sizeof(line), p, v0l2, 4);
			emit(line, p);
		}
	}
	/* granules MEM_HIGH_GB_FIRST..LAST are bits 0.. of the "hi" masks */
	p = put_txt(line, sizeof(line), 0, "GB ");
	p = put_hex(line, sizeof(line), p, MEM_HIGH_GB_FIRST, 2);
	p = put_txt(line, sizeof(line), p, "-");
	p = put_hex(line, sizeof(line), p, MEM_HIGH_GB_LAST, 2);
	emit(line, p);
	/* OFF: the filter passes everything. ON: VID 0 (the default for stream ids SSMT does not reassign; UFS is sid 6, Linux only moves PCIe's
	 * sids 4/5 to vid 1, gs101-s2mpu.dtsi:27-37) must be RD|WR on every high granule without a 2nd-level table. Which VID UFS has after ABL is
	 * not read (the SSMT register map is not cited here), so ON + open only means "open for VID 0". */
	if (!(ctrl0 & 1u))
		p = put_txt(line, sizeof(line), 0, "VERDICT OFF: no DMA filter");
	else if (v0hi == ((1ull << nhi) - 1) && v0l2 == 0)
		p = put_txt(line, sizeof(line), 0, "VERDICT ON, v0 hi open");
	else
		p = put_txt(line, sizeof(line), 0, "VERDICT ON, v0 hi NOT open");
	emit(line, p);
}

#else	/* not gs101 (or no mem block in soc.json): empty stub */

void mem_s2mpu_dump(void) { }

#endif
