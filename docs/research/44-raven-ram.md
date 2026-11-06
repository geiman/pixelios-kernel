# 44 - raven RAM: XNU uses 4 of 12 GiB; what it takes to use the 10 GiB high bank (2026-11-06)

Tags: *observed* = measured on raven, *read* = read from source/scripts, *guess*.

## Where things stand

- Physical DRAM (observed, `device/google/raven/hardware/live-fdt-2026-11-05.txt`, bootloader slider-1.3-11170084): three low
  `/memory` nodes `0x80000000+0x37200000`, `0xc0000000+0x20000000`, `0xe2500000+0x1db00000` (1.82 GiB with holes and the
  ABL / ramoops / framebuffer carve-outs), and five contiguous 2 GiB nodes `0x880000000..0xb00000000` (10 GiB), merged in
  `board.json` `dram_banks` into one bank. Total 11.82 GiB.
- XNU gets `board.json` `xnu_ram` = `0x880000000 + 0x100000000` (4 GiB): `tools/mkadt.py` writes it to `/chosen`
  `dram-base` / `dram-size` (`mkadt.py:87,95-96`), `tools/mkpayload.py` copies it into the payload header after checking it
  lies inside one `dram_banks` entry (`mkpayload.py:168-172`), and the shim passes it unchanged as `boot_args.physBase` /
  `memSize` / `memSizeActual` (`shim/loader.c:126-139`). The 4 GiB is a guess carried over from bluejay, where it is the
  whole high range (`board.json` notes `xnu_ram`, read).
- XNU sees it (observed, raven-xnu11-touch log `logs/raven-touch2-console.txt:5`): `vm_page_bootstrap: 258585 free pages,
  3559 wired pages` = 3.95 GiB free + 56 MiB wired at 16 KiB pages. The other 6 GiB (`0x980000000..0xb00000000`) are unused.

## Limits checked (read from source unless tagged)

- **Shim**: none. `memSize` is passed through; the shim's own MMU covers 64 GiB in 32 MiB blocks (`shim/mmu.c:5`) and is not
  used for XNU's RAM (everything is written with the MMU off before the jump, `loader.c:146-149`).
- **mkpayload**: the window must sit inside one bank: `0x880000000 + 0x280000000` fits the merged high bank exactly.
  32 MiB alignment (bluejay note, the 16K-granule L2 block): base and 10 GiB size are both multiples.
- **XNU `arm_vm_init`** (xnu-8796 + stack, `osfmk/arm64/arm_vm_init.c`): the kernel is built with `ARM_LARGE_MEMORY=1`
  (SDKROOT=macosx, `makedefs/MakeInc.def:333-336`; link address `0xfffffe0007004000` matches), so the kernel VA range is
  64 GiB + 512 GiB (`osfmk/mach/arm/vm_param.h:190-208`). Checks: `mem_size >= (VM_MAX - VM_MIN) / 2` panics
  (`:1896-1898`, limit ~288 GiB); `dynamic_memory_begin > VM_MAX_KERNEL_ADDRESS` panics (`:1949-1951`); the bootstrap
  tables must hold `2 * (l1_entries + 1)` pages with one L1 entry per 64 GiB (`:1957-1965`, `BOOTSTRAP_TABLE_SIZE`
  `osfmk/arm/pmap/pmap.h:113-115`). 10 GiB is far inside all three. Patch 0011 already pads the physmap for this layout.
- **Fixed addresses in the high bank**: none. Every hex constant in `board.json` above 32 GiB is the bank / `xnu_ram` base;
  the shim's pstore zone is `0xFD3FF000` (observed, `hardware/touch-run2-2026-11-06.txt:23`), ABL's USB event buffer is in
  `abl@f8800000` (observed, `hardware/probe-run1-2026-11-05.txt:23`), the payload lands in the low bank (`0x87A10A20`,
  observed, `hardware/xnu-run1-2-2026-11-05.txt:12`). The live FDT has no fixed reserved-memory `reg` in the high range;
  eight Linux CMA pools only list it in `alloc-ranges` (nothing is allocated before Linux runs).
- **Patches**: no patch hard-codes the 4 GiB window. 0034 already allows UFS DMA above 4 GiB (`CAP.64AS`); the Mali patches'
  `MALI_VA_END 0x100000000` is a GPU *virtual* address limit, not physical.

## The open risk: DMA filtering (S2MPU)

gs101 puts an S2MPU (stage-2 memory protection unit) in front of DMA masters. UFS sits behind `s2mpu_hsi2` at `0x14480000`
(shared with PCIe; read, `docs/research/12-wifi-bluejay.md:78`, from `gs101-s2mpu.dtsi`). It allows or blocks DMA per
1 GiB granule per VID (`gb_index = PA >> 30`, 64 granules, attr bits RD/WR at `L1ENTRY_ATTR(n, m)` `0x4004 + n*0x200 + m*8`;
read, `shim/gpus2mpu.c:4-10` citing `s2mpu-regs.h`). Today XNU's UFS DMA works inside granules `0x22..0x25` (observed: DMA at
`0x88457c000`, `0x884b98000`). Granules `0x26..0x2b` (the new 6 GiB) have never been DMA targets under XNU. The G3D S2MPU was
found disabled after ABL on bluejay (`docs/research/30`, observed there); the HSI2 instance on raven is not known (*guess*:
also disabled or open, since Linux loads its own S2MPU tables later).
A blocked UFS read would show as an I/O error or a hang; a blocked write (with `ufsrw=1`) could put bad data on the iOS root
(reversible: reflash `ios-root-20H392.simg`, but it would cost a reflash).

## Result of step 1 (observed 2026-11-06, `device/google/raven/hardware/mem-s2mpu-2026-11-06.txt`)

The HSI2 S2MPU is **off** at handoff: `CTRL0 = 0` (ENABLE clear), every L1 entry of all 8 VIDs reads 0, no fault recorded. It does not filter
UFS DMA anywhere, so it does not limit XNU's RAM window.

## Result of step 2 (observed 2026-11-06, `device/google/raven/hardware/ram-10g-2026-11-06.txt`)

With `xnu_ram` = 10 GiB: read-only root panics at the root mount (expected without `ufsrw=1`, no log); with `--rw`, lock screen and touch,
`vm_page_bootstrap: 650165 free pages, 5195 wired` (10 GiB managed, was 4 GiB), no new errors against the 4 GiB run. Still open (step 3):
nothing in the log shows a page above `0x980000000` in use.

## Result of step 3 (observed 2026-11-06, same record, runs 3-5)

xnu patch 0148 (`sweetmemtest`) passed every new granule `0x26..0x2b` in at least one of three runs: 569,337 page tests through DRAM
with 0 errors, and 11 UFS READs DMA'd straight into pages of those granules matching the bounce-buffer copy. Early in boot the allocator
hands out pages bottom-up, so reaching the top granules needed a late start (60 s) and a low floor (2 GiB). Limits: one pattern pass per
page, READ-direction DMA only. Conclusion: raven's XNU window is the whole 10 GiB high bank.

## Plan

1. **Read-only probe of `s2mpu_hsi2` on raven** (shim). Reuse the read-only pattern and register map of `gpus2mpu.c` with a
   new base: print CTRL0 (ENABLE) and the L1 attrs of all 8 VIDs x 64 granules, guarded reads only, no power gate (HSI2 is
   never power-gated, `shim/wlanprobe.c` header), no MMIO store. Add `s2mpu_hsi2` to `soc.json` (`mem.s2mpu_hsi2`) with its
   citation, a host test, and the boot-safety audit like `gpuprobe.o`. Done: `shim/memprobe.c` (`--mem-probe`). Needs an independent review before it leaves `wip/` (it reads hardware).
   Result decides step 2: disabled or all granules RD|WR = go; otherwise the usable granules bound the window.
2. **Boot with a larger window, read-only root first.** `board.json` `xnu_ram` size `0x280000000` (10 GiB) on a wip branch,
   image with `tools/build-xnu-image.sh --board raven --kernel K --cpus 8 --gui --touch` *without* `--rw` (UFS read-only).
   Check: `vm_page_bootstrap` ~ 655k pages, boot to the lock screen, UFS DMA addresses in the log, no UFS errors.
   Then the same with `--rw`.
3. **Pressure test.** Something that actually touches memory above `0x980000000`: e.g. a boot-arg-gated XNU self-test that
   allocates and checks wired pages until it gets PAs in each new granule, and one UFS read into such a page (design TBD).
   Until this passes, "10 GiB works" means "boots", not "all of it is usable" (rule 5).
4. Record each step in `device/google/raven/hardware/`, then commit `xnu_ram` with the evidence.

Not covered: the 1.82 GiB low banks (holes and carve-outs; XNU needs one contiguous cacheable range), iOS userland behaviour
with 10 GiB (*guess*: fine; iOS sizes jetsam from `hw.memsize`, and 8-16 GiB iPads exist).
