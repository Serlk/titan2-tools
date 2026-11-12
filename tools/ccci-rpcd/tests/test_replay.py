#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Host test for t2-ccci-rpcd: builds synthetic modem image containers and
# request fragments, runs the offline replay (-R/-o/-O) and checks the
# replies byte by byte, the DRDI window and that the image is unchanged.
# Run with: python3 -I tests/test_replay.py path/to/t2-ccci-rpcd

# Only os, sys, struct, hashlib and shutil (see ccci-fsd/tests).
import hashlib
import os
import shutil
import struct
import sys

HDR = 20
FRAG_MAX = 0xD94
MORE = 0x80000000
WAKE = 0x40000000
CH_RX, CH_TX = 32, 33
OP_PROP, OP_SAR, OP_DRDI, OP_CAPID = 0x400F, 0x4010, 0x4014, 0x4015
WINDOW = 0x10000
MAGIC = 0x58881688

BIN = None
TMP = None
failures = 0
checks = 0
_tmp_seq = 0


class Rng:
    """xorshift64*, deterministic test data."""

    def __init__(self, seed):
        self.s = (seed * 0x9E3779B97F4A7C15 + 1) & 0xFFFFFFFFFFFFFFFF

    def next(self):
        x = self.s
        x ^= x >> 12
        x ^= (x << 25) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 27
        self.s = x
        return (x * 0x2545F4914F6CDD1D) & 0xFFFFFFFFFFFFFFFF

    def bytes(self, n):
        out = bytearray()
        while len(out) < n:
            out += struct.pack("<Q", self.next())
        return bytes(out[:n])


def mkdtemp(prefix):
    global _tmp_seq
    _tmp_seq += 1
    d = os.path.join(TMP, "%s%d-%d" % (prefix, os.getpid(), _tmp_seq))
    os.mkdir(d, 0o700)
    return d


def spawn(argv):
    """Run argv; returns (exit code, stderr)."""
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.dup2(w, 2)
            fd = os.open(os.devnull, os.O_WRONLY)
            os.dup2(fd, 1)
            os.execv(argv[0], argv)
        finally:
            os._exit(127)
    os.close(w)
    err = b""
    while True:
        b = os.read(r, 65536)
        if not b:
            break
        err += b
    os.close(r)
    _, st = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(st), err


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def stream(*args):
    out = u32(len(args))
    for a in args:
        if isinstance(a, int):
            a = u32(a)
        out += u32(len(a)) + a + b"\0" * (-len(a) % 4)
    return out


def frag(op, payload, slot=0, word0=0, ch=CH_RX, length=None):
    ln = HDR + len(payload) if length is None else length
    return struct.pack("<5I", word0, ln, ch, slot, op) + payload


def req(op, *args, slot=0, word0=0):
    return [frag(op, stream(*args), slot, word0)]


# ---- container -------------------------------------------------------------

def section(name, data, align=0x10, hsize_field=0x200, last=False):
    h = struct.pack("<II", MAGIC, len(data)) + name.encode().ljust(32, b"\0")
    h += struct.pack("<IIIIIIII", 0, 0, 0x58891689, hsize_field, 1, 0, 1 if last else 0, align)
    h = h.ljust(0x200, b"\xA5")
    pad = (-len(data) % align) if align else 0
    return h + data + b"\xEE" * pad


def container(drdi, pad_before_drdi=False, with_drdi=True):
    img = section("md1rom", b"R" * 0x1234)
    img += section("cert1md", b"C" * 0x30, align=0, hsize_field=0)
    if pad_before_drdi:
        end = len(img)
        img += b"\0" * (((end + 0xFFF) & ~0xFFF) - end + 0x100)
    if with_drdi:
        drdi_off = len(img) + 0x200
        img += section(DRDI_NAME, drdi)
    else:
        drdi_off = None
    img += section("cert1", b"c" * 0x40)
    img += section("md1_file_map", b"M" * 0x20, last=True)
    img += b"\xFF" * 0x400        # bytes after the list end
    return img, drdi_off


DRDI_NAME = "md1drdi"
DRDI = Rng(7).bytes(0x30000)


def write_image(td, data, name="modem.img"):
    p = os.path.join(td, name)
    with open(p, "wb") as f:
        f.write(data)
    os.chmod(p, 0o444)
    return p


# ---- 0x4014 requests -------------------------------------------------------

def drdi_init(seq, pairs, version=3, n=None):
    b = bytes([1, seq, 0xAA, 0xAA, version, len(pairs) if n is None else n, 0xBB, 0xBB])
    for s, ln in pairs:
        b += u32(s) + u32(ln)
    return b.ljust(0xBC, b"\0")


def drdi_copy(seq, sets, flag=0):
    b = bytes([2, seq, 0, 0, flag, 0x77, 0, 0])
    for s, d, ln in sets:
        b += u32(s) + u32(d) + u32(ln)
    return b.ljust(0xBC, b"\0")


def status(b0=0, seq=0, b4=0, b5=0, b6=0):
    return bytes([b0, seq, 0, 0, b4, b5, b6, 0])


# ---- run -------------------------------------------------------------------

class Reply:
    def __init__(self, raw):
        self.raw = raw
        self.word0, self.len, self.ch, self.slot, op = struct.unpack_from("<5I", raw)
        self.op = op & 0xFFFF
        self.opmark = op & 0xFFFF0000
        s = raw[HDR:]
        n = struct.unpack_from("<I", s)[0]
        off, self.args = 4, []
        for _ in range(n):
            ln = struct.unpack_from("<I", s, off)[0]
            off += 4
            self.args.append(s[off:off + ln])
            off += ln + (-ln % 4)
        self.stream_len = off

    @property
    def ret(self):
        return struct.unpack("<i", self.args[0])[0]


class Run:
    pass


def run(fragments, image=None, extra=(), td=None):
    own = td is None
    if own:
        td = mkdtemp("rpcd-replay-")
    rin, rout = os.path.join(td, "in.bin"), os.path.join(td, "out.bin")
    win = os.path.join(td, "window.bin")
    with open(rin, "wb") as f:
        for fr in fragments:
            f.write(u32(len(fr)) + fr)
    argv = [BIN, "-v", "-R", rin, "-o", rout, "-O", win]
    if image:
        argv += ["-m", image]
    rc, err = spawn(argv + list(extra))
    res = Run()
    res.rc, res.err = rc, err.decode(errors="replace")
    check(rc == 0, "daemon exit %d: %s" % (rc, res.err[-2000:]))
    data = open(rout, "rb").read() if os.path.exists(rout) else b""
    res.window = open(win, "rb").read() if os.path.exists(win) else b""
    if own:
        shutil.rmtree(td)
    res.replies, off = [], 0
    while off < len(data):
        ln = struct.unpack_from("<I", data, off)[0]
        fr = data[off + 4:off + 4 + ln]
        off += 4 + ln
        r = Reply(fr)
        check(r.len == ln and ln <= 0x800, "reply frame length %d" % ln)
        check(r.ch == CH_TX, "reply channel")
        check(r.opmark == 0xFFFF0000, "reply op marker")
        check(not r.word0 & MORE, "reply word0 bit 31 clear")
        check(HDR + r.stream_len == ln, "reply stream fills the frame")
        res.replies.append(r)
    return res


def expect_drdi(r, ret, st, what):
    check(r.op == OP_DRDI, "%s: op" % what)
    check(len(r.args) == 2, "%s: two args" % what)
    check(r.ret == ret, "%s: ret %d, want %d" % (what, r.ret, ret))
    check(r.args[1] == st, "%s: status %s, want %s" % (what, r.args[1].hex(), st.hex()))


# ---- tests -----------------------------------------------------------------

def test_container_walk(td):
    img, doff = container(DRDI)
    p = write_image(td, img)
    res = run(req(OP_DRDI, drdi_init(1, [(0, len(DRDI))])), p)
    check("data at 0x%x, 0x%x bytes" % (doff, len(DRDI)) in res.err, "md1drdi located")
    expect_drdi(res.replies[0], 0, status(0, 1, 3), "INIT after walk")

    # padding retry before the md1drdi header
    img2, doff2 = container(DRDI, pad_before_drdi=True)
    p2 = write_image(td, img2, "padded.img")
    res = run(req(OP_DRDI, drdi_init(2, [])), p2)
    check("data at 0x%x" % doff2 in res.err, "md1drdi found after the padding retry")
    expect_drdi(res.replies[0], 0, status(0, 2, 3), "INIT padded")

    # no md1drdi: not prepared, failure A without seq
    img3, _ = container(DRDI, with_drdi=False)
    p3 = write_image(td, img3, "nodrdi.img")
    res = run(req(OP_DRDI, drdi_init(9, [])), p3)
    expect_drdi(res.replies[0], -1, status(0xFF, 0, 3, 0, 0xFF), "INIT not prepared")

    # bad magic at 0
    p4 = write_image(td, b"\0" * 0x1000, "zero.img")
    res = run(req(OP_DRDI, drdi_init(9, [])), p4)
    expect_drdi(res.replies[0], -1, status(0xFF, 0, 3, 0, 0xFF), "INIT bad image")

    # md1drdi running past the end of the image
    p5 = write_image(td, img[:doff + 0x100], "trunc.img")
    res = run(req(OP_DRDI, drdi_init(9, [])), p5)
    expect_drdi(res.replies[0], -1, status(0xFF, 0, 3, 0, 0xFF), "INIT truncated image")

    # no image at all
    res = run(req(OP_DRDI, drdi_init(9, [])))
    expect_drdi(res.replies[0], -1, status(0xFF, 0, 3, 0, 0xFF), "INIT without -m")

    # bare md1drdi blob (-b)
    p6 = write_image(td, DRDI, "md1drdi.bin")
    res = run(req(OP_DRDI, drdi_init(3, [(0, len(DRDI))])) +
              req(OP_DRDI, drdi_copy(4, [(0x100, 0x10, 0x20)], flag=1)), p6, ["-b"])
    expect_drdi(res.replies[0], 0, status(0, 3, 3), "raw INIT")
    expect_drdi(res.replies[1], 0, status(0, 4, 0), "raw COPY")
    check(res.window[0x10:0x30] == DRDI[0x100:0x120], "raw COPY data")


def test_init_exact(td):
    p = write_image(td, container(DRDI)[0])
    res = run(req(OP_DRDI, drdi_init(5, [(0, 0x100), (0x200, 0x300)]), slot=3, word0=0x1234), p)
    r = res.replies[0]
    want = struct.pack("<5I", 0x1234, 0x2C, CH_TX, 3, 0xFFFF4014)
    want += u32(2) + u32(4) + u32(0) + u32(8) + bytes([0, 5, 0, 0, 3, 0, 0, 0])
    check(r.raw == want, "INIT reply bytes %s" % r.raw.hex())

    frs = [req(OP_DRDI, drdi_init(6, [], version=2))[0],
           req(OP_DRDI, drdi_init(7, [], n=16))[0],
           req(OP_DRDI, drdi_init(8, [(0, len(DRDI)), (1, len(DRDI))]))[0],
           req(OP_DRDI, drdi_init(9, [(len(DRDI), 0)]))[0],
           req(OP_DRDI, drdi_init(10, [(0xFFFFFFFF, 2)]))[0],    # wraps to 1: accepted
           req(OP_DRDI, drdi_init(11, [], n=15))[0],
           req(OP_DRDI, b"\x01\x0c" + b"\0" * 0xB9)[0],             # 0xBB bytes
           req(OP_DRDI, drdi_init(12, []) + b"\0")[0],              # 0xBD bytes
           req(OP_DRDI)[0],                                         # no argument
           req(OP_DRDI, b"\x03\x0d" + b"\0" * 0xBA)[0]]             # cmd 3
    res = run(frs, p)
    rs = res.replies
    check(len(rs) == len(frs), "one reply per request")
    expect_drdi(rs[0], -1, status(0xFF, 6, 3, 0, 0xFF), "INIT bad version")
    expect_drdi(rs[1], -1, status(0xFF, 7, 3, 0, 0xFF), "INIT 16 sets")
    expect_drdi(rs[2], -1, status(0xFE, 8, 3, 0, 0xFF), "INIT range error")
    expect_drdi(rs[3], 0, status(0, 9, 3), "INIT range at the end")
    expect_drdi(rs[4], 0, status(0, 10, 3), "INIT wrapping range")
    expect_drdi(rs[5], 0, status(0, 11, 3), "INIT 15 sets")
    expect_drdi(rs[6], -1, status(), "0xBB-byte request")
    expect_drdi(rs[7], -1, status(), "0xBD-byte request")
    expect_drdi(rs[8], -1, status(), "request without argument")
    expect_drdi(rs[9], -1, status(0xFF, 13), "unknown cmd")
    check(res.window == b"\0" * WINDOW, "INIT writes nothing to the window")


def test_copy(td):
    p = write_image(td, container(DRDI)[0])
    h0 = hashlib.sha256(open(p, "rb").read()).hexdigest()
    sets = [(0x0, 0x0, 0x100), (0x2FF00, 0x8000, 0x100), (0x1000, 0xFF00, 0x100)]
    frs = [req(OP_DRDI, drdi_init(1, [(0, 0x100)] * 3))[0],
           req(OP_DRDI, drdi_copy(2, sets, flag=0), slot=1)[0],
           req(OP_DRDI, drdi_copy(3, [(0x5000, 0x1000, 0x10)], flag=0))[0]]
    res = run(frs, p)
    rs = res.replies
    expect_drdi(rs[1], 0, status(0, 2, 0xFF), "COPY non-final")
    check(rs[1].slot == 1, "reply in the request slot")
    expect_drdi(rs[2], 0, status(0, 3, 0xFF), "second non-final COPY")
    w = res.window
    check(w[0:0x100] == DRDI[0:0x100], "set 0 copied")
    check(w[0x8000:0x8100] == DRDI[0x2FF00:0x30000], "set 1 copied (end of md1drdi)")
    check(w[0xFF00:0x10000] == DRDI[0x1000:0x1100], "set 2 copied (end of window)")
    check(w[0x1000:0x1010] == DRDI[0x5000:0x5010], "re-copy without INIT allowed")
    check(w[0x100:0x1000] == b"\0" * 0xF00, "nothing else written")

    # final copy, then a skipped copy, then INIT re-arms
    frs = [req(OP_DRDI, drdi_init(1, [(0, 0x10)]))[0],
           req(OP_DRDI, drdi_copy(2, [(0x10, 0x0, 0x10)], flag=1))[0],
           req(OP_DRDI, drdi_copy(3, [(0x20, 0x100, 0x10)], flag=1))[0],
           req(OP_DRDI, drdi_copy(4, [(0x20, 0x200, 0x10)], flag=0))[0],
           req(OP_DRDI, drdi_init(5, [(0, 0x10)]))[0],
           req(OP_DRDI, drdi_copy(6, [(0x30, 0x300, 0x10)], flag=0))[0]]
    res = run(frs, p)
    rs, w = res.replies, res.window
    expect_drdi(rs[1], 0, status(0, 2, 0), "final COPY")
    expect_drdi(rs[2], 0, status(0, 3, 0), "COPY after final: skipped")
    expect_drdi(rs[3], 0, status(0, 4, 0), "non-final COPY after final: skipped")
    expect_drdi(rs[4], 0, status(0, 5, 3), "INIT re-arms")
    expect_drdi(rs[5], 0, status(0, 6, 0xFF), "COPY after re-arm")
    check(w[0:0x10] == DRDI[0x10:0x20], "final set copied")
    check(w[0x100:0x110] == b"\0" * 16 and w[0x200:0x210] == b"\0" * 16,
          "skipped copies wrote nothing")
    check(w[0x300:0x310] == DRDI[0x30:0x40], "copy after re-arm")

    # sets with len 0 only: nothing copied, status 0xFF but ret 0
    frs = [req(OP_DRDI, drdi_init(1, [(0, 0)] * 2))[0],
           req(OP_DRDI, drdi_copy(2, [(0, 0, 0), (5, 5, 0)], flag=1))[0],
           req(OP_DRDI, drdi_copy(3, [(0, 0, 0)], flag=7))[0]]
    res = run(frs, p)
    expect_drdi(res.replies[1], 0, status(0xFF, 2, 0), "COPY all empty, final flag")
    expect_drdi(res.replies[2], 0, status(0xFF, 3, 0xFF), "COPY all empty, flag 7")

    # only the first N sets count; the last INIT's N is used
    frs = [req(OP_DRDI, drdi_init(1, [(0, 0)]))[0],
           req(OP_DRDI, drdi_copy(2, [(0x40, 0x40, 0x10), (0xFFFFFF, 0xFFFFFF, 0xFFFF)]))[0]]
    res = run(frs, p)
    expect_drdi(res.replies[1], 0, status(0, 2, 0xFF), "set beyond N ignored")
    check(res.window[0x40:0x50] == DRDI[0x40:0x50], "set below N copied")

    # N is kept from an INIT that failed its range check
    frs = [req(OP_DRDI, drdi_init(1, [(0, 0)]))[0],
           req(OP_DRDI, drdi_init(2, [(0, 0), (0, len(DRDI) + 1)]))[0],
           req(OP_DRDI, drdi_copy(3, [(0, 0, 0), (0x80, 0x80, 0x8)]))[0]]
    res = run(frs, p)
    expect_drdi(res.replies[1], -1, status(0xFE, 2, 3, 0, 0xFF), "INIT range error")
    expect_drdi(res.replies[2], 0, status(0, 3, 0xFF), "COPY uses N=2 of the failed INIT")
    check(res.window[0x80:0x88] == DRDI[0x80:0x88], "set 1 copied with N=2")

    # COPY before any INIT: N = 0, nothing to do
    res = run(req(OP_DRDI, drdi_copy(4, [(0, 0, 0x10)], flag=1)), p)
    expect_drdi(res.replies[0], 0, status(0xFF, 4, 0), "COPY before INIT")
    check(res.window == b"\0" * WINDOW, "COPY before INIT wrote nothing")

    check(hashlib.sha256(open(p, "rb").read()).hexdigest() == h0, "image unchanged")


def test_copy_bounds(td):
    p = write_image(td, container(DRDI)[0])
    size = len(DRDI)
    bad = [("len over 64 KiB", (0, 0, 0x10001)),
           ("src+len past md1drdi", (size - 0xF, 0, 0x10)),
           ("src+len wraps", (0xFFFFFFF0, 0, 0x20)),
           ("dst+len past window", (0, 0xFFF1, 0x10)),
           ("dst huge", (0, 0xFFFFFFF0, 0x20))]
    for what, s in bad:
        frs = [req(OP_DRDI, drdi_init(1, [(0, 0), (0, 0)]))[0],
               req(OP_DRDI, drdi_copy(2, [(0x10, 0, 0x10), s], flag=1))[0],
               # copy state is -1 again: a later final copy is not skipped
               req(OP_DRDI, drdi_copy(3, [(0x20, 0x20, 0x10)], flag=1))[0]]
        res = run(frs, p)
        expect_drdi(res.replies[1], -1, status(0xFF, 2, 0), what)
        check(res.window[0:0x10] == b"\0" * 16, "%s: validated before any copy" % what)
        expect_drdi(res.replies[2], 0, status(0, 3, 0), "%s: next COPY runs" % what)
        check(res.window[0x20:0x30] == DRDI[0x20:0x30], "%s: next COPY copied" % what)

    # exact limits are accepted
    frs = [req(OP_DRDI, drdi_init(1, [(0, 0)]))[0],
           req(OP_DRDI, drdi_copy(2, [(size - 0x10000, 0, 0x10000)]))[0]]
    res = run(frs, p)
    expect_drdi(res.replies[1], 0, status(0, 2, 0xFF), "64 KiB copy to the end of md1drdi")
    check(res.window == DRDI[size - 0x10000:], "whole window copied")

    # -w makes the window smaller
    frs = [req(OP_DRDI, drdi_init(1, [(0, 0)]))[0],
           req(OP_DRDI, drdi_copy(2, [(0, 0x7FF0, 0x20)]))[0],
           req(OP_DRDI, drdi_copy(3, [(0, 0x7FE0, 0x20)]))[0]]
    res = run(frs, p, ["-w", "0x8000"])
    check(len(res.window) == 0x8000, "window size from -w")
    expect_drdi(res.replies[1], -1, status(0xFF, 2, 0), "dst+len past a 32 KiB window")
    expect_drdi(res.replies[2], 0, status(0, 3, 0xFF), "copy to the end of a 32 KiB window")


def run_fifo(td, image, fragments, between):
    """Replay through a FIFO: between() runs after md1drdi was located."""
    rin, rout = os.path.join(td, "fifo"), os.path.join(td, "out.bin")
    win = os.path.join(td, "window.bin")
    os.mkfifo(rin, 0o600)
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.dup2(w, 2)
            os.execv(BIN, [BIN, "-v", "-m", image, "-R", rin, "-o", rout, "-O", win])
        finally:
            os._exit(127)
    os.close(w)
    fd = os.open(rin, os.O_WRONLY)      # returns once the daemon opened it
    between()
    for fr in fragments:
        os.write(fd, u32(len(fr)) + fr)
    os.close(fd)
    err = b""
    while True:
        b = os.read(r, 65536)
        if not b:
            break
        err += b
    os.close(r)
    _, st = os.waitpid(pid, 0)
    check(os.waitstatus_to_exitcode(st) == 0, "fifo replay exit")
    data = open(rout, "rb").read()
    out, off = [], 0
    while off < len(data):
        ln = struct.unpack_from("<I", data, off)[0]
        out.append(Reply(data[off + 4:off + 4 + ln]))
        off += 4 + ln
    window = open(win, "rb").read()
    for n in ("fifo", "out.bin", "window.bin"):
        os.unlink(os.path.join(td, n))
    return out, window, err.decode(errors="replace")


def test_copy_io_error(td):
    img = container(DRDI)[0]
    p = write_image(td, img)
    frs = req(OP_DRDI, drdi_init(1, [(0, 0)])) + \
        req(OP_DRDI, drdi_copy(2, [(0, 0, 0x10)], flag=1)) + \
        req(OP_DRDI, drdi_copy(3, [(0, 0, 0x10)], flag=1))

    # baseline: the image stays in place
    rs, w, _ = run_fifo(td, p, frs, lambda: None)
    expect_drdi(rs[1], 0, status(0, 2, 0), "COPY baseline")
    check(w[:0x10] == DRDI[:0x10], "baseline data")

    # the image is replaced by another file after start-up: refused
    def swap():
        q = write_image(td, img, "other.img")
        os.rename(q, p)
    rs, w, err = run_fifo(td, p, frs, swap)
    expect_drdi(rs[1], -1, status(0xFF, 2, 0), "COPY from a replaced image")
    expect_drdi(rs[2], -1, status(0xFF, 3, 0), "second COPY from a replaced image")
    check(w == b"\0" * WINDOW, "nothing copied from a replaced image")
    check("not the image seen at start-up" in err, "replacement logged")

    # the image disappears after start-up: COPY fails, INIT still answers
    rs, w, _ = run_fifo(td, p, frs + req(OP_DRDI, drdi_init(4, [])), lambda: os.unlink(p))
    expect_drdi(rs[1], -1, status(0xFF, 2, 0), "COPY without image")
    expect_drdi(rs[3], 0, status(0, 4, 3), "INIT without image")
    check(w == b"\0" * WINDOW, "nothing copied without image")


def test_property(td):
    conf = os.path.join(td, "props.conf")
    with open(conf, "w") as f:
        f.write("# modem properties\n"
                "ro.boot.slot_suffix = _a\n"
                "\n"
                "persist.vendor.radio.foo=1\n"
                "empty.prop=\n"
                "dup=first\n"
                "dup=second\n"
                "no equals sign\n"
                "long=" + "x" * 92 + "\n"
                "max=" + "y" * 91 + "\n"
                "  spaced.value  =  a b c  \n")
    frs = [req(OP_PROP, b"ro.boot.slot_suffix", slot=2, word0=0x55)[0],
           req(OP_PROP, b"persist.vendor.radio.foo\0")[0],
           req(OP_PROP, b"nothing.here")[0],
           req(OP_PROP, b"empty.prop")[0],
           req(OP_PROP, b"dup")[0],
           req(OP_PROP, b"long")[0],
           req(OP_PROP, b"max")[0],
           req(OP_PROP, b"spaced.value")[0],
           req(OP_PROP, b"dup\0garbage")[0],
           req(OP_PROP, b"")[0],
           req(OP_PROP)[0],
           req(OP_PROP, b"A" * 0x400)[0],
           req(OP_PROP, b"max", b"extra arg ignored")[0]]
    res = run(frs, None, ["-p", conf])
    rs = res.replies
    check(len(rs) == len(frs), "one reply per property query")

    def prop(r, val, what):
        check(r.op == OP_PROP and len(r.args) == 2, "%s: shape" % what)
        check(r.ret == len(val), "%s: ret %d" % (what, r.ret))
        check(r.args[1] == val + b"\0", "%s: value %r" % (what, r.args[1]))

    want = struct.pack("<5I", 0x55, 0x28, CH_TX, 2, 0xFFFF400F)
    want += u32(2) + u32(4) + u32(2) + u32(3) + b"_a\0\0"
    check(rs[0].raw == want, "property reply bytes %s" % rs[0].raw.hex())
    prop(rs[1], b"1", "trailing NUL in the name")
    prop(rs[2], b"", "unknown property")
    want = struct.pack("<5I", 0, 0x28, CH_TX, 0, 0xFFFF400F)
    want += u32(2) + u32(4) + u32(0) + u32(1) + b"\0\0\0\0"
    check(rs[2].raw == want, "unknown property reply bytes %s" % rs[2].raw.hex())
    prop(rs[3], b"", "empty value")
    prop(rs[4], b"second", "last definition wins")
    prop(rs[5], b"", "too long value ignored")
    prop(rs[6], b"y" * 91, "91-character value")
    prop(rs[7], b"a b c", "whitespace trimmed")
    prop(rs[8], b"second", "name ends at the first NUL")
    prop(rs[9], b"", "empty name")
    prop(rs[10], b"", "no argument")
    prop(rs[11], b"", "very long name")
    prop(rs[12], b"y" * 91, "extra arguments ignored")
    check("ignored" in res.err, "bad config lines logged")

    # without -p every property is unset
    res = run(req(OP_PROP, b"ro.boot.slot_suffix"))
    prop(res.replies[0], b"", "no table")

    # a missing table file is fatal
    rc, _ = spawn([BIN, "-p", os.path.join(td, "nope"), "-R", conf, "-o",
                   os.path.join(td, "o.bin")])
    check(rc == 1, "missing property file: exit 1")


def test_sar_capid(td):
    p = write_image(td, container(DRDI)[0])
    h0 = hashlib.sha256(open(p, "rb").read()).hexdigest()
    frs = [req(OP_SAR)[0],
           req(OP_SAR, 1, 2, 3)[0],
           req(OP_CAPID, b"\0\0")[0],                           # bad length first: -1, -1
           req(OP_CAPID, u32(0x1234) + u32(0xABCD), slot=4)[0],
           req(OP_CAPID, u32(1))[0],                            # bad length: -1, 0 (left over)
           req(OP_CAPID)[0]]
    res = run(frs, p)
    rs = res.replies
    want = struct.pack("<5I", 0, 0x28, CH_TX, 0, 0xFFFF4010)
    want += u32(2) + u32(4) + u32(0) + u32(4) + u32(0)
    check(rs[0].raw == want, "SAR reply bytes %s" % rs[0].raw.hex())
    check(rs[1].raw == want, "SAR ignores its arguments")
    check(rs[2].op == OP_CAPID and rs[2].ret == -1 and rs[2].args[1] == u32(-1),
          "CAPID bad length before any save: {-1, -1}")
    want = struct.pack("<5I", 0, 0x28, CH_TX, 4, 0xFFFF4015)
    want += u32(2) + u32(4) + u32(0) + u32(4) + u32(0)
    check(rs[3].raw == want, "CAPID reply bytes %s" % rs[3].raw.hex())
    check("md_capid 0x00001234 md_aac 0x0000abcd" in res.err, "CAPID values logged")
    check(rs[4].ret == -1 and rs[4].args[1] == u32(0), "CAPID bad length after a save: {-1, 0}")
    check(rs[5].ret == -1 and rs[5].args[1] == u32(0), "CAPID without argument")
    check(hashlib.sha256(open(p, "rb").read()).hexdigest() == h0, "nothing written")
    check(sorted(os.listdir(td)) == ["modem.img"], "no file created")


def test_reject(td):
    long_arg = b"z" * 0x7F8        # stream 4 + 4 + 0x7F8 = 0x800: accepted
    frs = [req(0x4011, 1)[0],
           req(0x400E)[0],
           frag(OP_SAR, u32(7) + (u32(0) * 7)),                  # 7 args
           frag(OP_SAR, u32(1) + u32(0x10) + b"abc"),           # arg past the end
           frag(OP_SAR, u32(2) + u32(4) + b"abcd"),             # second arg missing
           frag(OP_SAR, b"\x01\x00"),                           # stream shorter than 4
           req(OP_SAR, long_arg)[0],
           req(OP_SAR, long_arg + b"q")[0],                     # 0x804 bytes: rejected
           frag(OP_SAR, u32(0))]                                # no args: served
    res = run(frs)
    rs = res.replies
    check(len(rs) == len(frs), "one reply per request (%d)" % len(rs))
    want = struct.pack("<5I", 0, 0x20, CH_TX, 0, 0xFFFF4011) + u32(1) + u32(4) + u32(-2)
    check(rs[0].raw == want, "unknown op reply bytes %s" % rs[0].raw.hex())
    check(rs[1].op == 0x400E and rs[1].args == [u32(-2)], "kernel op rejected")
    for i, what in ((2, "7 args"), (3, "arg past end"), (4, "missing arg"),
                    (5, "short stream"), (7, "stream over 0x800")):
        check(rs[i].op == OP_SAR and rs[i].args == [u32(-2)], "%s: -2" % what)
    check(rs[6].args == [u32(0), u32(0)], "stream of exactly 0x800 served")
    check(rs[8].args == [u32(0), u32(0)], "zero args served")


def test_framing(td):
    p = write_image(td, container(DRDI)[0])
    body = stream(drdi_init(1, [(0, 0x10)]))
    frs = [frag(OP_SAR, stream(), slot=5),                       # bad slot: dropped
           frag(OP_SAR, stream(), slot=0xFFFFFFFF),
           frag(OP_SAR, stream(), ch=14),                       # wrong channel
           frag(OP_SAR, stream(), length=0x99),                 # length field wrong
           b"\0" * 8,                                           # runt
           # 0x4014 over two fragments in slot 2, with slot 1 in between
           frag(OP_DRDI, body[:0x40], slot=2, word0=MORE | WAKE | 0x10),
           frag(OP_PROP, stream(b"x"), slot=1, word0=WAKE | 0x20),
           frag(OP_DRDI, body[0x40:], slot=2, word0=WAKE | 0x30),
           # op changes in the middle: partial request dropped, new one served
           frag(OP_SAR, stream()[:2], slot=3, word0=MORE),
           frag(OP_PROP, stream(b"y"), slot=3),
           # too long over several fragments: -2 after the last one
           frag(OP_SAR, b"\0" * 0xD80, slot=4, word0=MORE),
           frag(OP_SAR, b"\0" * 0x20, slot=4, word0=MORE),
           frag(OP_SAR, b"\0" * 0x20, slot=4),
           frag(OP_SAR, stream(), slot=0x10000)]                # bad slot again
    res = run(frs, p)
    rs = res.replies
    check(len(rs) == 4, "four replies (%d)" % len(rs))
    if len(rs) == 4:
        check(rs[0].op == OP_PROP and rs[0].slot == 1, "interleaved slot 1 first")
        check(rs[0].word0 == WAKE | 0x20, "word0 bit 30 passed back")
        check(rs[1].op == OP_DRDI and rs[1].slot == 2, "reassembled 0x4014 in slot 2")
        check(rs[1].word0 == WAKE | 0x30, "word0 of the last fragment, bit 31 clear")
        expect_drdi(rs[1], 0, status(0, 1, 3), "reassembled INIT")
        check(rs[2].op == OP_PROP and rs[2].slot == 3, "new request after op change")
        check(rs[3].op == OP_SAR and rs[3].slot == 4 and rs[3].args == [u32(-2)],
              "overlong request: -2")
    for s in ("drop: slot 5", "drop: slot 4294967295", "drop: channel 14",
              "drop: length field", "drop: packet of 8 bytes", "partial request dropped"):
        check(s in res.err, "logged: %s" % s)


def test_options(td):
    for argv in ([BIN], [BIN, "-d", "/dev/null", "-R", "x", "-o", "y"],
                 [BIN, "-R", "x"], [BIN, "-b", "-R", "x", "-o", "y"],
                 [BIN, "-O", "w", "-d", "/dev/null"],
                 [BIN, "-w", "0", "-R", "x", "-o", "y"],
                 [BIN, "-w", "12k", "-R", "x", "-o", "y"], [BIN, "extra"]):
        rc, _ = spawn(argv)
        check(rc == 2, "usage error for %s" % argv[1:])
    # a regular file is not a port
    f = os.path.join(td, "notaport")
    open(f, "w").close()
    rc, err = spawn([BIN, "-L", "-d", f])
    check(rc == 1 and b"not a character device" in err, "regular file refused as port")
    # a character device is not an image
    res = run(req(OP_DRDI, drdi_init(1, [])), "/dev/null")
    expect_drdi(res.replies[0], -1, status(0xFF, 0, 3, 0, 0xFF), "char device refused as image")


def main():
    global BIN, TMP
    BIN = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("t2-ccci-rpcd")
    TMP = os.environ.get("TMPDIR") or "/tmp"
    tests = [test_container_walk, test_init_exact, test_copy, test_copy_bounds,
             test_copy_io_error, test_property, test_sar_capid, test_reject,
             test_framing, test_options]
    for t in tests:
        td = mkdtemp("rpcd-test-")
        before = failures
        try:
            t(td)
        except Exception as e:  # report and go on
            check(False, "%s raised %r" % (t.__name__, e))
        print("%-24s %s" % (t.__name__, "ok" if failures == before else "FAILED"))
        shutil.rmtree(td)
    print("%d checks, %d failures" % (checks, failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
