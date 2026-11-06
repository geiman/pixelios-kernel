#pragma once
#include <stdint.h>

/*
 * memprobe.h - STRICTLY read-only dump of the HSI2 S2MPU (build.sh --mem-probe, gs101 only; docs/research/44).
 * The S2MPU sits between the UFS / PCIe DMA masters and DRAM and allows or blocks DMA per VID and per 1 GiB granule (PA >> 30).
 * XNU's RAM window would grow from granules 0x22..0x25 (4 GiB, UFS DMA observed working there) to the whole high bank;
 * this dump says whether the filter is enabled after ABL and, if it is, which granules each VID may read and write.
 * Base: soc.json mem.s2mpu_hsi2 (MEM_S2MPU_HSI2), the bank's granules MEM_HIGH_GB_FIRST..LAST (tools/boards.py _mem_macros).
 * HSI2 is never power-gated (wlanprobe.c header: pd_hsi2 is disabled in the DT), so no power gate; every read is guarded.
 * No MMIO store of any kind: audited like gpus2mpu.o (tools/test-bluejay-boot.py).
 */
void mem_s2mpu_dump(void);

/* Fault-guarded 32-bit MMIO read (shim.c; host tests substitute a fake window). */
int try_read32(uintptr_t address, uint32_t *value);
