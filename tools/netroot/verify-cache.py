#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Independent check of the apk cache of the Titan 2 Alpine root (scripts/netroot-serve.sh build)
# with the same chain as tools/initramfs/apk.py:
#   APKINDEX.<hash>.tar.gz: signature with the Alpine key from tools/initramfs/keys (RSA/SHA-1),
#   <pkg>-<ver>.<hash>.apk: sha1 of the control member = C: from the index, sha256 data = datahash.
# apk inside the container checks the same itself (--keys-dir with the same keys); this is a
# second check of every package that went into the image, independent of apk-tools.
#   verify-cache.py <cache-dir> <pkg-list-file>
# <pkg-list-file>: "name version" per line (from lib/apk/db/installed of the built rootfs).
# Exit 0: every package of the list is in the cache and verified.
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("t2apk", os.path.join(HERE, "..", "initramfs", "apk.py"))
apk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apk)


def main():
    cache, listf = sys.argv[1:3]
    index = {}
    for fn in sorted(os.listdir(cache)):
        if fn.startswith("APKINDEX.") and fn.endswith(".tar.gz"):
            for name, e in apk.load_index(os.path.join(cache, fn)).items():
                index[f"{name}-{e['V']}"] = e
            print(f"{fn}: signature OK", file=sys.stderr)
    files = {}
    for fn in os.listdir(cache):
        if fn.endswith(".apk") and not fn.startswith("APKINDEX."):
            base = fn[:-4].rsplit(".", 1)[0]  # cut off .<hash>
            files[base] = os.path.join(cache, fn)
    bad = 0
    n = 0
    for line in open(listf):
        if not line.strip():
            continue
        name, ver = line.split()
        key = f"{name}-{ver}"
        if key not in index:
            print(f"{key}: not in the signed indexes", file=sys.stderr)
            bad += 1
            continue
        if key not in files and index[key].get("I") == "0":
            # a meta package without files (alpine-base): apk does not download it, it puts
            # nothing into the image; its entry comes from the signed index
            print(f"{key}: meta package (I:0), no files", file=sys.stderr)
            continue
        if key not in files:
            print(f"{key}: not in the cache", file=sys.stderr)
            bad += 1
            continue
        apk.verify_apk(files[key], index[key])  # SystemExit on a mismatch
        n += 1
    print(f"packages verified: {n}, errors: {bad}", file=sys.stderr)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
