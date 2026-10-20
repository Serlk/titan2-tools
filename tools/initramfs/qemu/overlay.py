#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Test add-on to the initramfs for qemu (a second cpio, the kernel unpacks it on top):
# inittab with ::once:/etc/t2/qemu-test, the test itself and the host-side modules (cdc_ncm, cdc-acm).
#   overlay.py <kbuild-out> <out.cpio.gz> [id_test]
import gzip, importlib.util, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("t2build", os.path.join(HERE, "..", "build.py"))
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)
kb, out = sys.argv[1:3]
c = b.Cpio()
inittab = open(os.path.join(HERE, "..", "rootfs/etc/inittab")).read()
c.dir("etc", 0o755)
c.file("etc/inittab", (inittab + "::once:/etc/t2/qemu-test\n").encode(), 0o644)
c.dir("etc/t2", 0o755)
c.file("etc/t2/qemu-test", open(os.path.join(HERE, "qemu-test"), "rb").read(), 0o755)
c.dir("lib", 0o755)
c.dir("lib/t2-test", 0o755)
for m in ("drivers/net/mii.ko", "drivers/net/usb/usbnet.ko", "drivers/net/usb/cdc_ether.ko",
          "drivers/net/usb/cdc_ncm.ko", "drivers/usb/class/cdc-acm.ko"):
    c.file("lib/t2-test/" + os.path.basename(m), open(os.path.join(kb, m), "rb").read(), 0o644)
if len(sys.argv) > 3:
    c.file("lib/t2-test/id_test", open(sys.argv[3], "rb").read(), 0o600)
    # dbclient cannot read OpenSSH keys: dropbearconvert (verified Alpine package)
    import io, tarfile, zlib
    dl, rel = os.environ["T2_DL"], os.environ.get("ALPINE_RELEASE", "v3.24")
    d = os.path.join(dl, f"alpine-{rel}-{b.apk.ARCH}")
    e = b.apk.load_index(os.path.join(d, "APKINDEX.tar.gz"))["dropbear-convert"]
    m = b.apk.verify_apk(os.path.join(d, f"dropbear-convert-{e['V']}.apk"), e)
    t = tarfile.open(fileobj=io.BytesIO(zlib.decompress(m, 16 + zlib.MAX_WBITS)))
    c.file("lib/t2-test/dropbearconvert", t.extractfile("usr/bin/dropbearconvert").read(), 0o755)
open(out, "wb").write(gzip.compress(c.finish(), 9, mtime=0))
