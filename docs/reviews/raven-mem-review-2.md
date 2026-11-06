# raven RAM (10 GiB XNU window): review 2 (2026-11-06)

Independent review (different model family) of `docs/reviews/raven-mem-review-2-handoff.md`, as relayed by the user.
Line references are to `xnu/patches/0148-sweet-memtest.patch` at `8eb0f6a`.

## Findings

None. Review 1 F1 is closed.

## Verdicts

1. F1 closure: pass (read from source). A direct-READ timeout sets `ufs_fenced` before returning (`:59`); every later
   `ufs_scsi()` returns before clearing or rewriting the UTRD/UCD/PRDT and before any MMIO (`:37`); the only code that frees the
   test allocations is skipped once the fence is raised (`:209`). Nothing else in the applied XNU tree rings `REG_UTRLDBR` or
   touches this UTRD/UCD. So the timed-out target page stays allocated and its slot and command descriptor are never reused;
   later granules get `EIO` on their first (bounce) READ and issue no command.
2. Placement and locking: pass. Every `ufs_scsi()` caller (strategy, sync, init, sweet log, memtest) holds `sweet_ufs_lock`;
   the fence is set under it and other callers see it under it. The final unlocked read is safe: the only setter is a direct
   READ issued synchronously by the same memtest thread.
3. Default-off: pass. Without `sweetmemtest=1`, `ufs_data_pa` is 0, `data_pa == bounce_pa`, the fence cannot be raised and its
   check stays false; the bounce-buffer timeout path is unchanged.
4. Leaving `wip/`: approve. 0148 is review-cleared. Remaining before a push (not review findings): the fix in a commit (done,
   `8eb0f6a`) and the Linux gate.

A boot-arg fault injector is not required: it would only exercise the software branch, not real late-DMA behaviour; the
source-visible lifetime and reuse properties suffice.

With review 1 (`be4bff1`, `aba7831`, `44e0106`, `2065d1c` passed), every commit on `wip/raven-mem` is now review-cleared
(AGENTS.md rule 8). The reviewer made no repository, device or remote changes.
