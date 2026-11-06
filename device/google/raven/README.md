# Pixel 6 Pro (raven)

SoC: `soc/google/gs101`, shared with bluejay, so every SoC-level value (GIC, timer, UFS, UART, ACPM, GPU) comes from
`soc/google/gs101/soc.json` unchanged. Status: **scaffold, never booted.** Captures from this device go in `hardware/`
(sanitized: no serial numbers, IMEI, MAC addresses).

## What differs from bluejay

| Item | raven | Source | Tag |
|---|---|---|---|
| Framebuffer | 0xfac00000, 1440x3120, stride 5760, a8r8g8b8 | Linux d68ce4e71248 `gs101-raven.dts` | read from source |
| Volume/power keys | same pins as bluejay | `gs101-pixel-common.dtsi` | read from source |
| DRAM | 3 low banks + contiguous 10 GiB at 0x880000000 | live FDT, `hardware/live-fdt-2026-11-05.txt` | measured |
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
   **Done 2026-11-05** (`hardware/live-fdt-2026-11-05.txt`): `tools/pix check raven` prints `raven: ok`.

2. **First shim boot, display only.** `tools/pix build raven --avb`, then a temporary `fastboot boot` (see
   `device/google/bluejay/docs/GATE0.md`; replace `bluejay` with `raven`). `tools/fboot.sh` refuses raven
   until its product guard is extended in a reviewed change. **Done 2026-11-05** (`hardware/probe-run1-2026-11-05.txt`): the AVB
   image ran on the first try; report on the panel, all 8 CPUs, UFS cold re-init to HS-G4 and a full GPT read. pstore does not
   survive the reset on raven (ABL overwrites the zone), so read logs from the photo pager or a `--usb-gadget` build.

3. **XNU to the root mount.** **Done 2026-11-05** (`hardware/xnu-run1-2-2026-11-05.txt`): with PixOS's CI kernel, XNU draws
   on the 1440x3120 panel, passes its self-tests, finds `userdata` read-only over UFS (DMA in high RAM) and retries the root mount,
   since userdata holds LineageOS data. Needed `shim_align_payload`: LineageOS's vendor ramdisk leaves the payload unaligned.

4. **launchd and beyond** need an iOS root in `userdata`, which erases Android's data there; then touch (`sec_ts`) for raven.
