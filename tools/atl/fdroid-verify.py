#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Download an APK from the main F-Droid repository, checking the whole chain:
#   1. index-v1.jar is signed (JAR signature: PKCS#7 over the .SF) by a certificate with the
#      SHA-256 fingerprint FDROID_FP (the known fingerprint of f-droid.org/repo); checked with
#      openssl cms;
#   2. the .SF holds the digest of MANIFEST.MF, MANIFEST.MF the digest of index-v1.json;
#   3. the APK's sha256 = "hash" from index-v1.json.
#   fdroid-verify.py <index-v1.jar> <packageName> <dest-dir>
# The jar is not unpacked to disk (except the .SF/.RSA, into a temporary directory for openssl).
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

FDROID_FP = "43238d512c1e5eb2d6569f4a3afbf5523418b82e0a3ed1552770abb9a9c9ccab"
REPO = "https://f-droid.org/repo"


def manifest_sections(text):
    secs, cur = [], []
    for line in text.replace("\r\n", "\n").split("\n"):
        if line.startswith(" ") and cur:
            cur[-1] += line[1:]
        elif line == "":
            if cur:
                secs.append(cur)
            cur = []
        else:
            cur.append(line)
    if cur:
        secs.append(cur)
    return [dict(l.split(": ", 1) for l in s) for s in secs]


def digest_of(fields, data, suffix):
    for alg, fn in (("SHA-256", hashlib.sha256), ("SHA1", hashlib.sha1), ("SHA-1", hashlib.sha1)):
        k = f"{alg}-Digest{suffix}"
        if k in fields:
            return fields[k], base64.b64encode(fn(data).digest()).decode()
    raise SystemExit(f"no *-Digest{suffix} digest")


def verify_index(jar):
    z = zipfile.ZipFile(jar)
    names = z.namelist()
    sf = [n for n in names if n.startswith("META-INF/") and n.endswith(".SF")]
    if len(sf) != 1:
        raise SystemExit(f".SF signatures: {sf}")
    base = sf[0][:-3]
    blk = [n for n in names if n.startswith(base + ".") and n[len(base) + 1:] in ("RSA", "DSA", "EC")]
    if len(blk) != 1:
        raise SystemExit(f"signature blocks: {blk}")
    with tempfile.TemporaryDirectory() as td:
        sfp, blp, cert = (os.path.join(td, x) for x in ("a.sf", "a.p7", "cert.pem"))
        open(sfp, "wb").write(z.read(sf[0]))
        open(blp, "wb").write(z.read(blk[0]))
        subprocess.run(["openssl", "cms", "-verify", "-noverify", "-binary", "-inform", "DER",
                        "-in", blp, "-content", sfp, "-certsout", cert, "-out", os.devnull],
                       check=True, capture_output=True)
        der = subprocess.run(["openssl", "x509", "-in", cert, "-outform", "DER"],
                             check=True, capture_output=True).stdout
    fp = hashlib.sha256(der).hexdigest()
    if fp != FDROID_FP:
        raise SystemExit(f"index certificate {fp} != F-Droid {FDROID_FP}")
    mf = z.read("META-INF/MANIFEST.MF")
    sfsec = manifest_sections(z.read(sf[0]).decode())
    want, got = digest_of(sfsec[0], mf, "-Manifest")
    if want != got:
        raise SystemExit("digest of MANIFEST.MF in the .SF does not match")
    entry = next(s for s in manifest_sections(mf.decode()) if s.get("Name") == "index-v1.json")
    data = z.read("index-v1.json")
    want, got = digest_of(entry, data, "")
    if want != got:
        raise SystemExit("digest of index-v1.json does not match")
    print(f"index-v1.jar: F-Droid signature OK ({blk[0]}, certificate {fp[:16]}...)", file=sys.stderr)
    return json.loads(data)


def main():
    jar, pkg, dest = sys.argv[1:4]
    idx = verify_index(jar)
    vers = idx["packages"].get(pkg)
    if not vers:
        raise SystemExit(f"{pkg}: not in the index")
    v = max(vers, key=lambda e: e["versionCode"])
    if v.get("hashType", "sha256") != "sha256":
        raise SystemExit(f"{pkg}: hashType {v.get('hashType')}")
    path = os.path.join(dest, v["apkName"])
    if not os.path.isfile(path):
        with urllib.request.urlopen(f"{REPO}/{v['apkName']}", timeout=120) as r:
            open(path + ".part", "wb").write(r.read())
        os.replace(path + ".part", path)
    h = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if h != v["hash"]:
        os.remove(path)
        raise SystemExit(f"{path}: sha256 {h} != index {v['hash']}")
    print(f"{v['apkName']}: {pkg} {v.get('versionName')} sha256 OK, minSdk {v.get('minSdkVersion')}"
          f" target {v.get('targetSdkVersion')} nativecode {v.get('nativecode', [])}")


if __name__ == "__main__":
    main()
