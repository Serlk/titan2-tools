#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Host test for t2-ccci-fsd: builds request fragments, runs the offline
# replay (-R/-o) and checks the replies, the sandbox and the journal.
# Run with: python3 -I tests/test_replay.py path/to/t2-ccci-fsd

# Only os, sys, struct, zlib, hashlib and shutil: the stdlib process,
# temp-file and PRNG modules pull in the math extension, which is not
# always loadable on a half-upgraded host.
import hashlib
import os
import shutil
import struct
import sys
import zlib

HDR = 20
PAYLOAD_MAX = 0xD80
FRAG_MAX = 0xD94
REQ_MAX = 0x40DC
MORE = 0x80000000
WAKE = 0x40000000

OPEN, SEEK, READ, WRITE, CLOSE, CLOSE_ALL, CREATE_DIR = range(0x1001, 0x1008)
GET_FILE_SIZE, RENAME, DELETE, GET_ATTR, OPEN_VAR = 0x1009, 0x100B, 0x100F, 0x1010, 0x1011

MOVE, SHUTDOWN = 0x100C, 0x1017
CREAD, CWRITE, CREAD_EX = 0x1022, 0x1024, 0x1026

O_RDONLY, O_CREATE, O_TRUNC, O_DIR, O_COMMIT = 0x100, 0x10000, 0x20000, 0x800, 0x01000000

BIN = None
TMP = None
failures = 0
checks = 0
LAST_ERR = b""
_tmp_seq = 0


class Rng:
    """xorshift64*, deterministic test data."""

    def __init__(self, seed):
        self.s = (seed * 0x9E3779B97F4A7C15 + 1) & 0xFFFFFFFFFFFFFFFF

    def randrange(self, a, b=None):
        lo, hi = (0, a) if b is None else (a, b)
        x = self.s
        x ^= x >> 12
        x ^= (x << 25) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 27
        self.s = x
        return lo + ((x * 0x2545F4914F6CDD1D) & 0xFFFFFFFFFFFFFFFF) % (hi - lo)

    def choice(self, seq):
        return seq[self.randrange(len(seq))]


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


def path(s):
    return s.encode("utf-16-le") + b"\0\0"


def stream(*args):
    out = u32(len(args))
    for a in args:
        if isinstance(a, int):
            a = u32(a)
        out += u32(len(a)) + a + b"\0" * (-len(a) % 4)
    return out


def frags(op, body, slot=0, word0=0, ch=14):
    """Split an argument stream into request fragments."""
    out = []
    chunks = [body[i:i + PAYLOAD_MAX] for i in range(0, len(body), PAYLOAD_MAX)] or [b""]
    for i, c in enumerate(chunks):
        w0 = word0 | (MORE if i < len(chunks) - 1 else 0)
        out.append(struct.pack("<5I", w0, HDR + len(c), ch, slot, op) + c)
    return out


def req(op, *args, slot=0, word0=0):
    return frags(op, stream(*args), slot, word0)


def parse_stream(s):
    n = struct.unpack_from("<I", s)[0]
    off, args = 4, []
    for _ in range(n):
        ln = struct.unpack_from("<I", s, off)[0]
        off += 4
        args.append(s[off:off + ln])
        off += ln + (-ln % 4)
    return args


class Reply:
    def __init__(self, slot, op, word0, args, nfrags, raw=b""):
        self.slot, self.op, self.word0, self.args, self.nfrags = slot, op, word0, args, nfrags
        self.raw = raw

    @property
    def ret(self):
        return struct.unpack("<i", self.args[0])[0]

    def u32(self, i):
        return struct.unpack("<I", self.args[i])[0]


def run(root, fragments, extra=()):
    global LAST_ERR
    td = mkdtemp("fsd-replay-")
    rin, rout = os.path.join(td, "in.bin"), os.path.join(td, "out.bin")
    with open(rin, "wb") as f:
        for fr in fragments:
            f.write(u32(len(fr)) + fr)
    rc, err = spawn([BIN, "-r", root, "-R", rin, "-o", rout, *extra])
    LAST_ERR = err
    check(rc == 0, "daemon exit %d: %s" % (rc, err.decode(errors="replace")[-2000:]))
    data = open(rout, "rb").read() if os.path.exists(rout) else b""
    shutil.rmtree(td)
    replies, pending, off = [], {}, 0
    while off < len(data):
        ln = struct.unpack_from("<I", data, off)[0]
        fr = data[off + 4:off + 4 + ln]
        off += 4 + ln
        w0, w1, ch, slot, op = struct.unpack_from("<5I", fr)
        check(w1 == ln and ln <= FRAG_MAX, "reply fragment length")
        check(ch == 15, "reply channel")
        check(op & 0xFFFF0000 == 0xFFFF0000, "reply op marker")
        check(not w0 & WAKE, "reply word0 bit 30 clear")
        buf, n = pending.get(slot, (b"", 0))
        buf, n = buf + fr[HDR:], n + 1
        if w0 & MORE:
            check(ln == FRAG_MAX, "non-final reply fragment is full")
            pending[slot] = (buf, n)
            continue
        pending.pop(slot, None)
        replies.append(Reply(slot, op & 0xFFFF, w0, parse_stream(buf), n, buf))
    check(not pending, "no unterminated reply")
    return replies


def new_root():
    root = mkdtemp("fsd-test-")
    ro = os.path.join(root, "ro")
    for d in ("nvdata/md/NVRAM/NVD_IMEI", "nvdata/md_cmn", "protect_f/md", "protect_s/md",
              "nvcfg", "mdlpm", "firmware", "etc_md"):
        os.makedirs(os.path.join(ro, d))
    with open(os.path.join(ro, "nvdata/md/NVRAM/NVD_IMEI/MP0B_001"), "wb") as f:
        f.write(b"hello")
    with open(os.path.join(ro, "nvdata/md/NVRAM/ORIG"), "wb") as f:
        f.write(b"original")
    with open(os.path.join(ro, "firmware/fw.img"), "wb") as f:
        f.write(b"firmware")
    with open(os.path.join(ro, "protect_f/md/CAL"), "wb") as f:
        f.write(b"cal")
    outside = os.path.join(root, "outside")
    with open(outside, "wb") as f:
        f.write(b"secret")
    os.symlink(outside, os.path.join(ro, "nvdata/md/link"))
    os.symlink(root, os.path.join(ro, "nvdata/md/ldir"))
    os.symlink("../../..", os.path.join(ro, "nvdata/md/NVRAM/up"))
    return root


def tree_hash(d):
    h = hashlib.sha256()
    for dp, dn, fn in sorted(os.walk(d)):
        dn.sort()
        for n in sorted(fn + dn):
            p = os.path.join(dp, n)
            h.update(p.encode())
            if os.path.islink(p):
                h.update(os.readlink(p).encode())
            elif os.path.isfile(p):
                h.update(open(p, "rb").read())
    return h.hexdigest()


def journal(root):
    with open(os.path.join(root, "journal.log")) as f:
        return f.read()


IMEI = path("Z:\\NVRAM\\NVD_IMEI\\MP0B_001")


def test_basic(root):
    r = run(root, sum([
        req(OPEN, IMEI, O_RDONLY),                   # 1
        req(READ, 1, 5),
        req(GET_FILE_SIZE, 1, b"\0" * 4),
        req(SEEK, 1, 1, 0),
        req(READ, 1, 100),
        req(SEEK, 1, (-2) & 0xFFFFFFFF, 2),
        req(SEEK, 1, 10, 0),
        req(SEEK, 1, 0, 7),
        req(CLOSE, 1),
        req(CLOSE, 1),
        req(OPEN, path("Z:\\NVRAM\\NOPE"), 0),
        req(OPEN, path("Z:\\NOPE\\X"), 0),
        req(OPEN, path("z:/NVRAM/NVD_IMEI/MP0B_001"), O_RDONLY),
        req(GET_ATTR, IMEI),
        req(GET_ATTR, path("Z:\\NVRAM")),
        req(GET_ATTR, path("W:\\fw.img")),
        req(OPEN_VAR, IMEI, O_RDONLY),
        req(OPEN, path("Z:\\NVRAM"), 0),
        req(OPEN, path("Z:\\NVRAM"), O_DIR),
        req(CLOSE_ALL),
        req(READ, 1, 4),
    ], []))
    check(len(r) == 21, "basic: 21 replies, got %d" % len(r))
    check(r[0].ret == 1, "open -> handle 1")
    check(r[1].raw == stream(0, 5, b"hello"), "read 5: ret, count, data")
    check(r[2].ret == 0 and r[2].u32(1) == 5, "file size")
    check(r[3].ret == 1, "seek set 1")
    check(r[4].raw == stream(0, 4, b"ello"), "read rest")
    check(r[5].ret == 3, "seek end -2")
    check(r[6].ret == 10, "seek past the end allowed")
    check(r[7].ret == -2, "bad whence -2")
    check(r[8].ret == 0 and r[9].ret == -10, "close, close again -10")
    check(r[10].ret == -9, "missing file -9")
    check(r[11].ret == -9, "missing dir -9")
    check(r[12].ret == 1, "lower-case drive and / separators")
    check(r[13].ret == 0 and r[14].ret == 0x10 and r[15].ret == 0x01, "attributes")
    check(len(r[16].args) == 2 and r[16].args[1] == b"\0" * 8 and r[16].ret == 2, "open variant")
    check(r[17].ret == -16, "open a directory as file -16")
    check(r[18].ret == 3, "open a directory with 0x800")
    check(r[19].ret == 0 and r[20].ret == -10, "close all")
    check([x.op for x in r[:3]] == [OPEN & 0xFFFF, READ & 0xFFFF, GET_FILE_SIZE], "reply ops")


def test_copy_up_whiteout(root):
    ro_before = tree_hash(os.path.join(root, "ro"))
    rw = os.path.join(root, "rw/nvdata/md")
    r = run(root, sum([
        req(OPEN, IMEI, 0),                           # 1, rw on an ro file
        req(READ, 1, 2),
        req(WRITE, 1, b"XY", 2, b""),
        req(SEEK, 1, 0, 0),
        req(READ, 1, 10),
        req(CLOSE, 1),
        req(DELETE, path("Z:\\NVRAM\\ORIG")),
        req(GET_ATTR, path("Z:\\NVRAM\\ORIG")),
        req(OPEN, path("Z:\\NVRAM\\ORIG"), O_RDONLY),
        req(DELETE, path("Z:\\NVRAM\\ORIG")),
        req(OPEN, path("Z:\\NVRAM\\ORIG"), O_CREATE),  # 1, recreated empty
        req(GET_FILE_SIZE, 1, b""),
        req(CLOSE, 1),
        req(RENAME, path("Z:\\NVRAM\\NVD_IMEI\\MP0B_001"), path("Z:\\NVRAM\\MOVED")),
        req(GET_ATTR, IMEI),
        req(OPEN, path("Z:\\NVRAM\\MOVED"), O_RDONLY),
        req(READ, 1, 10),
        req(RENAME, path("Z:\\NVRAM\\MOVED"), path("Z:\\NVRAM\\ORIG")),
        req(RENAME, path("Z:\\NVRAM\\MOVED"), path("V:\\X")),
        req(CREATE_DIR, path("Z:\\NEWDIR")),
        req(CREATE_DIR, path("Z:\\NEWDIR")),
        req(CREATE_DIR, path("Z:\\NVRAM")),
        req(OPEN, path("Z:\\NEWDIR\\F"), O_TRUNC),     # 2
        req(WRITE, 2, b"abcdef", 6, b""),
        req(DELETE, path("Z:\\NEWDIR")),
    ], []))
    check(len(r) == 25, "copy-up: 25 replies, got %d" % len(r))
    check(r[1].args[2] == b"he", "read before copy-up")
    check(r[2].ret == 0 and r[2].u32(1) == 2, "write 2 bytes")
    check(r[4].args[2] == b"heXYo", "write at the position after the read")
    check(open(os.path.join(rw, "NVRAM/MOVED"), "rb").read() == b"heXYo", "copied up to rw/ (then renamed)")
    check(tree_hash(os.path.join(root, "ro")) == ro_before, "ro/ unchanged")
    check(r[6].ret == 0, "delete an ro file")
    check(r[7].ret == -9 and r[8].ret == -9 and r[9].ret == -9, "deleted file is gone")
    check(r[10].ret == 1 and r[11].u32(1) == 0, "recreated empty")
    check(not os.path.exists(os.path.join(rw, "NVRAM/.t2fsd.wh.ORIG")), "whiteout removed on create")
    check(r[13].ret == 0 and r[14].ret == -9, "rename")
    check(r[16].args[2] == b"heXYo", "renamed content")
    check(r[17].ret == -36, "rename onto existing -36")
    check(r[18].ret == -2, "rename across drives -2")
    check(r[19].ret == 0 and r[20].ret == -36 and r[21].ret == -36, "create dir")
    check(r[22].ret == 2 and r[23].ret == 0, "create with truncate flag")
    check(r[24].ret == -16, "delete of a directory refused")
    j = journal(root)
    check("COPYUP Z:/NVRAM/NVD_IMEI/MP0B_001 size=5 crc32=%08x" % zlib.crc32(b"hello") in j, "journal copy-up")
    check("WRITE Z:/NVRAM/NVD_IMEI/MP0B_001 off=2 len=2 crc32=%08x" % zlib.crc32(b"XY") in j, "journal write crc")
    check("DELETE Z:/NVRAM/ORIG (whiteout)" in j and "RENAME Z:/NVRAM/NVD_IMEI/MP0B_001 -> Z:/NVRAM/MOVED" in j,
          "journal delete/rename")
    check("MKDIR Z:/NEWDIR" in j and "CREATE Z:/NEWDIR/F" in j, "journal mkdir/create")
    # a second run: the merged view persists
    r = run(root, req(GET_ATTR, path("Z:\\NVRAM\\ORIG")) + req(GET_ATTR, IMEI))
    check(r[0].ret == 0 and r[1].ret == -9, "persistent whiteout/rename")


def test_multi_fragment(root):
    data = bytes(Rng(1).randrange(256) for _ in range(12000))
    fr = req(OPEN, path("Z:\\BIG"), O_CREATE) + req(WRITE, 1, data, len(data), b"", slot=3)
    check(len(fr) == 1 + 4, "write request is 4 fragments")
    fr += req(WRITE, 1, data, len(data), b"", slot=4)
    fr += req(SEEK, 1, 0, 0) + req(READ, 1, 16000, slot=2)
    r = run(root, fr)
    check(len(r) == 5, "multi-fragment: 5 replies")
    check(r[1].ret == 0 and r[1].u32(1) == len(data) and r[1].slot == 3, "multi-fragment write")
    check(r[2].ret == 0 and r[2].slot == 4, "second write")
    check(r[4].ret == 0 and r[4].args[2] == (data + data)[:16000] and r[4].nfrags == 5 and r[4].slot == 2,
          "multi-fragment read reply (%d fragments)" % r[4].nfrags)
    check(os.path.getsize(os.path.join(root, "rw/nvdata/md/BIG")) == 2 * len(data), "file size on disk")
    # a read larger than one reply: clamped, not over 0x40DC
    r = run(root, req(OPEN, path("Z:\\BIG"), O_RDONLY) + req(READ, 1, 0x10000))
    check(r[1].ret == 0 and len(r[1].args[2]) == 0x40AC and r[1].u32(1) == 0x40AC, "read clamped at 0x40AC")


def test_slots(root):
    per_slot = []
    for s in range(5):
            per_slot.append(frags(GET_ATTR, stream(path("Z:\\NVRAM\\NVD_IMEI\\MP0B_001" if s % 2 else "Z:\\NVRAM")),
                              slot=s, word0=0x1234 + s))
    # interleave: all first fragments, then the rest
    long = [frags(OPEN, stream(path("Z:\\NVRAM\\NVD_IMEI\\MP0B_001"), O_RDONLY, b"x" * 8000), slot=s)
            for s in range(5)]
    seq = [f[0] for f in long] + [f[1] for f in long] + [f[2] for f in long] + [f[0] for f in per_slot]
    r = run(root, seq)
    check(len(r) == 10, "slots: 10 replies, got %d" % len(r))
    check(sorted(x.slot for x in r[:5]) == list(range(5)), "every slot answered")
    check(sorted(x.ret for x in r[:5]) == [1, 2, 3, 4, 5], "Open: an extra argument is ignored")
    check([x.slot for x in r[5:]] == list(range(5)), "single-fragment slots")
    check([x.word0 for x in r[5:]] == [0x1234 + s for s in range(5)], "word0 echoed")
    check(r[5].ret == 0x10 and r[6].ret == 0, "slot results")
    r = run(root, req(GET_ATTR, IMEI, word0=WAKE | 7))
    check(r[0].word0 == 7, "bit 30 cleared in reply")


def test_malformed(root):
    good = req(GET_ATTR, path("Z:\\NVRAM"))
    bad = []
    bad += req(GET_ATTR, path("Z:\\NVRAM"), slot=5)
    bad += frags(GET_ATTR, stream(path("Z:\\NVRAM")), ch=13)
    f = bytearray(good[0])
    struct.pack_into("<I", f, 4, len(f) + 4)
    bad.append(bytes(f))                                  # length field disagrees
    bad.append(b"\x00" * 16)                              # header only
    bad.append(b"")                                       # empty
    big = struct.pack("<5I", 0, FRAG_MAX + 4, 14, 0, GET_ATTR) + b"\0" * (FRAG_MAX - HDR + 4)
    bad.append(big)                                       # longer than a fragment
    # overflow: continuation fragments beyond 0x40DC, then the end of it
    for i in range(6):
        bad.append(struct.pack("<5I", MORE, FRAG_MAX, 14, 1, OPEN) + b"\xAA" * PAYLOAD_MAX)
    bad.append(struct.pack("<5I", 0, HDR + 4, 14, 1, OPEN) + b"\xAA" * 4)
    bad += frags(GET_ATTR, u32(7) + b"\0" * 64, slot=2)    # 7 arguments
    bad += frags(GET_ATTR, u32(1) + u32(1000) + b"ab", slot=2)  # arg beyond the buffer
    bad += frags(GET_ATTR, u32(2) + u32(0xFFFFFFFF), slot=2)    # huge length
    bad += frags(GET_ATTR, b"", slot=2)                   # no num_args
    bad += frags(GET_ATTR, b"\1\0", slot=2)               # short num_args
    bad += req(READ, 99, 4)                     # bad handle
    bad += req(READ, b"\1\0", 4)                # handle of 2 bytes
    bad += req(WRITE, 1, b"ab", 100, b"")                 # len beyond data
    bad += req(OPEN, path("Z:\\X"))                       # missing flags
    bad += req(SEEK, 0, 0, 0)                             # handle 0
    bad += req(SEEK, 0xFFFFFFFF, 0, 0)
    r = run(root, bad + good)
    check(len(r) == 12, "malformed: 12 replies, got %d" % len(r))
    check(all(x.ret == -2 for x in r[:5]), "bad streams -> -2")
    check([x.ret for x in r[5:11]] == [-10, -2, -2, -2, -10, -10], "bad handles/args")
    check(r[-1].ret == 0x10, "still serving after malformed input")
    j = journal(root)
    check("BADPARAM op=0x1001 (Open) slot=0 nargs=1" in j, "bad params journaled")

    # random garbage, half of it with valid headers
    rnd = Rng(42)
    junk = []
    for _ in range(3000):
        kind = rnd.randrange(4)
        if kind == 0:
            junk.append(bytes(rnd.randrange(256) for _ in range(rnd.randrange(64))))
        else:
            body = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 200)))
            if kind == 2:
                body = u32(rnd.randrange(8)) + body
            op = rnd.choice([rnd.randrange(0x1000, 0x1030), rnd.randrange(1 << 32)])
            w0 = rnd.choice([0, MORE, rnd.randrange(1 << 32)])
            junk.append(struct.pack("<5I", w0, HDR + len(body), rnd.choice([14, 14, 15]),
                                    rnd.randrange(7), op) + body)
    # finish every slot that may be in the middle of a request
    ends = [struct.pack("<5I", 0, HDR + 4, 14, s, 0x1015) + u32(0) for s in range(5)]
    r = run(root, junk + ends + good)
    check(r and r[-1].ret == 0x10, "still serving after fuzz")


def test_escapes(root):
    ro_before = tree_hash(os.path.join(root, "ro"))
    names = [
        ("Z:\\..\\..\\etc\\passwd", -16),
        ("Z:\\NVRAM\\..\\..\\x", -16),
        ("Z:\\NVRAM\\.\\x", -3),
        ("Z:/../outside", -16),
        ("\\etc\\passwd", -3),
        ("/etc/passwd", -3),
        ("Zpasswd", -3),
        ("Z:passwd", -3),
        ("Z:\\\\NVRAM", -3),
        ("Z:\\NVRAM\\.t2fsd.wh.ORIG", -3),
        ("Z:\\a*b", -3),
        ("Z:\\a:b", -3),
        ("Z:\\caf\u00e9", -3),
        ("C:\\x", -19),
        ("T:\\x", -30),                # absent read-only drive: create refused
        ("Q:\\x", -30),
        ("K:\\x", -19),
        ("Z:\\" + "a" * 300, -49),
        ("Z:\\" + "\\".join(["abcdefgh"] * 40), -49),
        ("Z:\\link", -16),
        ("Z:\\ldir\\outside", -16),
        ("Z:\\NVRAM\\up\\outside", -16),
    ]
    fr = []
    for n, _ in names:
        fr += req(OPEN, path(n), O_CREATE)
    fr += req(OPEN, b"Z\0:\0\\\0x", 0)          # odd length
    fr += req(CREATE_DIR, path("Z:\\ldir\\new"))
    fr += req(DELETE, path("Z:\\link"))
    fr += req(RENAME, path("Z:\\link"), path("Z:\\stolen"))
    fr += req(GET_ATTR, path("Z:\\ldir"))
    r = run(root, fr)
    check(len(r) == len(names) + 5, "escapes: replies")
    for (n, want), x in zip(names, r):
        check(x.ret == want, "%r -> %d (got %d)" % (n[:40], want, x.ret))
    check(all(x.ret == -16 for x in r[-4:]) and r[-5].ret == -3, "ops on symlinks refused")
    check(tree_hash(os.path.join(root, "ro")) == ro_before, "ro/ unchanged by escapes")
    check(open(os.path.join(root, "outside"), "rb").read() == b"secret", "outside untouched")
    check(not os.path.exists(os.path.join(root, "rw/nvdata/md/stolen")), "nothing created")
    check("REFUSED" in journal(root), "escapes journaled")
    # a symlink planted in rw/ is refused as well
    os.symlink(os.path.join(root, "outside"), os.path.join(root, "rw/nvdata/md/evil"))
    r = run(root, req(OPEN, path("Z:\\evil"), 0) + req(OPEN, path("Z:\\evil"), O_TRUNC))
    check(r[0].ret == -16 and r[1].ret == -16, "rw/ symlink refused")
    check(open(os.path.join(root, "outside"), "rb").read() == b"secret", "outside untouched (rw)")


def test_drives(root):
    fr = []
    fr += req(OPEN, path("W:\\fw.img"), 0)                # 1
    fr += req(WRITE, 1, b"zz", 2, b"")
    fr += req(READ, 1, 8)
    fr += req(OPEN, path("W:\\new"), O_CREATE)
    fr += req(CREATE_DIR, path("R:\\d"))
    fr += req(DELETE, path("W:\\fw.img"))
    fr += req(OPEN, path("X:\\CAL"), 0)                   # 2
    fr += req(WRITE, 2, b"CAL!", 4, b"")
    fr += req(OPEN, path("Y:\\NEW"), O_CREATE)            # 3
    fr += req(OPEN, IMEI, O_RDONLY)                        # 4
    fr += req(WRITE, 4, b"q", 1, b"")
    r = run(root, fr)
    check(r[0].ret == 1 and r[1].ret == -30 and r[2].args[2] == b"firmware", "W: read-only")
    check(r[3].ret == -30 and r[4].ret == -30 and r[5].ret == -30, "W:/R: no create/mkdir/delete")
    check(r[7].ret == 0 and r[8].ret == 3, "X:/Y: writable by default")
    check(r[10].ret == -16, "write to a read-only handle -16")
    j = journal(root)
    check("PROTECT WRITE X:/CAL off=0 len=4" in j and "PROTECT CREATE Y:/NEW" in j, "PROTECT marked")
    check(not os.path.exists(os.path.join(root, "rw/firmware")), "no rw/ for W:")
    r = run(root, req(OPEN, path("X:\\CAL"), 0) + req(WRITE, 1, b"no", 2, b"") +
            req(OPEN, path("Y:\\NEW2"), O_CREATE) + req(GET_ATTR, path("X:\\CAL")) +
            req(DELETE, path("Y:\\NEW")), ["-p"])
    check(r[0].ret == 1 and r[1].ret == -30 and r[2].ret == -30 and r[3].ret == 0x01 and r[4].ret == -30,
          "-p refuses X:/Y: writes")
    check("REFUSED PROTECT write X:/CAL" in journal(root), "PROTECT refusal journaled")
    check(open(os.path.join(root, "rw/protect_f/md/CAL"), "rb").read() == b"CAL!", "X: content")


def test_fixed_ops(root):
    fr = []
    for op in (0x101D, 0x101E, 0x101F, 0x1020):
        fr += req(op, b"\0" * 8)
    fr += req(0x1021, path("Z:\\NVRAM"))
    fr += req(0x1023, 1, 2)
    fr += req(0x1008, path("Z:\\NVRAM"), b"abc")
    fr += req(0x100E)
    fr += req(0x2000, 1)
    fr += req(0x1015) + req(0x1016) + req(0x1019) + req(0x101C)
    for op in (0x100A, 0x100D, 0x1018, 0x101B):
        fr += req(op)
    fr += req(OPEN, IMEI, O_RDONLY) + req(0x1017) + req(CLOSE, 1)
    r = run(root, fr)
    rets = [x.ret for x in r]
    check(rets[:4] == [-16] * 4, "OTP -16")
    check(rets[4] == -16, "Restore -16")
    check(rets[5] == -2, "binary region -2")
    check(rets[6:9] == [-18] * 3, "unimplemented -18")
    check(rets[9:13] == [0, 1, 0, 0], "no-ops")
    check(rets[13:17] == [-18] * 4, "other unimplemented ops -18")
    check(rets[17:] == [1, 0, -10], "Shutdown closes handles")
    j = journal(root)
    check("UNIMPL op=0x1008 (RemoveDir) slot=0 nargs=2 lens=18,3" in j, "unimplemented logged with lengths")
    check("UNIMPL op=0x2000 (unknown) slot=0 nargs=1 lens=4" in j, "unknown op logged")
    check("REFUSED OTP op=0x101d" in j and 'REFUSED Restore slot=0 path="Z:\\NVRAM"' in j, "OTP/Restore logged")


def test_handles(root):
    fr = []
    for _ in range(129):
        fr += req(OPEN, IMEI, O_RDONLY)
    r = run(root, fr)
    check([x.ret for x in r[:128]] == list(range(1, 129)) and r[128].ret == -5, "handle limit 128, then -5")


def test_replay_file(root):
    # an oversized record and a truncated last record do not stop the replay
    td = mkdtemp("fsd-replay-")
    rin, rout = os.path.join(td, "in"), os.path.join(td, "out")
    good = req(GET_ATTR, path("Z:\\NVRAM"))[0]
    with open(rin, "wb") as f:
        f.write(u32(0x100000) + b"\0" * 0x100000 + u32(len(good)) + good + u32(100) + b"abc")
    rc, _ = spawn([BIN, "-r", root, "-R", rin, "-o", rout])
    out = open(rout, "rb").read()
    check(rc == 0 and len(out) == 4 + HDR + 4 + 8, "replay file robustness")
    shutil.rmtree(td)
    rc, _ = spawn([BIN, "-r", root, "-d", "/dev/null/x"])
    check(rc != 0, "missing port fails")
    rc, err = spawn([BIN, "-r", root, "-d", os.path.join(root, "outside")])
    check(rc != 0 and b"not a character device" in err, "regular file refused as port")
    if os.path.exists("/dev/loop0"):
        rc, err = spawn([BIN, "-r", root, "-d", "/dev/loop0"])
        check(rc != 0 and b"not a character device" in err, "block device refused as port")


# --- compound operations, Move, sync on close ---

J = 0xA5A5A5A5      # words the server must ignore


def status(done, err):
    return struct.pack("<Ii", done, err)


def cb_read(mask, flags=0, size=0, off=0, whence=0, length=0, handle=None, nbytes=None):
    """0x1022 (40 bytes) or, with handle, 0x1026 (48 bytes) control block."""
    w = [mask, J, J, flags, size, off, whence, J, length, J]
    if handle is not None:
        w += [handle, J]
    b = b"".join(u32(x) for x in w)
    return b if nbytes is None else (b + b"\0" * 64)[:nbytes]


def cb_write(mask, flags=0, off=0, whence=0, length=0, handle=-1, nbytes=44):
    w = [mask, J, J, flags, off, whence, J, length, handle, J, J]
    return (b"".join(u32(x) for x in w) + b"\0" * 64)[:nbytes]


def cread(p, **kw):
    return req(CREAD, path(p), cb_read(**kw))


def cread_ex(p, handle=-1, **kw):
    return req(CREAD_EX, path(p), cb_read(handle=handle, **kw))


def cwrite(p, data, **kw):
    return req(CWRITE, path(p), cb_write(**kw), data)


def r_read(done, err, size, data=b""):
    return stream(status(done, err), size, len(data), data)


def r_read_ex(done, err, size, data, handle):
    return stream(status(done, err), size, len(data), data, handle)


def r_write(done, err, handle, count):
    return stream(status(done, err), handle, count)


IMEI_S = "Z:\\NVRAM\\NVD_IMEI\\MP0B_001"
ORIG_S = "Z:\\NVRAM\\ORIG"


def expect(replies, cases):
    """cases: list of (index, expected raw stream, description)."""
    for i, want, what in cases:
        got = replies[i].raw if i < len(replies) else None
        check(got == want, "%s: got %r" % (what, None if got is None else parse_stream(got)))


def test_combined_read(root):
    big = bytes(Rng(7).randrange(256) for _ in range(0x5000))
    with open(os.path.join(root, "ro/nvdata/md/BIGR"), "wb") as f:
        f.write(big)
    ro_before = tree_hash(os.path.join(root, "ro"))
    fr = []
    fr += cread(IMEI_S, mask=0x1F, flags=O_RDONLY, size=0xDEAD, off=1, length=3)    # 0
    fr += cread(IMEI_S, mask=0x11, flags=O_RDONLY, size=0xDEAD)                     # 1
    fr += cread(IMEI_S, mask=0x1E, flags=O_RDONLY, size=0xDEAD, length=5)           # 2
    fr += cread(IMEI_S, mask=0x0F, flags=O_RDONLY, size=1, length=5)                # 3 no close bit
    fr += req(OPEN, IMEI, O_RDONLY) + req(CLOSE, 1)                                 # 4, 5
    fr += cread("Z:\\NVRAM\\NOPE", mask=0x1F, size=7, length=4)                     # 6
    fr += cread("Z:\\NOPE\\X", mask=0x1F, size=7, length=4)                         # 7
    fr += cread("C:\\x", mask=0x1F, size=7) + cread("T:\\x", mask=0x1F, size=7)     # 8, 9
    fr += cread("Z:\\NVRAM\\..\\NVRAM\\ORIG", mask=0x1F, size=7)                    # 10
    fr += cread("Z:\\NVRAM\\.\\ORIG", mask=0x1F, size=7)                            # 11
    fr += cread("Z:\\" + "a" * 300, mask=0x1F, size=7)                              # 12
    fr += cread(IMEI_S, mask=0x1F, flags=O_RDONLY, size=7, whence=5, length=5)      # 13 bad whence
    fr += cread(IMEI_S, mask=0x1F, flags=O_RDONLY, off=-10, length=5)               # 14 negative
    fr += cread(IMEI_S, mask=0x1F, flags=O_RDONLY, off=100, length=10)              # 15 beyond end
    fr += cread(IMEI_S, mask=0x1F, flags=O_RDONLY, off=3, length=100)               # 16 short read
    fr += cread(IMEI_S, mask=0x1F, flags=O_RDONLY, off=-2, whence=2, length=10)     # 17 seek end
    fr += cread(IMEI_S, mask=0x1D, flags=O_RDONLY, size=9, off=2, whence=1, length=2)  # 18 cur, no size
    fr += cread("Z:\\NVRAM", mask=0x19, flags=O_DIR, size=9, length=4)              # 19 read a dir
    fr += req(OPEN, IMEI, O_RDONLY) + req(CLOSE, 1)                                 # 20, 21
    fr += req(CREAD, path(IMEI_S), cb_read(0x1F, length=4, nbytes=36))              # 22 short block
    fr += req(CREAD, path(IMEI_S))                                                  # 23 one argument
    fr += req(CREAD, path(IMEI_S), cb_read(0x1F, flags=O_RDONLY, length=2, nbytes=48))  # 24 longer block
    fr += cread("Z:\\link", mask=0x1F, size=7)                                      # 25 symlink
    fr += cread("Z:\\NVRAM\\..\\x", mask=0x1E, size=7)                              # 26 no open: path unused
    fr += cread("Z:\\BIGR", mask=0x1F, flags=O_RDONLY, length=0x10000)              # 27 capped
    fr += cread("Z:\\BIGR", mask=0x1F, flags=O_RDONLY, length=0x40A0)               # 28 exact cap
    fr += cread("Z:\\BIGR", mask=0x1F, flags=O_RDONLY, off=0x10, length=0x40A1)     # 29
    fr += cread("Z:\\BIGR", mask=0x1F, flags=O_RDONLY, length=0)                    # 30 zero length
    fr += cread(IMEI_S, mask=0x3F, flags=O_RDONLY, length=5)                        # 31 write bit ignored
    fr += req(CREAD)                                                                # 32 no arguments
    r = run(root, fr)
    check(len(r) == 33, "combined read: 33 replies, got %d" % len(r))
    expect(r, [
        (0, r_read(0x1F, 0, 5, b"ell"), "0x1022 open+size+seek+read+close"),
        (1, r_read(0x11, 0, 0xDEAD), "0x1022 open+close, size echoed"),
        (2, r_read(0, -18, 0xDEAD), "0x1022 without open bit -18"),
        (3, r_read(0x0F, 0, 5, b"hello"), "0x1022 without close bit"),
        (6, r_read(0, -9, 7), "0x1022 missing file -9"),
        (7, r_read(0, -9, 7), "0x1022 missing directory -9"),
        (8, r_read(0, -19, 7), "0x1022 unknown drive -19"),
        (9, r_read(0, -9, 7), "0x1022 absent drive T: -9"),
        (10, r_read(0, -16, 7), "0x1022 .. -16"),
        (11, r_read(0, -3, 7), "0x1022 . component -3"),
        (12, r_read(0, -49, 7), "0x1022 too long -49"),
        (13, r_read(0x03, -2, 5), "0x1022 bad whence -2"),
        (14, r_read(0x03, -18, 5), "0x1022 negative position -18"),
        (15, r_read(0x1F, 0, 5), "0x1022 seek beyond end, read 0"),
        (16, r_read(0x1F, 0, 5, b"lo"), "0x1022 short read"),
        (17, r_read(0x1F, 0, 5, b"lo"), "0x1022 seek from end"),
        (18, r_read(0x1D, 0, 9, b"ll"), "0x1022 seek from current, no size step"),
        (19, r_read(0x01, -10, 9), "0x1022 read of a directory handle -10"),
        (22, r_read(0, -2, 0), "0x1022 short block -2"),
        (23, r_read(0, -2, 0), "0x1022 one argument -2"),
        (24, r_read(0x1F, 0, 5, b"he"), "0x1022 block longer than 40 bytes"),
        (25, r_read(0, -16, 7), "0x1022 symlink -16"),
        (26, r_read(0, -18, 7), "0x1022 no open bit: path not checked"),
        (27, r_read(0x1F, 0, 0x5000, big[:0x40A0]), "0x1022 read capped at 0x40A0"),
        (28, r_read(0x1F, 0, 0x5000, big[:0x40A0]), "0x1022 read of exactly 0x40A0"),
        (29, r_read(0x1F, 0, 0x5000, big[0x10:0x10 + 0x40A0]), "0x1022 read 0x40A1 capped"),
        (30, r_read(0x1F, 0, 0x5000), "0x1022 zero length"),
        (31, r_read(0x1F, 0, 5, b"hello"), "0x1022 write bit ignored"),
        (32, r_read(0, -2, 0), "0x1022 no arguments -2"),
    ])
    check(r[4].ret == 1 and r[20].ret == 1, "0x1022 leaves no handle open (also after failures)")
    check(r[27].nfrags == 5, "0x1022 large reply split into fragments")
    check(tree_hash(os.path.join(root, "ro")) == ro_before, "0x1022: ro/ unchanged")
    check(not os.path.exists(os.path.join(root, "rw/nvdata/md/NVRAM/NVD_IMEI/MP0B_001")),
          "0x1022: no copy-up")
    j = journal(root)
    check('COMBINED op=0x1022 (CombinedRead) slot=0 path="Z:\\NVRAM\\NVD_IMEI\\MP0B_001" '
          'mask=0x1f flags=0x100 off=1 whence=0 len=3' in j, "0x1022 decoded path journaled")
    check('path="Z:\\NVRAM\\..\\x" mask=0x1e' in j, "0x1022 path journaled without open")
    check("BADPARAM op=0x1022 (CombinedRead) slot=0 nargs=2 lens=54,36" in j, "0x1022 short block journaled")
    # -v shows the decoded path
    run(root, cread(IMEI_S, mask=0x1F, flags=O_RDONLY, length=1), ["-v"])
    check(b'COMBINED op=0x1022 (CombinedRead) slot=0 path="Z:\\NVRAM\\NVD_IMEI\\MP0B_001"' in LAST_ERR,
          "0x1022 path logged at -v")


def test_combined_write(root):
    ro_before = tree_hash(os.path.join(root, "ro"))
    rw = os.path.join(root, "rw/nvdata/md")
    fr = []
    fr += cwrite("Z:\\NEW", b"ABCD", mask=0x35, flags=O_CREATE, length=4)               # 0
    fr += cwrite("Z:\\NEW", b"xy", mask=0x25, off=2, length=2)                          # 1 stays open
    fr += req(WRITE, 1, b"Z", 1, b"")                                                   # 2
    fr += req(SEEK, 1, 0, 0) + req(READ, 1, 10) + req(CLOSE, 1)               # 3, 4, 5
    fr += req(OPEN, path("Z:\\NEW"), 0)                                                 # 6 -> 1
    fr += cwrite("Q:\\ignored", b"!", mask=0x35, whence=2, length=1, handle=1)          # 7
    fr += req(CLOSE, 1)                                                                 # 8 -10
    fr += req(OPEN, path("Z:\\NEW"), 0)                                                 # 9 -> 1
    fr += cwrite("", b"a", mask=0x24, length=1, handle=1)                               # 10
    fr += req(READ, 1, 10) + req(CLOSE, 1)                                    # 11, 12
    fr += cwrite("Z:\\NEW", b"", mask=0x34)                                             # 13 no handle, no open
    fr += cwrite("Z:\\NOPE", b"ab", mask=0x35, length=2)                                # 14
    fr += cwrite("C:\\x", b"ab", mask=0x35, length=2)                                   # 15
    fr += cwrite("Z:\\..\\x", b"ab", mask=0x35, flags=O_CREATE, length=2)               # 16
    fr += cwrite("W:\\fw.img", b"ab", mask=0x35, length=2)                              # 17 write -30
    fr += req(CLOSE, 1)                                                                 # 18 -10
    fr += cwrite("Z:\\NEW", b"ab", mask=0x35, length=10)                                # 19 len > data
    fr += req(CLOSE, 1)                                                                 # 20 -10
    fr += cwrite("Z:\\NEW", b"ab", mask=0x24, length=2, handle=50)                      # 21 bad handle
    fr += req(OPEN, path("Z:\\NEW"), 0)                                                 # 22 -> 1
    fr += cwrite("Z:\\NEW", b"", mask=0x10, handle=1)                                   # 23 close only
    fr += req(CLOSE, 1)                                                                 # 24 -10
    fr += cwrite(IMEI_S, b"XY", mask=0x35, off=2, length=2)                             # 25 copy-up
    fr += cwrite("Z:\\NEW", b"ab", mask=0x35, whence=3, length=2)                       # 26 bad whence
    fr += cwrite("Z:\\NEW", b"ab", mask=0x35, off=-1, length=2)                         # 27 negative
    fr += cwrite("Z:\\W2", b"ABCD", mask=0x35, flags=O_CREATE, length=2)                # 28 len < data
    fr += cwrite("Z:\\HOLE", b"Z", mask=0x35, flags=O_CREATE, off=10, length=1)         # 29 beyond end
    fr += cwrite("Z:\\NEW", b"ab", mask=0x35, flags=O_RDONLY, length=2)                 # 30 read-only handle
    fr += req(CWRITE, path("Z:\\NEW"), cb_write(0x35, length=2, nbytes=32), b"ab")      # 31 short block
    fr += req(CWRITE, path("Z:\\NEW"), cb_write(0x35, length=2))                        # 32 two arguments
    fr += cwrite("X:\\CAL", b"CAL!", mask=0x35, length=4)                               # 33 X: allowed
    fr += cwrite("Z:\\NEW", b"q", mask=0x3F, length=1)                                  # 34 read/size bits ignored
    fr += cwrite("Z:\\NEW", b"", mask=0x15, length=0)                                   # 35 no write step
    fr += req(CWRITE, path("Z:\\NEW"), cb_write(0x35, length=2, nbytes=36), b"zz")      # 36 36-byte block
    r = run(root, fr)
    check(len(r) == 37, "combined write: 37 replies, got %d" % len(r))
    expect(r, [
        (0, r_write(0x35, 0, 1, 4), "0x1024 create+seek+write+close"),
        (1, r_write(0x25, 0, 1, 2), "0x1024 without close bit keeps the handle"),
        (7, r_write(0x34, 0, 1, 1), "0x1024 passed handle, open bit ignored"),
        (10, r_write(0x24, 0, 1, 1), "0x1024 passed handle without close"),
        (13, r_write(0, -2, -1, 0), "0x1024 no handle, no open bit -2"),
        (14, r_write(0, -9, -1, 0), "0x1024 open failed -9"),
        (15, r_write(0, -19, -1, 0), "0x1024 unknown drive -19"),
        (16, r_write(0, -16, -1, 0), "0x1024 .. -16"),
        (17, r_write(0x05, -30, 1, -1), "0x1024 write to W: -30, count -1"),
        (19, r_write(0x01, -2, 1, 0), "0x1024 length beyond the data -2"),
        (21, r_write(0, -10, 50, 0), "0x1024 bad handle -10"),
        (23, r_write(0x10, 0, 1, 0), "0x1024 close only"),
        (25, r_write(0x35, 0, 1, 2), "0x1024 write to an ro/ file"),
        (26, r_write(0x01, -2, 1, 0), "0x1024 bad whence -2"),
        (27, r_write(0x01, -18, 1, 0), "0x1024 negative position -18"),
        (28, r_write(0x35, 0, 1, 2), "0x1024 writes word 7 bytes"),
        (29, r_write(0x35, 0, 1, 1), "0x1024 write beyond the end"),
        (30, r_write(0x05, -16, 1, -1), "0x1024 write through a read-only handle -16"),
        (31, r_write(0, -2, -1, 0), "0x1024 short block -2"),
        (32, r_write(0, -2, -1, 0), "0x1024 two arguments -2"),
        (33, r_write(0x35, 0, 1, 4), "0x1024 X: writable without -p"),
        (34, r_write(0x35, 0, 1, 1), "0x1024 size/read bits ignored"),
        (35, r_write(0x15, 0, 1, 0), "0x1024 without write step"),
        (36, r_write(0x35, 0, 1, 2), "0x1024 36-byte block"),
    ])
    check(r[2].ret == 0 and r[2].u32(1) == 1, "single Write on a compound handle")
    check(r[4].args[2] == b"ABxyZ" and r[5].ret == 0, "single Read/Close on a compound handle")
    check(r[6].ret == 1 and r[8].ret == -10, "compound close of a single-Open handle")
    check(r[11].args[2] == b"BxyZ!" and r[12].ret == 0, "handle position after compound write")
    check([r[i].ret for i in (18, 20, 24)] == [-10] * 3, "failed/closing compound requests close the handle")
    check(r[22].ret == 1, "handle table free after failures")
    check(open(os.path.join(rw, "NEW"), "rb").read() == b"zzxyZ!", "0x1024 file content")
    check(open(os.path.join(rw, "W2"), "rb").read() == b"AB", "0x1024 word 7 bytes on disk")
    check(open(os.path.join(rw, "HOLE"), "rb").read() == b"\0" * 10 + b"Z", "0x1024 hole")
    check(open(os.path.join(rw, "NVRAM/NVD_IMEI/MP0B_001"), "rb").read() == b"heXYo", "0x1024 copy-up")
    check(tree_hash(os.path.join(root, "ro")) == ro_before, "0x1024: ro/ unchanged")
    check(not os.path.exists(os.path.join(rw, "x")), "0x1024 .. created nothing")
    j = journal(root)
    check("CREATE Z:/NEW" in j and "WRITE Z:/NEW off=0 len=4 crc32=%08x" % zlib.crc32(b"ABCD") in j,
          "0x1024 create/write journaled")
    check("COPYUP Z:/NVRAM/NVD_IMEI/MP0B_001 size=5" in j, "0x1024 copy-up journaled")
    check("REFUSED write-protected write W:/fw.img" in j, "0x1024 W: refusal journaled")
    check("PROTECT WRITE X:/CAL off=0 len=4 crc32=%08x" % zlib.crc32(b"CAL!") in j, "0x1024 PROTECT journaled")
    check('COMBINED op=0x1024 (CombinedWrite) slot=0 path="Z:\\NEW" mask=0x35 flags=0x10000 '
          'off=0 whence=0 len=4 handle=-1' in j, "0x1024 decoded path journaled")
    check("BADPARAM op=0x1024 (CombinedWrite) slot=0 nargs=3 lens=14,32,2" in j, "0x1024 short block journaled")
    # -p refuses X:/Y: like the single Write and Open
    r = run(root, cwrite("X:\\CAL", b"no", mask=0x35, length=2) +
            cwrite("Y:\\NEW", b"no", mask=0x35, flags=O_CREATE, length=2), ["-p"])
    expect(r, [
        (0, r_write(0x05, -30, 1, -1), "0x1024 -p: X: write -30"),
        (1, r_write(0, -30, -1, 0), "0x1024 -p: Y: create -30"),
    ])
    check("REFUSED PROTECT write X:/CAL" in journal(root), "0x1024 -p refusal journaled")
    check(open(os.path.join(root, "rw/protect_f/md/CAL"), "rb").read() == b"CAL!", "0x1024 -p: X: unchanged")


def test_combined_read_ex(root):
    big = bytes(Rng(9).randrange(256) for _ in range(0x5000))
    with open(os.path.join(root, "ro/nvdata/md/BIGR"), "wb") as f:
        f.write(big)
    fr = []
    fr += cread_ex(IMEI_S, mask=0x0F, flags=O_RDONLY, off=1, length=2, handle=77)        # 0 -> 1
    fr += req(READ, 1, 10)                                                      # 1
    fr += cread_ex("Q:\\ignored", mask=0x1C, size=9, length=5, handle=1)                 # 2
    fr += req(CLOSE, 1)                                                                   # 3 -10
    fr += cread_ex(IMEI_S, mask=0x0E, size=9, length=5, handle=-1)                       # 4
    fr += cread_ex(IMEI_S, mask=0x0F, flags=O_RDONLY, whence=9, length=5)                # 5
    fr += req(OPEN, IMEI, O_RDONLY)                                                       # 6 -> 1
    fr += cread_ex(IMEI_S, mask=0x0C, size=9, whence=9, length=5, handle=1)              # 7
    fr += req(CLOSE, 1)                                                                   # 8 -10
    fr += cread_ex("Z:\\NOPE", mask=0x1F, size=9, length=5)                              # 9
    fr += cread_ex(IMEI_S, mask=0x08, size=9, length=5, handle=40)                       # 10
    fr += cread_ex(IMEI_S, mask=0x01, flags=0, size=9)                                   # 11 -> 1, rw
    fr += cwrite("", b"J", mask=0x24, whence=2, length=1, handle=1)                      # 12
    fr += cread_ex("", mask=0x1E, length=100, handle=1)                                  # 13
    fr += req(CREAD_EX, path(IMEI_S), cb_read(0x1F, length=5, handle=-1, nbytes=40))     # 14
    fr += cread_ex(IMEI_S, mask=0x11, flags=O_RDONLY, size=9)                            # 15
    fr += cread_ex("Z:\\BIGR", mask=0x1F, flags=O_RDONLY, length=0x10000)                # 16
    fr += cread_ex("C:\\x", mask=0x1F, size=9)                                           # 17
    fr += cread_ex("Z:\\a\\..\\b", mask=0x1F, size=9)                                    # 18
    fr += req(CREAD_EX, path(IMEI_S), cb_read(0x1F, length=5, handle=-1, nbytes=44))     # 19 44 bytes
    fr += req(CREAD_EX, path(IMEI_S), cb_read(0x1F, length=5, handle=-1), b"x")          # 20 3 args
    fr += cread_ex(IMEI_S, mask=0x1F, flags=O_RDONLY, off=100, length=5)                 # 21 beyond end
    r = run(root, fr)
    check(len(r) == 22, "combined read ex: 22 replies, got %d" % len(r))
    expect(r, [
        (0, r_read_ex(0x0F, 0, 5, b"el", 1), "0x1026 open, handle kept, word 10 ignored"),
        (2, r_read_ex(0x1C, 0, 9, b"hello", 1), "0x1026 passed handle, close"),
        (4, r_read_ex(0, -10, 9, b"", -1), "0x1026 no open bit, handle < 0"),
        (5, r_read_ex(0x03, -2, 5, b"", -1), "0x1026 later step failed: handle -1"),
        (7, r_read_ex(0, -2, 9, b"", -1), "0x1026 passed handle, step failed"),
        (9, r_read_ex(0, -9, 9, b"", -1), "0x1026 open failed -9"),
        (10, r_read_ex(0, -10, 9, b"", -1), "0x1026 passed handle not open -10"),
        (11, r_read_ex(0x01, 0, 9, b"", 1), "0x1026 open only"),
        (12, r_write(0x24, 0, 1, 1), "0x1024 on a 0x1026 handle"),
        (13, r_read_ex(0x1E, 0, 6, b"helloJ", 1), "0x1026 on a handle written by 0x1024"),
        (14, r_read_ex(0, -2, 0, b"", -1), "0x1026 40-byte block -2"),
        (15, r_read_ex(0x11, 0, 9, b"", 1), "0x1026 open+close returns the closed handle"),
        (16, r_read_ex(0x1F, 0, 0x5000, big[:0x4098], 1), "0x1026 read capped at 0x4098"),
        (17, r_read_ex(0, -19, 9, b"", -1), "0x1026 unknown drive -19"),
        (18, r_read_ex(0, -16, 9, b"", -1), "0x1026 .. -16"),
        (19, r_read_ex(0x1F, 0, 6, b"hello", 1), "0x1026 44-byte block accepted"),
        (20, r_read_ex(0x1F, 0, 6, b"hello", 1), "0x1026 extra argument ignored"),
        (21, r_read_ex(0x1F, 0, 6, b"", 1), "0x1026 read beyond the end: count 0"),
    ])
    check(r[1].args[2] == b"lo", "single Read continues a 0x1026 handle")
    check(r[3].ret == -10 and r[8].ret == -10, "0x1026 close/failure closes the handle")
    check(r[6].ret == 1, "0x1026 failure released the opened handle")
    j = journal(root)
    check('COMBINED op=0x1026 (CombinedReadEx) slot=0 path="Z:\\NVRAM\\NVD_IMEI\\MP0B_001" mask=0xf '
          'flags=0x100 off=1 whence=0 len=2 handle=77' in j, "0x1026 decoded path journaled")
    check("BADPARAM op=0x1026 (CombinedReadEx) slot=0 nargs=2" in j and
          "EXTRAARGS op=0x1026 (CombinedReadEx) slot=0 nargs=3" in j, "0x1026 bad/extra args journaled")


def mv(src, dst, flags):
    return req(MOVE, path(src), path(dst), flags)


def test_move(root):
    os.makedirs(os.path.join(root, "ro/nvdata/md/RODIR/sub"))
    for n, c in (("RODIR/sub/a", b"a"), ("RODIR/b", b"b")):
        with open(os.path.join(root, "ro/nvdata/md", n), "wb") as f:
            f.write(c)
    os.makedirs(os.path.join(root, "rw/nvdata/md"))
    with open(os.path.join(root, "rw/nvdata/md/LONG"), "wb") as f:
        f.write(b"x" * 20)
    ro_before = tree_hash(os.path.join(root, "ro"))
    rw = os.path.join(root, "rw/nvdata/md")
    long_name = "Z:\\" + "a" * 300
    fr = []
    fr += mv(ORIG_S, "Z:\\NVRAM\\COPY", 1)                                     # 0
    fr += req(GET_ATTR, path(ORIG_S))                                          # 1
    fr += mv(ORIG_S, "Z:\\NVRAM\\COPY2", 3)                                    # 2 copy + kill
    fr += req(GET_ATTR, path(ORIG_S))                                          # 3
    fr += mv(ORIG_S, "Z:\\K", 2) + req(GET_ATTR, path("Z:\\K"))                # 4, 5
    fr += mv(ORIG_S, "Z:\\K", 0) + mv(ORIG_S, "Z:\\K", 5)                      # 6, 7
    fr += mv(ORIG_S, "Z:\\K", 0x10000) + mv(ORIG_S, "Z:\\K", 0x20001)          # 8, 9
    fr += mv(ORIG_S, "z:/NVRAM/ORIG", 1)                                       # 10 same name
    fr += mv("Z:\\NVRAM\\..\\NVRAM\\ORIG", "Z:\\K", 1)                         # 11
    fr += mv(ORIG_S, "Z:\\..\\K", 1)                                           # 12
    fr += mv("C:\\x", "Z:\\K", 1) + mv(ORIG_S, "K:\\x", 1)                     # 13, 14
    fr += mv(long_name, "Z:\\K", 1)                                            # 15
    fr += mv("Z:\\NVRAM\\NOPE", "Z:\\K", 1)                                    # 16
    fr += mv("Z:\\NOPE\\X", "Z:\\K", 1)                                        # 17
    fr += mv(ORIG_S, "Z:\\NOPE\\X", 1)                                         # 18
    fr += mv(ORIG_S, "Z:\\NVRAM\\COPY2", 0x10002)                              # 19 delete dst
    fr += req(GET_ATTR, path("Z:\\NVRAM\\COPY2"))                              # 20
    fr += mv(ORIG_S, "Z:\\NVRAM\\COPY2", 0x10002)                              # 21 dst missing
    fr += mv(ORIG_S, "Z:\\LONG", 1)                                            # 22 overwrites anyway
    fr += mv("W:\\fw.img", "Z:\\FW", 3)                                        # 23 kill ro source
    fr += mv("W:\\fw.img", "Z:\\FW", 1)                                        # 24 copy from W:
    fr += mv(ORIG_S, "W:\\fw.img", 0x10001)                                    # 25 ro dst
    fr += mv(ORIG_S, "W:\\new", 1)                                             # 26
    fr += mv(ORIG_S, "W:\\fw.img", 1)                                          # 27
    fr += mv(ORIG_S, "Z:\\RODIR", 0x10001)                                     # 28 dir in ro/
    fr += req(GET_ATTR, path("Z:\\RODIR")) + req(GET_ATTR, path("Z:\\RODIR\\b"))  # 29, 30
    fr += req(CREATE_DIR, path("Z:\\DD")) + req(OPEN, path("Z:\\DD\\f"), O_CREATE)  # 31, 32 -> 1
    fr += req(CREATE_DIR, path("Z:\\DD\\E")) + req(OPEN, path("Z:\\DD\\E\\g"), O_CREATE)  # 33, 34 -> 2
    fr += mv(ORIG_S, "Z:\\DD", 0x10002)                                        # 35 dir in rw/
    fr += req(GET_ATTR, path("Z:\\DD"))                                        # 36
    fr += req(WRITE, 2, b"z", 1, b"") + req(CLOSE_ALL)                         # 37, 38
    fr += req(MOVE, path(ORIG_S), path("Z:\\K"))                               # 39 two args
    fr += req(MOVE, path(ORIG_S), path("Z:\\K"), b"\1\0")                      # 40 flags 2 bytes
    fr += mv("Z:\\link", "Z:\\K", 1)                                           # 41 symlink
    fr += mv("Z:\\NVRAM", "Z:\\K", 1)                                          # 42 dir as source
    fr += mv(ORIG_S, "Z:\\", 0x10001)                                          # 43 drive root
    fr += mv(ORIG_S, "X:\\NEW", 1)                                             # 44 X: without -p
    fr += mv(ORIG_S, "Z:\\NVRAM", 1)                                           # 45 dst is a dir
    fr += mv(ORIG_S, "Z:\\NVRAM\\.\\K", 1)                                     # 46
    r = run(root, fr)
    rets = [x.ret for x in r]
    check(len(r) == 47, "move: 47 replies, got %d" % len(r))
    check(all(len(x.args) == 1 for x in r if x.op == MOVE), "Move replies carry only the return code")
    check(rets[0] == 0 and rets[1] == 0, "Move copies; the source stays")
    check(rets[2] == 0 and rets[3] == 0, "Move with kill keeps the source")
    check(rets[4] == -18 and rets[5] == -9, "kill without copy -18, nothing created")
    check(rets[6:10] == [-2] * 4, "bad flags -2")
    check(rets[10] == -2, "same name -2")
    check(rets[11:13] == [-16, -16], ".. -16")
    check(rets[13:15] == [-19, -19], "unknown drive -19")
    check(rets[15] == -49, "too long -49")
    check(rets[16:19] == [-9, -9, -9], "missing source / directories -9")
    check(rets[19] == 0 and rets[20] == -9, "overwrite without copy removes the destination")
    check(rets[21] == -18, "overwrite without copy, no destination -18")
    check(rets[22] == 0, "copy overwrites without the overwrite flag")
    check(rets[23] == -45 and rets[24] == 0, "kill on a read-only source -45; copy from W:")
    check(rets[25] == -45 and rets[26] == -30 and rets[27] == -30, "read-only destination")
    check(rets[28] == 0 and rets[29] == 0 and rets[30] == -9, "overwrite replaces an ro/ directory")
    check(rets[31:35] == [0, 1, 0, 2], "rw/ tree set up")
    check(rets[35] == 0 and rets[36] == -9, "overwrite removes an rw/ directory tree")
    check(rets[37] == -16, "handle below a removed tree is stale")
    check(rets[39] == -2 and rets[40] == -2, "bad Move arguments -2")
    check(rets[41] == -16 and rets[42] == -16 and rets[43] == -16, "symlink, directory source, drive root -16")
    check(rets[44] == 0 and rets[45] == -16 and rets[46] == -3, "X: copy; directory destination; . component")
    for n, c in (("NVRAM/COPY", b"original"), ("LONG", b"original"), ("FW", b"firmware"),
                 ("RODIR", b"original")):
        check(open(os.path.join(rw, n), "rb").read() == c, "Move content of %s" % n)
    check(open(os.path.join(root, "rw/protect_f/md/NEW"), "rb").read() == b"original", "Move to X:")
    check(not os.path.exists(os.path.join(rw, "NVRAM/ORIG")), "Move does not copy the source up")
    check(not os.path.exists(os.path.join(rw, "DD")), "rw/ tree removed")
    check(not os.path.exists(os.path.join(root, "rw/firmware")), "nothing written for W:")
    check(tree_hash(os.path.join(root, "ro")) == ro_before, "Move: ro/ unchanged")
    check(open(os.path.join(root, "outside"), "rb").read() == b"secret", "Move: outside untouched")
    j = journal(root)
    crc = zlib.crc32(b"original")
    check('MOVE slot=0 src="Z:\\NVRAM\\ORIG" dst="Z:\\NVRAM\\COPY" flags=0x1' in j, "Move paths journaled")
    check("CREATE Z:/NVRAM/COPY" in j and
          "WRITE Z:/NVRAM/COPY off=0 len=8 crc32=%08x (copy of Z:/NVRAM/ORIG)" % crc in j,
          "Move journaled as create + write")
    check("TRUNCATE Z:/LONG" in j, "Move over an existing file journaled")
    check("DELETE Z:/NVRAM/COPY2" in j and "DELETE-TREE Z:/RODIR (whiteout)" in j and
          "DELETE-TREE Z:/DD" in j, "Move deletions journaled")
    check("PROTECT CREATE X:/NEW" in j and "PROTECT WRITE X:/NEW off=0 len=8" in j, "Move to X: PROTECT")
    check("BADPARAM op=0x100c (Move) slot=0 nargs=2" in j, "Move bad args journaled")
    # -p: X:/Y: are read-only for Move too
    r = run(root, mv(ORIG_S, "X:\\NEW2", 1) + mv(ORIG_S, "X:\\CAL", 0x10001) +
            mv("X:\\CAL", "Z:\\CAL", 3) + mv("X:\\CAL", "Z:\\CAL", 1), ["-p"])
    check([x.ret for x in r] == [-30, -45, -45, 0], "Move with -p")
    check("REFUSED PROTECT create X:/NEW2" in journal(root), "Move -p refusal journaled")
    run(root, mv(ORIG_S, "Z:\\V", 1), ["-v"])
    check(b'MOVE slot=0 src="Z:\\NVRAM\\ORIG" dst="Z:\\V" flags=0x1' in LAST_ERR, "Move paths at -v")


def test_sync_close(root):
    fr = []
    fr += req(OPEN, path("Z:\\S1"), O_CREATE) + req(CLOSE, 1)
    fr += req(OPEN, path("Z:\\S2"), O_TRUNC) + req(CLOSE, 1)
    fr += req(OPEN, IMEI, O_RDONLY | O_COMMIT) + req(CLOSE, 1)
    fr += req(OPEN, path(ORIG_S), O_RDONLY) + req(CLOSE, 1)
    fr += req(OPEN, path("Z:\\S1"), 0) + req(WRITE, 1, b"a", 1, b"") + req(CLOSE, 1)
    fr += req(OPEN, path("Z:\\S3"), O_CREATE) + req(CLOSE_ALL)
    fr += req(OPEN, path("Z:\\S5"), O_CREATE) + req(SHUTDOWN)
    fr += cwrite("Z:\\S4", b"ab", mask=0x35, flags=O_CREATE, length=2)
    fr += cwrite("Z:\\S6", b"ab", mask=0x35, flags=O_CREATE, length=9)       # fails, closed
    fr += cread("Z:\\NVRAM\\NVD_IMEI\\MP0B_001", mask=0x0F, flags=O_RDONLY | O_COMMIT, length=1)
    fr += req(OPEN, path("Z:\\NVRAM"), O_DIR | O_CREATE) + req(CLOSE, 1)
    r = run(root, fr, ["-v"])
    err = LAST_ERR.decode(errors="replace")
    lines = [l.split("t2-ccci-fsd: ", 1)[-1] for l in err.splitlines()]
    syncs = [l[5:] for l in lines if l.startswith("sync ")]
    check(syncs == ["Z:/S1", "Z:/S2", "Z:/NVRAM/NVD_IMEI/MP0B_001", "Z:/S4", "Z:/S6",
                    "Z:/NVRAM/NVD_IMEI/MP0B_001"], "sync on close: %r" % syncs)
    check(all(x.ret >= 0 for x in r if x.op in (OPEN, CLOSE)), "closes succeed")
    check(not any("fsync" in l for l in lines), "no sync failures")


# --- single operations, GetFileDetail ---

GET_DETAIL = 0x1025


def test_single_ops(root):
    big = bytes(Rng(11).randrange(256) for _ in range(0x5000))
    with open(os.path.join(root, "ro/nvdata/md/BIGR"), "wb") as f:
        f.write(big)
    rw = os.path.join(root, "rw/nvdata/md")
    fr = []
    fr += req(OPEN, IMEI, O_RDONLY)                                   # 0 -> 1
    fr += req(GET_FILE_SIZE, 1)                                       # 1 the modem's form: one argument
    fr += req(GET_FILE_SIZE, 1, b"\0" * 4)                            # 2 extra argument ignored
    fr += req(GET_FILE_SIZE, u32(1) + b"\xff" * 4)                    # 3 8-byte handle: first 4 bytes
    fr += req(GET_FILE_SIZE, 9)                                       # 4 not open
    fr += req(GET_FILE_SIZE)                                          # 5 missing
    fr += req(GET_FILE_SIZE, b"\1\0")                                 # 6 2-byte handle
    fr += req(READ, 1, 3)                                             # 7
    fr += req(READ, 1, 100)                                           # 8 short
    fr += req(READ, 1, 100)                                           # 9 at the end
    fr += req(READ, 9, 4)                                             # 10 bad handle
    fr += req(READ, 1)                                                # 11 missing length
    fr += req(SEEK, 1, 100, 0)                                        # 12 beyond the end
    fr += req(READ, 1, 4)                                             # 13 past the end
    fr += req(SEEK, 1, (-3) & 0xFFFFFFFF, 1)                          # 14 cur: 97
    fr += req(SEEK, 1, (-6) & 0xFFFFFFFF, 2)                          # 15 end - 6: -1 -> -18
    fr += req(SEEK, 1, (-2) & 0xFFFFFFFF, 2)                          # 16 end - 2: 3
    fr += req(SEEK, 1, 0, 3)                                          # 17 bad whence
    fr += req(SEEK, 1, 0)                                             # 18 missing whence
    fr += req(WRITE, 1, b"no", 2)                                     # 19 read-only handle
    fr += req(CLOSE, 1)                                               # 20
    fr += req(OPEN, path("Z:\\NVRAM"), O_DIR)                         # 21 -> 1, directory
    fr += req(READ, 1, 4) + req(SEEK, 1, 0, 0)                        # 22, 23
    fr += req(GET_FILE_SIZE, 1) + req(WRITE, 1, b"ab", 2)             # 24, 25
    fr += req(CLOSE, 1)                                               # 26 directory handle closes
    fr += req(OPEN, path("Z:\\N1"), O_CREATE | O_RDONLY)              # 27 -> 1, read/write anyway
    fr += req(WRITE, 1, b"abc", 3)                                    # 28
    fr += req(WRITE, 1, b"ab", 3)                                     # 29 length beyond data
    fr += req(WRITE, 1, b"ab")                                        # 30 missing length
    fr += req(WRITE, 77, b"ab", 2)                                    # 31 bad handle
    fr += req(SEEK, 1, 10, 0) + req(WRITE, 1, b"Z", 1)                # 32, 33 hole
    fr += req(CLOSE, 1)                                               # 34
    fr += req(OPEN, path("Z:\\N1"), O_CREATE)                         # 35 existing: opened, not -36
    fr += req(GET_FILE_SIZE, 1) + req(CLOSE, 1)                       # 36, 37
    fr += req(OPEN, path("Z:\\N1"), O_TRUNC | O_RDONLY)               # 38 truncated, read/write
    fr += req(WRITE, 1, b"T", 1) + req(CLOSE, 1)                      # 39, 40
    fr += req(OPEN, path("C:\\x"), 0)                                 # 41
    fr += req(OPEN, path("Z:\\NOPE\\X"), O_CREATE)                    # 42
    fr += req(OPEN, path("Z:\\NOPE"), O_DIR)                          # 43
    fr += req(OPEN, path("Z:\\NVRAM\\..\\ORIG"), 0)                   # 44
    fr += req(OPEN, path("Z:\\" + "a" * 300), 0)                      # 45
    fr += req(OPEN, IMEI)                                             # 46 missing flags
    fr += req(OPEN_VAR, IMEI, O_RDONLY, b"ABCDEFGH")                  # 47 -> 1
    fr += req(OPEN_VAR, path("Z:\\NOPE"), 0, b"12345678")             # 48 error, echoed
    fr += req(OPEN_VAR, IMEI, O_RDONLY, b"abcdefghIJKL")              # 49 -> 2, first 8 bytes
    fr += req(OPEN_VAR, IMEI, O_RDONLY, b"wxyz")                      # 50 -> 3, padded
    fr += req(OPEN_VAR, IMEI, O_RDONLY)                               # 51 -> 4, zeros
    fr += req(OPEN_VAR, IMEI, b"ABCDEFGH")                            # 52 flags of 8 bytes: 0x44434241
    fr += req(OPEN_VAR, IMEI)                                         # 53 missing flags
    fr += req(CLOSE_ALL)                                              # 54
    fr += req(CLOSE, 0) + req(CLOSE, 129) + req(CLOSE)                # 55, 56, 57
    fr += req(OPEN, path("Z:\\BIGR"), O_RDONLY) + req(READ, 1, 0x10000)  # 58, 59 capped
    fr += req(READ, 1, 0)                                             # 60 zero length
    fr += req(CLOSE, 1, 2, 3)                                         # 61 extras
    r = run(root, fr)
    check(len(r) == 62, "single ops: 62 replies, got %d" % len(r))
    E = stream
    expect(r, [
        (1, E(0, 5), "GetFileSize one argument"),
        (2, E(0, 5), "GetFileSize extra argument"),
        (3, E(0, 5), "GetFileSize 8-byte handle"),
        (4, E(-10, 0), "GetFileSize bad handle"),
        (5, E(-2, 0), "GetFileSize missing handle"),
        (6, E(-2, 0), "GetFileSize short handle"),
        (7, E(0, 3, b"hel"), "Read: ret, count, data"),
        (8, E(0, 2, b"lo"), "Read short"),
        (9, E(0, 0, b""), "Read at the end"),
        (10, E(-10, 0, b""), "Read bad handle"),
        (11, E(-2, 0, b""), "Read missing length"),
        (12, E(100), "Seek beyond the end"),
        (13, E(0, 0, b""), "Read past the end"),
        (14, E(97), "Seek current"),
        (15, E(-18), "Seek negative -18"),
        (16, E(3), "Seek from the end"),
        (17, E(-2), "Seek bad whence"),
        (18, E(-2), "Seek missing whence"),
        (19, E(-16, -1), "Write to a read-only handle"),
        (20, E(0), "Close"),
        (22, E(-10, 0, b""), "Read directory handle -10"),
        (23, E(-10), "Seek directory handle -10"),
        (24, E(-10, 0), "GetFileSize directory handle -10"),
        (25, E(-10, -1), "Write directory handle -10"),
        (26, E(0), "Close directory handle"),
        (27, E(1), "Open create with 0x100"),
        (28, E(0, 3), "Write: ret, count"),
        (29, E(-2, -1), "Write length beyond data"),
        (30, E(-2, -1), "Write missing length"),
        (31, E(-10, -1), "Write bad handle"),
        (32, E(10), "Seek beyond the end for a write"),
        (33, E(0, 1), "Write beyond the end"),
        (35, E(1), "Open create on an existing file"),
        (36, E(0, 11), "existing file not truncated"),
        (38, E(1), "Open truncate with 0x100"),
        (39, E(0, 1), "write after truncate"),
        (41, E(-19), "Open unknown drive -19"),
        (42, E(-9), "Open create in a missing directory -9"),
        (43, E(-9), "Open missing directory with 0x800 -9"),
        (44, E(-16), "Open .. -16"),
        (45, E(-49), "Open too long -49"),
        (46, E(-2), "Open missing flags"),
        (47, E(1, b"ABCDEFGH"), "0x1011 echoes 8 bytes"),
        (48, E(-9, b"12345678"), "0x1011 echoes on error"),
        (49, E(2, b"abcdefgh"), "0x1011 first 8 bytes"),
        (50, E(3, b"wxyz\0\0\0\0"), "0x1011 short third argument padded"),
        (51, E(4, b"\0" * 8), "0x1011 without third argument"),
        (52, E(5, b"\0" * 8), "0x1011 8-byte flags: first 4 bytes"),
        (53, E(-2, b"\0" * 8), "0x1011 missing flags"),
        (55, E(-10), "Close handle 0"),
        (56, E(-10), "Close handle 129"),
        (57, E(-2), "Close missing handle"),
        (59, E(0, 0x40AC, big[:0x40AC]), "Read capped at 0x40AC"),
        (60, E(0, 0, b""), "Read zero length"),
        (61, E(0), "Close with extra arguments"),
    ])
    check(open(os.path.join(rw, "N1"), "rb").read() == b"T", "create/truncate file content")
    j = journal(root)
    check("BADPARAM op=0x1009 (GetFileSize) slot=0 nargs=0" in j, "GetFileSize BADPARAM journaled")
    check("EXTRAARGS op=0x1009 (GetFileSize) slot=0 nargs=2 lens=4,4 (ignored)" in j, "extra args journaled")
    check("EXTRAARGS op=0x1005 (Close) slot=0 nargs=3" in j, "Close extras journaled")
    check("BADPARAM op=0x1003 (Read) slot=0 nargs=1" in j and "BADPARAM op=0x1004 (Write) slot=0 nargs=2" in j,
          "Read/Write BADPARAM journaled")
    check("EXTRAARGS op=0x1011" not in j, "0x1011 third argument is not extra")


def test_attributes(root):
    os.makedirs(os.path.join(root, "ro/firmware/sub"))
    fr = []
    for p in ("Z:\\NVRAM\\ORIG", "Z:\\NVRAM", "Z:\\", "W:\\fw.img", "W:\\sub", "Z:\\NVRAM\\NOPE",
              "Z:\\NOPE\\X", "C:\\x", "K:\\x", "Z:\\NVRAM\\..\\ORIG", "Z:\\NVRAM\\.\\ORIG",
              "Z:\\" + "a" * 300, "Z:\\link", "X:\\CAL"):
        fr += req(GET_ATTR, path(p))
    fr += req(GET_ATTR)
    r = run(root, fr)
    check([x.ret for x in r] == [0, 0x10, 0x10, 0x01, 0x10, -9, -9, -19, -19, -16, -3, -49, -16, 0, -2],
          "GetAttributes: %r" % [x.ret for x in r])
    r = run(root, req(GET_ATTR, path("X:\\CAL")) + req(GET_ATTR, path("Y:\\")), ["-p"])
    check([x.ret for x in r] == [0x01, 0x10], "GetAttributes with -p")


def test_file_detail(root):
    ro = os.path.join(root, "ro/nvdata/md")
    with open(os.path.join(ro, "NVRAM/ABCDE"), "wb") as f:
        f.write(b"x")
    os.utime(os.path.join(ro, "NVRAM/ABCDE"), (1000000001, 1000000002))
    os.utime(os.path.join(ro, "NVRAM"), (1100000001, 1100000002))

    def times(p):
        st = os.stat(p)
        return struct.pack("<QQQ", int(st.st_mtime), int(st.st_atime), int(st.st_ctime))

    z24 = b"\0" * 24
    # taken now: the copy-up below reads the ro/ file and may update its atime
    t_file, t_dir = times(os.path.join(ro, "NVRAM/ABCDE")), times(os.path.join(ro, "NVRAM"))
    name = path("Z:\\NVRAM\\ABCDE")
    check(len(name) == 30, "the modem's 30-byte path argument")
    fr = []
    fr += req(GET_DETAIL, name)                                    # 0 ro/ file
    fr += req(GET_DETAIL, path("Z:\\NVRAM"))                       # 1 directory
    fr += req(GET_DETAIL, path("Z:\\NVRAM\\NOPE"))                 # 2
    fr += req(GET_DETAIL, path("Z:\\NOPE\\X"))                     # 3
    fr += req(GET_DETAIL, path("C:\\x"))                           # 4
    fr += req(GET_DETAIL, path("Z:\\NVRAM\\..\\ORIG"))             # 5
    fr += req(GET_DETAIL, path("Z:\\" + "a" * 300))                # 6
    fr += req(GET_DETAIL, path("Z:\\link"))                        # 7 symlink
    fr += req(GET_DETAIL)                                          # 8 missing
    fr += req(GET_DETAIL, name, 1)                                 # 9 extra argument
    fr += req(DELETE, path("Z:\\NVRAM\\ORIG"))                     # 10
    fr += req(GET_DETAIL, path("Z:\\NVRAM\\ORIG"))                 # 11 whited out
    fr += req(OPEN, name, 0) + req(WRITE, 1, b"y", 1) + req(CLOSE, 1)  # 12-14 copy-up
    r = run(root, fr)
    check(len(r) == 15, "GetFileDetail: 15 replies, got %d" % len(r))
    expect(r, [
        (0, stream(0, t_file), "GetFileDetail ro/ file times"),
        (1, stream(0, t_dir), "GetFileDetail directory"),
        (2, stream(-9, z24), "GetFileDetail missing -9"),
        (3, stream(-9, z24), "GetFileDetail missing directory -9"),
        (4, stream(-19, z24), "GetFileDetail unknown drive -19"),
        (5, stream(-16, z24), "GetFileDetail .. -16"),
        (6, stream(-49, z24), "GetFileDetail too long -49"),
        (7, stream(-16, z24), "GetFileDetail symlink -16"),
        (8, stream(-2, z24), "GetFileDetail missing path -2"),
        (9, stream(0, t_file), "GetFileDetail extra argument"),
        (11, stream(-9, z24), "GetFileDetail whiteout -9"),
    ])
    check(struct.unpack_from("<QQ", r[0].args[1]) == (1000000002, 1000000001), "mtime, atime in seconds")
    rwf = os.path.join(root, "rw/nvdata/md/NVRAM/ABCDE")
    os.utime(rwf, (1200000001, 1200000002))
    t_rw = times(rwf)
    r = run(root, req(GET_DETAIL, name))
    check(r[0].raw == stream(0, t_rw) and t_rw[:8] == struct.pack("<Q", 1200000002), "GetFileDetail: the rw/ copy wins")
    j = journal(root)
    check("BADPARAM op=0x1025 (GetFileDetail) slot=0 nargs=0" in j, "GetFileDetail BADPARAM journaled")
    check("UNIMPL op=0x1025" not in j, "GetFileDetail implemented")


# --- Find, GetDrive, drives T:/Q: and unknown letters ---

FIND_FIRST, FIND_NEXT, FIND_CLOSE, GET_DRIVE = 0x1012, 0x1013, 0x1014, 0x101A
NO_REC = b"\0" * 0x34


def civil(t):
    """seconds since the epoch -> (year, month, mday, hour, min, sec), proleptic Gregorian."""
    days, secs = divmod(t, 86400)
    z = days + 719468
    era = z // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    return (y + (m <= 2), m, d, secs // 3600, secs // 60 % 60, secs % 60)


def dos(t, tz=0):
    """the factory DOS date-time: raw seconds, day of month in the month bits."""
    y, _m, d, hh, mm, ss = civil(int(t) + tz)
    return ((ss & 0x1F) | mm << 5 | hh << 11 | d << 16 | (d & 0xF) << 21 | ((y - 1980) & 0x7F) << 25)


def find_rec(attr, fits, st, tz=0):
    b = bytearray(0x34)
    b[0x0B], b[0x0C] = attr, fits
    struct.pack_into("<I", b, 0x0E, dos(st.st_atime, tz))
    struct.pack_into("<I", b, 0x16, dos(st.st_mtime, tz))
    struct.pack_into("<I", b, 0x1C, st.st_size & 0xFFFFFFFF)
    return bytes(b)


def name16(name, maxlen):
    if maxlen == 0:
        return b""
    return name[:maxlen - 1].encode("latin-1").decode("latin-1").encode("utf-16-le") + b"\0\0"


def ff(pattern, req=0, excl=0, maxlen=64):
    return req_(FIND_FIRST, path(pattern), bytes([req]), bytes([excl]), maxlen)


def req_(op, *args):
    return req(op, *args)


def fn(h, maxlen=64):
    return req(FIND_NEXT, h, maxlen)


def find_fixture(root):
    """Z:\\F: ro/ and rw/ layers with an override, a whiteout, an internal name and a symlink."""
    ro = os.path.join(root, "ro/nvdata/md/F")
    rw = os.path.join(root, "rw/nvdata/md/F")
    os.makedirs(os.path.join(ro, "sub"))
    os.makedirs(rw)
    files = {
        (ro, "A.txt"): b"ro-A", (ro, "B.TXT"): b"BB", (ro, "noext"): b"n", (ro, "c.mcf"): b"ccc",
        (ro, ".hidden"): b"h",
        (rw, "A.txt"): b"rw copy of A", (rw, "rwonly.txt"): b"rwonly",
        (rw, ".t2fsd.wh.noext"): b"", (rw, ".t2fsd.tmp.1.2"): b"tmp",
    }
    for (d, n), c in files.items():
        with open(os.path.join(d, n), "wb") as f:
            f.write(c)
    os.symlink(os.path.join(root, "outside"), os.path.join(ro, "lnk"))
    os.utime(os.path.join(ro, "B.TXT"), (1600000000, 1700000000))
    os.utime(os.path.join(rw, "A.txt"), (1600000001, 1700000001))
    os.utime(os.path.join(ro, "A.txt"), (1000000000, 1000000000))
    return ro, rw


def visible(root, rel):
    """stat of the visible copy below Z: (rw/ wins)."""
    for layer in ("rw", "ro"):
        p = os.path.join(root, layer, "nvdata/md", rel)
        if os.path.lexists(p):
            return os.stat(p)
    raise FileNotFoundError(rel)


def listing(r, first):
    """names of a FindFirst reply at index first and the FindNext replies after it until -6."""
    names = [r[first].args[2].decode("utf-16-le")[:-1]]
    i = first + 1
    while i < len(r) and r[i].op == FIND_NEXT and r[i].ret == 0:
        names.append(r[i].args[2].decode("utf-16-le")[:-1])
        i += 1
    return names, i


def test_find(root):
    old_tz = os.environ.get("TZ")
    os.environ["TZ"] = "UTC0"
    try:
        _test_find(root)
    finally:
        if old_tz is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = old_tz


def _test_find(root):
    ro_dir, rw_dir = find_fixture(root)
    fr = []
    fr += ff("Z:\\F\\*") + sum([fn(1) for _ in range(12)], [])          # 0..12
    fr += fn(1) + req(FIND_CLOSE, 1) + req(FIND_CLOSE, 1) + fn(1)       # 13..16
    r = run(root, fr)
    check(len(r) == 17, "find: 17 replies, got %d" % len(r))
    check(r[0].ret == 1 and len(r[0].args) == 3, "FindFirst returns handle 1 and 3 arguments")
    names, end = listing(r, 0)
    want = {"A.txt", "B.TXT", "c.mcf", "sub", "rwonly.txt", ".hidden"}
    check(set(names) - {".", ".."} == want and len(names) == len(set(names)),
          "merged listing: %r" % names)
    check(names[0] not in (".", ".."), "FindFirst skips . and ..")
    for i in range(end, 14):
        check(r[i].raw == stream(-6, NO_REC, b""), "FindNext at the end: -6, zeros, empty name (%d)" % i)
    check([x.ret for x in r[14:17]] == [0, -10, -10], "FindClose, again -10, FindNext on it -10")
    # every returned record byte-exact against the visible copy
    for i in range(0, end):
        n = names[i]
        rel = "F" if n == "." else "" if n == ".." else "F/" + n
        st = visible(root, rel) if rel else os.stat(os.path.join(root, "rw/nvdata/md"))
        attr = 0x10 if n in ("sub", ".", "..") else 0
        want_raw = stream(1 if i == 0 else 0, find_rec(attr, 1, st), name16(n, 64))
        check(r[i].raw == want_raw, "record of %r byte-exact" % n)
    b_rec = [r[i] for i in range(end) if names[i] == "B.TXT"][0].args[1]
    check(struct.unpack_from("<I", b_rec, 0x16)[0] ==
          (20 | 13 << 5 | 22 << 11 | 14 << 16 | 14 << 21 | 43 << 25), "mtime DOS value with the quirks")
    check(struct.unpack_from("<I", b_rec, 0x0E)[0] ==
          (8 | 26 << 5 | 12 << 11 | 13 << 16 | 13 << 21 | 40 << 25), "atime in the creation slot")
    a_rec = [r[i] for i in range(end) if names[i] == "A.txt"][0].args[1]
    check(struct.unpack_from("<I", a_rec, 0x1C)[0] == len(b"rw copy of A"), "A.txt from the rw/ copy")

    def first(pattern, **kw):
        return run(root, ff(pattern, **kw) + req(FIND_CLOSE, 1))[0]

    def all_names(pattern, req=0, excl=0):
        rr = run(root, ff(pattern, req, excl) + sum([fn(1) for _ in range(10)], []))
        if rr[0].ret < 0:
            return rr[0].ret
        return set(listing(rr, 0)[0]) - {".", ".."}

    check(all_names("Z:\\F\\*.txt") == {"A.txt", "rwonly.txt"}, "*.txt case-sensitive")
    check(all_names("Z:\\F\\*.TXT") == {"B.TXT"}, "*.TXT")
    check(all_names("Z:\\F\\?.txt") == {"A.txt"}, "?.txt")
    check(all_names("Z:\\F\\*.*") == {"A.txt", "B.TXT", "c.mcf", "rwonly.txt", ".hidden"},
          "*.* only names with a dot")
    check(all_names("Z:\\F\\*", req=0x10) == {"sub"}, "required directory attribute")
    check(all_names("Z:\\F\\*", excl=0x10) == want - {"sub"}, "excluded directory attribute")
    check(all_names("Z:\\F\\*", req=0x01) == -6, "no read-only file on Z:")
    check(all_names("Z:\\F\\r*o?ly.t*t") == {"rwonly.txt"}, "mixed wildcards")
    check(all_names("Z:\\F\\**c**") == {"c.mcf"}, "repeated stars")
    check(all_names("Z:\\F\\A.txt") == {"A.txt"}, "literal name")
    check(all_names("Z:\\F\\a.txt") == -6, "literal name is case-sensitive")
    check(all_names("Z:\\F\\noext") == -6, "whited-out ro/ name hidden")
    check(all_names("Z:\\F\\*fsd*") == -6, "internal names never listed")
    check(all_names("Z:\\F\\lnk") == -6, "symlink skipped")
    check(all_names("Z:\\F\\.") == -6 and all_names("Z:\\F\\..") == -6, "FindFirst never returns . or ..")
    check(all_names("Z:\\F\\") == -6, "empty pattern matches nothing")
    check(all_names("W:\\*", req=0x01) == {"fw.img"}, "W: files are read-only")
    check(all_names("Z:\\*", req=0x10) >= {"F", "NVRAM"}, "search in the drive root")
    check(all_names("Z:\\NVRAM\\NVD_IMEI\\*") == {"MP0B_001"}, "ro/-only directory")
    # name length cases on rwonly.txt (10 bytes)
    st = visible(root, "F/rwonly.txt")
    for maxlen, fits in ((64, 1), (11, 1), (10, 0), (2, 0), (1, 0), (0, 0)):
        x = first("Z:\\F\\rwonly.txt", maxlen=maxlen)
        check(x.raw == stream(1, find_rec(0, fits, st), name16("rwonly.txt", maxlen)),
              "max name length %d" % maxlen)
    check(len(first("Z:\\F\\rwonly.txt", maxlen=1).args[2]) == 2 and
          len(first("Z:\\F\\rwonly.txt", maxlen=0).args[2]) == 0, "name argument lengths")
    # FindNext with its own name length
    rr = run(root, ff("Z:\\F\\*.txt", maxlen=0) + fn(1, 4))
    nm = rr[1].args[2]
    check(rr[1].ret == 0 and len(nm) == 8 and nm[-2:] == b"\0\0", "FindNext uses its own maximum")
    # errors: ret, empty record, empty name
    errs = [
        ("Z:\\NOPE\\*", -9), ("Z:\\F\\A.txt\\*", -18), ("Z:\\NOPE\\X\\*", -9), ("C:\\x\\*", -19),
        ("K:\\*", -19), ("Z:*", -19), ("*", -19), ("\\*", -19), ("Z:\\..\\*", -16),
        ("Z:\\F\\..\\*", -16), ("Z:\\" + "a" * 256, -49), ("Z:\\F\\" + "a" * 255, -6),
        ("Z:\\" + "a\\" * 130 + "*", -49), ("Z:\\link\\*", -16), ("T:\\custom\\*.mcfopota", -9),
        ("T:\\*", -9), ("Q:\\*", -9), ("Z:\\F\\caf\u00e9", -3), ("W:\\nope\\*", -9),
    ]
    rr = run(root, sum([ff(p) for p, _ in errs], []))
    for (p, want_r), x in zip(errs, rr):
        check(x.raw == stream(want_r, NO_REC, b""), "FindFirst %r -> %d (got %d)" % (p[:30], want_r, x.ret))
    # handle table shared with Open/Close
    rr = run(root, ff("Z:\\F\\*.txt") + req(OPEN, IMEI, O_RDONLY) + fn(2) + req(READ, 1, 4) +
             req(SEEK, 1, 0, 0) + req(GET_FILE_SIZE, 1) + req(CLOSE, 1) + fn(1) +
             req(FIND_CLOSE, 2) + req(READ, 2, 4) + req(FIND_CLOSE, 9) + req(FIND_CLOSE) +
             req(FIND_NEXT, 1) + req(FIND_NEXT, 1, b"\1\0") + ff("Z:\\F\\*") + req(OPEN, IMEI, O_RDONLY))
    check([x.ret for x in rr] == [1, 2, -10, -10, -10, -10, 0, -10, 0, -10, -10, -2, -2, -2, 1, 2],
          "search handles share the Open table: %r" % [x.ret for x in rr])
    check(rr[2].raw == stream(-10, NO_REC, b"") and rr[12].raw == stream(-2, NO_REC, b""),
          "FindNext error shape")
    # handle exhaustion: a match without a free handle is -5, no match stays -6
    rr = run(root, sum([req(OPEN, IMEI, O_RDONLY) for _ in range(128)], []) +
             ff("Z:\\F\\*") + ff("Z:\\F\\nothing") + req(CLOSE, 5) + ff("Z:\\F\\*"))
    check(rr[128].raw == stream(-5, NO_REC, b"") and rr[129].ret == -6 and rr[131].ret == 5,
          "no free handle -5")
    # missing / short arguments
    rr = run(root, req(FIND_FIRST, path("Z:\\F\\*"), b"\0", b"\0") +
             req(FIND_FIRST, path("Z:\\F\\*"), b"", b"\0", 64) +
             req(FIND_FIRST, path("Z:\\F\\*"), b"\0", b"\0", b"\x40\0") +
             req(FIND_FIRST, path("Z:\\F\\*.mcf"), b"\0\xff", b"\0\xff", 64, 7) +
             req(FIND_FIRST, path("Z:\\F\\*.mcf"), u32(0), u32(0), u32(64) + u32(0)))
    check([x.raw for x in rr[:3]] == [stream(-2, NO_REC, b"")] * 3, "FindFirst missing/short arguments -2")
    check(rr[3].ret == 1 and rr[4].ret == 2, "only the first byte / 4 bytes count; extra argument ignored")
    # the modem's real request shape: lens 38+, 1, 1, 4
    x = run(root, req(FIND_FIRST, path("Z:\\NVRAM\\NVD_IMEI\\*"), b"\0", b"\0", 0x100))[0]
    check(len(path("Z:\\NVRAM\\NVD_IMEI\\*")) == 40 and x.ret == 1 and
          x.args[2] == "MP0B_001".encode("utf-16-le") + b"\0\0", "modem request shape")
    # the merged view follows changes made through the server
    rr = run(root, req(DELETE, path("Z:\\F\\B.TXT")) + req(OPEN, path("Z:\\F\\new.txt"), O_CREATE) +
             req(CLOSE, 1) + ff("Z:\\F\\*.*") + sum([fn(1) for _ in range(10)], []))
    got = set(listing(rr, 3)[0]) - {".", ".."}
    check(got == {"A.txt", "c.mcf", "rwonly.txt", ".hidden", "new.txt"}, "listing after delete/create: %r" % got)
    j = journal(root)
    check('FIND slot=0 path="T:\\custom\\*.mcfopota" attr=0x0/0x0 max=64 -> -9' in j, "FindFirst journaled")
    check("BADPARAM op=0x1012 (FindFirst) slot=0 nargs=3" in j, "FindFirst BADPARAM journaled")
    check("UNIMPL op=0x101" not in j, "find ops implemented")
    # local time zone: TZ two hours east of UTC
    os.environ["TZ"] = "UTC-2"
    x = run(root, ff("Z:\\F\\c.mcf"))[0]
    check(x.args[1] == find_rec(0, 1, visible(root, "F/c.mcf"), 7200), "DOS time in the local time zone")
    os.environ["TZ"] = "UTC0"


def test_get_drive(root):
    cases = [
        ((4, 1, 0), 0x5A), ((4, 2, 0), 0x5A), ((4, 1, 1), 0x5A), ((4, 1, 2), 0x5A), ((4, 1, 4), 0x5A),
        ((4, 2, 0x100), 0x5A), ((4, 1, 3), -2), ((4, 1, 5), -2), ((4, 1, 6), -2), ((4, 0, 0), -2),
        ((4, 3, 0), -2), ((8, 1, 0), -2), ((8, 2, 0), -2), ((0x10, 1, 0), -2), ((0x10, 2, 0), -2),
        ((0x0C, 1, 0), -2), ((0x1C, 2, 0), -2), ((0x20, 1, 0), -2), ((0, 1, 0), -2), ((5, 1, 0), -2),
        ((0x14, 1, 0), -2), ((0xFFFFFFFF, 1, 0), -2),
    ]
    fr = sum([req(GET_DRIVE, *a) for a, _ in cases], [])
    fr += req(GET_DRIVE, 4, 1) + req(GET_DRIVE, 4, 1, b"\0\0") + req(GET_DRIVE, 4, 1, 0, 9)
    fr += req(GET_DRIVE, u32(4) + u32(7), 1, 0)
    r = run(root, fr)
    for (a, want), x in zip(cases, r):
        check(x.raw == stream(want), "GetDrive %r -> %d (got %d)" % (a, want, x.ret))
    n = len(cases)
    check([x.raw for x in r[n:]] == [stream(-2), stream(-2), stream(0x5A), stream(0x5A)],
          "GetDrive missing/short/extra arguments")
    j = journal(root)
    check(j.count("BADPARAM op=0x101a (GetDrive)") == 2, "GetDrive BADPARAM only for missing arguments")
    check("EXTRAARGS op=0x101a (GetDrive) slot=0 nargs=4" in j, "GetDrive extras journaled")


def test_absent_drives(root):
    fr = []
    fr += req(OPEN, path("T:\\custom\\a.mcfopota"), O_RDONLY)          # 0
    fr += req(OPEN, path("T:\\a.mcfota"), 0)                           # 1
    fr += req(GET_ATTR, path("T:\\a")) + req(GET_ATTR, path("T:\\"))   # 2, 3
    fr += req(GET_DETAIL, path("T:\\a"))                               # 4
    fr += req(OPEN, path("T:\\a"), O_CREATE)                           # 5
    fr += req(CREATE_DIR, path("T:\\d")) + req(DELETE, path("T:\\a"))  # 6, 7
    fr += req(OPEN, path("Q:\\a"), 0) + req(GET_ATTR, path("Q:\\x\\y"))  # 8, 9
    fr += cread("T:\\custom\\a.mcfrawota", mask=0x1F, size=3)          # 10
    fr += req(OPEN, path("K:\\x"), 0) + req(CREATE_DIR, path("K:\\x"))  # 11, 12
    fr += req(DELETE, path("K:\\x")) + req(RENAME, path("K:\\x"), path("K:\\y"))  # 13, 14
    fr += req(GET_ATTR, path("k:\\x")) + req(GET_DETAIL, path("K:\\x"))  # 15, 16
    fr += mv("K:\\x", "Z:\\y", 1) + req(OPEN, path("A:\\x"), 0)        # 17, 18
    r = run(root, fr)
    expect(r, [
        (0, stream(-9), "T: open -9"), (1, stream(-9), "T: open -9"),
        (2, stream(-9), "T: attributes -9"), (3, stream(-9), "T: root -9"),
        (4, stream(-9, b"\0" * 24), "T: detail -9"), (5, stream(-30), "T: create refused"),
        (6, stream(-30), "T: mkdir refused"), (7, stream(-9), "T: delete -9"),
        (8, stream(-9), "Q: open -9"), (9, stream(-9), "Q: missing directory -9"),
        (10, r_read(0, -9, 3), "T: combined read -9"),
        (11, stream(-19), "unknown drive Open -19"), (12, stream(-19), "unknown drive CreateDir -19"),
        (13, stream(-19), "unknown drive Delete -19"), (14, stream(-19), "unknown drive Rename -19"),
        (15, stream(-19), "unknown drive GetAttributes -19"),
        (16, stream(-19, b"\0" * 24), "unknown drive GetFileDetail -19"),
        (17, stream(-19), "unknown drive Move -19"), (18, stream(-19), "A: -19"),
    ])
    check(not any(x.ret == -4 for x in r if len(x.args[0]) == 4), "-4 never sent")
    # with a sandbox copy of T: the files are served read-only
    os.makedirs(os.path.join(root, "ro/mdota/custom"))
    with open(os.path.join(root, "ro/mdota/custom/a.mcfopota"), "wb") as f:
        f.write(b"ota")
    r = run(root, req(OPEN, path("T:\\custom\\a.mcfopota"), 0) + req(READ, 1, 8) +
            req(WRITE, 1, b"x", 1) + req(GET_ATTR, path("T:\\custom\\a.mcfopota")) +
            ff("T:\\custom\\*.mcfopota"))
    check(r[0].ret == 1 and r[1].args[2] == b"ota" and r[2].ret == -30 and r[3].ret == 0x01 and
          r[4].ret == 2 and r[4].args[1][0x0B] == 0x01, "T: from ro/mdota, read-only")
    check(not os.path.exists(os.path.join(root, "rw/mdota")), "no rw/ for T:")


def main():
    global BIN, TMP
    BIN = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("t2-ccci-fsd")
    TMP = os.environ.get("TMPDIR") or "/tmp"
    tests = [test_basic, test_copy_up_whiteout, test_multi_fragment, test_slots, test_malformed,
             test_escapes, test_drives, test_fixed_ops, test_handles, test_replay_file,
             test_combined_read, test_combined_write, test_combined_read_ex, test_move,
             test_sync_close, test_single_ops, test_attributes, test_file_detail,
             test_find, test_get_drive, test_absent_drives]
    for t in tests:
        root = new_root()
        before = failures
        try:
            t(root)
        except Exception as e:  # report and go on
            check(False, "%s raised %r" % (t.__name__, e))
        print("%-24s %s" % (t.__name__, "ok" if failures == before else "FAILED"))
        shutil.rmtree(root)
    print("%d checks, %d failures" % (checks, failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
