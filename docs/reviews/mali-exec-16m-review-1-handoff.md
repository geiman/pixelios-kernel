# Mali executable mappings in one 16 MB window (xnu 0149): review 1 handoff (2026-11-06)

Work alone. Do not spawn subagents. Read-only review: do not edit files, commit, push, or run `fastboot`/`adb`.
Report findings as F1..Fn (blocker / should-fix / nit, file:line, concrete failure scenario), then a verdict on leaving
`wip/` (AGENTS.md rule 8). Reviewer must be a different model family from the author (Claude Opus).

## Scope

- Repo `pixelios-kernel`, branch `wip/mali-exec-16m`, commit `492f1bd` on `7179269` (`wip/raven-sec-ts-xnu` before
  `d998524`, which only touches `tools/build-xnu-image.sh`). Review `git show 492f1bd`:
  - `xnu/patches/0149-sweet-mali-exec-16m-window.patch` (new): `bsd/dev/sweet_mali_pt.h` gains `MALI_EXEC_WINDOW_PAGES`, a
    `_Static_assert` on `MALI_VA_START`, and `mali_va_align()`; `bsd/dev/sweet_mali.c` `sweet_mali_map()` calls it and refuses.
  - `shim/build.sh`: applies the 0149 hunk for `sweet_mali_pt.h` on top of 0104's new-file text before building
    `mali_pt_host_test` (same pattern as the 0130-0132 `sweet_wlan_wr.h` chain).
  - `shim/mali_pt_host_test.c`: new 0149 cases.
  - `tools/test_mali_exec_window_mutants.py` (new): 6 mutants of `mali_va_align()`.
- Base files for the patch: the post-series `sweet_mali.c` / `sweet_mali_pt.h` (0104 + 0107 + 0109 applied with
  `git apply --include`) were compared byte for byte with the local build tree `~/git/pixel6/xnu-local/dxb/xnu/bsd/dev/`
  before editing [observed].

## Why (the claim to check)

- Linux v6.12 `drivers/gpu/drm/panfrost/panfrost_gem.c`, `panfrost_gem_open()`: comment "Executable buffers cannot cross a
  16MB boundary as the program counter is 24-bits. We assume executable buffers will be less than 16MB and aligning
  executable buffers to their size will avoid crossing a 16MB boundary."; `align = size >> PAGE_SHIFT` for non-noexec BOs
  (2 MB for noexec BOs >= 2 MB), passed to `drm_mm_insert_node_generic` [read from source, fetched from
  raw.githubusercontent.com torvalds/linux tag v6.12; **line numbers were not recorded**, please locate by function]. No
  architecture condition in that path; `panfrost_drv.c` `dt_match[]` binds `arm,mali-valhall-jm` (the G78 class) with the
  same code [read].
- Counter-evidence, not resolved: Mesa 25.0.7 `src/panfrost/lib/kmod/panthor_kmod.c` `panthor_kmod_vm_alloc_va()` (CSF
  Valhall v10+, Mesa picks VAs itself) aligns executable BOs like any other (`size > 0x200000 ? 0x200000 : 0x1000`) [read].
  So the 24-bit PC limit may be Midgard/Bifrost-only (Mesa's only other mention: `midgard/midgard_compile.c:3129`).
  0149 follows the Linux driver the G78 would run under on Linux (rule 7: the cited behaviour, not a guess that v9 is exempt).
- Before 0149: `sweet_mali_map()` called `mali_va_alloc(&sm.va, n * 4u, 4u)` for every mapping (16 KB alignment), so an
  executable BO could straddle a 16 MB line.

## The change

- `mali_va_align(npages, base_align, flags)`: non-exec -> `base_align` (4 pages = 16 KB, unchanged); exec -> smallest power of
  two >= npages starting from `base_align`; 0 when exec and `npages == 0 || npages > 4096` (16 MB). A power-of-two-aligned
  block of size >= npages and <= 16 MB lies inside one 16 MB window. (Linux's align-to-size does not guarantee this for
  sizes that are not powers of two, e.g. 12 MB at 12 MB crosses 16 MB.)
- `mali_va_alloc()` aligns the page index relative to `MALI_VA_START`; the `_Static_assert` pins `MALI_VA_START` (0x2000000)
  to a 16 MB multiple so relative = absolute.
- `sweet_mali_map()`: refusal before `lck_mtx_lock`, returns EINVAL (contract in `sweet_mali.h`: EINVAL = nothing mapped),
  one `printf`. 0105 `errno_to_ioreturn(EINVAL)` = `kIOReturnBadArgument`.
- Mesa's executable BOs are small: shader pools `PAN_BO_EXECUTE` with 4096-byte slabs (`gallium/drivers/panfrost/pan_context.c:1100`,
  `pan_screen.c:973`), a backing BO of `MAX2(slab_size, sz)` (`pan_mempool.c:170`) [read, Mesa 25.0.7]; the 16 MB refusal
  should not trigger for real shaders.

## Verification done

- `mali_pt_host_test` (built the build.sh way, 0104 + 0149): PASS. New cases: helper values (12 -> 16, 4096 -> 4096,
  4100 -> 0, 0 -> 0, non-exec unchanged), and every 16 KB-multiple exec size from 4 to 4096 pages allocated after a
  1023 x 16 KB data mapping, asserting the start and last byte share a 16 MB window.
- `tools/test_mali_exec_window_mutants.py`: 6/6 killed (data mappings get exec alignment; > 16 MB accepted; 0 pages
  accepted; no rounding; rounding one step short; 8 MB window). The `sweet_mali_map()` refusal has no host build: review only.
- `validate-repo.py`, `check-rules.py --strict`: PASS (local clone with the reservation refs).
- Linux Docker gate: **`GATE PASS 492f1bd`** (image sha256:e06406f73f06..., unit 451 OK, 18 skipped)
  [observed, `~/git/pixel6/gate-docker/RESULTS.md`].
- Local kernel build (dxb, Xcode 15.4): 138 numbered patches applied, none skipped; kernel sha256
  `e52d8cb4433aad73374bc045340ca869e8dd2dad5154493ea678ed4c9367ac59`, contains the refusal string [observed]. Not built by CI.
- Hardware: **not booted.** Nothing maps executable BOs today (only the 0104 selftest's one-page EXEC mapping, which 0149
  aligns to 16 KB as before), so behaviour on boot should be unchanged until a GPU client runs [guess, from source].

## Asked of review 1

1. Is the Linux citation sufficient under rule 7 for applying the rule to the G78, given the panthor counter-evidence?
2. `mali_va_align()` / `mali_va_alloc()` interaction: correctness of the power-of-two bound, and whether the two-pass hint
   search in `mali_va_alloc()` can miss a free aligned block that exists (larger alignments than before now reach it).
3. The refusal: placement, error code, and whether any 0105 path (`boNew` with `SWEET_MALI_EXEC`, `importSurface`
   = non-exec) changes behaviour beyond the intended one.
4. VA waste: power-of-two alignment of exec BOs in a 4 GB window; any risk to the 1 GiB wired cap or to the selftest.
5. `shim/build.sh` hook and the mutant list: complete? (`tools/gate_select.py` selects `host-tests` and the builds for a change
   to the 0149 patch [observed, `g.select([...0149...])`].)
