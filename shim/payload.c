#include "payload.h"

uintptr_t shim_find_magic(uintptr_t start, uintptr_t end, const char *magic, unsigned len)
{
	if (!len || end < start || end - start < len)
		return 0;
	for (uintptr_t p = start; p <= end - len; p++) {
		unsigned i;
		for (i = 0; i < len; i++)
			if (*(volatile const uint8_t *)(p + i) != (uint8_t)magic[i])
				break;
		if (i == len)
			return p;
	}
	return 0;
}

/*
 * ABL places the vendor_boot ramdisk first and ours right after it, unpadded, so the payload's alignment depends on the
 * vendor ramdisk's length (LineageOS raven, 2026-11-05: 0x1A10A22 bytes, payload at ...A22 -> alignment fault on the
 * first header load). Slide [addr, end) down to the 8-byte boundary below addr, overwriting at most 7 trailing bytes
 * of the vendor ramdisk, which the shim never reads. Byte copy, ascending: safe for this downward overlap and for any
 * alignment. Returns the aligned address, or 0 if the boundary would fall below start.
 */
uintptr_t shim_align_payload(uintptr_t start, uintptr_t addr, uintptr_t end)
{
	uintptr_t to = addr & ~(uintptr_t)7;
	if (addr < start || end < addr || to < start)
		return 0;
	if (to == addr)
		return addr;
	for (uintptr_t i = 0; i < end - addr; i++)
		*(volatile uint8_t *)(to + i) = *(volatile const uint8_t *)(addr + i);
	return to;
}
