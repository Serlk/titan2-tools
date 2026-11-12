#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# The layout of the GPUFreq shared memory read here follows the GPL gpufreq_v2.h of the
# MediaTek downstream kernel (kernel_device_modules).
"""Compare Titan 2 devfreq with GPUEB while requesting several OPPs.

Run as root on MT6878 Linux. Only maps the GPUFreq reserved RAM, read-only;
never accesses GPU/PLL MMIO. Restores runtime PM and devfreq limits on exit.
"""
import json
import mmap
import os
from pathlib import Path
import struct
import sys
import time

df = Path('/sys/class/devfreq/13000000.gpu')
power = df.resolve().parent.parent / 'power/control'
with open('/proc/device-tree/compatible', 'rb') as f:
    if b'mediatek,mt6878\0' not in f.read():
        raise SystemExit('This check requires MT6878')

# Use the bootloader's carveout, rather than assuming a physical address.
reserved = Path('/proc/device-tree/reserved-memory')
regions = [p for p in reserved.iterdir() if (p / 'compatible').exists()
           and b'mediatek,me_GPUEB_SHARED\0' in (p / 'compatible').read_bytes()]
if len(regions) != 1:
    raise SystemExit('GPUFreq shared-memory carveout not uniquely identified')
reg = (regions[0] / 'reg').read_bytes()
address, size = struct.unpack('>QQ', reg)
if address % mmap.PAGESIZE or size < 0x4000:
    raise SystemExit('Unexpected carveout alignment or size')
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
mem = mmap.mmap(fd, 0x4000, flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ, offset=address)
os.close(fd)

def word(offset):
    return struct.unpack_from('<I', mem, offset)[0]

if word(0) != 0xbabadada:
    raise SystemExit('GPUFreq shared-memory magic mismatch')

def read(name):
    return (df / name).read_text().strip()

def write(name, value):
    (df / name).write_text(str(value))

def snapshot(requested):
    return dict(requested=requested, target=int(read('target_freq')),
                current=int(read('cur_freq')), gpu_khz=word(0x44),
                stack_khz=word(0x48), con1_gpu_khz=word(0x4c),
                con1_stack_khz=word(0x50), opp=word(4),
                power_count=word(0x1c), active_count=word(0x2c),
                dvfs_state=hex(word(0xcc)))

original = {name: read(name) for name in ['min_freq', 'max_freq']}
original_power = power.read_text().strip()
freqs = [int(f) for f in read('available_frequencies').split()]
try:
    power.write_text('on')
    for freq in [freqs[0], freqs[len(freqs) // 2], freqs[-1], freqs[0]]:
        write('min_freq', freqs[0])
        write('max_freq', freq)
        write('min_freq', freq)
        time.sleep(0.3)
        result = snapshot(freq)
        print(json.dumps(result), flush=True)
        if '--verify' in sys.argv:
            if not result['current'] or result['current'] != result['gpu_khz'] * 1000:
                raise RuntimeError('devfreq disagrees with GPUEB')
            if result['gpu_khz'] * 1000 != freq:
                raise RuntimeError('GPUEB did not apply the requested OPP (check limiters)')
finally:
    write('min_freq', freqs[0])
    write('max_freq', original['max_freq'])
    write('min_freq', original['min_freq'])
    power.write_text(original_power)
    mem.close()
