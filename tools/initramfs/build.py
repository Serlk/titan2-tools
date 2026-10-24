#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Build the Titan 2 initramfs (cpio newc + gzip): busybox + dropbear from verified
# Alpine aarch64 packages and the tools/initramfs/rootfs overlay. Package files are taken directly
# from the tar inside the .apk (not unpacked on the host), owner is 0:0 everywhere, timestamps are fixed.
#
#   build.py <dl-dir> <release> <out.cpio.gz>
#
# tools/initramfs/authorized_keys (or $T2_AUTHORIZED_KEYS, if set) -> /root/.ssh/authorized_keys, key-only
# login; otherwise root has an EMPTY password (this initramfs only, see titan2-docs: docs/hardware/usb-gadget.md).
import gzip
import importlib.util
import io
import os
import stat
import sys
import tarfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("t2apk", os.path.join(HERE, "apk.py"))
apk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apk)

MTIME = int(os.environ.get("SOURCE_DATE_EPOCH", "1767225600"))  # 2026-01-01

# package -> {path in package: path in initramfs}; None = unchanged
PACKAGES = {
    "busybox-static": {"bin/busybox.static": "bin/busybox"},
    "busybox-extras": {"bin/busybox-extras": None},  # udhcpd (not in busybox-static)
    "dropbear": {"usr/sbin/dropbear": None, "usr/bin/dropbearkey": None},
    "dropbear-dbclient": {"usr/bin/dbclient": None},
    "dropbear-scp": {"usr/bin/scp": None},
    "musl": {"lib/ld-musl-aarch64.so.1": None, "lib/libc.musl-aarch64.so.1": None},
    "zlib": {"usr/lib/libz.so.1": None, "usr/lib/libz.so.1.3.2": None},
    "utmps-libs": {"usr/lib/libutmps.so.0.1": None, "usr/lib/libutmps.so.0.1.3.3": None},
    "skalibs-libs": {"usr/lib/libskarnet.so.2.15": None, "usr/lib/libskarnet.so.2.15.0.0": None},
    # hardware checks (community): I2C buses and input events
    "i2c-tools": {"usr/sbin/i2cdetect": None, "usr/sbin/i2cget": None, "usr/sbin/i2cset": None,
                  "usr/sbin/i2cdump": None, "usr/sbin/i2ctransfer": None,
                  "usr/lib/libi2c.so.0": None, "usr/lib/libi2c.so.0.1.1": None},
    "evtest": {"usr/bin/evtest": None},
    # e2fsck: checks the root on the linux partition before the rw mount (/etc/t2/diskroot)
    "e2fsprogs": {"sbin/e2fsck": None},
    "e2fsprogs-libs": {"usr/lib/libext2fs.so.2": None, "usr/lib/libext2fs.so.2.4": None,
                       "usr/lib/libe2p.so.2": None, "usr/lib/libe2p.so.2.3": None},
    "libcom_err": {"usr/lib/libcom_err.so.2": None, "usr/lib/libcom_err.so.2.1": None},
    "libblkid": {"usr/lib/libblkid.so.1": None, "usr/lib/libblkid.so.1.1.0": None},
    "libuuid": {"usr/lib/libuuid.so.1": None, "usr/lib/libuuid.so.1.3.0": None},
    "libeconf": {"usr/lib/libeconf.so.0": None, "usr/lib/libeconf.so.0.8.3": None},
}

DIRS = {
    "bin": 0o755, "sbin": 0o755, "lib": 0o755, "usr": 0o755, "usr/bin": 0o755,
    "usr/sbin": 0o755, "usr/lib": 0o755, "etc": 0o755, "etc/dropbear": 0o700,
    "dev": 0o755, "proc": 0o555, "sys": 0o555, "run": 0o755, "tmp": 0o1777,
    "var": 0o755, "var/empty": 0o755, "mnt": 0o755, "root": 0o700, "root/.ssh": 0o700,
}


class Cpio:
    def __init__(self):
        self.buf = io.BytesIO()
        self.ino = 721
        self.names = {}

    def _add(self, name, mode, data=b"", rdev=(0, 0)):
        name = name.strip("/")
        if not name or ".." in name.split("/"):
            raise SystemExit(f"bad name {name!r}")
        if name in self.names:
            raise SystemExit(f"duplicate {name}")
        self.names[name] = mode
        self.ino += 1
        nb = name.encode() + b"\0"
        hdr = "070701" + "".join(f"{v:08x}" for v in (
            self.ino, mode, 0, 0, 1, MTIME, len(data), 0, 0, rdev[0], rdev[1], len(nb), 0))
        self.buf.write(hdr.encode() + nb)
        self.buf.write(b"\0" * (-(110 + len(nb)) % 4))
        self.buf.write(data)
        self.buf.write(b"\0" * (-len(data) % 4))

    def dir(self, name, perm=0o755):
        self._add(name, stat.S_IFDIR | perm)

    def file(self, name, data, perm):
        self._add(name, stat.S_IFREG | perm, data)

    def symlink(self, name, target):
        self._add(name, stat.S_IFLNK | 0o777, target.encode())

    def chr(self, name, perm, major, minor):
        self._add(name, stat.S_IFCHR | perm, rdev=(major, minor))

    def finish(self):
        self._add("TRAILER!!!", 0)
        return self.buf.getvalue()


def main():
    dl, rel, out = sys.argv[1:4]
    d = os.path.join(dl, f"alpine-{rel}-{apk.ARCH}")
    pkgs = apk.load_indexes(d)
    c = Cpio()
    for n, p in sorted(DIRS.items()):
        c.dir(n, p)

    for pkg, files in PACKAGES.items():
        e = pkgs[pkg]
        member = apk.verify_apk(os.path.join(d, f"{pkg}-{e['V']}.apk"), e)
        t = tarfile.open(fileobj=io.BytesIO(zlib.decompress(member, 16 + zlib.MAX_WBITS)))
        seen = set()
        for ti in t.getmembers():
            if ti.name not in files:
                continue
            dst = files[ti.name] or ti.name
            if ti.isreg():
                c.file(dst, t.extractfile(ti).read(), ti.mode & 0o7777 & ~0o6000)
            elif ti.issym():
                c.symlink(dst, ti.linkname)
            else:
                raise SystemExit(f"{pkg}:{ti.name}: unexpected type")
            seen.add(ti.name)
        if seen != set(files):
            raise SystemExit(f"{pkg}: missing {set(files) - seen}")
        print(f"{pkg}-{e['V']}: {len(seen)} files", file=sys.stderr)

    # overlay
    root = os.path.join(HERE, "rootfs")
    for dp, dns, fns in os.walk(root):
        dns.sort()
        rel_d = os.path.relpath(dp, root)
        for dn in dns:
            n = os.path.normpath(os.path.join(rel_d, dn))
            if n not in c.names:
                c.dir(n, 0o755)
        for fn in sorted(fns):
            src = os.path.join(dp, fn)
            n = os.path.normpath(os.path.join(rel_d, fn))
            st = os.lstat(src)
            if stat.S_ISLNK(st.st_mode):
                c.symlink(n, os.readlink(src))
            else:
                c.file(n, open(src, "rb").read(), 0o755 if st.st_mode & 0o111 else 0o644)

    # t2-reboot (reboot with a reboot-mode argument): built by build.sh from tools/t2-reboot
    t2r = os.path.join(HERE, "..", "t2-reboot", "t2-reboot")
    c.file("usr/bin/t2-reboot", open(t2r, "rb").read(), 0o755)

    keys = os.environ.get("T2_AUTHORIZED_KEYS") or os.path.join(HERE, "authorized_keys")
    if os.path.isfile(keys) and open(keys, "rb").read().strip():
        c.file("root/.ssh/authorized_keys", open(keys, "rb").read(), 0o600)
        shadow = "root:*:20454:0:::::\n"  # no password: SSH by key only, serial login by Enter
        print("authorized_keys: key login", file=sys.stderr)
    else:
        shadow = "root::20454:0:::::\n"  # EMPTY password, dropbear -B
        print("no authorized_keys: root with an EMPTY password", file=sys.stderr)
    c.file("etc/shadow", (shadow + "sshd:!:20454:0:::::\n").encode(), 0o600)

    # /init: picks the mode (plain: busybox init; t2.root=: network root or the linux partition)
    c.symlink("init", "etc/t2/init")
    c.symlink("bin/sh", "busybox")
    c.chr("dev/console", 0o600, 5, 1)
    c.chr("dev/null", 0o666, 1, 3)
    c.chr("dev/kmsg", 0o644, 1, 11)

    data = c.finish()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(gzip.compress(data, 9, mtime=0))
    with open(out + ".list", "w") as f:
        for n, m in sorted(c.names.items()):
            if n != "TRAILER!!!":
                f.write(f"{stat.filemode(m)} {n}\n")
    print(f"{out}: {len(data)} bytes cpio, {os.path.getsize(out)} bytes gz", file=sys.stderr)


if __name__ == "__main__":
    main()
