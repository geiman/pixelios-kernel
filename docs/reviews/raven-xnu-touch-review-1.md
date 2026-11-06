# raven iOS touch + framebuffer: review 1 (2026-11-06)

Independent review (different model family) of `docs/reviews/raven-xnu-touch-review-1-handoff.md`, as relayed by the user.
Line references are to the files as they were then. Disposition of each finding: `raven-xnu-touch-review-2-handoff.md`.

## Findings

- **F1 (blocker)**, 0146 `tg_cs_stream`: READ_ALL_EVENT replies above 64 bytes are split into FIFO bursts while CS stays
  asserted; with `sweettouchcs=1` every `tg_abl_burst()` turns the channel off and pulses `CH_SW_RST` between chunks. Nothing
  cited proved the reset cannot glitch SCK under CPOL=1 while CS is low; Linux sends the reply as one SPI transfer. Failure:
  eight queued events -> 72-byte reply -> a clock discontinuity corrupts the frame; the retry may lose queued events and leave
  HID contacts stale. Prove on CPOL=1 or use a continuous transfer before leaving `wip/` (rule 7).
- **F2 (should-fix)**, 0146 header: release is classified UP before checking ttype; downstream (`sec_ts.c:2059-2068`) reports
  only NORMAL, PALM, GRIP, WET, GLOVE for every action. Failure: a HOVER/STYLUS/unknown release on an active TID gives a false
  finger-up. The host test codified the broader behaviour.
- **F3 (should-fix)**, 0146 status path: only the watchdog reset is recognized; downstream releases all fingers on
  `TYPE_STATUS_EVENT_ERR` + `SEC_TS_ERR_EVENT_QUEUE_FULL` (`sec_ts.c:3232-3240`). Failure: after queue exhaustion dropped
  releases, `tg.down[]` stays set and iOS contacts stick.
- **F4 (should-fix)**, CR-02: the mutation run was scratch-only; the gate runs only the unmutated host test. Example: weakening
  `len > SEC_TS_MAX_WRITE_DATA` to accept nine bytes is not caught by a legal-eight-byte boundary case. The reported survivor
  is equivalent (for n == 32 both branches return 32, and all greater values return 32).
- **F5 (nit)**: FTM5 log line `"after init:  ch="` lost one space even when `tg.sec == false`.
- **F6 (nit)**, `device/google/raven/board.json`: the touch note still said `/sweet-touch` starts the "FTM5-only XNU driver".

## Other checks (passed)

- 0145's old path is unchanged for 1080x2400 panels.
- 0147's 1080x2400 edge union equals the old edge test (logical-edge bands are strict subsets of the panel-edge bands); its
  clamps do not activate, and normalized maxima stay below 1.0.
- Raven's negative offsets and `sweetfb_native=1` arithmetic are correct.
- Bad-frame retry matches downstream's full request/reply retry.
- The protocol parser cannot select sec_ts without the complete NUL-terminated `"sec_ts"`.
- The watchdog recovery's first-start order is reasonable (it restores the only touch-function state this driver keeps).
- The pre-existing bluejay poke mask/value error should be a separate reviewed fix.

## Verdicts

- 0145: code-review pass; satisfies the independent-review part of rule 8; keep on `wip/` until the Linux gate and XNU CI are green.
- 0146: must remain on `wip/` (F1); F2 and F3 should also be fixed.
- 0147: code-review pass, contingent on 0145; keep on `wip/` until gate/CI.
- tools/host tests: `mkadt` logic passes; keep on `wip/` until F4 is resolved; clean up F5/F6 before promotion.

The reviewer made no repository changes; `git diff --check` and `tools.test_mkadt_touch` passed; the stack was reconstructed
selectively from the local pristine tree (the stagers need network access).
