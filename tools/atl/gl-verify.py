#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Check a file from atl_test_apks (ATL on GitLab) against its git blob id from the GitLab API (tree JSON):
#   gl-verify.py <tree.json> <file>
# blob id = sha1("blob <len>\0" + content), the same hash as in the repository's git tree.
import hashlib
import json
import os
import sys

tree, path = sys.argv[1:3]
want = {e["name"]: e["id"] for e in json.load(open(tree))}[os.path.basename(path)]
data = open(path, "rb").read()
got = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
if got != want:
    raise SystemExit(f"{path}: blob {got} != {want}")
print(f"{os.path.basename(path)}: git blob {got} OK")
