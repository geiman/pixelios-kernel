# Mali executable mappings in one 16 MB window (xnu 0149): review 3, recheck of review 2 F1 (2026-11-06)

Independent review (different model family) of the review 2 F1 fix `6f8e71e` (`docs/reviews/mali-exec-16m-review-2.md`
"Fix status"), as relayed by the user.

## Findings

None. Review 2 F1 is closed: the call-before-lock mutant now gives `mali_va_alloc_map not called with sm_lock held`; lock
removal and an intervening unlock are covered too. The pins are appropriately scoped and fail closed.

Reviewer ran the six tests of `tools/test_mali_exec_window_mutants.py` with `ResourceWarning` promoted to an error: pass;
worktree left clean.

## Verdict

AGENTS.md rule 8 independent review: satisfied. The branch stays on `wip/` until the test-only commit is gated
(done: `GATE PASS 6f8e71e`, `~/git/pixel6/gate-docker/RESULTS.md`) and a hardware boot is recorded.

## Boot

Recorded: `device/google/raven/hardware/mali0149-boot-2026-11-06.txt` (raven-mali0149-r1, lock screen + touch, observed by the
user). It does not exercise 0149's code path (no `sweetmali` on raven).
