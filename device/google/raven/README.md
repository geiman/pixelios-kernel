# Pixel 6 Pro (raven)

SoC: `soc/google/gs101`, shared with bluejay, so every SoC-level value (GIC, timer, UFS, UART, ACPM, GPU) comes from
`soc/google/gs101/soc.json` unchanged. Status: **scaffold, never booted.** Captures from this device go in `device/google/raven/hardware/`
(sanitized: no serial numbers, IMEI, MAC addresses).

## What differs from bluejay

| Item | raven | Source | Tag |
|---|---|---|---|
| Framebuffer | 0xfac00000, 1440x3120, stride 5760, a8r8g8b8 | Linux d68ce4e71248 `gs101-raven.dts` | read from source |
| Volume/power keys | same pins as bluejay | `gs101-pixel-common.dtsi` | read from source |
| DRAM | 3 low banks + contiguous 10 GiB at 0x880000000 | live FDT, `device/google/raven/hardware/live-fdt-2026-11-05.txt` | measured |
| Touch controller | Samsung S6SY79x (`sec_ts`), same SPI11 wiring as bluejay | live FDT | measured; needs a new driver |
| Panel | Samsung S6E3HC3, 1440x3120 | live FDT | measured |
| userdata, USB DMA range | 112914 MiB (as bluejay); abl@f8800000 16 MiB | sysfs, live FDT | measured |

## Bring-up, in order

1. **Capture the live device tree** (rooted Android, phone reporting `ro.product.device` = `raven`):

   ```sh
   adb shell getprop ro.product.device         # must say raven
   adb shell su -c 'cat /sys/firmware/fdt' > /tmp/raven-live.dtb
   dtc -I dtb -O dts /tmp/raven-live.dtb > /tmp/raven-live.dts
   ```

   On LineageOS without `su`, enable Developer options → Rooted debugging and use `adb root` + `adb exec-out cat /sys/firmware/fdt`.
   Keep the raw dtb out of git: `/chosen` carries bootargs with the serial number. Commit only a sanitized excerpt.
   **Done 2026-11-05** (`device/google/raven/hardware/live-fdt-2026-11-05.txt`): `tools/pix check raven` prints `raven: ok`.

2. **First shim boot, display only.** `tools/pix build raven --avb`, then a temporary `fastboot boot` (see
   `device/google/bluejay/docs/GATE0.md`; replace `bluejay` with `raven`). `tools/fboot.sh` refuses raven
   until its product guard is extended in a reviewed change. **Done 2026-11-05** (`device/google/raven/hardware/probe-run1-2026-11-05.txt`): the AVB
   image ran on the first try; report on the panel, all 8 CPUs, UFS cold re-init to HS-G4 and a full GPT read. pstore does not
   survive the reset on raven (ABL overwrites the zone), so read logs from the photo pager or a `--usb-gadget` build.

3. **XNU to the root mount.** **Done 2026-11-05** (`device/google/raven/hardware/xnu-run1-2-2026-11-05.txt`): with PixOS's CI kernel, XNU draws
   on the 1440x3120 panel, passes its self-tests, finds `userdata` read-only over UFS (DMA in high RAM) and retries the root mount,
   since userdata holds LineageOS data. Needed `shim_align_payload`: LineageOS's vendor ramdisk leaves the payload unaligned.

4. **iOS GUI.** **Done 2026-11-05** (`device/google/raven/hardware/ios-gui-2026-11-05.txt`): iOS 16.7.16 lock screen on the panel. Needs: the iOS root
   flashed to `userdata` (as a sparse image on macOS), `/product` from the IPSW's D22AP device tree, the framebuffer presenting
   1125x2436 (now xnu 0145, `device/google/raven/hardware/fb-patch-0145-2026-11-06.txt`), and a first boot on a fresh root.

5. **Touch in iOS.** **Done 2026-11-06** (`device/google/raven/hardware/touch-xnu-2026-11-06.txt`): xnu 0146 (sec_ts path of the touch driver) and
   0147 (SweetHID panel size); lock screen moves and unlocks, margin touches clamp to the presented display. CI kernels (Xcode 16.0)
   boot with touch too (`device/google/raven/hardware/ci-xcode16-2026-11-06.txt`).

6. **Next:** more RAM (the 10 GiB high bank), then Wi-Fi/GPU.

Build an image (kernel from CI or the local build; on Linux, since `shim/build.sh` host tests stop on macOS after the image is
written): `tools/build-xnu-image.sh --board raven --kernel K --cpus 8 --rw --gui --touch [--out FILE]`. It makes a fresh ADT with
`tools/mkadt.py --board raven` (needs the iPhone X device tree research/ipsw-x/d22ap.adt, docs/REQUIREMENTS.md) unless `--adt` is given; `--touch` adds board.json
`touch_xnu_bootargs` and the shim's touch rails. Boot with `fastboot boot` from the bootloader (not fastbootd).
