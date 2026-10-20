#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Download and verify Alpine (aarch64) packages for the Titan 2 initramfs.
#
#   apk.py fetch <dl-dir> <release> <pkg>...   download APKINDEX and packages, verify the chain
#   apk.py list  <dl-dir> <release> <pkg>...   same without network: only verify what is already downloaded
#
# Repositories: main (APKINDEX.tar.gz) and community (APKINDEX-community.tar.gz); a package is looked
# up in main first. Both indexes are signed with the same Alpine keys.
#
# Chain of trust (everything offline except the HTTPS downloads themselves):
#   1. APKINDEX.tar.gz is signed with an Alpine key (RSA/SHA-1 over the second gzip member);
#      the keys are in tools/initramfs/keys (checked against alpinelinux.org/keys and aports/alpine-keys).
#   2. For a package, C: in the index = "Q1" + base64(sha1(control member of the .apk)).
#   3. datahash from .PKGINFO (in the verified control) = sha256(data member of the .apk).
# Package files are not unpacked on the host: tools/initramfs/build.py reads the tar directly from the .apk.
import base64
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zlib

MIRROR = os.environ.get("ALPINE_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")
ARCH = "aarch64"
KEYDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "keys")


def gzip_members(data):
    """Split a concatenation of gzip streams into the original (compressed) pieces."""
    out, pos = [], 0
    while pos < len(data):
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        d.decompress(data[pos:])
        if not d.eof:
            raise ValueError("truncated gzip member")
        end = len(data) - len(d.unused_data)
        out.append(data[pos:end])
        pos = end
    return out


def tar_of(member):
    return tarfile.open(fileobj=io.BytesIO(zlib.decompress(member, 16 + zlib.MAX_WBITS)))


def verify_rsa_sha1(sig_name, sig, payload):
    key = os.path.join(KEYDIR, sig_name.split(".SIGN.RSA.", 1)[1])
    if not sig_name.startswith(".SIGN.RSA.") or not os.path.isfile(key):
        raise SystemExit(f"no trusted key for {sig_name}")
    with tempfile.TemporaryDirectory() as t:
        sp, pp = os.path.join(t, "sig"), os.path.join(t, "payload")
        open(sp, "wb").write(sig)
        open(pp, "wb").write(payload)
        r = subprocess.run(["openssl", "dgst", "-sha1", "-verify", key, "-signature", sp, pp],
                           capture_output=True, text=True)
    if r.returncode != 0 or "Verified OK" not in r.stdout:
        raise SystemExit(f"signature mismatch ({sig_name}): {r.stdout}{r.stderr}")


def load_index(path):
    data = open(path, "rb").read()
    m = gzip_members(data)
    if len(m) != 2:
        raise SystemExit(f"{path}: no signature")
    sig = tar_of(m[0])
    names = [n for n in sig.getnames() if n.startswith(".SIGN.")]
    if len(names) != 1:
        raise SystemExit(f"{path}: signatures {names}")
    verify_rsa_sha1(names[0], sig.extractfile(names[0]).read(), b"".join(m[1:]))
    idx = tar_of(m[1]).extractfile("APKINDEX").read().decode()
    pkgs = {}
    for block in idx.split("\n\n"):
        f = {}
        for line in block.splitlines():
            if len(line) > 2 and line[1] == ":":
                f.setdefault(line[0], line[2:])
        if "P" in f:
            pkgs[f["P"]] = f
    return pkgs


REPOS = ("main", "community")


def index_path(d, repo):
    return os.path.join(d, "APKINDEX.tar.gz" if repo == "main" else f"APKINDEX-{repo}.tar.gz")


def load_indexes(d):
    """Indexes of all downloaded repositories; each entry has a "repo" field; main takes precedence."""
    pkgs = {}
    for repo in reversed(REPOS):
        path = index_path(d, repo)
        if repo != "main" and not os.path.isfile(path):
            continue
        for name, e in load_index(path).items():
            e["repo"] = repo
            pkgs[name] = e
    return pkgs


def verify_apk(path, ent):
    data = open(path, "rb").read()
    m = gzip_members(data)
    if len(m) != 3:
        raise SystemExit(f"{path}: expected 3 gzip members, got {len(m)}")
    c = "Q1" + base64.b64encode(hashlib.sha1(m[1]).digest()).decode()
    if c != ent["C"]:
        raise SystemExit(f"{path}: control sha1 {c} != index {ent['C']}")
    info = tar_of(m[1]).extractfile(".PKGINFO").read().decode()
    dh = [l.split(" = ", 1)[1] for l in info.splitlines() if l.startswith("datahash = ")]
    if not dh or hashlib.sha256(m[2]).hexdigest() != dh[0]:
        raise SystemExit(f"{path}: datahash mismatch")
    return m[2]


def fetch(url, dest):
    tmp = dest + ".part"
    with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
        f.write(r.read())
    os.replace(tmp, dest)


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("fetch", "list"):
        raise SystemExit(__doc__ if __doc__ else "usage: apk.py fetch|list dl release pkg...")
    mode, dl, rel, want = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    d = os.path.join(dl, f"alpine-{rel}-{ARCH}")
    os.makedirs(d, exist_ok=True)
    if mode == "fetch":
        for repo in REPOS:
            fetch(f"{MIRROR}/{rel}/{repo}/{ARCH}/APKINDEX.tar.gz", index_path(d, repo))
    pkgs = load_indexes(d)
    print(f"APKINDEX {rel}/{ARCH}: signature OK, {len(pkgs)} packages", file=sys.stderr)
    # so:/name dependencies must be satisfied by the list itself (no automatic resolution)
    provided = set(want)
    for p in want:
        if p not in pkgs:
            raise SystemExit(f"no package {p}")
        for prov in pkgs[p].get("p", "").split():
            provided.add(prov.split("=")[0])
    for p in want:
        if p not in pkgs:
            raise SystemExit(f"no package {p}")
        e = pkgs[p]
        f = os.path.join(d, f"{p}-{e['V']}.apk")
        if mode == "fetch" and not os.path.isfile(f):
            fetch(f"{MIRROR}/{rel}/{e['repo']}/{ARCH}/{p}-{e['V']}.apk", f)
        verify_apk(f, e)
        missing = [x for x in e.get("D", "").split()
                   if not x.startswith("!") and x.split("=")[0] not in provided]
        print(f"{f}\t{e['V']}\tOK" + (f"\tunsatisfied: {' '.join(missing)}" if missing else ""))


if __name__ == "__main__":
    main()
