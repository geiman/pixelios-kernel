#!/usr/bin/env python3
"""Decode the persistent console log (xnu patch 0047, "sweetlog") from the last 4 MiB + 4 KiB of userdata.

  adb exec-out dd if=/dev/block/by-name/userdata bs=4096 skip=$((BLOCKS - 1025)) count=1025 > tail.bin
  tools/sweetlog-decode.py tail.bin > console.txt

Layout (patch 0047): device block 0 = header "SWEETLOG" | u32 version 1 | u32 ring size | u64 total bytes written |
u64 flush sequence (little-endian); the ring follows, byte n at ring offset n % ring size.
"""
import struct
import sys

BLOCK = 4096
data = open(sys.argv[1], 'rb').read()
if len(data) < BLOCK or data[:8] != b'SWEETLOG':
    sys.exit('no SWEETLOG header in the first block (log never flushed, or wrong offset)')
version, ring_size, total, seq = struct.unpack_from('<IIQQ', data, 8)
if version != 1 or ring_size == 0 or len(data) < BLOCK + ring_size:
    sys.exit(f'unexpected header: version {version}, ring {ring_size}, file {len(data)} bytes')
ring = data[BLOCK:BLOCK + ring_size]
if total <= ring_size:
    text = ring[:total]
else:
    at = total % ring_size
    text = ring[at:] + ring[:at]
sys.stderr.write(f'sweetlog: version {version}, ring {ring_size} bytes, {total} bytes written, flush #{seq}'
                 f'{", oldest bytes overwritten" if total > ring_size else ""}\n')
sys.stdout.buffer.write(text)
