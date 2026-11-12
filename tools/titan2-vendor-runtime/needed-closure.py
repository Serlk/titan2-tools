#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
"""
DT_NEEDED closure of Android binaries of the factory firmware, for the vendor-root
file lists (files.d/*.txt of titan2-vendor-runtime and the packages using it): which .so files a daemon or
HAL pulls in, and where they live in the extracted partitions.

  needed-closure.py SRC BIN...      SRC = the extracted partitions (system_a, system_ext_a,
                                    vendor_a, product_a); BIN = path below SRC
Prints "<source path>\t<path in the root>" lines (the files.d list format) and
the libraries it could not find. Only ELF headers are read (interface facts).
"""
import os
import subprocess
import sys

LIBDIRS = [
    ("system_a/system/lib64", "system/lib64"),
    ("system_ext_a/lib64", "system/lib64"),
    ("vendor_a/lib64", "vendor/lib64"),
    ("vendor_a/lib64/hw", "vendor/lib64/hw"),
    ("product_a/lib64", "system/lib64"),
    ("system_a/system/lib64/bootstrap", "system/lib64"),
]


def needed(path):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    return [l.split("[")[1].rstrip("]") for l in out.splitlines() if "(NEEDED)" in l]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src = sys.argv[1]
    todo = [b for b in sys.argv[2:]]
    seen = {}
    missing = set()
    for b in todo:
        p = os.path.join(src, b)
        if not os.path.isfile(p):
            sys.exit(f"no {p}")
        seen[os.path.basename(b)] = (b, None)
    queue = list(todo)
    while queue:
        b = queue.pop()
        for lib in needed(os.path.join(src, b)):
            if lib in seen or lib in missing:
                continue
            for d, dst in LIBDIRS:
                cand = os.path.join(src, d, lib)
                if os.path.isfile(cand) or os.path.islink(cand):
                    if os.path.islink(cand) and not os.path.exists(cand):
                        continue  # dangling (apex) link
                    rel = os.path.join(d, lib)
                    seen[lib] = (rel, os.path.join(dst, lib))
                    queue.append(rel)
                    break
            else:
                missing.add(lib)
    for name, (rel, dst) in sorted(seen.items()):
        if dst is None:  # one of the inputs: it goes to the same path below the root
            dst = rel.split("/", 1)[1] if rel.startswith(("system_a/system/", "system_ext_a/", "vendor_a/", "product_a/")) else rel
            if rel.startswith("system_a/system/"):
                dst = rel[len("system_a/"):]
            elif rel.startswith("system_ext_a/"):
                dst = "system/" + rel[len("system_ext_a/"):]
        # files.d source paths are relative to the partition roots
        srcp = rel.replace("system_a/system/", "system/", 1).replace("system_ext_a/", "system_ext/", 1)
        srcp = srcp.replace("vendor_a/", "vendor/", 1).replace("product_a/", "product/", 1)
        size = os.path.getsize(os.path.join(src, rel))
        print(f"{srcp}\t{dst}\t# {size // 1024} KiB")
    for m in sorted(missing):
        print(f"# MISSING {m} (apex or bootstrap only?)", file=sys.stderr)


if __name__ == "__main__":
    main()
