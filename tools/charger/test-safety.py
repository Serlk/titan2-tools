#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
"""Exercise actual driver PM/CHG_EN functions with injected regmap failures.
Not a hardware, concurrency, or full-kernel test.

    tools/charger/test-safety.py [KERNEL_TREE]

KERNEL_TREE: a titan2-linux checkout (default: KERNEL_SRC, else linux/ in the root of this
repository, the SRC of scripts/build-mainline.sh).
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
tree = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('KERNEL_SRC', root / 'linux'))
driver = tree / 'drivers/power/supply/mt6375-charger.c'
if not driver.is_file():
    sys.exit(f'{driver}: not found (pass the kernel tree: test-safety.py <titan2-linux checkout>)')
source = driver.read_text()
def function(name):
    start = source.index('static ', source.rfind('\n}', 0, source.index(name + '(')) + 2)
    end = source.index('\n}', source.index(name + '(')) + 2
    return source[start:end]

stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#define MT6375_REG_CHG_TOP1 0x120
#define MT6375_CHG_EN 1
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define system_freezable_wq 0
struct device { void *data; };
struct platform_device { struct device dev; };
struct mt6375_chg {
 struct device *dev; void *regmap; int lock, monitor;
 bool configured, suspended, stopping, fault_latched, chg_on;
 const char *reason;
};
static unsigned int reg;
static int write_error, read_error, mismatch, writes, queued, cancelled;
static void mutex_lock(int *p) { assert(!*p); *p = 1; }
static void mutex_unlock(int *p) { assert(*p); *p = 0; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static int regmap_update_bits(void *m, int a, int mask, int v) {
 (void)m; assert(a == MT6375_REG_CHG_TOP1); writes++;
 if (write_error) return -EIO;
 if (!mismatch) reg = (reg & ~mask) | v;
 return 0;
}
static int regmap_read(void *m, int a, unsigned int *v) {
 (void)m; assert(a == MT6375_REG_CHG_TOP1);
 if (read_error) return -EIO;
 *v = reg; return 0;
}
static void cancel_delayed_work_sync(int *p) { (void)p; cancelled++; }
static void mod_delayed_work(int q, int *p, int t) { (void)q; (void)p; (void)t; queued++; }
'''
tests = r'''
int main(void) {
 struct platform_device p = {0};
 struct mt6375_chg c = {.dev = &p.dev, .configured = true};
 p.dev.data = &c;
 reg = 1;
 assert(mt6375_chg_suspend(&p.dev) == 0);
 assert(!(reg & 1) && c.suspended && cancelled == 1);
 assert(mt6375_chg_resume(&p.dev) == 0);
 assert(!c.suspended && queued == 1 && !(reg & 1));
 reg = 1; write_error = 1;
 assert(mt6375_chg_suspend(&p.dev) == -EIO);
 assert(!c.suspended && c.fault_latched && queued == 2 && cancelled == 1);
 write_error = 0; read_error = 1;
 assert(mt6375_set_chg_en(&c, false, "read error") == -EIO);
 read_error = 0; mismatch = 1; reg = 1;
 assert(mt6375_set_chg_en(&c, false, "readback mismatch") == -EIO);
 mismatch = 0;
 mt6375_chg_stop(&p);
 assert(c.stopping && !(reg & 1) && cancelled == 2);
 int previous = queued;
 mt6375_chg_resume(&p.dev);
 assert(queued == previous);
 c.configured = false; c.stopping = false; reg = 1; previous = writes;
 assert(mt6375_chg_suspend(&p.dev) == 0);
 mt6375_chg_resume(&p.dev);
 mt6375_chg_stop(&p);
 assert(writes == previous && reg == 1);
 puts("PASS: disable/readback, suspend failure, resume, stop, report-only");
}
'''
with tempfile.TemporaryDirectory(prefix='titan2-charger-test-') as d:
    c = Path(d) / 'test.c'
    c.write_text(stub + '\n'.join(function(n) for n in (
        'mt6375_set_chg_en', 'mt6375_chg_suspend', 'mt6375_chg_resume', 'mt6375_chg_stop')) + tests)
    binary = Path(d) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
