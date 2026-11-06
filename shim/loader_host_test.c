#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "payload.h"

int main(void)
{
	uint8_t initrd[9000];
	memset(initrd, 0x5a, sizeof(initrd));
	memcpy(initrd + 4096, "SWEETXNU", 8);
	assert(shim_find_magic((uintptr_t)initrd, (uintptr_t)initrd + sizeof(initrd), "SWEETXNU", 8) ==
	       (uintptr_t)initrd + 4096);
	assert(shim_find_magic((uintptr_t)initrd, (uintptr_t)initrd + 4096, "SWEETXNU", 8) == 0);

	/* shim_align_payload: an unaligned payload slides down to the 8-byte boundary, bytes intact (raven, LineageOS). */
	static uint8_t rd[256] __attribute__((aligned(8)));
	uintptr_t base = (uintptr_t)rd, end = base + sizeof(rd);
	for (unsigned off = 0; off < 16; off++) {
		memset(rd, 0xee, sizeof(rd));
		uintptr_t at = base + 64 + off;
		for (uintptr_t q = at; q < end; q++)
			*(uint8_t *)q = (uint8_t)(q - at);
		uintptr_t got = shim_align_payload(base, at, end);
		assert(got == (at & ~(uintptr_t)7) && (got & 7) == 0);
		for (uintptr_t q = 0; q < end - at; q++)
			assert(((uint8_t *)got)[q] == (uint8_t)q);
	}
	assert(shim_align_payload(base, base + 8, end) == base + 8);	/* aligned: untouched */
	assert(shim_align_payload(base + 4, base + 6, end) == 0);	/* boundary below start: refused */
	assert(shim_align_payload(base + 16, base + 8, end) == 0);	/* addr below start */
	assert(shim_align_payload(base, base + 9, base + 8) == 0);	/* end below addr */
	return 0;
}
