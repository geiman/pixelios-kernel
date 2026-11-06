# Mali executable mappings in one 16 MB window (xnu 0149): review 1 (2026-11-06)

Independent review (different model family) of `docs/reviews/mali-exec-16m-review-1-handoff.md`, as relayed by the user.
Reviewed commit `492f1bd`. Verdict: keep on `wip/` while F1 is open.

## Findings

- **F1 (should-fix, test coverage)** `xnu/patches/0149-sweet-mali-exec-16m-window.patch:10`: the mutants stop at
  `mali_va_align()` and never exercise the `sweet_mali_map()` integration or its refusal. `shim/build.sh` extracts only
  `sweet_mali_pt.h`. Passing `0` instead of `MALI_F_EXEC`, restoring the fixed alignment in the `mali_va_alloc()` call, or
  removing the `align == 0` refusal would keep every host test and mutant green; the first two let executable mappings cross a
  16 MB line, the third breaks the documented EINVAL contract. Asked: a host/source mutant covering the `sweet_mali.c` call and
  refusal, or move the whole decision into host-tested pure code that `sweet_mali_map()` uses directly.

## Answers to the handoff

1. Linux citation: sufficient for adopting the invariant conservatively. v6.12 applies it without a model check in
   `panfrost_gem_open()`, and `panfrost_drv.c` matches `arm,mali-valhall-jm`. Panthor's CSF behaviour does not rebut an arch-9
   JM rule. **But** do not claim v6.12 supports the exact G78: its model table (`panfrost_gpu.c`) lists Valhall G57 IDs, not G78.
2. Alignment math: correct (the power of two is >= the size and divides 4096 pages; `MALI_VA_START` is 32 MB-aligned).
   Two-pass allocator: no miss; the `hint + npages` bound of pass 1 is looser than needed, not restrictive.
3. Refusal: before locking or mapping, EINVAL; `boNew` exec BOs over 16 MB fail as intended; `importSurface` stays non-exec.
4. VA/cap: alignment only consumes VA, the 1 GiB wired accounting is unchanged; small shader BOs and the one-page selftest keep
   16 KB alignment. Adversarial fragmentation can give an earlier ENOMEM, never a cap violation.
5. Hook/mutants: wired correctly; the six helper mutants pass and are useful but incomplete (F1).

Reviewer ran `python3 -B tools/test_mali_exec_window_mutants.py`: both tests pass; worktree left clean.

## Fix status

- F1, both remedies: the whole VA decision moved into `mali_va_alloc_map()` (`sweet_mali_pt.h`, host-tested: data 16 KB
  packing, data aligned after a 4 KB hint, exec window, refusal leaves `*va` and the hint untouched, no-room = -2), and
  `sweet_mali_map()` calls it directly with one flag translation `mflags`, also used for the page entries. The kernel-only
  rest is covered by source pins in `tools/test_mali_exec_window_mutants.py` (body of `sweet_mali_map()` after
  0104+0107+0109+0149): translation once and before the call, exactly one `mali_va_alloc_map(&sm.va, n * 4u, mflags, &va)`,
  no `mali_va_alloc(`/`mali_va_align(` in the body, failure branch = unlock, -1 -> EINVAL, else ENOMEM, every `mali_pt_map`
  passes `mflags`. 8 source mutants (the three named in F1 among them) each break a pin; 10 header mutants are killed.
  Change of placement: the refusal now happens under `sm_lock` (unlocked before the `printf` and the return); nothing is
  allocated on it.
- Found while fixing: the first version of the test's patch-chain helper skipped a patch with no `diff --git` header
  (0109) silently; it now parses `---`/`+++` pairs and raises when a listed patch does not touch the file.
- Citation: the header comment and the patch description now say v6.12's model table lists G57 IDs, not the G78, and the
  invariant is adopted conservatively.
- Verification: `mali_pt_host_test` PASS; mutant test 6/6 tests OK; chained sources equal the edited base files byte for
  byte; `validate-repo.py`, `check-rules.py --strict` PASS; local kernel build 138/138 patches, sha256
  `2d2718e1d144cc32e746fadb3fddcd29031a3c6b962fd705ceff67f11c73da2a`. Same warning classes as the existing file
  (`-Wdeclaration-after-statement` at the new declarations). Linux gate and review 2 pending; not booted.
