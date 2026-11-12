#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Host test for t2-speechd: runs the offline replay (-R/-o) with scripted
# modem packets, modem state and clock, and checks every message the daemon
# writes byte by byte against the protocol description,
# the order of the ALSA actions, the ack bookkeeping and the timeouts. The
# last tests run the real daemon on /dev/null and talk to it through the
# control socket with the client mode.
# Run with: python3 -I tests/test_replay.py path/to/t2-speechd

# Only os, sys, struct, shutil, socket, signal, time (see ccci-fsd/tests).
import os
import shutil
import signal
import socket
import struct
import sys
import time

BIN = None
TMP = None
failures = 0
checks = 0
_tmp_seq = 0

STOCK_COMMON = [0x0000, 0xDABD, 0x7918, 0x2A00, 0x8001] + [0] * 7
RATE_ENUM = {8000: 0, 16000: 1, 32000: 2, 48000: 3}
DL_ATTEN = [22, 19, 16, 13, 10, 7, 4]

EARPIECE_ON = [
    "RCV Mux=Voice Playback", "Handset Volume=0", "Earpiece Switch=on",
]
SPEAKER_ON = ["Speaker Volume=512", "Speaker Switch=on"]
MIC_ON = [
    "UL_SRC_MUX=AMIC", "MISO0_MUX=UL1_CH1", "MISO1_MUX=UL1_CH2",
    "PGA_L_Mux=AIN0", "ADC_L_Mux=Left Preamplifier", "PGA_R_Mux=AIN2",
    "ADC_R_Mux=Right Preamplifier", "PGA1 Volume=2", "PGA2 Volume=2",
    "Main Mic Switch=on", "Second Mic Switch=on",
]
UL_ON = ["PCM_1_PB_CH1 ADDA_UL_CH1=1", "PCM_1_PB_CH2 ADDA_UL_CH2=1"]
DL_EAR_ON = ["ADDA_DL_CH1 PCM_1_CAP_CH1=1", "ADDA_DL_CH2 PCM_1_CAP_CH1=1"]
DL_SPK_ON = ["I2SOUT4_CH1 PCM_1_CAP_CH1=1", "I2SOUT4_CH2 PCM_1_CAP_CH1=1"]


def mkdtemp(prefix):
    global _tmp_seq
    _tmp_seq += 1
    d = os.path.join(TMP, "%s%d-%d" % (prefix, os.getpid(), _tmp_seq))
    os.mkdir(d, 0o700)
    return d


def spawn(argv, env=None):
    """Run argv; returns (exit code, stderr)."""
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.dup2(w, 2)
            fd = os.open(os.devnull, os.O_WRONLY)
            os.dup2(fd, 1)
            if env is None:
                os.execv(argv[0], argv)
            else:
                os.execve(argv[0], argv, env)
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
        print("  FAIL: " + what)


# --- packets ---

def mbox(mid, p16=0, p32=0):
    """AP -> MD mailbox as the daemon writes it."""
    return struct.pack("<IHHII", 0xFFFFFFFF, p16 & 0xFFFF, mid, 5, p32)


def ap_payload(mid, typ, data):
    n = len(data)
    return struct.pack("<IIIHHHHH", 0, n + 6, 5, n + 6, mid, 0xA2A2, typ,
                       n) + data


def md_mbox(mid, p16=0, p32=0):
    """MD -> AP mailbox; channel 4 as the kernel delivers it."""
    return struct.pack("<IHHII", 0xFFFFFFFF, p16 & 0xFFFF, mid, 4, p32)


def md_record(typ, data, idx=1, total=1, size=None):
    return struct.pack("<HHHHH", 0x1234, typ,
                       len(data) if size is None else size, idx,
                       total) + data


def md_payload(mid, typ, data, idx=1, total=1, length=None, size=None):
    rec = md_record(typ, data, idx, total, size)
    tot = 16 + len(rec)
    return struct.pack("<IIIHH", 0, tot if length is None else length, 4,
                       len(rec), mid) + rec


def ack(mid):
    return md_mbox(mid | 0x8000)


def info(rate=48000, mute=0, ext=0, enh=3, common=STOCK_COMMON, smartpa=1,
         app=0):
    b = bytearray(128)
    b[0] = app
    b[2] = RATE_ENUM[rate]
    struct.pack_into("<H", b, 0x0A, ext)
    b[0x11] = mute
    struct.pack_into("<H", b, 0x12, enh)
    for i, c in enumerate(common):
        struct.pack_into("<H", b, 0x1C + 2 * i, c)
    b[0x54] = smartpa
    return bytes(b)


def speech_on(rate=48000, mute=0, **kw):
    return ap_payload(0x2F20, 0x19, info(rate, mute, **kw))


def alive(head=1, ver=0x0203, size=0x20):
    data = struct.pack("<HH", head, ver) + bytes(range(size - 4))
    return md_payload(0xAFA0, 0x1A, data)


# --- running ---

class Run:
    def __init__(self, rc, err, lines):
        self.rc = rc
        self.err = err
        self.lines = lines

    def tx(self):
        return [(t, b) for k, t, b in self.lines if k == "T"]

    def txb(self):
        return [b for _, b in self.tx()]

    def alsa(self):
        return [(t, a) for k, t, a in self.lines if k == "A"]

    def ctls(self):
        return [a[4:] for _, a in self.alsa() if a.startswith("ctl ")]

    def replies(self):
        return [(t, r) for k, t, r in self.lines if k == "R"]

    def rtext(self):
        return [r for _, r in self.replies()]

    def shm(self):
        return [b for k, _, b in self.lines if k == "S"]

    def kinds(self):
        """T/A/R in order, T as message id, A as text."""
        out = []
        for k, t, v in self.lines:
            if k == "T":
                out.append(("T", msg_id(v)))
            elif k == "A":
                out.append(("A", v))
        return out


def msg_id(b):
    return struct.unpack_from("<H", b, 6 if b[:4] == b"\xff" * 4 else 14)[0]


def run(td, script, extra=(), name="s", codec=True):
    """codec=True runs with -r 1 (the daemon sets the codec too, the
    behaviour before 2026-10-29, which the byte-exact ALSA checks below
    describe); codec=False is the default -r 0 (PCM 1 switches only)."""
    sp = os.path.join(td, name + ".txt")
    op = os.path.join(td, name + ".out")
    with open(sp, "w") as f:
        for item in script:
            if isinstance(item, bytes):
                f.write("rx " + item.hex() + "\n")
            else:
                f.write(item + "\n")
    rc, err = spawn([BIN] + (["-r", "1"] if codec else []) + list(extra) +
                    ["-R", sp, "-o", op])
    lines = []
    if os.path.exists(op):
        with open(op) as f:
            for ln in f:
                ln = ln.rstrip("\n")
                k = ln[:1]
                if k == "T":
                    _, t, h = ln.split(" ", 2)
                    lines.append(("T", int(t), bytes.fromhex(h)))
                elif k in "AR":
                    _, t, rest = (ln.split(" ", 2) + [""])[:3]
                    lines.append((k, int(t), rest))
                elif k == "S":
                    lines.append(("S", 0, bytes.fromhex(ln[2:])))
    if rc:
        print("  (rc %d) %s" % (rc, err.decode(errors="replace")[-400:]))
    return Run(rc, err, lines)


def start_ack(rate=48000, device=None):
    cmd = "cmd call-start --rate %d" % rate
    if device:
        cmd += " --device " + device
    return [cmd, "time 1", ack(0x2F20)]


def ok(r, what):
    check(r.rc == 0, what + ": exit code 0")
    check(not any(x.startswith("error") for x in r.rtext()),
          what + ": no error reply " + repr(r.rtext()))


# --- tests ---

def test_speech_on_info(td):
    for dev, ctl_out, ctl_dl in (("earpiece", EARPIECE_ON, DL_EAR_ON),
                                 ("speaker", SPEAKER_ON, DL_SPK_ON)):
        for rate in (48000, 16000, 32000):
            r = run(td, start_ack(rate, dev))
            ok(r, "%s %d" % (dev, rate))
            tx = r.txb()
            exp = speech_on(rate)
            check(len(tx) >= 1 and tx[0] == exp,
                  "%s %d: speech on byte-exact\n   got %s\n   exp %s" %
                  (dev, rate, tx[0].hex() if tx else None, exp.hex()))
            check(len(exp) == 150, "speech on is 22 + 128 bytes")
            # AP path first, PCM open, then speech on
            check(r.ctls() == ctl_out + MIC_ON + UL_ON + ctl_dl,
                  "%s: control order %r" % (dev, r.ctls()))
            k = r.kinds()
            pi = k.index(("A", "pcm open %d" % rate))
            ti = k.index(("T", 0x2F20))
            check(pi < ti and all(x[0] == "A" for x in k[:pi]),
                  "%s %d: controls, PCM, then speech on" % (dev, rate))
            check("speech-on: acked" in r.rtext(), "speech on acked reply")
    # 8 kHz earpiece works, speaker refuses (FS199x PLL >= 16 kHz)
    r = run(td, start_ack(8000))
    check(r.txb()[:1] == [speech_on(8000)], "earpiece 8 kHz speech on")
    r = run(td, ["cmd call-start --rate 8000 --device speaker"])
    check(r.rtext()[-1].startswith("error") and not r.tx() and not r.alsa(),
          "speaker at 8 kHz refused, nothing done")
    r = run(td, ["cmd call-start --rate 44100"])
    check(r.rtext()[-1].startswith("error") and not r.tx(), "bad rate refused")
    # default rate 48 kHz; -f changes it; -A/-C change fields 0x54 / 0x1C
    r = run(td, ["cmd call-start"])
    check(r.txb()[:1] == [speech_on(48000)], "default rate 48000")
    r = run(td, ["cmd call-start"], ["-f", "16000", "-A", "0", "-C"])
    check(r.txb()[:1] == [speech_on(16000, smartpa=0, common=[0] * 12)],
          "-f 16000 -A 0 -C")
    # mute state goes into field 0x11
    r = run(td, ["cmd mute on", "cmd call-start"])
    check(r.txb()[:1] == [speech_on(48000, mute=1)], "UL mute bit in block")
    check(r.txb()[0][22 + 0x11] == 1 and r.txb()[0][22 + 0x05] == 0,
          "mute info 1, parameter status 0 (modem defaults)")
    # -H '' leaves Handset Volume alone
    r = run(td, ["cmd call-start"], ["-H", ""])
    check("Handset Volume=0" not in r.ctls() and r.ctls()[0] == "RCV Mux=Voice Playback",
          "-H '' skips Handset Volume")
    r = run(td, ["cmd call-start"], ["-H", "6"])
    check("Handset Volume=6" in r.ctls(), "-H 6")


def test_gains(td):
    for vol in range(7):
        r = run(td, ["cmd volume %d" % vol] + start_ack())
        tx = r.txb()
        check(tx[1:] == [mbox(0x2F00, -DL_ATTEN[vol]), mbox(0x2F01, 92)],
              "volume %d: DL %d dB, UL 23 dB x4: %r" %
              (vol, -DL_ATTEN[vol], [b.hex() for b in tx[1:]]))
    check(mbox(0x2F00, -22)[4:6] == b"\xea\xff", "-22 dB is 0xFFEA")
    # the default volume index is 4 (-10 dB)
    r = run(td, start_ack())
    check(r.txb()[1] == mbox(0x2F00, -10), "default volume 4")
    # in a call: 0x2F00 only when the value changes, no 0x2F01
    r = run(td, start_ack() + ["cmd volume 4", "cmd volume 5", "cmd volume 5",
                               "cmd volume 0"])
    check(r.txb()[3:] == [mbox(0x2F00, -7), mbox(0x2F00, -22)],
          "volume changes in call: %r" % [b.hex() for b in r.txb()[3:]])
    check(not any(msg_id(b) in (0x2F7B,) for b in r.txb()),
          "no 0x2F7B without a parameter blob")
    # outside a call: nothing sent
    r = run(td, ["cmd volume 2"])
    check(not r.tx() and r.rtext() == ["ok"], "volume outside a call")
    for bad in ("7", "-1", "x", ""):
        r = run(td, ["cmd volume " + bad])
        check(r.rtext()[-1].startswith("error"), "volume %r refused" % bad)
    # gains wait for the speech-on ack (one A1 outstanding, FIFO)
    r = run(td, ["cmd call-start", "time 30", ack(0x2F20)])
    t = r.tx()
    check([x[0] for x in t] == [0, 30, 30], "gains after the ack: %r" %
          [x[0] for x in t])


def test_mute(td):
    r = run(td, start_ack() + ["cmd mute on", "time 3", ack(0x2F02),
                               "cmd mute off", ack(0x2F02)])
    tx = r.txb()
    check(tx[3:] == [mbox(0x2F02, 1), mbox(0x2F02, 0)], "mute on/off: %r" %
          [b.hex() for b in tx[3:]])
    check(r.rtext().count("mute: acked") == 2, "mute acked replies")
    # mute outside a call is only remembered
    r = run(td, ["cmd mute on"])
    check(not r.tx(), "mute outside a call: nothing sent")
    r = run(td, ["cmd mute maybe"])
    check(r.rtext()[-1].startswith("error"), "mute bad arg")


def test_speech_off(td):
    r = run(td, start_ack() + ["time 10", "cmd call-stop", ack(0x2F02),
                               "time 2", ack(0x2F21), "time 1", ack(0x2F02)])
    ok(r, "call-stop")
    tx = r.txb()[3:]
    check(tx == [mbox(0x2F02, 1), mbox(0x2F21), mbox(0x2F02, 0)],
          "UL mute, speech off, UL mute restore: %r" % [b.hex() for b in tx])
    check(mbox(0x2F21) == bytes.fromhex("ffffffff0000212f0500000000000000"),
          "speech off mailbox bytes")
    k = r.kinds()
    i_mute = k.index(("T", 0x2F02))
    i_close = k.index(("A", "pcm close"))
    i_off = k.index(("T", 0x2F21))
    check(i_mute < i_close < i_off, "mute, PCM closed, paths off, then 0x2F21")
    ctl_off = [c for c in r.ctls() if "Volume" not in c and
               (c.endswith("=0") or c.endswith("=off") or c.endswith("=Open"))]
    check(ctl_off == ["ADDA_DL_CH2 PCM_1_CAP_CH1=0", "ADDA_DL_CH1 PCM_1_CAP_CH1=0",
                      "PCM_1_PB_CH2 ADDA_UL_CH2=0", "PCM_1_PB_CH1 ADDA_UL_CH1=0",
                      "Second Mic Switch=off", "Main Mic Switch=off",
                      "Earpiece Switch=off", "RCV Mux=Open"],
          "path off in reverse order: %r" % ctl_off)
    check("speech-off: acked" in r.rtext(), "speech off acked")
    # muted before the call: restored to 1
    r = run(td, ["cmd mute on"] + start_ack() + ["cmd call-stop", ack(0x2F02),
                                                 ack(0x2F21), ack(0x2F02)])
    check(r.txb()[-3:] == [mbox(0x2F02, 1), mbox(0x2F21), mbox(0x2F02, 1)],
          "mute restored to on")
    # speech off without a call: still sent, paths of both devices off
    r = run(td, ["cmd call-stop", ack(0x2F02), ack(0x2F21), ack(0x2F02)])
    check([msg_id(b) for b in r.txb()] == [0x2F02, 0x2F21, 0x2F02],
          "unpaired call-stop still sends 0x2F21")
    check("warning: no call was active" in r.rtext(), "unpaired warning")
    check("I2SOUT4_CH1 PCM_1_CAP_CH1=0" in r.ctls() and
          "ADDA_DL_CH1 PCM_1_CAP_CH1=0" in r.ctls(), "both outputs off")
    # a second call after the first: gains sent again
    r = run(td, start_ack() + ["cmd call-stop", ack(0x2F02), ack(0x2F21),
                               ack(0x2F02)] + start_ack())
    ids = [msg_id(b) for b in r.txb()]
    check(ids == [0x2F20, 0x2F00, 0x2F01, 0x2F02, 0x2F21, 0x2F02, 0x2F20,
                  0x2F00, 0x2F01], "second call: %r" % [hex(i) for i in ids])


def test_acks(td):
    # an ack not in pair is ignored; the caller gives up after 500 ms
    r = run(td, ["cmd call-start", "time 10", ack(0x2F21), "time 1000",
                 ack(0x2F20)])
    rp = r.replies()
    check(("speech-on: pending" in r.rtext()) and rp[0][0] == 500,
          "unpaired ack ignored, caller blocked 500 ms: %r" % rp)
    check(b"not in pair" in r.err, "not in pair logged")
    t = r.tx()
    check([x[0] for x in t] == [0, 1010, 1010],
          "gains queued until the real ack at 1010: %r" % [x[0] for x in t])
    # an ack with nothing outstanding
    r = run(td, [ack(0x2F20)])
    check(not r.tx() and b"nothing outstanding" in r.err,
          "stray ack ignored")
    # a payload-form MD message with id AFxx of an A1 still counts as ack
    r = run(td, ["cmd call-start", md_payload(0xAF20, 0, b"")])
    check("speech-on: acked" in r.rtext(), "payload-form ack accepted")
    # high byte must be 0xAF: 0x2F20 echoed back is not an ack
    r = run(td, ["cmd call-start", md_mbox(0x2F20)])
    check("speech-on: pending" in r.rtext(), "0x2F20 echo is not an ack")
    # status shows the pending id
    r = run(td, ["cmd call-start", "cmd status"])
    check(any("pending=0x2f20" in x for x in r.rtext()), "status pending id")
    # mute: 50 ms caller block
    r = run(td, start_ack() + ["time 5", "cmd mute on", "cmd status"])
    rp = r.replies()
    mt = [t for t, x in rp if x.startswith("mute:")]
    check(mt == [56] and "mute: pending" in r.rtext(),
          "mute caller blocked 50 ms: %r" % rp)
    # device switch: routing 300 ms
    # device switch without acks: 0x2F03 stays outstanding (60 s), the rest
    # waits in the FIFO; the command returns after 50 + 50 + 300 + 50 + 50 ms
    r = run(td, start_ack() + ["time 9", "cmd device speaker"])
    check([msg_id(b) for b in r.txb()][3:] == [0x2F03] and
          ("routing: pending" in r.rtext()) and r.replies()[-1][0] == 510,
          "0x2F2B queued behind the unacked 0x2F03: %r" % r.replies())


def test_timeouts(td):
    # queue worker: 60 s, then a local ack and the next message goes out
    r = run(td, ["cmd call-start", "time 70000"])
    t = r.tx()
    check([(x[0], msg_id(x[1])) for x in t] ==
          [(0, 0x2F20), (60000, 0x2F00), (60000, 0x2F01)],
          "speech on times out at 60 s: %r" % [(x[0], hex(msg_id(x[1]))) for x in t])
    check(b"no ack in 60000 ms" in r.err and b"synthesised" in r.err,
          "timeout logged, ack synthesised")
    # the synthesised ack sets the speech bit
    r = run(td, ["cmd call-start", "time 70000", "cmd status"])
    check(any("md_status=0x1" in x for x in r.rtext()), "speech bit set by local ack")
    # a late ack after the timeout is not in pair any more
    r = run(td, ["cmd call-start", "time 60000", ack(0x2F20)], ["-T", "1000"])
    check(b"nothing outstanding" in r.err, "late ack dropped (-T 1000)")
    # -T shortens the worker wait
    r = run(td, ["cmd call-start", "time 5000"], ["-T", "1500"])
    check([x[0] for x in r.tx()] == [0, 1500, 1500], "-T 1500")
    # caller block times: speech on 500, speech off 500, mute 50, routing 300
    r = run(td, start_ack() + ["cmd call-stop", "time 5000"])
    rp = r.replies()
    # call-stop: 0x2F02 blocks 50 ms; 0x2F21 queued behind, caller 500 ms
    # from its own enqueue -> returns at 1 + 50 + 500 + 50
    check(rp[-1][0] == 601, "call-stop with no acks returns at 601: %r" % rp[-1:])
    r = run(td, start_ack() + ["cmd device speaker", ack(0x2F03), ack(0x2F02),
                               "time 1000", ack(0x2F2B), ack(0x2F03),
                               ack(0x2F02)])
    rt = [(t, x) for t, x in r.replies() if x.startswith("routing")]
    # reply lines carry the time the command returned: 1 + 300 + 50 + 50
    check(rt == [(401, "routing: pending")], "routing caller block 300 ms: %r" % rt)
    t = [(x[0], msg_id(x[1])) for x in r.tx()][3:]
    check(t == [(1, 0x2F03), (1, 0x2F02), (1, 0x2F2B), (1001, 0x2F03),
                (1001, 0x2F02)], "routing ack at 1001 releases the queue: %r" % t)


def test_device_switch(td):
    r = run(td, start_ack() + ["cmd device speaker", ack(0x2F03), ack(0x2F02),
                               ack(0x2F2B), ack(0x2F03), ack(0x2F02)])
    ok(r, "device speaker")
    tx = r.txb()[3:]
    exp = [mbox(0x2F03, 1), mbox(0x2F02, 1),
           ap_payload(0x2F2B, 0x19, info(48000, mute=3)),
           mbox(0x2F03, 0), mbox(0x2F02, 0)]
    check(tx == exp, "switch sequence: %r" % [b.hex() for b in tx])
    k = r.kinds()
    i2 = [i for i, x in enumerate(k) if x == ("T", 0x2F02)][0]
    i2b = k.index(("T", 0x2F2B))
    sw = [x[1] for x in k[i2 + 1:i2b]]
    check(sw == ["ctl ADDA_DL_CH2 PCM_1_CAP_CH1=0", "ctl ADDA_DL_CH1 PCM_1_CAP_CH1=0",
                 "ctl Earpiece Switch=off", "ctl RCV Mux=Open",
                 "ctl Speaker Volume=512", "ctl Speaker Switch=on",
                 "ctl I2SOUT4_CH1 PCM_1_CAP_CH1=1", "ctl I2SOUT4_CH2 PCM_1_CAP_CH1=1"],
          "output switched between the mutes and 0x2F2B: %r" % sw)
    check(("A", "pcm close") not in k, "PCM stays open")
    # with the user mute on, UL stays muted afterwards
    r = run(td, ["cmd mute on"] + start_ack() +
            ["cmd device speaker", ack(0x2F03), ack(0x2F02), ack(0x2F2B),
             ack(0x2F03), ack(0x2F02)])
    check(r.txb()[-1] == mbox(0x2F02, 1), "UL mute kept")
    # 16 kHz call: switch to speaker ok; 8 kHz refused
    r = run(td, start_ack(8000) + ["cmd device speaker"])
    check(r.rtext()[-1].startswith("error") and len(r.tx()) == 3,
          "speaker switch at 8 kHz refused")
    # same device: nothing
    r = run(td, start_ack() + ["cmd device earpiece"])
    check(len(r.tx()) == 3, "same device: nothing sent")
    # outside a call: the next call uses it
    r = run(td, ["cmd device speaker"] + start_ack())
    check("Speaker Switch=on" in r.ctls() and r.txb()[0] == speech_on(),
          "device remembered for the next call")
    # a required control failing: switched back, error
    r = run(td, start_ack() + ["ctlfail I2SOUT4_CH1 PCM_1_CAP_CH1",
                               "cmd device speaker", ack(0x2F03), ack(0x2F02),
                               ack(0x2F03), ack(0x2F02)])
    check(r.rtext()[-1].startswith("error") and
          0x2F2B not in [msg_id(b) for b in r.txb()] and
          r.ctls()[-1] == "ADDA_DL_CH2 PCM_1_CAP_CH1=1",
          "failed switch reverts, no 0x2F2B")
    r = run(td, ["cmd device bluetooth"])
    check(r.rtext()[-1].startswith("error"), "bad device")


def test_notifications(td):
    # MD alive, direct payload: answered 0x2FA0
    r = run(td, [alive(), "cmd status"])
    check(r.txb() == [mbox(0x2FA0)], "alive answered")
    check(any("md_version=0x1/0x203" in x for x in r.rtext()), "versions kept")
    # wrong size / type / fragment: no answer
    r = run(td, [alive(size=0x1C)])
    check(not r.tx(), "alive of wrong size not answered")
    r = run(td, [md_payload(0xAFA0, 0x14, bytes(0x20))])
    check(not r.tx(), "alive of wrong type not answered")
    r = run(td, [md_payload(0xAFA0, 0x1A, bytes(0x20), idx=1, total=2)])
    check(not r.tx(), "alive in two fragments not answered")
    # EPOF: 0x2F78, then everything dropped until alive
    r = run(td, start_ack() + [md_mbox(0xAF78), "cmd volume 6", "cmd mute on",
                               md_mbox(0xAF75, 0x8010)])
    tx = r.txb()
    check(tx[3:] == [mbox(0x2F78)], "EPOF answered, then nothing: %r" %
          [b.hex() for b in tx[3:]])
    check(("A", "pcm close") in r.kinds(), "Voice Call closed on EPOF")
    # codec info: 0x2F90, strings logged
    ci = b"AMR-WB\0".ljust(0x5C, b"\0") + b"HD on\0".ljust(0xB8 - 0x5C, b"\0")
    r = run(td, [md_payload(0xAF90, 0x14, ci), "cmd status"])
    check(r.txb() == [mbox(0x2F90)], "codec info answered")
    check(any("codec_info=AMR-WB hd_voice=HD on" in x for x in r.rtext()),
          "codec strings: %r" % r.rtext())
    # band info: no answer, decoded both ways
    for p16, band in ((0x8010, "wb"), (0x8030, "fb"), (0x0008, "wb"),
                      (0x0010, "swb"), (0x0018, "fb"), (0x0020, "nb"),
                      (0x8000, "nb")):
        r = run(td, [md_mbox(0xAF75, p16), "cmd status"])
        check(not r.tx() and any(("band=%s " % band) in x for x in r.rtext()),
              "band %#x -> %s" % (p16, band))
    # ignored: 0xAF70, 0xAFE4; unknown ids dropped
    r = run(td, [md_mbox(0xAF70), md_mbox(0xAFE4, 1), md_mbox(0xAF11),
                 md_mbox(0x1234)])
    check(not r.tx(), "AF70/AFE4/unknown: nothing sent")
    # eCall request 0xAFB5 is answered 0x2FB5; data 0xAF64 -> 0x2F64
    r = run(td, [md_mbox(0xAFB5), md_mbox(0xAF64)])
    check(r.txb() == [mbox(0x2FB5), mbox(0x2F64)], "AFB5 / AF64 answered")
    # answers bypass the queue: alive answered while 0x2F20 waits
    r = run(td, ["cmd call-start", alive(), ack(0x2F20)])
    ids = [msg_id(b) for b in r.txb()]
    check(ids[:2] == [0x2F20, 0x2FA0], "0x2FA0 not queued behind 0x2F20")


def test_shm(td):
    r = run(td, ["shm-dump 0 0x80", "shm-dump 0xCFE0 32"])
    hdr, tail = r.shm()
    exp = bytearray(0x80)
    exp[0:32] = b"\x0a" * 32
    struct.pack_into("<I", exp, 0x20, 1)
    struct.pack_into("<IIII", exp, 0x28, 0x80, 0x3000, 0, 0)
    struct.pack_into("<IIII", exp, 0x38, 0x3080, 0x2000, 0, 0)
    struct.pack_into("<IIII", exp, 0x48, 0x5080, 0x7F60, 0, 0)
    struct.pack_into("<I", exp, 0x7C, 0x7C)
    check(hdr == bytes(exp), "sph_shm header: %s" % hdr.hex())
    check(tail == b"\x0a" * 32, "tail guard")
    # indirect alive through the md_data ring, wrapping at the end
    rec = md_record(0x1A, struct.pack("<HH", 2, 0x77) + bytes(28))
    idx = 0x7F60 - 6
    put = []
    for i, b in enumerate(rec):
        put.append("shm-put %#x %02x" % (0x5080 + (idx + i) % 0x7F60, b))
    r = run(td, put + ["shm-put 0x50 %s" % struct.pack("<II", idx, 36).hex(),
                       md_mbox(0xAFA0, len(rec), idx), "shm-dump 0x50 4",
                       "cmd status"])
    check(r.txb() == [mbox(0x2FA0)], "indirect alive answered")
    check(r.shm() == [struct.pack("<I", (idx + len(rec)) % 0x7F60)],
          "read index advanced and wrapped")
    check(any("md_version=0x2/0x77" in x for x in r.rtext()), "indirect versions")
    # indirect record with a bad header or length: no answer
    r = run(td, ["shm-put 0x5080 " + md_record(0x1A, bytes(0x20)).hex(),
                 md_mbox(0xAFA0, 20, 0)])
    check(not r.tx(), "indirect length mismatch not answered")
    r = run(td, [md_mbox(0xAFA0, 42, 0x7F60)])
    check(not r.tx(), "indirect index out of range")
    # ring indices reset after the speech-off ack
    r = run(td, ["shm-put 0x30 0100000002000000", "shm-put 0x50 0300000004000000"]
            + start_ack() + ["cmd call-stop", ack(0x2F02), ack(0x2F21),
                             ack(0x2F02), "shm-dump 0x20 0x38"])
    d = r.shm()[0]
    check(d[0:4] == b"\x01\0\0\0" and d[0x10:0x18] == bytes(8) and
          d[0x30:0x38] == bytes(8), "indices reset, AP flag back to 1")
    # modem reading: reset refused, indices kept
    r = run(td, ["shm-put 0x24 02000000", "shm-put 0x30 01000000"] + start_ack() +
            ["cmd call-stop", ack(0x2F02), ack(0x2F21), "shm-dump 0x30 4"])
    check(r.shm() == [b"\x01\0\0\0"] and b"modem is reading" in r.err,
          "reset refused while the modem reads")


def test_modem_reset(td):
    # lost during a call (state file), back with alive: call refreshed
    r = run(td, ["cmd volume 3", "cmd mute on"] + start_ack() +
            ["time 100", "state down", "time 300", "cmd volume 5",
             "time 1000", "state ready", "time 50", alive(), "time 5",
             ack(0x2F20), "time 5", ack(0x2F02)])
    k = r.kinds()
    ic = k.index(("A", "pcm close"))
    check(k[ic - 1] == ("T", 0x2F01), "PCM closed when the modem went away")
    after = k[ic + 1:]
    check(after == [("T", 0x2FA0), ("A", "pcm open 48000"), ("T", 0x2F20),
                    ("T", 0x2F00), ("T", 0x2F01), ("T", 0x2F02)],
          "refresh: answer, PCM, speech on, gains, mute: %r" % after)
    tx = r.tx()
    # volume 5 was set while the modem was away: -7 dB in the refresh
    check(tx[-4][1] == speech_on(48000, mute=1) and
          tx[-3][1] == mbox(0x2F00, -7) and tx[-2][1] == mbox(0x2F01, 92) and
          tx[-1][1] == mbox(0x2F02, 1),
          "refreshed messages carry the current state: %r" %
          [x[1].hex() for x in tx[-4:]])
    check(tx[-4][0] == 1451, "refresh right after the alive at 1451")
    check(b"modem lost: modem not ready during speech" in r.err, "loss logged")
    # messages from the modem while lost are ignored (acks too)
    r = run(td, ["cmd call-start", "state down", "time 10", ack(0x2F20),
                 "cmd status"])
    check(any("lost=1" in x for x in r.rtext()) and
          b"ignored: modem lost" in r.err, "ack ignored while lost")
    check("speech-on: not acked" in r.rtext(), "pending speech on cancelled -> local ack")
    # write ENODEV: lost; outside a call no refresh, alive clears it
    r = run(td, ["txfail 1"] + start_ack() + ["cmd status", alive(),
                                              ack(0x2F20), "cmd status"])
    st = [x for x in r.rtext() if x.startswith("modem=")]
    check("lost=1" in st[0] and "lost=0" in st[1], "ENODEV -> lost, alive -> back")
    ids = [msg_id(b) for b in r.txb()]
    check(ids == [0x2FA0, 0x2F20, 0x2F00, 0x2F01],
          "after ENODEV: only the alive answer and the refresh: %r" %
          [hex(i) for i in ids])
    # no alive for 10 s while the state file says ready: resume
    r = run(td, start_ack() + ["state down", "time 500", "state ready",
                               "time 9000", "cmd status", "time 2000",
                               "cmd status"])
    st = [x for x in r.rtext() if x.startswith("modem=")]
    check("lost=1" in st[0] and "lost=0" in st[1], "resume without alive: %r" % st)
    check([msg_id(b) for b in r.txb()][3:4] == [0x2F20], "refresh after resume")
    # call-stop while lost: AP side down, messages dropped, lost cleared
    r = run(td, start_ack() + ["state down", "time 300", "cmd call-stop",
                               "cmd status"])
    check(len(r.tx()) == 3 and any("lost=0" in x and "call" not in x
                                   for x in r.rtext() if x.startswith("modem=")),
          "call-stop while lost")
    check(any("call=off" in x for x in r.rtext()), "call off")
    # without a state source: ready only after alive (-S none)
    r = run(td, ["cmd call-start"], ["-S", "none"])
    check(not r.tx() and "warning: modem not ready" in r.rtext(),
          "-S none: not ready before alive")
    rp = r.replies()
    check(rp[0][0] == 480, "call-start waited 480 ms for the modem: %r" % rp)
    r = run(td, [alive(), "cmd call-start"], ["-S", "none"])
    check([msg_id(b) for b in r.txb()][:2] == [0x2FA0, 0x2F20], "-S none: alive -> ready")
    r = run(td, ["cmd call-start"], ["-S", "none", "-a"])
    check(r.txb()[:1] == [speech_on()], "-S none -a: ready at once")
    # state down at call start, ready after 100 ms: speech on then
    r = run(td, ["state down", "cmd call-start", "time 100", "state ready",
                 "time 5", ack(0x2F20)])
    t = r.tx()
    check(t and t[0][0] == 100 and msg_id(t[0][1]) == 0x2F20,
          "call-start polls for ready: %r" % [(x[0], hex(msg_id(x[1]))) for x in t])


def test_recovery(td):
    st = os.path.join(td, "state")
    # a previous instance died during a call with speech on pending
    with open(st, "w") as f:
        f.write("pending=0x2f20\nmd_status=0x1\ncall=1\ndevice=speaker\nepof=0\n")
    r = run(td, ["time 2000", ack(0x2F21)], ["-p", st])
    t = r.tx()
    check([(x[0], x[1]) for x in t] == [(1000, mbox(0x2F21))],
          "waits 1 s for the old ack, then speech off: %r" % t)
    check("I2SOUT4_CH1 PCM_1_CAP_CH1=0" in r.ctls(), "old speaker path off")
    with open(st) as f:
        txt = f.read()
    check("pending=0\n" in txt and "md_status=0\n" in txt and "call=0" in txt,
          "state file cleared: %r" % txt)
    # the old ack arrives in time
    with open(st, "w") as f:
        f.write("pending=0x2f21\nmd_status=0x0\ncall=0\n")
    r = run(td, ["time 100", ack(0x2F21)], ["-p", st])
    check(not r.tx(), "old speech-off acked: nothing more to send")
    # codec info dropped during the recovery wait
    with open(st, "w") as f:
        f.write("pending=0x2f20\nmd_status=0x0\ncall=0\n")
    r = run(td, [md_payload(0xAF90, 0x14, bytes(0xB8)), "time 2000"], ["-p", st])
    check(not any(msg_id(b) == 0x2F90 for b in r.txb()),
          "codec info dropped while recovering")
    # nothing persisted: nothing sent at start
    r = run(td, ["time 5000"], ["-p", os.path.join(td, "none")])
    check(not r.tx() and not r.alsa(), "fresh start: nothing sent")
    # the state file follows the call
    p2 = os.path.join(td, "s2")
    r = run(td, ["cmd call-start"], ["-p", p2])
    with open(p2) as f:
        txt = f.read()
    check("pending=0x2f20" in txt and "call=1" in txt, "persisted: %r" % txt)


def test_codec_routes_off(td):
    """-r 0 (default): only the PCM 1 interconnect switches, the codec is
    left to the Voice Call UCM verb (tools/ucm/Titan2/VoiceCall.conf)."""
    for dev, ctl_dl in (("earpiece", DL_EAR_ON), ("speaker", DL_SPK_ON)):
        r = run(td, start_ack(48000, dev), codec=False)
        ok(r, "-r 0 %s" % dev)
        check(r.ctls() == UL_ON + ctl_dl,
              "-r 0 %s: PCM 1 switches only: %r" % (dev, r.ctls()))
        check(r.txb()[:1] == [speech_on()], "-r 0 %s: speech on unchanged" % dev)
    # -H/-K have nothing to set
    r = run(td, start_ack(), ["-H", "6", "-K", "900"], codec=False)
    check(r.ctls() == UL_ON + DL_EAR_ON, "-r 0: -H/-K ignored")
    # call-stop: the switches back, in reverse order, nothing else
    r = run(td, start_ack() + ["cmd call-stop", ack(0x2F02), ack(0x2F21),
                               ack(0x2F02)], codec=False)
    check(r.ctls()[4:] == ["ADDA_DL_CH2 PCM_1_CAP_CH1=0", "ADDA_DL_CH1 PCM_1_CAP_CH1=0",
                           "PCM_1_PB_CH2 ADDA_UL_CH2=0", "PCM_1_PB_CH1 ADDA_UL_CH1=0"],
          "-r 0 call-stop: %r" % r.ctls()[4:])
    check([msg_id(b) for b in r.txb()] == [0x2F20, 0x2F00, 0x2F01, 0x2F02,
                                           0x2F21, 0x2F02], "-r 0: messages unchanged")
    # device switch: only the downlink switches move between the mutes
    r = run(td, start_ack() + ["cmd device speaker", ack(0x2F03), ack(0x2F02),
                               ack(0x2F2B), ack(0x2F03), ack(0x2F02)], codec=False)
    ok(r, "-r 0 device speaker")
    k = r.kinds()
    i2 = [i for i, x in enumerate(k) if x == ("T", 0x2F02)][0]
    i2b = k.index(("T", 0x2F2B))
    check([x[1] for x in k[i2 + 1:i2b]] ==
          ["ctl ADDA_DL_CH2 PCM_1_CAP_CH1=0", "ctl ADDA_DL_CH1 PCM_1_CAP_CH1=0",
           "ctl I2SOUT4_CH1 PCM_1_CAP_CH1=1", "ctl I2SOUT4_CH2 PCM_1_CAP_CH1=1"],
          "-r 0 switch: downlink only: %r" % [x[1] for x in k[i2 + 1:i2b]])
    # a codec control failing cannot matter; a PCM 1 switch failing still does
    r = run(td, ["ctlfail RCV Mux", "ctlfail Main Mic Switch"] + start_ack(),
            codec=False)
    ok(r, "-r 0: codec controls are not written")
    r = run(td, ["ctlfail ADDA_DL_CH1 PCM_1_CAP_CH1", "cmd call-start"], codec=False)
    check(r.rtext()[-1].startswith("error") and not r.tx(), "-r 0: PCM 1 switch required")
    # unpaired call-stop and crash recovery: both outputs' downlink off, no codec
    r = run(td, ["cmd call-stop", ack(0x2F02), ack(0x2F21), ack(0x2F02)], codec=False)
    check(sorted(r.ctls()) == sorted(["ADDA_DL_CH2 PCM_1_CAP_CH1=0", "ADDA_DL_CH1 PCM_1_CAP_CH1=0",
                                      "I2SOUT4_CH2 PCM_1_CAP_CH1=0", "I2SOUT4_CH1 PCM_1_CAP_CH1=0"] +
                                     ["PCM_1_PB_CH2 ADDA_UL_CH2=0", "PCM_1_PB_CH1 ADDA_UL_CH1=0"] * 2),
          "-r 0 unpaired call-stop: %r" % r.ctls())
    st = os.path.join(td, "state0")
    with open(st, "w") as f:
        f.write("pending=0\nmd_status=0x1\ncall=1\ndevice=speaker\nepof=0\n")
    r = run(td, ["time 100", ack(0x2F21)], ["-p", st], codec=False)
    check(r.ctls() == ["I2SOUT4_CH2 PCM_1_CAP_CH1=0", "I2SOUT4_CH1 PCM_1_CAP_CH1=0",
                       "PCM_1_PB_CH2 ADDA_UL_CH2=0", "PCM_1_PB_CH1 ADDA_UL_CH1=0"],
          "-r 0 recovery: %r" % r.ctls())
    # status says which mode
    r = run(td, ["cmd status"], codec=False)
    check(any("codec_routes=0" in x for x in r.rtext()), "status codec_routes=0")
    r = run(td, ["cmd status"])
    check(any("codec_routes=1" in x for x in r.rtext()), "status codec_routes=1")


def test_alsa_errors(td):
    r = run(td, ["pcmfail 1"] + start_ack())
    check(r.txb()[:1] == [speech_on()] and
          [a for _, a in r.alsa() if a.startswith("pcm")] ==
          ["pcm close", "pcm open 48000"], "PCM retried once")
    r = run(td, ["pcmfail 2", "cmd call-start", "cmd status"])
    check(r.rtext()[0].startswith("error") and not r.tx(),
          "PCM failing twice: error, no speech on")
    check(r.ctls()[-1] == "RCV Mux=Open", "path undone after the PCM failure")
    check(any("call=off" in x for x in r.rtext()), "no call")
    r = run(td, ["ctlfail PCM_1_PB_CH1 ADDA_UL_CH1", "cmd call-start"])
    check(r.rtext()[-1].startswith("error") and not r.tx() and
          ("A", "pcm open 48000") not in r.kinds(), "required control fails")
    r = run(td, ["ctlfail PGA1 Volume"] + start_ack())
    ok(r, "optional control may fail")


def test_framing(td):
    bad = [
        b"\xff" * 8,                                   # < 16
        md_payload(0xAF90, 0x14, bytes(0xB8))[:30],    # cut
        b"\0" * 16 + b"\x34\x12",                      # header cut short
        struct.pack("<IIIHH", 0, 26, 4, 10, 0xAFA0) +
        struct.pack("<HHHHH", 0x4321, 0x1A, 0, 1, 1),  # bad sync
        md_payload(0xAFA0, 0x1A, bytes(0x20), size=0xD67),
    ]
    r = run(td, bad)
    check(r.rc == 0 and not r.tx(), "malformed packets dropped")
    # length word mismatch: logged, still handled
    r = run(td, [md_payload(0xAF90, 0x14, bytes(0xB8), length=99)])
    check(r.txb() == [mbox(0x2F90)] and b"length word" in r.err,
          "length mismatch logged, handled")
    # pseudo-random packets: no crash, no sanitizer report
    x = 0x12345678
    pk = []
    for i in range(400):
        n = 16 + (i * 7) % 300
        b = bytearray()
        for _ in range(n):
            x ^= (x << 13) & 0xFFFFFFFF
            x ^= x >> 17
            x ^= (x << 5) & 0xFFFFFFFF
            b.append(x & 0xFF)
        if i % 3 == 0:
            b[0:4] = b"\xff" * 4
            b[7] = 0xAF
        elif i % 3 == 1:
            b[0:4] = bytes(4)
            b[15] = 0xAF
            b[16:18] = b"\x34\x12"
        pk.append(bytes(b))
    r = run(td, ["cmd call-start"] + pk + ["cmd status", "cmd call-stop"])
    check(r.rc == 0 and r.rtext()[-1] == "ok", "fuzz survived")
    # bad commands
    for c in ("", "frobnicate", "call-start --rate", "call-start --device",
              "call-stop now", "status x", "a b c d e f g h i j"):
        r = run(td, ["cmd " + c])
        check(r.rtext() and r.rtext()[-1].startswith("error"), "bad command %r" % c)
    r = run(td, start_ack() + ["cmd call-start"])
    check(r.rtext()[-1].startswith("error") and len(r.tx()) == 3,
          "second call-start refused")
    r = run(td, ["bogus"])
    check(r.rc == 1, "bad script line: exit 1")


def test_options(td):
    rc, _ = spawn([BIN])
    check(rc == 2, "no arguments: usage")
    rc, _ = spawn([BIN, "-d", "/dev/null", "-R", "x", "-o", "y"])
    check(rc == 2, "-d with -R refused")
    rc, _ = spawn([BIN, "-R", "x"])
    check(rc == 2, "-R without -o")
    for o in (["-f", "44100"], ["-A", "3"], ["-T", "0"], ["-r", "2"], ["-r", ""]):
        rc, _ = spawn([BIN] + o + ["-R", "x", "-o", "y"])
        check(rc == 2, "bad %r" % o)
    f = os.path.join(td, "plain")
    open(f, "w").close()
    rc, err = spawn([BIN, "-d", f, "-s", os.path.join(td, "s"), "-p", "none"])
    check(rc == 1 and b"not a character device" in err, "regular file refused")
    rc, err = spawn([BIN, "-s", os.path.join(td, "nosock"), "status"])
    check(rc == 1, "client without a daemon fails")


def client(sock, *args):
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.dup2(w, 1)
            os.execv(BIN, [BIN, "-s", sock] + list(args))
        finally:
            os._exit(127)
    os.close(w)
    out = b""
    while True:
        b = os.read(r, 65536)
        if not b:
            break
        out += b
    os.close(r)
    _, st = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(st), out.decode()


def test_client_fake_server(td):
    sock = os.path.join(td, "fake.sock")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock)
    srv.listen(1)
    for reply, code in ((b"speech-on: acked\nok\n", 0),
                        (b"error: nope\n", 1)):
        pid = os.fork()
        if pid == 0:
            c, _ = srv.accept()
            data = b""
            while not data.endswith(b"\n"):
                d = c.recv(256)
                if not d:
                    break
                data += d
            c.sendall(b"got " + data + reply if code == 0 else reply)
            c.close()
            os._exit(0 if data == b"call-start --rate 16000 --device speaker\n" else 3)
        rc, out = client(sock, "call-start", "--rate", "16000", "--device",
                         "speaker")
        _, st = os.waitpid(pid, 0)
        check(os.waitstatus_to_exitcode(st) == 0, "client sends the joined line")
        check(rc == code, "client exit code %d for %r" % (code, reply))
        if code == 0:
            check(out.startswith("got call-start") and out.endswith("ok\n"),
                  "client prints the reply: %r" % out)
    srv.close()


def test_daemon_socket(td):
    """The real loop on /dev/null (a character device that reads EOF)."""
    sock = os.path.join(td, "d.sock")
    st = os.path.join(td, "state")
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    errp = os.path.join(td, "err")
    pid = os.fork()
    if pid == 0:
        try:
            fd = os.open(errp, os.O_WRONLY | os.O_CREAT, 0o600)
            os.dup2(fd, 2)
            os.execve(BIN, [BIN, "-d", "/dev/null", "-S", "none", "-a", "-m",
                            "none", "-p", st, "-I", "none", "-s", sock, "-c",
                            "hw:NoSuchCard", "-P", "hw:NoSuchCard,4"], env)
        finally:
            os._exit(127)
    for _ in range(100):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    check(os.path.exists(sock), "daemon socket created")
    check(os.stat(sock).st_mode & 0o777 == 0o660, "socket mode 0660")
    rc, out = client(sock, "status")
    check(rc == 0 and "modem=ready port=open" in out and out.endswith("ok\n"),
          "status over the socket: %r" % out)
    rc, out = client(sock, "volume", "3")
    check(rc == 0 and out == "ok\n", "volume over the socket")
    rc, out = client(sock, "call-start")
    check(rc == 1 and out.rstrip().endswith("could not be set (see the log)"),
          "call-start without the sound card fails: %r" % out)
    rc, out = client(sock, "bogus")
    check(rc == 1 and out.startswith("error"), "bad command over the socket")
    os.kill(pid, signal.SIGTERM)
    _, status = os.waitpid(pid, 0)
    check(os.waitstatus_to_exitcode(status) == 0, "daemon exits 0 on SIGTERM")
    check(not os.path.exists(sock), "socket removed")
    with open(errp, "rb") as f:
        err = f.read()
    check(b"ERROR: AddressSanitizer" not in err and b"runtime error" not in err,
          "no sanitizer report")


def main():
    global BIN, TMP
    BIN = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("t2-speechd")
    TMP = os.environ.get("TMPDIR") or "/tmp"
    tests = [test_speech_on_info, test_gains, test_mute, test_speech_off,
             test_acks, test_timeouts, test_device_switch, test_notifications,
             test_shm, test_modem_reset, test_recovery, test_codec_routes_off,
             test_alsa_errors,
             test_framing, test_options, test_client_fake_server,
             test_daemon_socket]
    for t in tests:
        td = mkdtemp("speechd-test-")
        before = failures
        try:
            t(td)
        except Exception as e:  # report and go on
            check(False, "%s raised %r" % (t.__name__, e))
        print("%-26s %s" % (t.__name__, "ok" if failures == before else "FAILED"))
        shutil.rmtree(td)
    print("%d checks, %d failures" % (checks, failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
