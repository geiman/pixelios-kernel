# Mali executable mappings in one 16 MB window (xnu 0149): review 2 handoff (2026-11-06)

Work alone. Do not spawn subagents. Read-only review: do not edit files, commit, push, or run `fastboot`/`adb`.
Report findings as F1..Fn (blocker / should-fix / nit, file:line, concrete failure scenario), then a verdict on leaving
`wip/` (AGENTS.md rule 8). Reviewer must be a different model family from the author (Claude Opus).
Review 1 and the handoff it answered: `docs/reviews/mali-exec-16m-review-1.md`, `docs/reviews/mali-exec-16m-review-1-handoff.md`.

## Scope

- Repo `pixelios-kernel`, branch `wip/mali-exec-16m`, commits after review 1's `492f1bd`:
  `git diff 492f1bd 4d8ab9c -- xnu shim tools` (`8516e5e` is the review 1 handoff only).
  - `171f393` answers F1: `xnu/patches/0149-sweet-mali-exec-16m-window.patch` (regenerated), `shim/mali_pt_host_test.c`,
    `tools/test_mali_exec_window_mutants.py`, `docs/reviews/mali-exec-16m-review-1.md`.
  - `4d8ab9c`: `file_diff` closes the patch file (the 171f393 gate log showed ResourceWarnings).

## Changes since review 1

- **F1 (should-fix, no coverage of the `sweet_mali_map()` integration).** Both suggested remedies:
  - The whole VA decision is `mali_va_alloc_map(v, npages, flags, *va)` in `sweet_mali_pt.h`: `mali_va_align()` with
    `MALI_MAP_ALIGN_PAGES` (4 = 16 KB), then `mali_va_alloc()`; 0 and `*va`, -1 refused (exec over one 16 MB window, or
    npages 0; nothing allocated), -2 no free VA. Host test: data packed at 16 KB; data aligned after a 4 KB `mali_va_alloc`
    moved the hint; exec 4092 pages inside one window; a refusal leaves `*va` and `hint` untouched; data over 16 MB allowed;
    a full allocator gives -2 for data and exec.
  - `sweet_mali_map()` computes `const uint32_t mflags = (flags & SWEET_MALI_EXEC) ? MALI_F_EXEC : 0;` once and uses it for
    `mali_va_alloc_map()` and every `mali_pt_map()`. On `ae != 0`: `lck_mtx_unlock`, then -1 -> `printf` + EINVAL, else
    ENOMEM. **Placement changed:** the refusal is now decided under `sm_lock` (review 1 accepted the earlier before-lock
    placement); nothing is allocated or mapped on that path.
  - Source pins (`pin_errors()`) on the `sweet_mali_map()` body of the post-series text (0104 + 0107 + 0109 + 0149):
    translation exactly once and before the call; exactly one `mali_va_alloc_map(&sm.va, n * 4u, mflags, &va)`; no
    `mali_va_alloc(` / `mali_va_align(` in the body; the failure branch exactly as above (regex); every
    `mali_pt_map(&sm.pt, ...)` ends `, mflags);`. 8 source mutants, the three of F1 among them (translation to 0, call
    with 0, fixed 16 KB alignment restored; plus refusal -> ENOMEM, refusal removed, lock kept on failure, page entries
    ignore mflags, translation moved after the call), each must break a pin. Header mutants: 10 (6 from review 1 + 4 on
    `mali_va_alloc_map`), all killed.
- **Found while fixing:** the first version of the test's chain helper skipped 0109 (no `diff --git` header) silently.
  `file_diff()` now parses `---`/`+++ b/<path>` pairs as well, and a listed patch that does not touch the file raises
  (`test_source_chain_is_complete`). One real survivor during the fix (data alignment cut to 4 KB: every test size was a
  16 KB multiple) is killed by the 4 KB-hint case.
- **Citation (review 1 answer 1):** header comment and patch description now say v6.12's model table (`panfrost_gpu.c`)
  lists G57 IDs, not the G78, and the invariant is adopted conservatively.

## Verification done

- `mali_pt_host_test` built the build.sh way (0104 + 0149 hunk): PASS. `tools/test_mali_exec_window_mutants.py`: 6 tests OK,
  also with `-W error::ResourceWarning`. The chained `sweet_mali.c` and `sweet_mali_pt.h` equal the edited base files byte for
  byte [observed].
- `validate-repo.py`, `check-rules.py --strict`: PASS (local clone with the reservation refs).
- Linux Docker gate: **`GATE PASS 171f393`**, unit 455 OK (18 skipped) [observed, `~/git/pixel6/gate-docker/RESULTS.md`].
  `4d8ab9c` (test file only) not gated separately.
- Local kernel build (dxb, Xcode 15.4) with the regenerated 0149: 138/138 numbered patches, none skipped; kernel sha256
  `2d2718e1d144cc32e746fadb3fddcd29031a3c6b962fd705ceff67f11c73da2a`, contains the refusal string. New warnings are
  `-Wdeclaration-after-statement` at the new declarations (`sweet_mali.c:360`, `sweet_mali_pt.h:106`), the class the files
  already have [observed]. Not built by CI.
- Hardware: **not booted.**

## Asked of review 2

1. F1: is the coverage now sufficient (helper host test + source pins + mutants), and are the pins too brittle or too loose?
2. The refusal decided under `sm_lock` instead of before it: any problem (lock hold time, the `printf` after unlock)?
3. Anything in the regenerated 0149 that changed beyond F1 and the citation.
4. Verdict on leaving `wip/`; remaining before a push is the boot.
