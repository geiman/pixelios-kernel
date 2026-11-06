# raven RAM (10 GiB XNU window): review 2 handoff (2026-11-06)

Work alone. Do not spawn subagents. Read-only review: do not edit files, commit, push, or run `fastboot`/`adb`.
Report findings as F1..Fn (blocker / should-fix / nit, file:line, concrete failure scenario), then a verdict per item on leaving
`wip/` (AGENTS.md rule 8). Review 1 findings and the handoff it answered: `docs/reviews/raven-mem-review-1.md`,
`docs/reviews/raven-mem-review-1-handoff.md`.

## Scope

- Repo `pixelios-kernel`, local branch `wip/raven-mem` (not pushed), 7 commits on `80e9724`: the 5 of review 1, then
  `8eb0f6a` (the F1 fix, `xnu/patches/0148-sweet-memtest.patch` only: `git diff 2065d1c 8eb0f6a`) and the commit that adds the review files.
- Review 1 passed `be4bff1`, `aba7831`, `44e0106`, `2065d1c`; only 0148 (`527af3a` + this diff) is up for review.
- XNU with the stack applied: `tools/stack-apply --keep` (0001-0148), file `bsd/dev/sweet_ufs.c`.

## Changes since review 1, per finding

- **F1 (blocker, direct-READ timeout frees pages the host may still DMA into).** Took the reviewer's second option: treat a
  timeout of a READ into `ufs_data_pa` as fatal for UFS and keep every allocation (read from source, the patched tree):
  - new `static bool ufs_fenced` next to `ufs_data_pa`, set only in `ufs_scsi()`'s timeout branch when `data_pa != bounce_pa`
    (i.e. the override was in effect for that command), under `sweet_ufs_lock` like every `ufs_scsi()` call; never cleared;
  - `ufs_scsi()` returns `EIO` before touching the UTRD/UCD/PRDT or any register when `ufs_fenced` is set, so the slot, UCD and
    page are never reused by any caller (strategy, sweet log, sync-cache, init TUR, memtest);
  - `mt_thread()` checks `ufs_fenced` after the per-granule loop and, if set, prints
    `SWEET MEM: RESULT FAIL (a direct UFS READ timed out; UFS stopped, N x 32 MiB kept allocated)` and returns without
    `kmem_free`. Later granules' `mt_ufs_read_into()` calls after the fence get `EIO` from the first (bounce) READ and touch
    nothing. The unlocked read of `ufs_fenced` in `mt_thread` is on the thread that issued every direct READ.
  - Not done: no doorbell-clear wait / host reset after `UTRLCLR` (option 1). The pre-existing bounce-buffer timeout path is
    unchanged (review 1: the permanent bounce buffer has no lifetime problem).
  - Consequence (by design): after a fence the root filesystem and the sweet log stop (the log thread prints `write failed`
    and stops once). The diagnostic is opt-in (`sweetmemtest=1`).
- **Verdict 7 note (`machine_info.memory_size` 32-bit):** no change; not a limit (VM sizing uses `mem_size`/`max_mem`).
- **Verdict 8 (fault injection):** not added. There is no kernel test harness for `sweet_ufs.c`; forcing a real UFS timeout
  needs hardware fault injection. Open to a boot-arg that fakes the timeout branch without ringing the doorbell if the reviewer
  thinks it is worth the code.

## Checks run (2026-11-06, this Mac)

- `tools/stack-apply --only-new 0148`: PASS (137/137 patches + 3 stagers on xnu-8796.141.3).
- `python3 tools/check-rules.py --strict`: PASS (0 new, 70 baselined).
- `python3 -B -m unittest tools/test_memprobe_mutants.py tools/test_mkadt_touch.py tools/test_build_xnu_image_raven.py`: 16 OK.
- memprobe host test, raven and bluejay headers: pass. `shellcheck -S warning shim/build.sh`: clean.
- Kernel build (DEVELOPMENT, SWEET, Xcode 15.4): clean (only the 4 upstream `Skipping patch`), `kernel.development.sweet`
  sha256 `1094295f5b1f8e83...`; image `raven-mem-r1` (`--rw --gui --touch`, `sweetmemtest=1 sweetmemtestdelay=60
  sweetmemtestgb=6 sweetmemtestfloor=2`), payload `phys_base 0x880000000 mem_size 0x280000000`, sha256 `25f37a5fccf2e617...`.
- Device (observed on hardware, 2026-11-06, raven, `fastboot boot raven-mem-r1`, log `logs/raven-mem-r1-sweetlog.bin`,
  decoded boot-args match the image): `vm_page_bootstrap: 650163 free pages, 5197 wired`; allocated 157 x 32 MiB; granules
  `0x26 0x27 0x28 0x2a 0x2b` tested 223,667 pages, 0 errors, UFS direct READ `match` in each; `0x29` got no pages this run, so
  `RESULT INCOMPLETE (0 granules with errors or a UFS mismatch, 1 not reached)`; all chunks freed (free 449774 -> 453569).
  No `SWEET UFS ... timed out`, no `UFS stopped`, no `SWEET LOG: write failed`; the log kept flushing after the test. The
  fence path itself was not exercised (no way to force a timeout). Coverage of `0x29` is already in the 5 runs of
  `hardware/ram-10g-2026-11-06.txt` (unchanged kernel paths outside the fence).
- Not run: `tools/check-all.sh` (needs Linux; the Docker gate thread), fork CI (branch unpushed).

## Asked of review 2

1. Does the fence close F1: is there any path where a page that was the target of a timed-out direct READ is freed, or where the
   slot/UCD is reused after such a timeout?
2. Is `ufs_fenced`'s placement (checked at the top of `ufs_scsi`, before any DMA-structure write) and its locking correct?
3. Anything in the fence that changes behaviour without `sweetmemtest=1` (it should be unreachable: `data_pa == bounce_pa`)?
4. Verdict on 0148 leaving `wip/`.
