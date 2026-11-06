# raven iOS touch (XNU sec_ts driver): review 2 handoff (2026-11-06)

Work alone. Do not spawn subagents. Read-only review: do not edit files, commit, push, or run `fastboot`/`adb`.
Report findings as F1..Fn (blocker / should-fix / nit, file:line, concrete failure scenario), then a verdict per item on leaving
`wip/` (AGENTS.md rule 8). Review 1 findings and the handoff it answered: `docs/reviews/raven-xnu-touch-review-1.md`,
`docs/reviews/raven-xnu-touch-review-1-handoff.md`.

## Scope

- Repo `pixelios-kernel`, branch `wip/raven-sec-ts-xnu` on `1246f32` (= xnu 0145, which review 1 passed), **uncommitted**:
  `git diff 1246f32` plus the untracked files: `xnu/patches/0146-sweet-gs101-touch-sec-ts.patch`,
  `xnu/patches/0147-sweet-hid-touch-panel-size.patch` (unchanged since review 1), `shim/sweet_sec_ts_host_test.c`,
  `tools/test_sec_ts_mutants.py` (new), `device/google/raven/hardware/spi11-dt-2026-11-06.txt` (new),
  `device/google/raven/hardware/touch-xnu-2026-11-06.txt`, the review files in `docs/reviews/`.
- Sources as in review 1: `sec_touch` `b23d1f8dfb9e` (`~/git/pixel6/xnu-local/refs/sec_touch`); `spi-s3c64xx.c` from
  `kernel/gs` `6e771b230c52`, now also fetched to `~/git/pixel6/xnu-local/refs/kernel-gs/spi-s3c64xx.c`.

## Changes since review 1, per finding

- **F1 (blocker, chunked READ_ALL_EVENT under one CS, CPOL=1).** No code change; evidence that the stock kernel does the same
  on raven. Raven's SPI11 / touch DT read read-only from the running recovery kernel (`hardware/spi11-dt-2026-11-06.txt`,
  observed): no `dma-mode`, `samsung,spi-fifosize = 0x40`, touch `controller-data` `samsung,spi-chip-select-mode = 0`,
  `spi-cpol`, `spi-cpha`. In `spi-s3c64xx.c` that means CPU mode (`:1487-1490`), `fifo_lvl` 64 (`:959`, `:1776-1784`), a
  transfer above 64 bytes sent as 64-byte chunks (`:1002-1011`, `:1107-1124`) with `flush_fifo()` (`:1105`: channels off,
  PACKET_CNT 0, SW_RST set, drain, SW_RST clear) between them, and the manual chip select asserted per chunk (`:660-664`) and
  released once after the message (`:1157-1158`, `:745-760`). So Linux on raven clocks a > 64-byte sec_ts reply with a
  channel reset between 64-byte chunks under one asserted CS in mode 3, the shape `tg_cs_stream` + `tg_abl_burst` use. Cited
  in the comment above `tg_cs_stream`. Please check the reading of `transfer_one_message` and whether the remaining difference
  matters: Linux clears SW_RST inside `flush_fifo` before the next chunk is set up; `tg_abl_burst` leaves SW_RST set after
  its drain and clears it at the start of the next burst (the channel is off and in reset between chunks in both).
- **F2 (release of unreported types).** `sec_ts_parse_event` now returns IGNORED for any ttype other than NORMAL, GLOVE, WET,
  PALM, GRIP *before* looking at the action (0146 header, `if (r.ttype != SEC_TS_TT_NORMAL && ...`). Host test: hover,
  stylus, unknown (15) releases ignored; NORMAL, GLOVE, WET, GRIP releases UP.
- **F3 (queue full).** New `sec_ts_queue_full()` (eid STATUS, stype ERR = `sec_ts.h:120`, id `SEC_TS_ERR_EVENT_QUEUE_FULL` =
  `sec_ts.h:388`; `sec_ts.c:3232-3240`); `tg_sec_handle` calls `tg_release_all()` on it. ESD (`:3241-3251`) is not acted on:
  its reset is under `USE_RESET_DURING_POWER_ON`, which this source tree never defines (grep). Host test covers queue-full,
  ESD, an INFO event with id 1 (wet mode) and a coordinate event with the same bytes.
- **F4 (mutations committed).** `tools/test_sec_ts_mutants.py`, modelled on `tools/test_wlan_wr_mutants.py` because
  `tools/mutate.py` (named by CR-02) does not exist in the repo. Header extracted from the 0146 patch (a test asserts the awk
  rule of `shim/build.sh` gives the same bytes), every occurrence replaced, `gcc` + run with output capped; survivor = FAIL,
  nomatch = FAIL, killed-but-listed-equivalent = FAIL. 41 mutants: 39 killed, 2 listed EQUIVALENT (and surviving, as required) with the implying clause
  (`n > 32` overflow bound, agreed in review 1; `tid == 0 ||` removed: unsigned `tid - 1u` wraps to UINT_MAX and the remaining
  clause rejects it). The first run left 3 real survivors; the host test gained the cases that kill them (sync byte wrong with a
  consistent checksum; watchdog bytes on a coordinate event id; a valid finger with the gesture / empty event ids). The
  review's example (`len > MAX` weakened to accept 9) is a mutant, and an 8-byte legal write frame is now tested.
  Run: `python3 -B -m unittest tools/test_sec_ts_mutants.py` (picked up by `unittest discover`; no `check-all.sh` change needed).
- **F5.** `after init:  ch=` has its two spaces back (the run-1 log shows the one-space version, run 2 two).
- **F6.** `device/google/raven/board.json` touch note rewritten for the 0146 node.

## Also changed (found while answering F1)

- **SPI feedback delay.** Raven's DT has `samsung,spi-feedback-delay = 0`; bluejay's has 1, which both driver setup paths
  write. New board key `touch_spi_fb_delay` (raven 0), mkadt property `spi-fb-delay` (only when the key exists; integer 0..3
  else `ValueError`), driver: optional property, value > 3 refuses to start, written as `FB_CLK[1:0]` at the end of
  `tg_spi_setup` (after the USI reset is released). bluejay / sweet ADTs byte-identical (checked again).
- Observed side note (touch-xnu record, run 2): run 1 already read `FB_CLK = 0` after setup although the
  `sweettouchcs=1` setup writes 1. Guess: that write lands while `USI_CON` reset is set and does not stick. So the bluejay
  `sweettouchcs=1` path may not be setting the feedback delay it intends either (pre-existing, not changed here).

## Verification done

- `tools/stack-apply --only-new 0147`: PASS (136/136 + 3 stagers). `git apply --check` of 0147 on the new 0146: OK.
- Clean local kernel build (Xcode 15.4): 140 patches applied, no warnings in `sweet_touch_gs101.c` / `sweet_sec_ts.h`.
  Kernel sha256 `f61ff55dc6d21bad70aceadcb74810a6e5aad8e709b0ed6f32f82929258574cd`. Not built by CI.
- Host test (extracted header): pass. `test_sec_ts_mutants`: 3 tests OK. `test_mkadt_touch`: 7 OK (raven `spi-fb-delay` = 0,
  bluejay has none, bad values 4 / -1 / "0" / True / 1.0 refused). `check-rules.py --strict`: PASS.
  `validate-repo.py`: only the pre-existing failures (series gaps 0121-0133, raven README paths). `check-all.sh` cannot run on
  macOS (`flock`); please run it on Linux if available.
- Hardware, observed (`hardware/touch-xnu-2026-11-06.txt`, "Run 2"): raven-xnu11-touch, image sha256
  `607c254f5c6d27df6d40c8fb6851a230976112c5548f439eabb9da85898e6232`: touch works, user's checks pass (corners, swipe from the
  bottom of the glass, two fingers, under the finger). XNU log: identify as run 1, `ch=0xc fb=0`, 18 finger-downs (two
  fingers), 8 in the margin, no failure / timeout / overflow / queue-full / watchdog line. The F1 multi-chunk path (>= 8 queued
  events) and the F2/F3 paths did not occur in either run's log: they are exercised by source equivalence and host tests only.
- Bluejay not booted with 0145-0147.

## Asked of review 2

1. F1: is the evidence sufficient under AGENTS rule 7, and does the SW_RST timing difference noted above matter?
2. F2-F6 and the feedback-delay change: correct and complete; bluejay path still identical (FB_CLK untouched without the
   property; the new property check runs before `tg_parse`).
3. `tools/test_sec_ts_mutants.py`: meets CR-02 in substance given `tools/mutate.py` is absent? The two EQUIVALENT clauses hold?
4. Verdict per item on leaving `wip/` (0146, 0147, tools/tests), and what must precede promotion (Linux gate, XNU CI).

## Not in scope / known open

- The bluejay poke string with mask/value swapped (review 1: separate fix) and the bluejay `FB_CLK` observation above.
- ESD recovery (compiled out in the source driver).
