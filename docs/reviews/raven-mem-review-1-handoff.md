# raven RAM: 10 GiB XNU window, HSI2 S2MPU probe, memory self-test: review 1 handoff (2026-11-06)

Work alone. Do not spawn subagents. Read-only review: do not edit files, commit, push, or run `fastboot`/`adb`.
Report findings as F1..Fn (blocker / should-fix / nit, file:line, concrete failure scenario), then a verdict per item on leaving
`wip/` (AGENTS.md rule 8). Tag what you checked against source vs what you infer.

## Scope

- Repo `pixelios-kernel`, local branch `wip/raven-mem` (not pushed), **5 commits** on `80e9724` (the merge of the reviewed
  `device/raven` host-test fixes into `wip/raven-sec-ts-xnu`): review `git diff 80e9724..wip/raven-mem` (14 files, +902/-3).
  - `be4bff1` shim: read-only HSI2 S2MPU dump (`--mem-probe`): `shim/memprobe.c/.h`, `shim/memprobe_host_test.c`,
    `shim/build.sh`, `shim/shim.c`, `soc/google/gs101/soc.json` (`mem.s2mpu_hsi2` + note), `tools/boards.py` (`_MEM_PINS`,
    `_mem_macros`), `tools/test-bluejay-boot.py` (object audit), `tools/test_memprobe_mutants.py`.
  - `aba7831` `docs/research/44-raven-ram.md` + `device/google/raven/hardware/mem-s2mpu-2026-11-06.txt`.
  - `44e0106` clang pragma around the `#include "memprobe.c"` in the host test (same pattern as the reviewed `9884d02`).
  - `527af3a` `xnu/patches/0148-sweet-memtest.patch` (`bsd/dev/sweet_ufs.c` only).
  - `2065d1c` `device/google/raven/board.json` `xnu_ram` 4 GiB -> 10 GiB, `hardware/ram-10g-2026-11-06.txt`, research 44 results.
- Goal: XNU on raven (Pixel 6 Pro, gs101) uses the whole 10 GiB high DRAM bank `0x880000000..0xb00000000` instead of the first 4 GiB.
- Sources (local copies): soc-gs `20407875` (`android-gs-raviole-6.1-android16`) `drivers/soc/google/s2mpu/{s2mpu-regs.h,s2mpu-lib.c}` in
  `~/git/pixel6/xnu-local/refs/soc-gs/`; kernel/devices/google/gs101 `c2916541` (`android-gs-raviole-6.1-android15-qpr2`)
  `dts/gs101-s2mpu.dtsi` in `~/git/pixel6/xnu-local/refs/devices-gs101/` (the raviole 5.10 branch no longer exists upstream).
  XNU with the stack applied: `tools/stack-apply --keep` (0001-0148).

## What each part does

- **`--mem-probe` (shim, read-only):** at `MEM_S2MPU_HSI2` (`0x14480000`) prints CTRL0/CTRL1/CFG/FAULT_STATUS, FAULT_INFO/PA_LOW for
  VID 0/1, and for all 8 VIDs x 64 granules a `rw=` mask (RD|WR) and an `l2=` mask (2nd-level table) plus failed reads, then a verdict
  for the board's highest DRAM bank (`MEM_HIGH_GB_FIRST..LAST` from `dram_banks`): OFF / ON + VID 0 open / ON + not open / unreadable.
  Guarded `try_read32` only; no power gate (HSI2 is never power-gated, per the reviewed `wlanprobe.c` header).
- **xnu 0148 (`sweetmemtest=1`, default off):** a kernel thread, `sweetmemtestdelay` s (default 60) after UFS is up:
  allocate `kmem_alloc(kernel_map, 32 MiB, KMA_DATA | KMA_NOPAGEWAIT, VM_KERN_MEMORY_DIAG)` chunks up to `sweetmemtestgb` GiB
  (default 3, max 8) while `vm_page_free_count` stays above `sweetmemtestfloor` GiB (default 5, min 1); for every 16 KiB page past
  `gPhysBase + 4 GiB`: write `(pa + 8i) ^ SEED`, `flush_dcache64(pa, PAGE_SIZE, TRUE)`, read back, count errors; for one page per new
  granule `mt_ufs_read_into(pa)`: under `sweet_ufs_lock`, READ(10) the partition's first block into the bounce buffer, copy it, poison
  the page with 0xA5, flush, set `ufs_data_pa = pa`, READ the block again (the PRDT points at `pa`), clear the override, flush, compare.
  `ufs_scsi()` uses the override only when `!write && len <= UFS_BLOCK`. Then free all chunks and print per-granule lines + RESULT.
- **`board.json` `xnu_ram`:** `["0x880000000", "0x280000000"]`. Static limits (research 44, read from source): the shim passes
  `memSize` through; mkpayload needs one bank (fits exactly); xnu-8796 `arm_vm_init` with `ARM_LARGE_MEMORY` allows ~288 GiB.

## Asked of review 1 (numbered)

1. **0148 timeout path.** If the second READ times out, `ufs_scsi()` writes UTRLCLR and returns `ETIMEDOUT`; the override is cleared, the
   page is freed a little later by `kmem_free`. Can the controller still complete that DMA after UTRLCLR (into a page that is free or
   reused)? Same question for an error/abort. If unprovable, what is the safe pattern (keep the page allocated forever on error? reset
   the host?). The normal path has the same property for the bounce buffer, but that buffer is never freed.
2. **0148 cache maintenance.** Poison via the physmap (`phystokv`), `flush_dcache64(pa, 4096, TRUE)` before and after the DMA, compare
   through the same VA. Correct for a non-coherent DMA master on this SoC? Is anything missing (a DSB between the poison flush and the
   doorbell beyond what `ufs_scsi()` already does)? Same for the pattern test: does `flush_dcache64` by PA really force the read-back to
   come from DRAM (or could a prefetch refill the line between the flush and the read, making the test only a cache test)?
3. **0148 override scope.** `ufs_data_pa` is a plain static, set/cleared only inside `mt_ufs_read_into` under `sweet_ufs_lock`, and every
   `ufs_scsi` caller holds that lock (check). Any path where a normal READ could see a non-zero override (e.g. the persistent-log
   writer, `sweet_log_*`, which issues writes; strategy reads)?
4. **0148 memory safety.** `kmem_alloc` of 32 MiB chunks with `KMA_NOPAGEWAIT` on `kernel_map` while iOS runs: the free-count check
   races with other allocators (bounded by one chunk?); 8 GiB of `kernel_map` VA OK? `pmap_find_phys` per page; static arrays in a
   one-shot thread; `extern vm_map_address_t phystokv(uint64_t)` declared in bsd/ against `pmap_paddr_t` (`uint64_t
   __kernel_ptr_semantics`, `osfmk/arm/pmap_public.h:39-40`): acceptable?
5. **0148 default-off.** With no `sweetmemtest` boot-arg nothing changes: confirm `sweet_memtest_start` returns early and `ufs_scsi`
   behaves identically (the only change there is `data_pa`, equal to `bounce_pa` when the override is 0).
6. **memprobe.** Strictly read-only (source and object audit)? Register offsets and the 8-VID count against `s2mpu-regs.h:3-5,20-25`,
   `s2mpu-lib.c:314,325`. The verdict's assumption that UFS uses VID 0 (SSMT not read) is stated in the code and the record: acceptable?
   `tools/boards.py _mem_macros` (highest bank, 1 GiB alignment, < 64 GiB) and the generated header for bluejay (`0x22..0x25`).
7. **Evidence for 10 GiB.** `hardware/ram-10g-2026-11-06.txt`: runs 3-5 together pass every new granule `0x26..0x2b` (569,337 page
   tests, 0 errors; 11 UFS READ DMAs into those granules matching). Is that sufficient (AGENTS rule 7) to make 10 GiB the default
   `xnu_ram`, given the stated limits (one pattern pass, READ-direction DMA only)? Anything the static-limit analysis in research 44 missed
   (e.g. a SWEET patch that sizes something from `mem_size`, zone/kalloc maps, the 1 GiB `ARM64_PHYSMAP_SLIDE_RANGE`)?
8. **Tests.** `memprobe_host_test.c` (fake register file; no `try_write32` to link) and `test_memprobe_mutants.py` (13 mutants, all
   killed): gaps? 0148 has no host test (kernel-only code): is that acceptable for a default-off diagnostic, or should the
   granule/verdict arithmetic move into a pure header with a host test like `sweet_sec_ts.h`?

## Verification done

- `tools/stack-apply --only-new 0148`: PASS (137/137 patches + 3 stagers). Clean local kernel build (darwin-xnu-build `663906528d1c`,
  Xcode 15.4): no new warning classes in `sweet_ufs.c` (only the file's existing `-Wdeclaration-after-statement`).
- `memprobe_host_test` passes for raven (granules 22-2B) and bluejay (22-25), with `-Werror` on Apple clang (no real gcc on this Mac; the Linux gate builds it with gcc);
  `test_memprobe_mutants`: 13/13 killed; `tools/test-bluejay-boot.py` on the raven `--mem-probe` image: PASS
  (`memprobe object audit: 26 stores, all stack-based`); `check-rules --strict`: PASS; `shellcheck shim/build.sh`: clean; board/rule/gate
  unit tests (101): OK. `tools/check-all.sh` not run (macOS); the Linux/Docker gate is a separate thread.
- Hardware, observed 2026-11-06 (records in `device/google/raven/hardware/`):
  - `mem-s2mpu-2026-11-06.txt`: HSI2 S2MPU CTRL0 = 0 (disabled), all L1 entries of 8 VIDs = 0, no fault.
  - `ram-10g-2026-11-06.txt`: run 1 (10 GiB, read-only root) panics at the root mount (expected, `ufsrw` off; no log); run 2 (10 GiB,
    `--rw`) lock screen + touch, `vm_page_bootstrap 650163 free + 5197 wired`; runs 3-5 (`sweetmemtest`) as above, each with
    lock screen, touch, no panic.
- Images were built on macOS where `shim/build.sh` still stops at `usbprobe_host_test` after writing the image (known, not this change).

## Not in scope / known open

- The SSMT (stream-id to VID map) of `s2mpu_hsi2` is not read: its register map is not cited anywhere in the repo.
- DMA in the write direction (memory to device) into the new granules is not tested (UFS writes always come from the low bounce buffer).
- The low banks (1.82 GiB with holes) stay unused by XNU.
