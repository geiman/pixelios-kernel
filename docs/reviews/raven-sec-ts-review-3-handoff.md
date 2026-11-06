# raven sec_ts touch probe: review 3 handoff (2026-11-06)

## Scope

- Repo `pixelios-kernel`, branch `wip/raven-sec-ts`, on `device/raven` HEAD `df8e3eb`. **Uncommitted**: review `git diff df8e3eb`
  plus the two new files (`shim/sectouch_host_test.c`, `device/google/raven/hardware/touch-run1-2026-11-05.txt`).
- Plus this handoff and `device/google/raven/hardware/touch-run2-2026-11-06.txt` (new, hardware record).
- 9 files, +893/-13: `shim/touchprobe.c/.h` (sec_ts identify, SPI mode 3), `shim/sectouch_host_test.c` (fake S6SY79x),
  `shim/build.sh`, `tools/boards.py`, `tools/test_mkadt_touch.py`, `device/google/raven/board.json`, `docs/AUTOMATION.md`.
- Goal of the change: identify the raven touch chip (Samsung S6SY79x, `sec,sec_ts`) over SPI11 from the shim. Phase 1 only:
  no XNU driver, no `/sweet-touch` node (raven deliberately has no `touch_chip_id`).
- Source citations are against Google `kernel/gs` `6e771b230c52` (`spi-s3c64xx.c`, `sec_ts.c`) unless a line says otherwise.

## History

- Review 1: 1 blocker + 4 should-fix + 1 nit (F1-F6), all fixed.
- Review 2: F1-F5 verified PASS, no blocker/should-fix. Two nits, verdict "not OK to leave `wip/`" until nit 1 is fixed.

## Changes since review 2

1. **Nit 1 (F6 citation in board.json).** `device/google/raven/board.json:46` now reads `spi-s3c64xx.c:55-56, cleared at :780-782,
   set at :784-788, written at :790`, the same wording as `shim/touchprobe.h:101-102`.
2. **Nit 2 (A-E coverage).** New `test_each_variant()` (`shim/sectouch_host_test.c:431`). For n = 1..4 the fake engine
   stalls until n variants have logged `VARIANT ?: timeout` (hook in `con_str`, `:235`), so variant B, C, D, E in turn is the
   one that completes. Asserts per run: exactly n timeouts, `VARIANT <n> completes`, `bad_mode == 0 && bad_cs == 0`,
   `3/3 sec_ts frames ok`, `id=sec-ts-ok-app`.
   - Fake model change for E: `cs_low()` (`:80`) treats chip select as asserted when the CS pad is on the SPI function and
     `CS_REG` has `CS_AUTO` (literal `BIT(1)`, `S3C64XX_SPI_SLAVE_AUTO` at `spi-s3c64xx.c:80`, set by `enable_cs`
     `:654-659`); otherwise it reads the GPIO data bit as before. (Review 3 corrected an earlier `:71`, a mainline line number.)
     Check this does not mask a real CS fault in other tests (the pad is on the SPI function only inside variant E).
   - The run now prints 8 `PASS sectouch <name>:` lines plus `PASS sectouch_host_test`.
   - Mutation check (scratch copy, not committed): variant E without `TP_CS_AUTO` -> `test_each_variant` fails on
     `bad_cs`; `CH_CFG = 0` after `spi_engine_init` in variant C only -> fails on `bad_mode`. Unmutated passes.
3. No change to `touchprobe.c`, `touchprobe.h`, write helpers, `wmask`/`w32_allowed`, or register addresses since review 2.

## Verification done

- Build: `PIXELIOS_HOST_TESTS=always ./build.sh --board raven --avb --touch-probe=run,mux,cs,osc,rst,rails --out out/raven-sects3`
  with the Arm GNU 15.2.rel1 toolchain (`CROSS_PREFIX`, `CROSS_OBJDUMP`, `CROSS_NM` set). Ends with
  `host tests & safety checks passed` (raw and lz4 headers).
- On this Mac, `chk` was run non-exiting (scratch copy of build.sh) because pstore, usbprobe, wlan*, gpu* host tests fail on
  Apple Silicon for bluejay too (known; EL1 `mrs`, Mach-O `_end/_head`). All 74 FAILs were in that set; none touch-related.
  Please rerun on Linux if available.
- `python3 -m unittest tools/test_mkadt_touch.py`: OK.
- Image `shim/out/raven-sects3/shim-boot-avb.img`, sha256 `6616af142288153f7d86dd5e14f73671f9e0e092882973bb13b2ae0ac43e0a5e`.
- Hardware: `fastboot boot` (RAM only) on raven 2026-11-06, first run of the review-1/2 fixes
  (`device/google/raven/hardware/touch-run2-2026-11-06.txt`, transcribed from photos). Result identical to run 1:
  boot-complete event after 0 tries, `3/3 sec_ts frames ok`, boot status 0x20 (app), `id=sec-ts-ok-app moved=F,R,I`,
  SPI `ch=0xc` after init. No `VARIANT A-E` lines: the first identify answered, so the fallback (and the new
  `test_each_variant` path) is exercised only by the host test, not on hardware.

## Asked of review 3

- Confirm nit 1 and nit 2 are closed, and that the `cs_low()` model change is faithful to `s3c64xx_spi_set_cs`
  (`enable_cs`, `spi-s3c64xx.c:627`, auto-CS branch `:654-659`) and doesn't weaken the CS assertions in the other tests.
- Verdict on leaving `wip/` (commit on `device/raven`).

## Not in scope / known open

- No `con_service()` pumping during sec_ts delays (review 2 recommended keeping it this way; revisit only if USB drops on hardware).
- `tools/fboot.sh` / `tools/phone-guard` still refuse raven (AGENTS.md hard rule 2); separate reviewed change.
- Identify data lines are clipped at the panel's right edge (DEVICE_ID[1..4], ID[1..2]); cosmetic, not fixed here.
