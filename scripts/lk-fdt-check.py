#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
"""Offline check of a DTB (and boot images) against the known requirements of the Titan 2 factory LK.

LK is not modified, so everything it expects from our DTB has to be checked before flashing.
The requirements are described in titan2-docs: docs/lk-boot-requirements.md, §1–§4 (DTB
selection, properties LK writes into the DTB, image addresses) and §6 (firmware loading).

Usage (the host python is partly broken, so run it in the container):
  docker run --rm -u $(id -u):$(id -g) -v "$PWD":/work titan2-kbuild \
      python3 /work/scripts/lk-fdt-check.py /work/out/mainline-images-usb-minimal/titan2.dtb
  ... lk-fdt-check.py --vendor-boot vendor_boot.img [--boot boot.img] [--init-boot init_boot.img]

The DTB may be a raw FDT, a dt_table (0xd7b7ab1e, LK takes the first entry) or embedded
in vendor_boot (then the DTB is extracted the same way LK does it).

Exit status: 0 if there is no FAIL (WARN is allowed), 1 otherwise.
Only the standard library is used.
"""
import argparse
import struct
import sys

FDT_MAGIC = 0xD00DFEED
DT_TABLE_MAGIC = 0xD7B7AB1E
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9

# addresses of the preloader's mblock reservations (see §4)
MB_KERNEL = 0x40000000
MB_DTB = 0x47C80000
MB_DTB_SIZE = 0x400000
MB_RAMDISK = 0x66F00000
MB_RAMDISK_SIZE = 0x4000000
VENDOR_RAMDISK_TYPE_PLATFORM = 1
VENDOR_RAMDISK_TYPE_RECOVERY = 2


# ---------------------------------------------------------------- FDT parser
class Node:
    def __init__(self, name, parent=None):
        self.name = name
        self.parent = parent
        self.props = {}       # name -> bytes, insertion order is kept
        self.children = []

    @property
    def path(self):
        if self.parent is None:
            return "/"
        p = self.parent.path
        return (p if p != "/" else "") + "/" + self.name

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


def parse_fdt(blob):
    magic, totalsize, off_struct, off_strings, _rsv, version = struct.unpack_from(">6I", blob, 0)
    if magic != FDT_MAGIC:
        raise ValueError("no FDT magic (0x%08x)" % magic)
    size_strings, size_struct = struct.unpack_from(">2I", blob, 32)
    strings = blob[off_strings:off_strings + size_strings]
    pos = off_struct
    end = off_struct + size_struct
    root = None
    cur = None
    while pos < end:
        tok, = struct.unpack_from(">I", blob, pos)
        pos += 4
        if tok == FDT_BEGIN_NODE:
            z = blob.index(b"\0", pos)
            name = blob[pos:z].decode(errors="replace")
            pos = (z + 1 + 3) & ~3
            n = Node(name, cur)
            if cur is None:
                root = n
            else:
                cur.children.append(n)
            cur = n
        elif tok == FDT_END_NODE:
            cur = cur.parent
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack_from(">2I", blob, pos)
            pos += 8
            z = strings.index(b"\0", nameoff)
            pname = strings[nameoff:z].decode(errors="replace")
            cur.props[pname] = bytes(blob[pos:pos + ln])
            pos = (pos + ln + 3) & ~3
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise ValueError("unknown FDT token 0x%x @0x%x" % (tok, pos - 4))
    return root, totalsize, version


def name_eq(nodename, comp):
    """libfdt fdt_nodename_eq_: "soc" matches "soc@0", "soc@0" matches only "soc@0"."""
    if nodename == comp:
        return True
    return "@" not in comp and nodename.startswith(comp + "@")


def path_offset(root, path):
    """fdt_path_offset without aliases (LK always passes paths starting with '/')."""
    n = root
    for comp in [c for c in path.split("/") if c]:
        nxt = None
        for c in n.children:
            if name_eq(c.name, comp):
                nxt = c
                break
        if nxt is None:
            return None
        n = nxt
    return n


def by_compatible(root, compat):
    """fdt_node_offset_by_compatible(fdt, -1, compat): first node in DT order."""
    want = compat.encode()
    for n in root.walk():
        c = n.props.get("compatible")
        if c is not None and want in c.split(b"\0"):
            return n
    return None


def strncmp(a, b, n):
    """C strncmp; a and b are byte strings (NUL is implied past the end)."""
    for i in range(n):
        ca = a[i] if i < len(a) else 0
        cb = b[i] if i < len(b) else 0
        if ca != cb:
            return ca - cb
        if ca == 0:
            return 0
    return 0


def cstr(v):
    return v.split(b"\0", 1)[0]


def u32(v, i=0):
    return struct.unpack_from(">I", v, 4 * i)[0]


# ---------------------------------------------------------------- reporting
class Report:
    def __init__(self):
        self.fails = 0
        self.warns = 0

    def _p(self, tag, msg):
        print("%-4s %s" % (tag, msg))

    def ok(self, m): self._p("OK", m)
    def info(self, m): self._p("..", m)

    def warn(self, m):
        self.warns += 1
        self._p("WARN", m)

    def fail(self, m):
        self.fails += 1
        self._p("FAIL", m)

    def section(self, title):
        print("\n== " + title)


# ---------------------------------------------------------------- images
def extract_dtb_from_blob(blob, r):
    magic, = struct.unpack_from(">I", blob, 0)
    if magic == DT_TABLE_MAGIC:
        cnt, = struct.unpack_from(">I", blob, 16)
        off = 32 + 32 * cnt
        r.info("dt_table: %d entries, LK takes the first blob at offset 0x%x without board-id matching" % (cnt, off))
        if cnt == 0:
            r.fail("dt_table has no entries")
        return blob[off:]
    return blob


def check_vendor_boot(path, r):
    r.section("vendor_boot (%s)" % path)
    d = open(path, "rb").read()
    if d[:8] != b"VNDRBOOT":
        r.fail("no VNDRBOOT magic")
        return None
    ver, page, kaddr, raddr, vrsize = struct.unpack_from("<5I", d, 8)
    tags, = struct.unpack_from("<I", d, 2076)
    hdr_size, dtb_size = struct.unpack_from("<2I", d, 2096)
    r.info("header v%d, page %d, kernel_addr 0x%x, ramdisk_addr 0x%x, tags_addr 0x%x" % (ver, page, kaddr, raddr, tags))
    if ver < 3:
        r.fail("vendor_boot v%d: LK (v3/v4) expects header_version 3 or 4" % ver)
    for nm, val, want in (("kernel_addr", kaddr, MB_KERNEL), ("ramdisk_addr", raddr, MB_RAMDISK),
                          ("tags_addr (the DTB is placed here)", tags, MB_DTB)):
        (r.ok if val == want else r.fail)("%s = 0x%x (need 0x%x, preloader mblock)" % (nm, val, want))
    if dtb_size == 0:
        r.fail("dtb_size == 0 -> LK finds no DTB (header v%d) and stops" % ver)
        return None
    rnd = lambda x: (x + page - 1) // page * page
    off = rnd(hdr_size) + rnd(vrsize)
    dtb = d[off:off + dtb_size]
    if ver >= 4:
        tsize, tnum, tesz, bcsize = struct.unpack_from("<4I", d, 2112)
        toff = off + rnd(dtb_size)
        r.info("vendor_ramdisk_table: %d entries of %d bytes, bootconfig %d bytes" % (tnum, tesz, bcsize))
        # LK counts the entries as table_size / 0x6c and looks for an entry of the requested type;
        # if there is none, it stops with an assertion
        n = tsize // 0x6C
        types = []
        tot = 0
        for i in range(n):
            sz, _o, typ = struct.unpack_from("<3I", d, toff + i * 0x6C)
            types.append(typ)
            tot += sz
        if VENDOR_RAMDISK_TYPE_PLATFORM in types:
            r.ok("vendor ramdisk of type PLATFORM present (types=%s)" % types)
        else:
            r.fail("no vendor ramdisk of type PLATFORM (types=%s) -> LK assertion, boot stops" % types)
        if VENDOR_RAMDISK_TYPE_RECOVERY not in types:
            r.warn("no vendor ramdisk of type RECOVERY: booting to recovery (volume up) stops on the LK "
                   "assertion (seen in LK's log on 2026-10-20); normal boot is not affected")
        if vrsize + 0x400 > MB_RAMDISK_SIZE:
            r.fail("vendor ramdisk 0x%x + 0x400 > mb_ramdisk 64 MiB" % vrsize)
    return extract_dtb_from_blob(dtb, r)


def check_boot(path, r):
    r.section("boot (%s)" % path)
    d = open(path, "rb").read()
    if d[:8] != b"ANDROID!":
        r.fail("no ANDROID! magic")
        return
    ksize, = struct.unpack_from("<I", d, 8)
    ver, = struct.unpack_from("<I", d, 40)
    if ver < 3:
        r.warn("boot header v%d (only v4 has been tested)" % ver)
        return
    k = d[4096:4096 + 4]
    if k[:3] == b"\x1f\x8b\x08":
        r.ok("kernel is gzip, %d bytes" % ksize)
    elif k == b"\x02\x21\x4c\x18":
        r.ok("kernel is legacy LZ4, %d bytes" % ksize)
    else:
        r.fail("kernel is not gzip/legacy-lz4 (%s) -> LK panics (not a compressed image)" % k.hex())
    cmd = cstr(d[44:44 + 1536])
    r.info("boot.img cmdline: %d bytes" % len(cmd))


def check_init_boot(path, r):
    r.section("init_boot (%s)" % path)
    d = open(path, "rb").read()
    (r.ok if d[:8] == b"ANDROID!" else r.fail)("ANDROID! magic (with v4 LK requires a valid init_boot)")


# ---------------------------------------------------------------- DT checks
def check_container(blob, r):
    r.section("FDT")
    root, total, ver = parse_fdt(blob)
    r.info("totalsize %d bytes, version %d" % (total, ver))
    if total > MB_DTB_SIZE:
        r.fail("DTB is larger than mb_dtb (4 MiB)")
    elif total > MB_DTB_SIZE - 0x80000:
        r.warn("DTB is larger than 3.5 MiB: LK adds the memory layout, about 47 reserved nodes, atags etc.")
    else:
        r.ok("size fits into the 4 MiB mb_dtb with margin")
    return root


def check_lk_dt_writes(root, r):
    r.section("DT properties written by LK / mblock (§2)")
    ac = root.props.get("#address-cells")
    sc = root.props.get("#size-cells")
    if ac and sc and u32(ac) == 2 and u32(sc) == 2:
        r.ok("root #address-cells = <2>, #size-cells = <2>")
    else:
        r.fail("root needs #address-cells = <2>, #size-cells = <2> (LK writes 16-byte reg entries)")

    ch = path_offset(root, "/chosen")
    if ch is None:
        r.fail("no /chosen")
    else:
        ba = ch.props.get("bootargs")
        if ba is None:
            r.fail("no /chosen/bootargs -> LK cannot build the kernel cmdline and stops with an assertion")
        else:
            r.ok("/chosen/bootargs: %d bytes (total cmdline limit after LK is 2047, otherwise PANIC)" % len(cstr(ba)))
            if len(cstr(ba)) > 1024:
                r.warn("bootargs longer than 1 KiB: with LK's additions it can easily exceed 2047")
        ks = ch.props.get("kaslr-seed")
        if ks is not None and len(ks) != 8:
            r.fail("/chosen/kaslr-seed is %d bytes long, must be exactly 8 -> LK assertion" % len(ks))
        for p in ("linux,initrd-start", "linux,initrd-end"):
            if p in ch.props:
                r.info("/chosen/%s is set in the DT, LK overwrites it (4 bytes)" % p)

    mem = path_offset(root, "/memory")
    (r.ok if mem is not None else r.fail)("/memory (LK writes reg and the memory layout into it)%s"
                                          % ("" if mem is None else " = " + mem.path))

    rm = path_offset(root, "/reserved-memory")
    if rm is None:
        r.fail("no /reserved-memory -> LK stops")
    else:
        a = rm.props.get("#address-cells")
        s = rm.props.get("#size-cells")
        if a and s and u32(a) == 2 and u32(s) == 2:
            r.ok("/reserved-memory 2/2")
        else:
            r.fail("/reserved-memory: need #address-cells = <2>, #size-cells = <2>")
        if "ranges" not in rm.props:
            r.warn("/reserved-memory has no ranges;")
        for c in rm.children:
            if "reg" in c.props:
                v = c.props["reg"]
                regs = [(struct.unpack_from(">Q", v, i)[0], struct.unpack_from(">Q", v, i + 8)[0])
                        for i in range(0, len(v) - 15, 16)]
                r.warn("static reserved %s reg=%s: an overlap (even an exact match) with an LK mblock reservation "
                       "-> LK assertion" % (c.name, ", ".join("0x%x+0x%x" % x for x in regs)))
            elif "size" in c.props:
                r.info("dynamic reserved %s: the first alloc-ranges must fit size in free DRAM "
                       "(otherwise LK assertion)" % c.name)

    for what, paths, why in (
            ("emicen", ("/soc/emicen", "/emicen"), "LK writes the EMI info here, otherwise LK assertion"),
            ("emiisu", ("/soc/emiisu", "/emiisu"), "LK writes the EMI info here, otherwise LK assertion"),
            ("wifi", ("/wifi",), "LK writes memory-region here, otherwise "
                                 "LK panics")):
        hit = next((path_offset(root, p) for p in paths if path_offset(root, p) is not None), None)
        if hit is None:
            r.fail("no %s (%s): %s" % (what, " | ".join(paths), why))
        else:
            r.ok("%s -> %s" % (what, hit.path))

    n = by_compatible(root, "mediatek, soc-dbg-error-flag")
    if n is None:
        r.fail('no node with compatible "mediatek, soc-dbg-error-flag" (with the space) -> LK assertion')
    else:
        r.ok("soc-dbg-error-flag -> %s" % n.path)

    m = by_compatible(root, "mediatek,mt6878-mmc")
    if m is not None:
        r.info("when booting from UFS, LK sets status=disabled on the first mediatek,mt6878-mmc: %s" % m.path)

    mdl = root.props.get("model")
    if mdl is None:
        r.info("no /model: LK does not write a model")


def check_lk_dt_writes_full(root, r):
    """Full list of DT changes LK makes before starting the kernel (titan2-docs: docs/lk-boot-requirements.md §7)."""
    r.section("DT properties written by LK: full list (§7)")
    PG, K64 = 0xFFF, 0xFFFF

    # ADSP shared memory
    mbox = by_compatible(root, "mediatek,scp-audio-mbox")
    share = by_compatible(root, "mediatek,reserve-memory-adsp_share")
    total = 0
    names = []
    if mbox is not None:
        for k, v in mbox.props.items():
            if "adsp-rsv" in k and len(v) >= 4:
                total += (u32(v) + PG) & ~PG
                names.append("%s=0x%x" % (k, u32(v)))
    if share is not None:
        r.warn("mediatek,reserve-memory-adsp_share present (%s): LK does not allocate adsp_shared_reserved but appends "
               "size to that node; this path has not been tested" % share.path)
    elif mbox is None:
        r.fail('no node with compatible "mediatek,scp-audio-mbox" -> LK cannot reserve '
               '"adsp_shared_reserved" -> LK panics (seen on the 2nd attempt)')
    elif total == 0:
        r.fail("scp-audio-mbox %s has no non-zero adsp-rsv-* properties -> size 0 -> panic" % mbox.path)
    else:
        r.ok("scp-audio-mbox %s: %s -> adsp_shared_reserved 0x%x (16 MiB aligned, below 0xa0000000)"
             % (mbox.path, ", ".join(names), (total + K64) & ~K64))
    sad = by_compatible(root, "mediatek,snd-audio-dsp")
    if sad is not None:
        r.warn("mediatek,snd-audio-dsp present (%s): LK appends one more adsp-rsv-audio to scp-audio-mbox" % sad.path)

    # SSPM shared memory
    sspm = by_compatible(root, "mediatek,sspm")
    if sspm is not None and "sspm-res-ram-size" in sspm.props:
        sz = u32(sspm.props["sspm-res-ram-size"])
        if sz == 0:
            r.fail("mediatek,sspm %s: sspm-res-ram-size = 0 -> zero-size mblock allocation -> panic" % sspm.path)
        else:
            r.info("mediatek,sspm: LK allocates sspm_ap-shared 0x%x below 2 GiB" % sz)

    # MET reserved memory (eng/userdebug LK only)
    if by_compatible(root, "mediatek,met_res_ram") is not None:
        r.info("mediatek,met_res_ram: a user LK skips it (a userdebug LK allocates memory from the subnode sizes)")

    # kaslr (duplicates check_lk_dt_writes, but also checks kaslr-status)
    ch = path_offset(root, "/chosen")
    if ch is not None and "kaslr-status" in ch.props:
        r.info("/chosen/kaslr-status = %r (\"disabled\" -> LK adds nokaslr)" % cstr(ch.props["kaslr-status"]))

    # last step: LK adds the /bootprof node itself and stops with an assertion if it already exists
    if any(name_eq(c.name, "bootprof") for c in root.children):
        r.fail("/bootprof already exists in the root -> LK cannot add it (EXISTS) -> LK assertion")
    else:
        r.ok("no /bootprof (LK creates it itself)")

    # nodes LK creates itself; if they already exist, LK only logs it
    for nm, who in (("debug-kinfo", "mrdump"), ("mdproperties", "ccci"),
                    ("md_attr_node", "ccci")):
        if any(c.name == nm for c in root.children):
            r.info("/%s already exists: LK (%s) does not create it (log message only)" % (nm, who))

    # 24: watchdog
    wd = path_offset(root, "/watchdog") or path_offset(root, "/soc/watchdog")
    if wd is not None:
        v = wd.props.get("apwdt_en", wd.props.get("apwdten"))
        if v is not None and len(v) >= 4 and u32(v) == 0:
            r.info("%s: apwdt_en = 0 -> LK disables the hardware WDT before the kernel" % wd.path)
        else:
            r.info("%s: apwdt_en is non-zero or unset -> LK leaves the WDT as is" % wd.path)

    # several other LK steps require /chosen (checked in check_lk_dt_writes).
    if path_offset(root, "/memory-ssmr-features") is not None:
        r.info("/memory-ssmr-features present: LK rewrites size/alloc-ranges of "
               "ssheap-reserved-cma_memory / ssmr-reserved-cma-memory (if present)")


def status_okay(node):
    """Same check as LK does for SCP/VCP: strncmp(status, "okay", len(status)) == 0."""
    st = node.props.get("status")
    if st is None:
        return False, "no status"
    return strncmp(st, b"okay", len(st)) == 0, "status=%r" % cstr(st).decode(errors="replace")


def check_fw_loaders(root, r):
    r.section("firmware loading by LK (§6), normal mode")

    # scp
    n = by_compatible(root, "mediatek,scp")
    if n is None:
        r.fail('scp: no node with compatible "mediatek,scp" -> LK assertion, boot stops')
    else:
        ok, why = status_okay(n)
        if not ok:
            r.fail("scp: %s on %s (need \"okay\") -> LK assertion, boot stops" % (why, n.path))
        else:
            r.ok("scp: %s, status okay -> LK loads scp_<slot> and starts the SCP (as the factory system does)" % n.path)
        sd = n.props.get("secure-dump")
        secure = bool(sd) and len(sd) > 0 and strncmp(sd, b"enable", 7) == 0
        if secure:
            sds = n.props.get("secure-dump-size")
            if not sds or u32(sds) == 0:
                r.fail("scp: secure-dump = enable without a non-zero secure-dump-size -> LK assertion")
        sp = n.props.get("scp-protect")
        if sp and len(sp) > 0 and strncmp(sp, b"disable", 8) != 0 and not secure:
            r.fail("scp: scp-protect enabled without secure-dump = enable -> LK assertion")
        mt = n.props.get("scp-mem-tbl")
        if mt:
            if len(mt) > 0x137:
                r.warn("scp: scp-mem-tbl is %d bytes, more than LK accepts: LK skips the tables" % len(mt))
            elif len(mt) % 12:
                r.warn("scp: scp-mem-tbl length is not a multiple of 12")
            else:
                bad = [u32(mt, i * 3) for i in range(len(mt) // 12) if u32(mt, i * 3) > 0x18]
                if bad:
                    r.warn("scp: id > 0x18 in scp-mem-tbl %s: LK stops parsing" % bad)
        else:
            r.warn("scp: no scp-mem-tbl: LK skips it, but the SCP starts without a memory table (unlike the factory DT)")
        md = n.props.get("memorydump")
        if md is not None and len(md) != 20:
            r.warn("scp: memorydump must have 5 cells (now %d bytes), otherwise LK only logs it" % len(md))

    # ccu
    n = by_compatible(root, "mediatek,ccu_rproc")
    if n is None:
        r.ok("ccu: no mediatek,ccu_rproc -> the CCU is not loaded")
    else:
        sec = n.props.get("secured")
        if sec is not None and strncmp(sec, b"yes", len(sec)) == 0:
            r.warn("ccu: %s secured=yes -> LK loads the CCU firmware (ccu partition) and issues an SMC; errors -> LK assertion"
                   % n.path)
        else:
            r.ok("ccu: %s without secured=yes -> skipped, 0" % n.path)

    # spm
    n = by_compatible(root, "mediatek,sleep")
    r.ok("spm: %s" % ("mediatek,sleep -> %s, LK writes spmfw_version" % n.path if n
                      else "no mediatek,sleep -> log message only; spmfw goes to ATF without the DT"))
    r.ok("apu, adsp: nothing to load; dpm: does not read the FDT")

    # apusys_rv
    hit = None
    for p in ("/apusys-rv@190e1000", "/soc/apusys-rv@190e1000", "/apusys_rv", "/soc/apusys_rv"):
        hit = path_offset(root, p)
        if hit is not None:
            break
    if hit is None:
        r.fail("apusys_rv: none of /apusys-rv@190e1000, /soc/apusys-rv@190e1000, /apusys_rv, /soc/apusys_rv "
               "-> LK assertion")
    elif "status" not in hit.props:
        r.fail("apusys_rv: %s has no status -> LK assertion" % hit.path)
    elif strncmp(hit.props["status"], b"okay", 4) == 0:
        r.warn("apusys_rv: %s status okay -> LK loads the APU firmware (as the factory system does); \"disabled\" is better for mainline"
               % hit.path)
    else:
        r.ok("apusys_rv: %s status=%r -> the APU is not loaded" % (hit.path, cstr(hit.props["status"]).decode()))

    # modem
    md = path_offset(root, "/mddriver") or path_offset(root, "/soc/mddriver")
    if md is None:
        r.ok("modem: no /mddriver -> LK loads the modem as the factory system does (dump settings skipped)")
    else:
        dis = any(name.startswith("ref-args") and b"md_disable" in v.split(b"\0")
                  for name, v in md.props.items())
        r.info("modem: %s%s" % (md.path, ", ref-args contains md_disable -> the modem is NOT loaded (untested)"
                                if dis else ""))

    # gpueb
    g = path_offset(root, "/soc/gpueb")
    r.ok("gpueb: /soc/gpueb %s (DT optional; the preloader must power on MFG0, otherwise LK assertion)"
         % ("found" if g else "absent"))

    # vcp
    n = by_compatible(root, "mediatek,vcp")
    if n is None:
        r.fail('vcp: no node with compatible "mediatek,vcp" -> LK assertion, boot stops')
    else:
        ok, why = status_okay(n)
        if not ok:
            r.fail("vcp: %s on %s (need \"okay\") -> LK assertion, boot stops" % (why, n.path))
        sup = n.props.get("vcp-support")
        if sup and u32(sup) != 0:
            ss = n.props.get("vcp-sram-size")
            ds = n.props.get("vcp-dram-size")
            ssv = struct.unpack(">i", ss[:4])[0] if ss else 0
            if ssv < 1 or not ds or u32(ds) == 0:
                r.fail("vcp: vcp-support != 0, but vcp-sram-size/vcp-dram-size are zero -> LK panics")
            elif ok:
                r.warn("vcp: vcp-support=%d -> LK loads the VCP firmware (as the factory system does)" % u32(sup))
        elif ok:
            r.ok("vcp: %s status okay, vcp-support=0 -> VCP is not loaded, 0" % n.path)

    # connsys
    c = path_offset(root, "/soc/consys") or path_offset(root, "/consys")
    gp = path_offset(root, "/soc/gps") or path_offset(root, "/gps")
    wf = path_offset(root, "/soc/wifi") or path_offset(root, "/wifi")
    if c and gp and wf:
        r.warn("connsys: consys/gps/wifi present -> LK's CONNSYS firmware loader runs, OVERWRITES compatible of %s and writes emi-*" % c.path)
    else:
        r.ok("connsys: no %s -> skipped, 0" % ", ".join(n for n, x in (("consys", c), ("gps", gp), ("wifi", wf))
                                                       if x is None))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dtb", nargs="?", help="DTB / dt_table (unless --vendor-boot is given)")
    ap.add_argument("--vendor-boot", help="vendor_boot.img: check the header and take the DTB from it")
    ap.add_argument("--boot", help="boot.img: check the kernel compression")
    ap.add_argument("--init-boot", help="init_boot.img: check the magic")
    a = ap.parse_args()
    if not a.dtb and not a.vendor_boot:
        ap.error("need a DTB or --vendor-boot")
    r = Report()
    blob = None
    if a.vendor_boot:
        blob = check_vendor_boot(a.vendor_boot, r)
    if a.dtb:
        blob = extract_dtb_from_blob(open(a.dtb, "rb").read(), r)
    if a.boot:
        check_boot(a.boot, r)
    if a.init_boot:
        check_init_boot(a.init_boot, r)
    if blob is not None:
        try:
            root = check_container(blob, r)
        except ValueError as e:
            r.fail("DTB cannot be parsed: %s" % e)
        else:
            check_lk_dt_writes(root, r)
            check_lk_dt_writes_full(root, r)
            check_fw_loaders(root, r)
    print("\nTotal: %d FAIL, %d WARN" % (r.fails, r.warns))
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
