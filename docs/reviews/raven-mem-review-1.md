# raven RAM (10 GiB XNU window): review 1 (2026-11-06)

Independent review (different model family) of `docs/reviews/raven-mem-review-1-handoff.md`, as relayed by the user.
Line references are to the files as they were then (`wip/raven-mem` at `2065d1c`). Disposition of each finding:
`raven-mem-review-2-handoff.md`.

## Findings

- **F1 (blocker, P0)**, 0148 `ufs_scsi` timeout + `mt_ufs_read_into` (`0148-sweet-memtest.patch:103`): if the direct READ
  times out, `ufs_scsi()` writes `UTRLCLR` and returns without waiting for `UTRLDBR` to clear or otherwise proving the DMA
  stopped. `mt_ufs_read_into()` then clears `ufs_data_pa`, the test continues and finally frees every chunk; a late completion
  can DMA into freed or reused kernel memory. The permanent bounce buffer has no such lifetime problem. Source-confirmed up to
  the missing quiescence check; the corruption is the resulting inference. The shim and the other local UFS implementation
  both wait for the doorbell after `UTRLCLR`. Safe options: quiesce the host and confirm the slot inactive before freeing; or
  treat `ETIMEDOUT` as fatal (refuse further UFS commands and keep the allocations, or panic while they are allocated).
  Leaking only the page while normal UFS traffic continues is not enough: the same slot and UCD get reused.
  0148 stays on `wip/` until fixed.

## Verdicts

1. 0148 timeout: **block** (F1). Commands returning `EIO` have already cleared the doorbell; the unproven case is `ETIMEDOUT`.
2. Cache maintenance: pass. The SWEET build uses the non-coherent `flush_dcache64` (PA -> `phystokv_range`, clean+invalidate to
   PoC, `dsb sy`). The pre-DMA flush orders the poison before the doorbell; the post-DMA flush invalidates before the compare; a
   later prefetch must refill from DRAM, so the check is not cache-only.
3. Override scope: pass. Strategy, logging, init and memtest paths serialize `ufs_scsi()` on `sweet_ufs_lock`; writes ignore
   the override; it accepts only reads of at most one block.
4. Memory safety: pass except F1. The 4 KiB DMA stays inside one physically contiguous 16 KiB page; `KMA_NOPAGEWAIT` fails
   cleanly; the free-count floor is advisory (concurrent allocators can cross it), acceptable for an opt-in diagnostic; 8 GiB of
   VA fits the large-memory kernel VA layout; the `phystokv` declaration matches the arm64 type.
5. Default-off: pass. Without `sweetmemtest=1` no thread starts and `ufs_data_pa` stays 0; PRDT addresses stay the bounce
   buffer; only an inert load/conditional is added.
6. Memprobe: pass. Offsets, attribute bits, 8 VIDs, 64 granules and the HSI2 base match the cited Google sources; reads only;
   the source/link and object audits cover accidental writers; the VID-0/SSMT limit is stated accurately.
7. 10 GiB evidence: pass. CPU access in every new 1 GiB granule and device-to-memory UFS DMA in each, with the HSI2 S2MPU
   observed disabled. Limits as documented: one pattern, no retention/stress test, READ-direction DMA only. The unsafe timeout
   path was not exercised, so F1 does not invalidate the hardware results. `ARM64_PHYSMAP_SLIDE_RANGE` is random padding, not a
   cap on mapped RAM.
   Note (non-blocking): `machine_info.memory_size` is 32-bit and truncates 10 GiB to 2 GiB; XNU's own comment says it should
   be capped at 2 GiB. VM sizing uses the wider `mem_size`/`max_mem`, so this is not a new limit.
8. Tests: adequate after F1. Memprobe host and mutation coverage is strong. Moving the memtest arithmetic into a pure header is
   optional and would not cover the controller-lifetime failure; a timeout fault-injection test would be worth more if a
   practical kernel test mechanism exists.

Overall: `be4bff1`, `44e0106`, `aba7831`, `2065d1c` acceptable to leave `wip/`; `527af3a` (0148) is not, so the branch does not
leave `wip/` as a unit until F1 is resolved.

The reviewer made no repository, device or remote changes.
