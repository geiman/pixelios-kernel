# Mali executable mappings in one 16 MB window (xnu 0149): review 2 (2026-11-06)

Independent review (different model family) of `docs/reviews/mali-exec-16m-review-2-handoff.md`, as relayed by the user.
Reviewed `wip/mali-exec-16m` at `fc1f165` (scope `492f1bd..4d8ab9c`). Verdict: keep on `wip/` until F1 is fixed and rechecked;
after that the remaining release evidence is the hardware boot.

## Findings

- **F1 (should-fix, test coverage)** `tools/test_mali_exec_window_mutants.py:109`: the source pins do not check that
  `lck_mtx_lock(&sm_lock)` comes before `mali_va_alloc_map()`. With the call moved above the lock in an in-memory mutant and the
  failure branch unchanged, `pin_errors()` returned `[]`. Failure: a refactor that allocates before locking but keeps the pinned
  failure branch would unlock an unowned mutex on an oversized executable mapping or VA exhaustion (panic or lock corruption)
  while every pin and mutant passes. Asked: pin lock-before-call ordering and add a mutant that moves the call above the lock.

## Answers to the handoff

1. Coverage otherwise strong: refusal preserves `*va` and allocator state, data alignment and oversized data are tested, no-room
   is distinguished from refusal. The exact-text pins are formatting-sensitive but fail closed: acceptable for this small body.
2. Refusal under `sm_lock`: safe. A successful allocation already held the lock; the refused path adds only helper arithmetic, and
   the `printf` comes after the unlock.
3. No unintended production change beyond consolidating alignment/allocation, reusing `mflags` for the PTE permissions and the
   citation fix.
4. `file_diff()` closes its file and handles 0109's headerless section.

Reviewer ran `python3 -B -W error::ResourceWarning tools/test_mali_exec_window_mutants.py`: 6 tests pass; worktree left clean.

## Fix status

- F1: `pin_errors()` now requires exactly one `lck_mtx_lock(&sm_lock);` in the `sweet_mali_map()` body, before the
  `mali_va_alloc_map()` call, with no `lck_mtx_unlock(` between them. Three source mutants added, each caught: the call moved
  above the lock (the reviewer's case), the lock removed, an unlock inserted before the call. 11 source mutants, 10 header
  mutants. `0149` itself is unchanged (test-only fix), so the kernel build `2d2718e1...` and `GATE PASS 171f393` still apply
  to the patch; the test change needs the gate.
