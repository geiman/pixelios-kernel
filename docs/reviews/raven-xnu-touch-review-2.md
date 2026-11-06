# raven iOS touch (XNU sec_ts driver): review 2 (2026-11-06)

Independent review (different model family) of `docs/reviews/raven-xnu-touch-review-2-handoff.md`, as relayed by the user.

## Findings

- **F1 (should-fix)**, 0146 `sweet_touch_gs101_init`: a present but malformed `spi-fb-delay` (null value or < 4 bytes) was
  treated as absent, so the driver continued with FB delay 1 instead of refusing the ADT. Failure: a truncated raven property
  selects delay 1 instead of the measured 0 while the driver reports a normal start. Once `SecureDTGetProperty()` succeeds,
  require a non-null value, >= 4 bytes and a value <= 3, else stop the driver.
  **Fixed** (same day): the property, once found, must be non-null, >= 4 bytes and 0..3, otherwise "driver not started".

## Review-1 disposition (reviewer)

- Prior F1 (transport): closed. Raven's stock Linux path splits CPU-mode transfers into 64-byte chunks, calls `flush_fifo()`
  between them, re-asserts the already-active manual CS and releases it only after the message (`spi-s3c64xx.c:1002`, `:1105`).
  Holding `SW_RST` until the next burst adds no wire transition: channels are off, CPOL/CPHA stay in `CH_CFG`, and reset is
  cleared before the next packet is configured. The evidence is sufficient under rule 7.
- Prior F2, F3: closed. Prior F4: closed in substance (the absent `tools/mutate.py` is a repository-wide CR-02 gap, not a
  defect of this change). Prior F5, F6: closed.
- Both EQUIVALENT clauses hold.
- Bluejay unchanged: no new ADT property, `tg.sec == false`, mode 0, FB delay still 1.
- The feedback-delay source and placement are correct (raven's measured DT says 0; written after the USI reset release).

## Verdicts

- 0146: passes independent review once F1 is fixed; promotion still needs the Linux full gate and green XNU CI.
- 0147: pass (unchanged since review 1); may leave `wip/` after the gate and XNU CI, with its dependencies.
- Tools/tests: pass; the ADT generator cannot emit the malformed property of F1. Promotion needs the Linux gate.
- 0145: prior pass unchanged.

The reviewer modified no files and ran no device commands; `git diff --check 1246f32` passed.
