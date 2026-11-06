# raven iOS touch (XNU sec_ts driver) + framebuffer: review 1 handoff (2026-11-06)

Work alone. Do not spawn subagents. Read-only review: do not edit files, commit, push, or run `fastboot`/`adb`.
Report findings as F1..Fn, each tagged blocker / should-fix / nit, with file:line and a concrete failure scenario.
End with a verdict per patch (0145, 0146, 0147, tools) on leaving `wip/` (AGENTS.md rule 8).

## Scope

- Repo `pixelios-kernel`. Two local branches, nothing pushed:
  - `wip/raven-fb-present`: commit `1246f32` (xnu patch 0145 + two hardware notes), on `device/raven` `7334b55`.
  - `wip/raven-sec-ts-xnu`, on `1246f32`, **uncommitted**: review `git diff 1246f32` plus the untracked files
    `xnu/patches/0146-sweet-gs101-touch-sec-ts.patch`, `xnu/patches/0147-sweet-hid-touch-panel-size.patch`,
    `shim/sweet_sec_ts_host_test.c`, `device/google/raven/hardware/touch-xnu-2026-11-06.txt`, and this file.
- Files: 0145 (23 lines), 0146 (654 lines: new `bsd/dev/sweet_sec_ts.h`, changes to `bsd/dev/sweet_touch_gs101.c`),
  0147 (76 lines, `iokit/Kernel/arm/hid/SweetHID.cpp`), `tools/mkadt.py`, `tools/boards.py` (comment only),
  `tools/test_mkadt_touch.py`, `shim/build.sh` (host-test wiring), `shim/sweet_sec_ts_host_test.c`.
- Goal: iOS touch on the Pixel 6 Pro (raven, Samsung S6SY79x `sec,sec_ts` on SPI11), mapped onto the presented 1125x2436
  display, with bluejay (FTM5) and sweet behaviour unchanged.
- Patched XNU tree to read the code in context: `tools/stack-apply --keep` (prints the scratch path; whole stack 0001-0147).
- Sources: Google `kernel/google-modules/touch/sec_touch` at `b23d1f8dfb9e` (local clone:
  `~/git/pixel6/xnu-local/refs/sec_touch`), cited as `sec_ts.c:N` / `sec_ts.h:N`; `spi-s3c64xx.c` cites are
  `kernel/gs 6e771b230c52`, same as the reviewed shim (`shim/touchprobe.h:101-102`).

## What each patch does

- **0145** (`SweetFramebuffer.cpp`, AppleCLCD::start): drops patch 0060's "panel <= 1125x2436" condition, so every panel is
  presented as 1125x2436 unless `sweetfb_native=1`. On raven `fOffX/fOffY = (int32_t)(1125 - 1440) / 2 = -157`, `-342`, and
  `present()` (`dx -= fOffX`, then its existing clipping) shifts layers into the panel centre. Replaces a 2-NOP binary hack.
- **0146** (`sweet_touch_gs101.c` + new `sweet_sec_ts.h`): `/sweet-touch` property `protocol` = `"sec_ts"` sets `tg.sec`
  and `tg.ch_mode = CPOL_L | CPHA_B`; no property = FTM5. `chip-id` becomes optional for sec_ts. sec_ts path: CH_CFG mode 3
  (engine init and after the ABL setup), PUD := 0 on five pads and reset + boot-complete wait *before* identify (always for
  sec_ts), identify = DEVICE_ID / ID / BOOT_STATUS (must be 0x20 app) / PANEL_INFO, enable = read SET_TOUCHFUNCTION, OR 0x61,
  write back, SENSE_ON; poll = READ_ONE_EVENT, then READ_ALL_EVENT for `byte7 & 0x3f` more (overflow > 31 ->
  CLEAR_EVENT_STACK), decode, `sweet_hid_touch()`. Firmware watchdog event (INFO, ack 0, data1 0x20) -> release all, re-enable.
  Transport `tg_cs_stream()`: one chip-select assertion, bursts of <= 64 bytes (FIFO), chip select held low between bursts.
  `tg_sec_read()`: request (8 bytes) in one CS, 100 us (550 us for READ_ALL_EVENT), reply in a second CS, up to 3 tries
  1 ms apart on a bad frame (as `sec_ts_read_internal`).
- **0147** (`SweetHID.cpp`): panel size from `PE_current_console` (the call AppleCLCD uses; 1080x2400 if none) instead of
  hard-coded 1080x2400. Clamp to the panel, offset to the presented display, clamp to it (margin touches -> nearest edge).
  Edge contact = old panel-edge band OR the same band measured at the presented display's edges.
- **tools**: `mkadt.py` emits `/sweet-touch` for `touch_protocol` `sec_ts` with `protocol = "sec_ts"` and no `chip-id`,
  refuses sec_ts + chip id and unknown protocols. Test updated (raven now HAS the node).

## Asked of review 1 (numbered)

1. **Bluejay / sweet unchanged.** Every 0146 change must be gated on `tg.sec` or be a no-op for FTM5. Check in particular:
   `tg_spi_engine_init` now writes `CH_CFG = tg.ch_mode` (0 for FTM5, was literal 0); the `if (tg.cs_abl || tg.sec)` block;
   the `printf` format changes (FTM5 text must come out identical); `tg_parse` with `(!tg.sec && !chip-id)`; the variant loop
   gated `!tg.sec`. For 0147: confirm the claim that on a 1080x2400 panel the new edge test equals the old one (logical band
   lies inside the panel band: lx = x+22, ly = y+18) and the clamps never bite; and `sweetfb_native=1` (now panel size, was
   1080x2400). For 0145: panels <= 1125x2436 take the same branch as before.
2. **sec_ts framing and decode vs source.** `sweet_sec_ts.h`: read request (`sec_ts.c:310-324`), reply length (`:326-328`),
   acceptance (`:411-435`), write frame (`:162-183`, incl. `(len+1)` length field and 4-byte zero padding), coordinate
   bitfield layout (`sec_ts.h:832-845`, GCC LE bitfields), status layout (`sec_ts.h:812-827`), boot-complete
   (`sec_ts.c:1096-1110`), watchdog (`:3192-3210`), left-event / overflow (`:3140-3157`), ttype policy (finger: NORMAL/GLOVE/WET;
   cancel: PALM/GRIP per `:2148-2153`; others ignored per `:2059-2068`). Check every cited line number.
3. **Transport risks.** (a) READ_ALL_EVENT replies up to 256 bytes are clocked as 4 bursts with CS held low; each ABL burst ends
   with a SW_RST pulse and FIFO drain (`tg_abl_burst`). Is resetting the channel mid-transfer safe with CPOL=1 (does SCK
   glitch on SW_RST)? Linux sends one 256-byte transfer (`sec,i2c-burstmax` absent -> 1024, `sec_ts.c:3870-3874`).
   (b) Retrying READ_ONE_EVENT after a bad frame: can an event be lost or duplicated (Linux does the same retry)?
   (c) Stack use: `tg_sec_read` has a 264-byte rx + `tg_sec_drain` a 256-byte buffer, on a kernel thread.
   (d) `delay()` / `tg_sleep_ms()` usage matches the FTM5 path's conventions.
4. **Enable / recovery order.** First start: SET_TOUCHFUNCTION then SENSE_ON (`sec_ts_fw_init :4378-4392`). After a watchdog
   event Linux sends SENSE_ON then `sec_ts_reinit` (touch function etc.); 0146 repeats the first-start order. Acceptable?
5. **Protocol property parse.** `SecureDTGetProperty` + `psize >= sizeof("sec_ts") && strncmp(..., sizeof("sec_ts"))`:
   unknown string -> driver not started. Any way a malformed ADT reaches the sec_ts path with garbage?
6. **0147 mapping.** Integer arithmetic with negative offsets, clamp order, `nx = (lx << 16) / fLogicalW` range (must stay
   < 1.0), `fEdge` semantics for a margin touch.
7. **Host test quality.** `shim/sweet_sec_ts_host_test.c` (CR-01 / CR-02): events are encoded through a copy of the downstream
   bitfield struct; observed frames from `touch-run1` are used. Mutation check run (scratch, not committed): 10 mutants of
   the header, 9 killed; survivor `n > SEC_TS_MAX_EVENTS - 1u` -> `n > SEC_TS_MAX_EVENTS` is equivalent (both return 32 for
   every n >= 32). Agree it is equivalent? Missing cases?
8. **mkadt.** bluejay and sweet ADTs byte-identical before/after (checked with `mkadt.build(seed=b"\0"*64)`, see below);
   raven gains only the node. Validation errors raise `ValueError`.

## Verification done

- `tools/stack-apply --only-new 0145` / `0146` / `0147`: PASS (136/136 patches + 3 stagers).
- Local kernel build (darwin-xnu-build `663906528d1c`, Xcode 15.4, clean tree, route in
  `device/google/raven/hardware/local-xnu-build-2026-11-06.txt`): 0145-0147 applied, no new warnings in
  `sweet_touch_gs101.c` / `sweet_sec_ts.h` / `SweetHID.cpp` (3 pre-existing SweetHID warnings). Kernel sha256
  `ac9c9d9f3154620dc2eb1cefeb350de3e407770484691d7ad8000e41786556d4`. Not built by CI yet (needs a push).
- Host test: header extracted from the 0146 patch by the `build.sh` awk line is byte-identical to the tree's file;
  `gcc -O1 -Wall -Wextra -Werror` passes; `sweet_sec_ts_host_test: all checks passed`.
- `tools/check-rules.py --strict`: PASS (0 new). `python3 -m unittest test_mkadt_touch test_patch_numbers test_patch_guard`: OK.
- ADT: bluejay identical, sweet identical, raven 14156 -> 16048 bytes (the `/sweet-touch` node).
- `tools/check-all.sh` does not run on macOS (needs `flock`); please run the gate on Linux if available.
- Hardware (observed by the user, 2026-11-06):
  - raven-xnu9-0145: lock screen, no binary patch (`hardware/fb-patch-0145-2026-11-06.txt`).
  - raven-xnu10-touch: touch works; taps at the visible corners, swipe up from the bottom of the glass (margin), two
    fingers, touches under the finger all passed (`hardware/touch-xnu-2026-11-06.txt`). The XNU console log was not read,
    so the sec_ts error paths (bad frames, overflow, watchdog) are exercised only by reading, not on hardware.
  - Bluejay was not booted with 0145-0147.

## Not in scope / known open

- Pre-existing, not changed here: the bluejay poke example `m:10c00640=0=10;...;m:10c0180c=5=f;` in the 0088 driver header,
  `tools/test_run_preflight.py:7` and `tools/agent-guard:1698` has mask and value swapped for the grammar
  `m:ADDR=MASK=VAL` (`sweet_touch_poke.h:9`, parser `:184-186`): the mux op writes nothing. Raven uses
  `m:10c00640=10=0;d:100;m:10c0180c=f=5;`. Say whether this should be a separate fix.
- `SurfaceWidth/SurfaceHeight` (mm) in SweetHID are still sweet's 68x151; raven's panel is larger (not needed for mapping).
- The raven touch boot-args live in the image record only; `tools/build-xnu-image.sh` is bluejay-only.
