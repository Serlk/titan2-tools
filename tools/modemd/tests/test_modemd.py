#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Host test for t2-modemd: a fake modem on a pty answers the AT port, a fake
# MIPC peer on a second pty answers the MIPC port (framing from
#), an "ip" stub records its argv, a
# stub t2-speechd socket records call-start/call-stop. The daemon is driven
# through its control socket with the binary's client mode.
# Run with: python3 -I tests/test_modemd.py path/to/t2-modemd

# Only the standard library: os, sys, select, socket, struct, pty, tty, ...
import os
import pty
import re
import select
import signal
import socket
import struct
import sys
import time
import tty

BIN = None
TMP = None
SRC_MIPC_H = None
failures = 0
checks = 0
_seq = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL: %s" % what)
    return cond


def mkdtemp(prefix):
    global _seq
    _seq += 1
    d = os.path.join(TMP, "%s%d-%d" % (prefix, os.getpid(), _seq))
    os.mkdir(d, 0o700)
    return d


def mipc_consts():
    """MIPC_* constants from mipc.h, so the fake peer uses the same ids."""
    c = {}
    with open(SRC_MIPC_H) as f:
        for line in f:
            m = re.match(r"#define\s+(MIPC_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+|\d+)u?\b", line)
            if m:
                c[m.group(1)] = int(m.group(2), 0)
    return c


MC = None  # MIPC constants


# --- MIPC framing ---

def tlv(tid, value):
    if isinstance(value, int):
        value = struct.pack("<I", value)
    raw = struct.pack("<HH", tid, len(value)) + value
    pad = (-len(raw)) % 8
    return raw + b"\0" * pad


def tlv_u8(tid, v):
    return tlv(tid, bytes([v]))


def tlv_str(tid, s):
    return tlv(tid, s.encode() + b"\0")


def frame(msgid, txid, slot, payload=b""):
    return struct.pack("<IIBBHHH", MC["MIPC_MAGIC"], 0, slot, 0, msgid, txid, len(payload)) + payload


def parse_tlvs(payload):
    out = {}
    off = 0
    while off + 4 <= len(payload):
        tid, ln = struct.unpack_from("<HH", payload, off)
        if ln == 0:
            off += 16
            continue
        out[tid & 0x7FFF] = payload[off + 4:off + 4 + ln]
        off += (4 + ln + 7) & ~7
    return out


# --- the fake modem (AT port on a pty) ---

class FakeModem:
    def __init__(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        tty.setraw(self.master)
        self.path = os.ttyname(self.slave)
        self.rx = b""
        self.cmds = []          # every command received, in order
        self.handlers = {}      # exact command -> response (str or callable)
        self.regex_handlers = []
        self.calls = []         # (id, dir, stat, mode, number)
        self.pending_pdu = None  # command waiting for a PDU after "> "
        self.pdus = []
        self.fragment = 0       # > 0: write responses in chunks of this size
        self.frag_delay = 0.0
        self.echo = False
        self.cpin = "READY"
        self.default()

    def default(self):
        h = self.handlers
        h["ATI"] = "MTK2\r\nMOLY.NR17.R1.MP5.RC.MP.V51.2.\r\nOK"
        h["AT+CGMR"] = "MOLY.NR17.R1.MP5.RC.MP.V51.2.P2, 2025/12/23 13:47\r\nOK"
        h["AT+CFUN?"] = "+CFUN: 1\r\nOK"
        h["AT+CPIN?"] = lambda c: "+CPIN: %s\r\nOK" % self.cpin
        h["AT+CREG?"] = '+CREG: 2,1,"1234","0001ABCD",7\r\nOK'
        h["AT+CEREG?"] = '+CEREG: 2,1,"1234","0001ABCD",7\r\nOK'
        h["AT+CGREG?"] = '+CGREG: 2,1,"1234","0001ABCD",7\r\nOK'
        h["AT+CIREG?"] = "+CIREG: 2,0\r\nOK"
        h["AT+COPS?"] = '+COPS: 0,0,"CarrierA",7\r\nOK'
        h["AT+CSQ"] = "+CSQ: 12,99\r\nOK"
        h["AT+CGDCONT?"] = '+CGDCONT: 0,"IP","internet.carrier1","10.20.30.47",0,0\r\nOK'
        h["AT+CGACT?"] = "OK"
        h["AT+CGCONTRDP=0"] = ('+CGCONTRDP: 0,5,"internet.carrier1.MNC001.MCC001.GPRS",'
                               '"10.20.30.47.255.255.255.255","","192.0.2.53","192.0.2.54"\r\nOK')
        h["AT+CGPADDR=0"] = '+CGPADDR: 0,"10.20.30.47",""\r\nOK'
        h["AT+CGPADDR"] = "+CME ERROR: unknown"
        h["AT+CGDATA=?"] = "+CME ERROR: unknown"
        h["AT+BAD"] = "+CME ERROR: unknown"
        h["AT+BAD2"] = "ERROR"
        h["AT+SLOW"] = None  # never answered
        # MediaTek data flow
        h['AT+EAPNACT=1,"internet.carrier1","default",0'] = "+CGEV: ME PDN ACT 0\r\nOK"
        h['AT+EPDN=0,"ifst",20'] = '+EPDN: 0,"new",7,202\r\nOK'
        h['AT+EPDN=0,"addr"'] = '+EPDN: 0,"addr",1,"10.20.30.47"\r\nOK'
        h['AT+EIF=202, "ifst", "up", 1'] = "OK"
        h["AT+EAPNACT=0,0,1"] = "+CGEV: ME PDN DEACT 0\r\nOK"
        h["AT+CLCC"] = lambda c: self.clcc()
        self.regex_handlers.append((re.compile(r"^AT\+CMGS=(\d+)$"), self.cmgs))

    def clcc(self):
        lines = ['+CLCC: %d,%d,%d,%d,0,"%s",129' % k for k in self.calls]
        return "\r\n".join(lines + ["OK"])

    def cmgs(self, cmd, m):
        self.pending_pdu = cmd
        return "\r\n> "

    def set_call(self, cid, direction, stat, mode=0, number=""):
        self.calls = [k for k in self.calls if k[0] != cid] + [(cid, direction, stat, mode, number)]

    def clear_call(self, cid):
        self.calls = [k for k in self.calls if k[0] != cid]

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        if self.fragment:
            for i in range(0, len(data), self.fragment):
                os.write(self.master, data[i:i + self.fragment])
                if self.frag_delay:
                    time.sleep(self.frag_delay)
        else:
            os.write(self.master, data)

    def urc(self, line):
        self.write("\r\n" + line + "\r\n")

    def respond(self, cmd):
        if self.pending_pdu is not None:
            # the PDU, terminated by Ctrl-Z
            self.pdus.append(cmd)
            self.pending_pdu = None
            self.write("\r\n+CMGS: %d\r\n\r\nOK\r\n" % len(self.pdus))
            return
        self.cmds.append(cmd)
        if self.echo:
            self.write(cmd + "\r\n")
        resp = self.handlers.get(cmd, "OK")
        if cmd not in self.handlers:
            for rx, fn in self.regex_handlers:
                m = rx.match(cmd)
                if m:
                    resp = fn(cmd, m)
                    break
        if callable(resp):
            resp = resp(cmd)
        if resp is None:
            return
        if resp.endswith("> "):
            self.write(resp)
        else:
            self.write("\r\n" + resp + "\r\n")

    def service(self):
        try:
            data = os.read(self.master, 4096)
        except OSError:
            return
        self.rx += data
        while True:
            # commands end with \r; a PDU ends with Ctrl-Z
            m = re.search(rb"[\r\x1a]", self.rx)
            if not m:
                break
            cmd = self.rx[:m.start()]
            self.rx = self.rx[m.end():]
            if self.rx.startswith(b"\n"):
                self.rx = self.rx[1:]
            if cmd:
                self.respond(cmd.decode(errors="replace"))

    def wait_cmd(self, pattern, loop, timeout=3.0, start=None):
        rx = re.compile(pattern)
        start = len(self.cmds) if start is None else start
        deadline = time.monotonic() + timeout

        def found():
            return any(rx.search(c) for c in self.cmds[start:])
        loop.pump(found, deadline)
        return found()

    def close(self):
        os.close(self.master)
        os.close(self.slave)


# --- the fake MIPC peer ---

class FakeMipc:
    def __init__(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        tty.setraw(self.master)
        self.path = os.ttyname(self.slave)
        self.rx = b""
        self.msgs = []          # (msgid, txid, slot, tlvs)
        self.registered = []
        self.fragment = False
        self.drop_first_test = True
        self.act_result = 0
        self.tid = 202
        self.aid = 0
        self.eif_first = True
        self.registered_cmds = []
        self.imscfg_result = 0
        self.apn_result = 0
        self.at_result = 0
        self.at_reply = {}          # tunnel command -> response text (default "OK")
        # an interface the modem raises by itself while our request is pending
        self.eif_during_act = None   # (tid, addr)

    def send(self, data):
        if self.fragment:
            for i in range(0, len(data), 7):
                os.write(self.master, data[i:i + 7])
                time.sleep(0.002)
        else:
            os.write(self.master, data)

    def cnf(self, msgid, txid, payload=b"", slot=0xFF):
        self.send(frame(msgid, txid, slot, payload))

    def ind(self, msgid, payload, slot=1):
        self.send(frame(msgid, 0xFFFF, slot, payload))

    def eif_ind(self, cmd, tid=None, addr=b"\x0a\x14\x1f\xdd", array=False):
        tid = self.tid if tid is None else tid
        p = tlv(MC["MIPC_T_EIF_TRANS_INTF_ID"], tid) + tlv_u8(MC["MIPC_T_EIF_CMD"], cmd)
        if cmd == 0 and addr is None:
            pass        # no address record (seen on mainline)
        elif cmd == 0 and array:
            # the record as an array element (descriptor 0x8105, element 0xFFFF)
            p += tlv_u8(MC["MIPC_T_EIF_V4_COUNT"], 1)
            p += struct.pack("<HHIIHH", MC["MIPC_T_EIF_V4_RECORDS"], 0, 0, 0, 0xFFFF, 1)
            p += tlv(0xFFFF, addr + b"\0" * 8)
        elif cmd == 0:
            p += tlv_u8(MC["MIPC_T_EIF_V4_COUNT"], 1)
            p += tlv(MC["MIPC_T_EIF_V4_RECORDS"], addr + b"\0" * 8)
        self.ind(MC["MIPC_INTERNAL_EIF_IND"], p)

    def ims_pdn_ind(self, aid=1, action=1, ptype=2, iface=101):
        """IMS_PDN_IND 0x4A03 full form as captured."""
        p = (tlv(0x8100, b"\0" * 18 + b"\xff\xff") + tlv_u8(MC["MIPC_T_IMSPDN_IND_TYPE"], 0) +
             tlv(MC["MIPC_T_IMSPDN_AID"], aid) + tlv_u8(MC["MIPC_T_IMSPDN_ACTION"], action) +
             tlv(MC["MIPC_T_IMSPDN_TYPE"], ptype) + tlv(MC["MIPC_T_IMSPDN_IFACE"], iface) +
             tlv_u8(0x0105, 1) + tlv(0xFFFF, 0))
        self.ind(MC["MIPC_IMS_PDN_IND"], p)

    def ims_pdn_reset(self):
        """IMS_PDN_IND short form: the bookkeeping reset."""
        self.ind(MC["MIPC_IMS_PDN_IND"], tlv_u8(MC["MIPC_T_IMSPDN_IND_TYPE"], 1) +
                 tlv(MC["MIPC_T_IMSPDN_TYPE"], 0))

    def eipspi_cmd(self, txid, index, lo=0x100, hi=0x0FFFFFFF, own=b"\x0a\x28\xc1\x9b"):
        """INTERNAL_EIPSPI_CMD 0x8305 as captured: header byte 8 = 0x11."""
        p = (tlv(MC["MIPC_T_SPI_MIN"], lo) + tlv(MC["MIPC_T_SPI_MAX"], hi) +
             tlv(MC["MIPC_T_SPI_INDEX"], index) + tlv(0x0101, 0) +
             tlv(MC["MIPC_T_SPI_ADDR1"], b"\x04\0\0\0" + b"\0" * 16) +
             tlv(MC["MIPC_T_SPI_ADDR2"], b"\x04\0\0\0" + own + b"\0" * 12) +
             tlv_u8(MC["MIPC_T_SPI_PROTO"], 50))
        self.send(frame(MC["MIPC_INTERNAL_EIPSPI_CMD"], txid, 0x11, p))

    def rsps(self):
        return [x for x in self.msgs if x[0] == MC["MIPC_INTERNAL_EIPSPI_RSP"]]

    def handle(self, msgid, txid, slot, payload):
        tl = parse_tlvs(payload)
        self.msgs.append((msgid, txid, slot, tl, payload))
        if msgid == MC["MIPC_INTERNAL_TEST_REQ"]:
            if self.drop_first_test:
                self.drop_first_test = False
                return
            self.cnf(MC["MIPC_INTERNAL_TEST_CNF"], txid, tlv(0, 0))
        elif msgid == MC["MIPC_INTERNAL_OPEN_REQ"]:
            self.cnf(MC["MIPC_INTERNAL_OPEN_CNF"], txid,
                     tlv(0, 0) + tlv(MC["MIPC_T_OPEN_VERSION"], 7) + tlv(MC["MIPC_T_OPEN_CNF_TIMEOUT"], 300000))
        elif msgid == MC["MIPC_INTERNAL_REGISTER_IND_REQ"]:
            self.registered.append((slot, struct.unpack("<H", tl[MC["MIPC_T_REGISTER_IND_ID"]][:2])[0]))
            self.cnf(MC["MIPC_INTERNAL_REGISTER_IND_CNF"], txid, tlv(0, 0))
        elif msgid == MC["MIPC_INTERNAL_REGISTER_CMD_REQ"]:
            self.registered_cmds.append((slot, struct.unpack("<H", tl[MC["MIPC_T_REGISTER_CMD_ID"]][:2])[0]))
            self.cnf(MC["MIPC_INTERNAL_REGISTER_CMD_CNF"], txid, tlv(0, 0))
        elif msgid == MC["MIPC_INTERNAL_EIPSPI_RSP"]:
            pass    # an answer to our command: recorded only
        elif msgid == MC["MIPC_APN_ADD_PROFILE_LIST_REQ"]:
            # result, then the count and the array echoed
            self.cnf(MC["MIPC_APN_ADD_PROFILE_LIST_CNF"], txid, tlv(0, self.apn_result) + payload, slot)
        elif msgid == MC["MIPC_IMS_SET_CONFIG_REQ"]:
            self.cnf(MC["MIPC_IMS_SET_CONFIG_CNF"], txid, tlv(0, self.imscfg_result), slot)
        elif msgid == MC["MIPC_SYS_AT_REQ"]:
            cmd = tl.get(MC["MIPC_T_AT_STRING"] & 0x7FFF, b"").rstrip(b"\0").decode()
            self.cnf(MC["MIPC_SYS_AT_CNF"], txid,
                     tlv(0, self.at_result) + tlv_str(MC["MIPC_T_AT_STRING"], self.at_reply.get(cmd, "OK")), slot)
        elif msgid == MC["MIPC_DATA_ACT_CALL_REQ"]:
            # the interface indication first, then the confirm; the TLV
            # order in the confirm is deliberately shuffled (hash-map order)
            if self.eif_during_act:
                self.eif_ind(0, tid=self.eif_during_act[0], addr=self.eif_during_act[1])
                self.eif_during_act = None
            if self.eif_first:
                self.eif_ind(0)
            p = (tlv(MC["MIPC_T_CNF_TRANS_INTF_ID"], self.tid) +
                 tlv(MC["MIPC_T_CNF_EPS_BEARER"], 5) +
                 tlv(0x812F, b"") + tlv(0xFFFD, 1) + tlv(0xFFFE, 2) + tlv(0xFFFF, 3) +
                 tlv_u8(MC["MIPC_T_CNF_AID"], self.aid) +
                 tlv_str(MC["MIPC_T_CNF_APN"], "internet.carrier1") +
                 tlv_u8(MC["MIPC_T_CNF_PDP_TYPE"], 1) +
                 tlv_u8(MC["MIPC_T_CNF_V4_COUNT"], 1) +
                 tlv(MC["MIPC_T_CNF_V4_ADDRS"], b"\x0a\x14\x1f\xdd") +
                 tlv_u8(MC["MIPC_T_CNF_V4_DNS_COUNT"], 2) +
                 tlv(MC["MIPC_T_CNF_V4_DNS"], bytes([192, 0, 2, 53, 192, 0, 2, 54])) +
                 tlv(MC["MIPC_T_CNF_MTU_V4"], 0) +
                 tlv(0, self.act_result))
            self.cnf(MC["MIPC_DATA_ACT_CALL_CNF"], txid, p, slot)
        elif msgid == MC["MIPC_DATA_DEACT_CALL_REQ"]:
            aid = tl.get(MC["MIPC_T_DEACT_AID"], bytes([self.aid]))[0]
            self.cnf(MC["MIPC_DATA_DEACT_CALL_CNF"], txid,
                     tlv(0, 0) + tlv_u8(MC["MIPC_T_CNF_AID"], aid), slot)
        else:
            self.cnf(msgid + 1, txid, tlv(0, 2), slot)

    def service(self):
        try:
            self.rx += os.read(self.master, 65536)
        except OSError:
            return
        while len(self.rx) >= 16:
            magic, _, slot, _, msgid, txid, ln = struct.unpack_from("<IIBBHHH", self.rx, 0)
            if magic != MC["MIPC_MAGIC"]:
                raise AssertionError("bad magic from daemon: %r" % self.rx[:16])
            if len(self.rx) < 16 + ln:
                break
            payload = self.rx[16:16 + ln]
            self.rx = self.rx[16 + ln:]
            self.handle(msgid, txid, slot, payload)

    def wait_msg(self, msgid, loop, timeout=3.0):
        start = len(self.msgs)
        deadline = time.monotonic() + timeout

        def found():
            return any(x[0] == msgid for x in self.msgs[start:])
        loop.pump(found, deadline)
        for x in self.msgs[start:]:
            if x[0] == msgid:
                return x
        return None

    def close(self):
        os.close(self.master)
        os.close(self.slave)


# --- the t2-speechd stub ---

class FakeSpeechd:
    def __init__(self, path):
        self.path = path
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        self.sock.listen(4)
        self.sock.setblocking(False)
        self.cmds = []
        self.times = []

    def service(self):
        try:
            c, _ = self.sock.accept()
        except BlockingIOError:
            return
        c.settimeout(1.0)
        data = b""
        try:
            while b"\n" not in data:
                b = c.recv(256)
                if not b:
                    break
                data += b
        except socket.timeout:
            pass
        line = data.decode(errors="replace").strip()
        self.cmds.append(line)
        self.times.append(time.monotonic())
        if line.startswith("volume 9"):
            c.sendall(b"error: bad volume\n")
        else:
            c.sendall(b"acked\nok\n")
        c.close()

    def wait_cmd(self, prefix, loop, timeout=3.0, start=None):
        start = len(self.cmds) if start is None else start
        deadline = time.monotonic() + timeout

        def found():
            return any(c.startswith(prefix) for c in self.cmds[start:])
        loop.pump(found, deadline)
        return found()

    def close(self):
        self.sock.close()


# --- the daemon and the loop ---

class Daemon:
    def __init__(self, workdir, modem, mipc=None, speechd=None, extra_cfg=None, extra_args=None):
        self.workdir = workdir
        self.modem = modem
        self.mipc = mipc
        self.speechd = speechd
        self.sock = os.path.join(workdir, "ctl.sock")
        self.state = os.path.join(workdir, "fsm")
        self.iplog = os.path.join(workdir, "ip.log")
        self.resolv = os.path.join(workdir, "resolv.conf")
        self.log = os.path.join(workdir, "daemon.log")
        self.monitors = []
        self.events = []
        self.events_mark = 0
        with open(self.state, "w") as f:
            f.write("state: ready\n")
        ipstub = os.path.join(workdir, "ip")
        with open(ipstub, "w") as f:
            f.write('#!/bin/sh\necho "$@" >> "%s"\nexit 0\n' % self.iplog)
        os.chmod(ipstub, 0o755)
        cfg = {
            "ip_cmd": ipstub,
            "resolv_file": self.resolv,
            "manage_resolv": "0",
            "speechd_socket": speechd.path if speechd else os.path.join(workdir, "nospeechd"),
            "cmd_timeout_ms": "400",
            "call_timeout_ms": "600",
            "sms_timeout_ms": "1000",
            "ready_poll_ms": "50",
            "reopen_ms": "100",
            "call_poll_ms": "200",
            "signal_poll_ms": "0",
            "settle_ms": "50",
            "data_backend": "mipc" if mipc else "at",
            "mipc_port": mipc.path if mipc else "/dev/null",
            "mipc_timeout_ms": "2000",
        }
        if extra_cfg:
            cfg.update(extra_cfg)
        self.conf = os.path.join(workdir, "t2-modemd.conf")
        with open(self.conf, "w") as f:
            f.write("# test configuration\n")
            for k, v in cfg.items():
                f.write("%s = %s\n" % (k, v))
        argv = [BIN, "-v", "-d", modem.path, "-S", self.state, "-s", self.sock, "-c", self.conf]
        if extra_args:
            argv += extra_args
        self.pid = os.fork()
        if self.pid == 0:
            fd = os.open(self.log, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            os.dup2(fd, 2)
            os.dup2(fd, 1)
            os.execv(argv[0], argv)
            os._exit(127)
        # wait for the socket
        deadline = time.monotonic() + 5
        while not os.path.exists(self.sock) and time.monotonic() < deadline:
            self.pump(lambda: False, time.monotonic() + 0.02)

    def set_state(self, st):
        with open(self.state, "w") as f:
            f.write("state: %s\n" % st)

    def pump(self, until, deadline):
        """Service the fake peers until until() or the deadline."""
        while True:
            if until():
                return True
            now = time.monotonic()
            if now >= deadline:
                return until()
            fds = [self.modem.master]
            if self.mipc:
                fds.append(self.mipc.master)
            if self.speechd:
                fds.append(self.speechd.sock.fileno())
            fds += [s.fileno() for s in self.monitors]
            try:
                r, _, _ = select.select(fds, [], [], min(0.05, deadline - now))
            except (OSError, ValueError):
                return until()
            for fd in r:
                if fd == self.modem.master:
                    self.modem.service()
                elif self.mipc and fd == self.mipc.master:
                    self.mipc.service()
                elif self.speechd and fd == self.speechd.sock.fileno():
                    self.speechd.service()
                else:
                    for s in self.monitors:
                        if s.fileno() == fd:
                            try:
                                data = s.recv(65536)
                            except OSError:
                                data = b""
                            if not data:
                                self.monitors.remove(s)
                                s.close()
                            else:
                                for line in data.decode(errors="replace").split("\n"):
                                    if line:
                                        self.events.append(line)

    def client(self, *args, timeout=8.0):
        """Run the binary in client mode; returns (exit code, output)."""
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.dup2(w, 1)
            os.dup2(w, 2)
            os.execv(BIN, [BIN, "-s", self.sock] + list(args))
            os._exit(127)
        os.close(w)
        out = b""
        done = [False]

        def poll_child():
            nonlocal out
            rr, _, _ = select.select([r], [], [], 0)
            if rr:
                b = os.read(r, 65536)
                if b:
                    out += b
                else:
                    done[0] = True
            return done[0]
        self.pump(poll_child, time.monotonic() + timeout)
        while True:
            b = os.read(r, 65536)
            if not b:
                break
            out += b
        os.close(r)
        _, st = os.waitpid(pid, 0)
        return os.waitstatus_to_exitcode(st), out.decode(errors="replace")

    def monitor(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.sock)
        s.sendall(b"monitor\n")
        s.setblocking(False)
        self.monitors.append(s)
        return s

    def mark(self):
        """Later wait_event calls only see events after this point."""
        self.events_mark = len(self.events)

    def wait_event(self, pattern, timeout=3.0, fresh=False):
        rx = re.compile(pattern)
        start = len(self.events) if fresh else self.events_mark
        deadline = time.monotonic() + timeout

        def found():
            return any(rx.search(e) for e in self.events[start:])
        self.pump(found, deadline)
        return found()

    def wait(self, seconds):
        self.pump(lambda: False, time.monotonic() + seconds)

    def ip_calls(self):
        if not os.path.exists(self.iplog):
            return []
        with open(self.iplog) as f:
            return [l.strip() for l in f if l.strip()]

    def stop(self, sig=signal.SIGTERM, timeout=5.0):
        os.kill(self.pid, sig)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                for s in self.monitors:
                    s.close()
                self.monitors = []
                return os.waitstatus_to_exitcode(st)
            self.pump(lambda: False, time.monotonic() + 0.05)
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        for s in self.monitors:
            s.close()
        self.monitors = []
        return -9

    def daemon_log(self):
        with open(self.log) as f:
            return f.read()


def wait_init(d, modem):
    """The init sequence ends with AT+CSQ (modem_refresh_reg)."""
    ok = modem.wait_cmd(r"^AT\+CSQ$", d, 5.0, start=0)
    d.wait(0.1)
    return ok


# --- tests ---

def test_init_and_engine():
    print("init, AT engine")
    wd = mkdtemp("init")
    modem = FakeModem()
    modem.fragment = 3            # every response in 3-byte pieces
    modem.frag_delay = 0.001
    sp = FakeSpeechd(os.path.join(wd, "speechd.sock"))
    d = Daemon(wd, modem, speechd=sp)
    d.monitor()
    check(wait_init(d, modem), "init sequence reached AT+CSQ")
    cmds = modem.cmds
    check(cmds[0] == "ATE0", "first command ATE0: %r" % cmds[:2])
    check("AT+CMEE=2" in cmds, "AT+CMEE=2 sent")
    for c in ["AT+CREG=2", "AT+CEREG=2", "AT+CIREG=2", "AT+CLIP=1", "AT+CMGF=0", "AT+CGEREP=2,1",
              "AT+CMOD=0", "ATS0=0", "AT+CSSN=1,1", "AT+ECPI=4294967295", "AT+EAIC=2", "AT+EVOCD=1"]:
        check(c in cmds, "%s in init" % c)
    check(d.wait_event(r"^modem ready$"), "event modem ready")
    check(d.wait_event(r"^sim ready READY$"), "event sim ready")
    check(d.wait_event(r"^reg eps home LTE$"), "event reg eps home LTE")
    check(d.wait_event(r"^reg operator CarrierA LTE$"), "event operator")
    check(d.wait_event(r"^signal -89 dBm rssi=12 ber=99$"), "event signal")

    rc, out = d.client("status")
    check(rc == 0 and "modem: ready" in out, "status ok: %r" % out[:200])
    check("model: MTK2" in out and "revision: MOLY.NR17.R1.MP5.RC.MP.V51.2.P2" in out, "model/revision parsed")
    check("stack: SIM1" in out and "sim: ready READY" in out, "sim status")

    # URC in the middle of a response: the response stays intact
    modem.fragment = 0
    modem.handlers["AT+COPS?"] = lambda c: '+CREG: 1,"1234","0001ABCD",7\r\n+COPS: 0,0,"CarrierA",7\r\nOK'
    rc, out = d.client("net")
    check(rc == 0 and "operator: CarrierA LTE" in out, "net with a URC inside the response: %r" % out)
    modem.handlers["AT+COPS?"] = '+COPS: 0,0,"CarrierA",7\r\nOK'

    # raw AT: CME error text, ERROR, timeout, final result
    rc, out = d.client("at", "AT+BAD")
    check(rc == 1 and "error: +CME ERROR: unknown" in out, "CME error reported: %r" % out)
    rc, out = d.client("at", "AT+BAD2")
    check(rc == 1 and "error: ERROR" in out, "ERROR reported: %r" % out)
    t0 = time.monotonic()
    rc, out = d.client("at", "AT+SLOW")
    dt = time.monotonic() - t0
    check(rc == 1 and "error: timeout" in out, "timeout reported: %r" % out)
    check(0.3 < dt < 1.5, "timeout after cmd_timeout_ms (%.2fs)" % dt)
    rc, out = d.client("at", "AT+CGMR")
    check(rc == 0 and "MOLY.NR17" in out and out.strip().endswith("ok"), "raw AT response + OK: %r" % out)
    # echo on: the echoed command line is dropped
    modem.echo = True
    rc, out = d.client("at", "AT+CSQ")
    check(rc == 0 and "+CSQ: 12,99" in out and "AT+CSQ" not in out.split("+CSQ")[0], "echo dropped: %r" % out)
    modem.echo = False
    # deny list and a non-AT string
    rc, out = d.client("at", 'AT+CLCK="SC",1,"1234"')
    check(rc == 1 and "refused" in out, "AT+CLCK refused: %r" % out)
    check(not any(c.startswith("AT+CLCK") for c in modem.cmds), "AT+CLCK never sent")
    rc, out = d.client("at", "hello")
    check(rc == 1 and "refused" in out, "non-AT refused")
    # a stray final result and an unknown URC become events, not crashes
    modem.urc("OK")
    modem.urc("+EIND: 128")
    modem.urc("+SOMETHING: 1,2")
    check(d.wait_event(r"^urc \+EIND: 128$"), "vendor URC event")
    check(d.wait_event(r"^urc \+SOMETHING: 1,2$"), "unknown URC event")
    # registration URCs
    d.mark()
    modem.urc("+CEREG: 2")
    check(d.wait_event(r"^reg eps searching$"), "CEREG URC -> event")
    d.mark()
    modem.urc('+CEREG: 1,"1234","0001ABCD",7')
    check(d.wait_event(r"^reg eps home LTE$"), "CEREG URC back home")
    modem.urc("+CIREGU: 1,5")
    check(d.wait_event(r"^reg ims registered$"), "CIREGU URC -> ims registered")
    # oversized / garbage input
    os.write(modem.master, b"\r\n" + b"X" * 3000 + b"\r\n")
    os.write(modem.master, b"\x00\x01\x02garbage\r\n")
    rc, out = d.client("status")
    check(rc == 0, "daemon alive after garbage")
    # unknown command, help
    rc, out = d.client("frobnicate")
    check(rc == 1 and "error: unknown command" in out, "unknown command")
    rc, out = d.client("help")
    check(rc == 0 and "data up" in out, "help")
    rc = d.stop()
    check(rc == 0, "clean exit on SIGTERM (%d)" % rc)
    log = d.daemon_log()
    check("AddressSanitizer" not in log and "runtime error" not in log, "no sanitizer report")
    modem.close()
    sp.close()


def test_radio_sim():
    print("radio, SIM, PIN, stack")
    wd = mkdtemp("radio")
    modem = FakeModem()
    modem.cpin = "SIM PIN"
    d = Daemon(wd, modem)
    d.monitor()
    check(wait_init(d, modem), "init")
    check(d.wait_event(r"^sim pin SIM PIN$"), "sim pin event")
    rc, out = d.client("pin", "12")
    check(rc == 1 and "4 to 8 digits" in out, "short PIN refused")
    modem.cpin = "READY"
    rc, out = d.client("pin", "1234")
    check(rc == 0 and 'AT+CPIN="1234"' in modem.cmds, "PIN entered: %r" % out)
    check("1234" not in d.daemon_log(), "PIN not logged")
    rc, out = d.client("pin", "1234")
    check(rc == 1 and "does not ask for a PIN" in out, "PIN refused when READY")
    rc, out = d.client("radio", "on", "3")
    check(rc == 0 and "AT+EFUN=3" in modem.cmds, "radio on 3 -> AT+EFUN=3")
    rc, out = d.client("radio", "on", "1")
    check(rc == 0 and "AT+CFUN=1" in modem.cmds, "radio on 1 -> AT+CFUN=1")
    rc, out = d.client("radio", "on", "7")
    check(rc == 1, "bad mask refused")
    modem.handlers["AT+CFUN?"] = "+CFUN: 4\r\nOK"
    rc, out = d.client("radio", "off")
    check(rc == 0 and "AT+CFUN=4" in modem.cmds, "radio off -> AT+CFUN=4")
    rc, out = d.client("status")
    check("radio: off (CFUN 4)" in out, "status shows radio off")
    modem.handlers["AT+CFUN?"] = "+CFUN: 1\r\nOK"
    # the SIM2 stack: ESUO=5, voice init again, state re-read
    n = len(modem.cmds)
    rc, out = d.client("sim", "2")
    check(rc == 0 and "AT+ESUO=5" in modem.cmds[n:] and "stack: SIM2" in out, "sim 2 -> AT+ESUO=5: %r" % out[:100])
    check("AT+ECPI=4294967295" in modem.cmds[n:], "voice init re-run on the new stack")
    check("AT+CPIN?" in modem.cmds[n:] and "AT+CEREG?" in modem.cmds[n:], "state re-read")
    rc, out = d.client("sim", "1")
    check(rc == 0 and "AT+ESUO=4" in modem.cmds, "sim 1 -> AT+ESUO=4")
    rc, out = d.client("sim", "3")
    check(rc == 1, "sim 3 refused")
    # scan
    modem.handlers["AT+COPS=?"] = ('+COPS: (2,"CarrierA","CarrierA","00101",7),(1,"CarrierB","CarrierB","00102",7),'
                                   '(3,"CarrierC","CarrierC","00103",2),,(0,1,2,3,4),(0,1,2)\r\nOK')
    rc, out = d.client("scan")
    check(rc == 0 and 'current "CarrierA" "CarrierA" 00101 LTE' in out and 'forbidden "CarrierC"' in out, "scan: %r" % out)
    d.stop()
    modem.close()


def test_data_at():
    print("data, AT backend (MediaTek templates and the 3GPP route)")
    wd = mkdtemp("dataat")
    modem = FakeModem()
    d = Daemon(wd, modem, extra_cfg={"data_backend": "at"})
    d.monitor()
    check(wait_init(d, modem), "init")
    n = len(modem.cmds)
    rc, out = d.client("data", "up")
    check(rc == 0, "data up ok: %r" % out)
    seq = modem.cmds[n:]
    want = ['AT+CGDCONT?', 'AT+EAPNACT=1,"internet.carrier1","default",0', 'AT+EPDN=0,"ifst",20',
            'AT+CGCONTRDP=0', 'AT+EPDN=0,"addr"', 'AT+EIF=202, "ifst", "up", 1']
    idx = [seq.index(w) if w in seq else -1 for w in want]
    check(all(i >= 0 for i in idx) and idx == sorted(idx), "MTK AT sequence in order: %r" % seq)
    ips = d.ip_calls()
    check(ips and ips[0] == "link add dev wwan0-2 parentdev wwan0 type wwan linkid 2", "link add linkid 2 (tid 202): %r" % ips)
    check("addr add 10.20.30.47/32 dev wwan0-2" in ips, "address /32")
    check("link set dev wwan0-2 mtu 1500" in ips and "link set dev wwan0-2 up" in ips, "mtu and up")
    check("route replace default dev wwan0-2 metric 600" in ips, "default route metric 600")
    # the EIF confirm comes after the link is up
    eif_i = seq.index('AT+EIF=202, "ifst", "up", 1')
    check(seq.index('AT+EPDN=0,"addr"') < eif_i, "EIF confirm after the address query")
    with open(d.resolv) as f:
        resolv = f.read()
    check("nameserver 192.0.2.53" in resolv and "nameserver 192.0.2.54" in resolv, "resolv.conf: %r" % resolv)
    check("dns: 192.0.2.53 192.0.2.54" in out and "address: 10.20.30.47" in out, "reply shows address and dns")
    check("transIntfId 202" in out and "aid 0" in out, "reply shows aid/tid: %r" % out[-400:])
    check(d.wait_event(r"^data up aid=0 link=wwan0-2 addr=10.20.30.47$"), "event data up")
    rc, out = d.client("data", "status")
    check(rc == 0 and "data: up aid 0 apn internet.carrier1 via at" in out and "data path: DL rx" in out, "data status: %r" % out)
    rc, out = d.client("data", "up")
    check(rc == 1 and "data is up" in out, "second data up refused")
    rc, out = d.client("sim", "2")
    check(rc == 1 and "data is up" in out, "sim switch refused while data is up")
    n = len(modem.cmds)
    rc, out = d.client("data", "down")
    check(rc == 0 and "AT+EAPNACT=0,0,1" in modem.cmds[n:], "data down -> EAPNACT=0: %r" % out)
    ips = d.ip_calls()
    check("link del dev wwan0-2" in ips and "route del default dev wwan0-2 metric 600" in ips, "link removed")
    check(not os.path.exists(d.resolv), "resolv.conf removed")
    # network deactivation URC takes the link down
    rc, out = d.client("data", "up", "internet.carrier1")
    check(rc == 0, "data up with apn")
    os.unlink(d.iplog)
    d.mark()
    modem.urc("+CGEV: NW PDN DEACT 0")
    check(d.wait_event(r"^data down NW PDN DEACT 0$"), "NW PDN DEACT -> data down event")
    check("link del dev wwan0-2" in d.ip_calls(), "link removed on DEACT")
    # +EIF ifup URC gives the address when the responses have none
    modem.handlers['AT+EPDN=0,"addr"'] = "OK"
    modem.handlers["AT+CGPADDR=0"] = "+CME ERROR: unknown"
    modem.handlers["AT+CGCONTRDP=0"] = '+CGCONTRDP: 0,5,"internet.carrier1","","","192.0.2.53","192.0.2.54"\r\nOK'
    old = modem.handlers['AT+EAPNACT=1,"internet.carrier1","default",0']

    def eapnact(c):
        modem.urc('+EIF: 202,ifup,1,"10.20.30.48",0')
        return old
    modem.handlers['AT+EAPNACT=1,"internet.carrier1","default",0'] = eapnact
    d.mark()
    rc, out = d.client("data", "up")
    check(rc == 0 and "address: 10.20.30.48" in out, "address from +EIF ifup: %r" % out[-300:])
    check(d.wait_event(r"^data if 202 ifup 10.20.30.48 -$"), "event data if ifup")
    modem.urc("+EIF: 202,ifdown,0")
    check(d.wait_event(r"^data down eif ifdown$"), "+EIF ifdown -> data down")
    # a failing activation is reported, nothing configured
    os.unlink(d.iplog)
    modem.handlers['AT+EAPNACT=1,"internet.carrier1","default",0'] = "+CME ERROR: 33"
    rc, out = d.client("data", "up")
    check(rc == 1 and "+CME ERROR: 33" in out, "failed activation reported: %r" % out)
    check(not d.ip_calls(), "no ip commands after a failed activation")
    d.stop()
    modem.close()

    # the plain 3GPP route (empty templates), explicit cid and apn
    wd = mkdtemp("data3gpp")
    modem = FakeModem()
    d = Daemon(wd, modem, extra_cfg={"data_backend": "at", "data_bind_cmds": "", "data_deact_cmds": "",
                                     "route_metric": "700", "data_mtu": "0"})
    d.monitor()
    check(wait_init(d, modem), "init")
    n = len(modem.cmds)
    rc, out = d.client("data", "up")
    check(rc == 0, "3gpp data up: %r" % out)
    seq = modem.cmds[n:]
    check("AT+CGDCONT?" in seq and "AT+CGCONTRDP=0" in seq and "AT+CGPADDR=0" in seq, "3gpp queries: %r" % seq)
    check(not any(c.startswith("AT+CGACT=1") for c in seq), "default bearer not re-activated")
    ips = d.ip_calls()
    check(ips[0] == "link add dev wwan0-0 parentdev wwan0 type wwan linkid 0", "linkid 0 from data_linkid")
    check("route replace default dev wwan0-0 metric 700" in ips, "metric 700")
    check(not any("mtu" in i for i in ips), "mtu 0 = not set")
    n = len(modem.cmds)
    rc, out = d.client("data", "down")
    check(rc == 0 and not any(c.startswith("AT+CGACT=0") for c in modem.cmds[n:]), "cid 0 not deactivated")
    # a new context for another APN: CGDCONT define + CGACT
    modem.handlers["AT+CGCONTRDP=1"] = "+CME ERROR: unknown"
    modem.handlers["AT+CGPADDR=1"] = '+CGPADDR: 1,"10.1.2.3"\r\nOK'
    modem.handlers["AT+CGACT=1,1"] = "OK"
    n = len(modem.cmds)
    rc, out = d.client("data", "up", "ims")
    check(rc == 0 and 'AT+CGDCONT=1,"IP","ims"' in modem.cmds[n:] and "AT+CGACT=1,1" in modem.cmds[n:],
          "new context defined and activated: %r" % out)
    check("addr add 10.1.2.3/32 dev wwan0-0" in d.ip_calls(), "address of cid 1")
    n = len(modem.cmds)
    rc, out = d.client("data", "down")
    check(rc == 0 and "AT+CGACT=0,1" in modem.cmds[n:], "cid 1 deactivated on down")
    d.stop()
    modem.close()


def test_data_mipc():
    print("data, MIPC backend")
    wd = mkdtemp("datamipc")
    modem = FakeModem()
    mp = FakeMipc()
    d = Daemon(wd, modem, mipc=mp)
    d.monitor()
    check(wait_init(d, modem), "init")
    check(d.wait_event(r"^mipc (open|ready)$"), "mipc port opened after modem ready")
    check(d.wait_event(r"^mipc ready$", 5.0), "mipc session ready")
    tests = [x for x in mp.msgs if x[0] == MC["MIPC_INTERNAL_TEST_REQ"]]
    check(len(tests) >= 2 and all(x[2] == 0xFF for x in tests), "TEST_REQ repeated with slot 0xFF (%d)" % len(tests))
    opens = [x for x in mp.msgs if x[0] == MC["MIPC_INTERNAL_OPEN_REQ"]]
    check(len(opens) == 1 and opens[0][2] == 0xFF, "one OPEN_REQ, slot 0xFF")
    tl = opens[0][3]
    check(struct.unpack("<I", tl[0x0100])[0] == 2 and tl[0x0102] == b"\x01" and tl[0x0101].endswith(b"\0"),
          "OPEN TLVs: version 2, name string, flag 1: %r" % tl)
    check(sorted(mp.registered) == sorted([(1, 0x4302), (1, 0x4201), (1, 0x4202), (1, 0x4001), (1, 0x4A03)]),
          "registered indications for slot 1: %r" % mp.registered)
    check(mp.registered_cmds == [(0xFF, 0x8305)], "INTERNAL_EIPSPI_CMD registered (slot 0xFF): %r" % mp.registered_cmds)
    # txids count up from 0 (first TEST_REQ = 0)
    check(mp.msgs[0][1] == 0 and mp.msgs[1][1] == 1, "txid starts at 0 and counts: %r" % [x[1] for x in mp.msgs[:3]])
    rc, out = d.client("status")
    check("mipc: %s open, session ready" % mp.path in out, "status shows mipc session: %r" % out)

    # activation: APN from the attach context (AT port), request TLVs per
    mp.fragment = True
    n = len(modem.cmds)
    rc, out = d.client("data", "up")
    check(rc == 0, "mipc data up ok: %r" % out)
    check("AT+CGDCONT?" in modem.cmds[n:], "APN read from the AT port")
    req = [x for x in mp.msgs if x[0] == MC["MIPC_DATA_ACT_CALL_REQ"]]
    check(len(req) == 1 and req[0][2] == 1, "one DATA_ACT_CALL_REQ for slot 1")
    tl = req[0][3]
    check(tl.get(0x0101) == b"internet.carrier1\0", "APN TLV 0x0101 with NUL")
    check(tl.get(0x0102) == struct.pack("<I", 1), "APN type 0x0102 u32 1")
    check(tl.get(0x0103) == b"\x01" and tl.get(0x0104) == b"\x01", "PDP type IP (u8 1) 0x0103/0x0104")
    check(tl.get(0x0105) == b"\x00", "auth type 0x0105 u8 0")
    check(tl.get(0x0106) == b"\0" and tl.get(0x0107) == b"\0", "empty user/password strings 0x8106/0x8107")
    check(tl.get(0x0108) == b"\x7f", "0x0108 u8 0x7F")
    check(tl.get(0x0109) == struct.pack("<I", 0x7FFFFFFF), "bearer mask 0x0109 0x7FFFFFFF")
    check(tl.get(0x010A) == b"\x00" and tl.get(0x010F) == struct.pack("<I", 0), "0x010A u8 0, 0x010F u32 0")
    check(0x010D not in tl, "no traffic descriptor by default")
    ips = d.ip_calls()
    check(ips and ips[0] == "link add dev wwan0-2 parentdev wwan0 type wwan linkid 2", "link wwan0-2 from transIntfId 202: %r" % ips)
    check("addr add 10.20.31.221/32 dev wwan0-2" in ips, "address from the confirm")
    with open(d.resolv) as f:
        resolv = f.read()
    check("nameserver 192.0.2.53" in resolv and "nameserver 192.0.2.54" in resolv, "DNS from the confirm")
    check("eps bearer id: 5" in out and "aid 0, transIntfId 202 -> linkid 2" in out, "reply details: %r" % out)
    check(d.wait_event(r"^data if 202 ifup 10.20.31.221 -$"), "EIF_IND event")
    check(d.wait_event(r"^data up aid=0 link=wwan0-2 addr=10.20.31.221$"), "event data up")
    check(not any(c.startswith("AT+EAPNACT") or c.startswith("AT+CGACT=") for c in modem.cmds[n:]),
          "no AT activation on the mipc backend")
    rc, out = d.client("data", "status")
    check("via mipc" in out and "transIntfId 202" in out, "data status via mipc")
    # deactivation
    rc, out = d.client("data", "down")
    check(rc == 0, "data down")
    deact = [x for x in mp.msgs if x[0] == MC["MIPC_DATA_DEACT_CALL_REQ"]]
    check(len(deact) == 1 and deact[0][3].get(0x0101) == b"\x00" and deact[0][3].get(0x0102) == b"\x01",
          "DEACT_REQ aid 0 cause 1: %r" % (deact[0][3] if deact else None))
    check("link del dev wwan0-2" in d.ip_calls(), "link removed")
    # traffic descriptor on request; indication-driven teardown
    d.stop()
    modem.close()
    mp.close()

    wd = mkdtemp("datamipc2")
    modem = FakeModem()
    mp = FakeMipc()
    mp.drop_first_test = False
    mp.eif_first = False
    mp.tid = 305
    mp.aid = 3
    d = Daemon(wd, modem, mipc=mp, extra_cfg={"mipc_traffic_descriptor": "1", "data_apn": "internet"})
    d.monitor()
    check(wait_init(d, modem), "init")
    check(d.wait_event(r"^mipc ready$", 5.0), "mipc ready (no dropped test)")
    rc, out = d.client("data", "up")
    check(rc == 0, "data up with descriptor: %r" % out)
    req = [x for x in mp.msgs if x[0] == MC["MIPC_DATA_ACT_CALL_REQ"]][0][3]
    td = req.get(0x010D)
    check(td is not None and len(td) == 672 and td[0] == len("internet") and td[4:12] == b"internet" and td[12:] == b"\0" * 660,
          "672-byte traffic descriptor: DNN length and string")
    check(req.get(0x0101) == b"internet\0", "APN from data_apn")
    check("link add dev wwan0-5 parentdev wwan0 type wwan linkid 5" in d.ip_calls(), "linkid 305 mod 100 = 5")
    # DEACT_IND for our aid takes the link down
    os.unlink(d.iplog)
    d.mark()
    mp.ind(MC["MIPC_DATA_DEACT_CALL_IND"], tlv_u8(0x0100, 3) + tlv(0x0102, 36))
    check(d.wait_event(r"^data down deact indication cause 36$"), "DEACT_IND -> data down")
    check("link del dev wwan0-5" in d.ip_calls(), "link removed on DEACT_IND")
    # EIF down indication
    rc, out = d.client("data", "up")
    check(rc == 0, "data up again")
    d.mark()
    mp.eif_ind(1, tid=305)
    check(d.wait_event(r"^data down eif ifdown$"), "EIF_IND down -> data down")
    # a non-zero result is an error
    mp.act_result = 5
    rc, out = d.client("data", "up")
    check(rc == 1 and "result 5" in out, "non-zero result reported: %r" % out)
    mp.act_result = 0
    # a bad frame from the modem is dropped, the session survives the next good one
    os.write(mp.master, b"\x00" * 20)
    d.wait(0.2)
    mp.ind(MC["MIPC_DATA_ACT_CALL_IND"], tlv(0, 0) + tlv_u8(0x0100, 1))
    check(d.wait_event(r"^data pdn act aid 1"), "ACT_IND event after a dropped bad frame")
    # AT URC over the MIPC tunnel
    mp.ind(MC["MIPC_SYS_AT_IND"], tlv_str(0x8100, "+CGEV: ME PDN ACT 1"))
    check(d.wait_event(r"^urc-mipc \+CGEV: ME PDN ACT 1$"), "SYS_AT_IND event")
    # modem lost: the mipc port is closed, data down; reopened after ready
    rc, out = d.client("data", "up")
    check(rc == 0, "data up before loss")
    d.mark()
    d.set_state("exception")
    check(d.wait_event(r"^modem lost"), "modem lost event")
    check(d.wait_event(r"^mipc lost"), "mipc lost event")
    check(d.wait_event(r"^data down"), "data down on loss")
    d.mark()
    d.set_state("ready")
    check(d.wait_event(r"^modem ready$", 5.0), "modem ready again")
    check(d.wait_event(r"^mipc ready$", 5.0), "mipc session restarted")
    rc = d.stop()
    check(rc == 0, "clean exit")
    modem.close()
    mp.close()


def apn_rec(apn, ptype, pdp, auth):
    """One 284-byte APN profile record (MIPC profile table)."""
    r = bytearray(284)
    struct.pack_into("<I", r, 0, 0xFFFFFFFF)
    r[12:12 + len(apn)] = apn.encode()
    struct.pack_into("<I", r, 244, 0x7FFDFFFF)
    struct.pack_into("<I", r, 248, ptype)
    r[252] = pdp
    r[253] = pdp
    r[254] = auth
    struct.pack_into("<I", r, 256, 1)
    return bytes(r)


def apn_list(profiles):
    """APN_ADD_PROFILE_LIST_REQ payload: count, descriptor 0x8101, elements from 0xFFFF down."""
    n = len(profiles)
    first = 0x10000 - n
    p = tlv_u8(0x0100, n) + struct.pack("<HHIIHH", 0x8101, 0, 0, 0, first, n)
    for i, pr in enumerate(profiles):
        p += tlv(first + i, apn_rec(*pr))
    return p


def radio_fake(modem, start=4):
    """AT+CFUN? follows AT+CFUN=1/4 and AT+EFUN=<n>."""
    st = {"cfun": start}
    modem.handlers["AT+CFUN?"] = lambda c: "+CFUN: %d\r\nOK" % st["cfun"]
    modem.handlers["AT+CFUN=1"] = lambda c: (st.update(cfun=1), "OK")[1]
    modem.handlers["AT+CFUN=4"] = lambda c: (st.update(cfun=4), "OK")[1]
    modem.handlers["AT+EFUN=3"] = lambda c: (st.update(cfun=1), "OK")[1]
    return st


def wait_ims_setup(d, what="setup done", timeout=5.0):
    """The start-up may run before a monitor is accepted: poll ims status."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rc, out = d.client("ims", "status")
        if what in out:
            return True
        d.wait(0.1)
    return False


def test_ims():
    print("IMS: start-up, the modem's own IMS PDN (EIF_IND/IMS_PDN_IND), EIPSPI, registration")
    wd = mkdtemp("ims")
    modem = FakeModem()
    modem.handlers["AT+EIMS?"] = "+CME ERROR: unknown"
    # the "init" comes as a line inside the answer (same prefix as the command)
    modem.handlers['AT+EIMSPDN= "onoff",1'] = '+EIMSPDN: "init"\r\nOK'
    radio = radio_fake(modem, 4)     # the radio is off at start (modem restart)
    mp = FakeMipc()
    d = Daemon(wd, modem, mipc=mp)
    d.monitor()
    check(wait_init(d, modem), "init")
    check(d.wait_event(r"^mipc ready$", 5.0), "mipc ready")
    check(wait_ims_setup(d), "ims setup done")
    check(modem.wait_cmd(r"^AT\+CFUN=1$", d, 5.0, start=0), "radio on after the start-up")
    check(d.wait_event(r"^radio start on$"), "radio start event")
    check(radio["cfun"] == 1, "radio is on")
    cmds = modem.cmds
    onoff = 'AT+EIMSPDN= "onoff",1'
    first = cmds.index(onoff) if onoff in cmds else -1
    check(first > 0, "the observed onoff form sent")
    seq = cmds[first - 1:first + 3]
    check(seq == ["AT+CIREG=2", onoff, "AT+EIMSCFG=1,0,0,0,1,1", "AT+EIMS?"],
          "start-up sequence CIREG=2, onoff, EIMSCFG, EIMS?: %r" % seq)
    check("AT+EIMS=1" not in cmds, "AT+EIMS=1 not sent when AT+EIMS? cannot query")
    check("event: ims pdn init" in d.daemon_log(), "+EIMSPDN init inside the onoff answer handled")
    rc, out = d.client("ims", "status")
    check(rc == 0 and "ims: on, setup done (AT+EIMS? cannot query; AT+EIMS=1 not sent)" in out,
          "ims status after setup: %r" % out)
    # the MIPC IMS configuration, byte-exact
    cfg = [x for x in mp.msgs if x[0] == MC["MIPC_IMS_SET_CONFIG_REQ"]]
    key_form = tlv(0x0100, 1) + tlv_str(0x0101, "mtk_ct_volte_support") + tlv_u8(0x0102, 0x32)
    flag_form = tlv(0x0100, 0) + tlv(0x0102, bytes([1, 0, 0, 0, 1, 1]))
    check(len(cfg) == 2, "two IMS_SET_CONFIG_REQ at start-up: %d" % len(cfg))
    if len(cfg) == 2:
        check(cfg[0][4] == key_form and len(cfg[0][3][0x0101]) == 21 and cfg[0][2] == 1,
              "key/value form byte-exact, slot 1: %r" % cfg[0][4])
        check(cfg[1][4] == flag_form and cfg[1][2] == 1, "feature-switch form byte-exact: %r" % cfg[1][4])
    at = [x for x in mp.msgs if x[0] == MC["MIPC_SYS_AT_REQ"]]
    check(len(at) == 1 and at[0][3].get(0x0100) == b"AT+EIMSRCS=0,0\0" and at[0][2] == 1,
          "AT+EIMSRCS=0,0 through the AT tunnel, slot 1: %r" % at)
    order = [x[0] for x in mp.msgs if x[0] in (MC["MIPC_IMS_SET_CONFIG_REQ"], MC["MIPC_SYS_AT_REQ"],
                                                MC["MIPC_APN_ADD_PROFILE_LIST_REQ"])]
    check(order == [0x0A00, 0x0A00, 0x0113, 0x0003], "order key form, flags form, APN list, tunnel: %r" % order)
    apn = [x for x in mp.msgs if x[0] == MC["MIPC_APN_ADD_PROFILE_LIST_REQ"]]
    exp = apn_list([("internet.carrier1", 0x29, 1, 1), ("ims", 0x02, 3, 0), ("sos", 0x100, 3, 0)])
    check(len(apn) == 1 and apn[0][4] == exp and len(exp) == 888 and apn[0][2] == 1,
          "APN profile list byte-exact (internet, ims, sos; 888 bytes), slot 1: %r" % (apn[0][4][:40] if apn else None))
    log = d.daemon_log()
    i_apn = log.find("mipc: > msg 0x0113")
    i_onoff = log.find('at: > AT+EIMSPDN= "onoff",1')
    i_rcs = log.find("mipc: at tunnel > AT+EIMSRCS=0,0")
    i_radio = log.find("at: > AT+CFUN=1")
    check(0 <= log.find("mipc: > msg 0x0a00") < i_apn < i_onoff < i_rcs < i_radio,
          "IMS_SET_CONFIG < APN list < onoff < EIMSRCS < radio on: %r" % [i_apn, i_onoff, i_rcs, i_radio])
    # apn profiles: on demand, and a non-zero result
    n = len(mp.msgs)
    rc, out = d.client("apn", "profiles")
    apn = [x for x in mp.msgs[n:] if x[0] == MC["MIPC_APN_ADD_PROFILE_LIST_REQ"]]
    check(rc == 0 and len(apn) == 1 and apn[0][4] == exp, "apn profiles re-sends the list: %r" % out)
    mp.apn_result = 0x1416f2
    rc, out = d.client("apn", "profiles")
    check(rc == 1 and "result 0x1416f2" in out, "apn profiles: non-zero result is an error: %r" % out)
    mp.apn_result = 0
    check("AT+EIMSRCS" not in " ".join(modem.cmds), "AT+EIMSRCS not on the plain AT port")
    log = d.daemon_log()
    check(0 <= log.find("mipc: session ready") < log.find('at: > AT+EIMSPDN= "onoff",1'),
          "start-up waits for the MIPC session")
    # ims config: the feature-switch form again on demand
    d.mark()
    n = len(mp.msgs)
    rc, out = d.client("ims", "config")
    cfg = [x for x in mp.msgs[n:] if x[0] == MC["MIPC_IMS_SET_CONFIG_REQ"]]
    check(rc == 0 and len(cfg) == 1 and cfg[0][4] == flag_form, "ims config re-sends the flags: %r" % out)
    check(d.wait_event(r"^ims config sent$"), "ims config event")
    mp.imscfg_result = 5
    rc, out = d.client("ims", "config")
    check(rc == 1 and "result 5" in out, "a non-zero CNF result is an error: %r" % out)
    mp.imscfg_result = 0
    check((1, 0x4A03) in mp.registered and (0xFF, 0x8305) in mp.registered_cmds,
          "IMS_PDN_IND and EIPSPI_CMD registered")

    # internet first: its own EIF_IND (202) is ours, no IMS link for it
    rc, out = d.client("data", "up")
    check(rc == 0, "internet data up: %r" % out)
    with open(d.resolv) as f:
        resolv_before = f.read()
    check(not any("wwan0-2" in e and e.startswith("ims") for e in d.events), "internet EIF_IND not taken for IMS")
    nact = len([x for x in mp.msgs if x[0] == MC["MIPC_DATA_ACT_CALL_REQ"]])

    # the modem raises the IMS PDN by itself (observed boot timeline)
    os.unlink(d.iplog)
    d.mark()
    n = len(modem.cmds)
    mp.eif_ind(0, tid=101, addr=b"\x0a\x28\xc1\x9b")
    mp.ims_pdn_ind(aid=1, action=1, ptype=2, iface=101)
    check(d.wait_event(r"^ims pdn up link=wwan0-1 transIntfId=101 addr=10.40.193.155$"), "ims pdn up event")
    check(d.wait_event(r"^ims pdn ind aid=1 action=1 type=ims iface=101$"), "IMS_PDN_IND full form parsed")
    d.wait(0.2)
    ips = d.ip_calls()
    check("link add dev wwan0-1 parentdev wwan0 type wwan linkid 1" in ips, "IMS link wwan0-1 (101 mod 100): %r" % ips)
    check("addr add 10.40.193.155/32 dev wwan0-1" in ips, "address from the indication's IPv4 record")
    check("link set dev wwan0-1 mtu 1500" in ips and "link set dev wwan0-1 up" in ips, "MTU and up")
    check(not any("route" in c for c in ips), "no route for the IMS PDN: %r" % ips)
    with open(d.resolv) as f:
        check(f.read() == resolv_before, "DNS file untouched by the IMS PDN")
    check(len([x for x in mp.msgs if x[0] == MC["MIPC_DATA_ACT_CALL_REQ"]]) == nact, "no DATA_ACT_CALL_REQ for IMS")
    check(not any(c.startswith('AT+EIMSPDN="confirm"') for c in modem.cmds[n:]), "no AT confirm sent")

    # two INTERNAL_EIPSPI_CMD -> two RSP with the same txid, distinct SPIs in range
    mp.eipspi_cmd(3, 0)
    mp.eipspi_cmd(4, 1)
    d.pump(lambda: len(mp.rsps()) >= 2, time.monotonic() + 3)
    rs = mp.rsps()
    check(len(rs) == 2, "two EIPSPI_RSP: %r" % rs)
    if len(rs) == 2:
        check([x[1] for x in rs] == [3, 4], "same txids 3, 4")
        check(all(x[2] == 0x11 for x in rs), "header byte 8 echoed (0x11)")
        idx = [struct.unpack("<I", x[3][0x0100])[0] for x in rs]
        check(idx == [0, 1], "request index echoed: %r" % idx)
        check(all(x[3].get(0x0101) == b"\x00" for x in rs), "result u8 0")
        spis = [struct.unpack("<I", x[3][0x0102])[0] for x in rs]
        check(all(0x100 <= v <= 0x0FFFFFFF for v in spis) and spis[0] != spis[1],
              "SPIs distinct and in [0x100, 0x0fffffff]: %r" % [hex(v) for v in spis])
    check(d.wait_event(r"^ims spi index=0 spi=0x[0-9a-f]{8}$"), "ims spi event")
    check("ims: eipspi address 2 (0x8103) 10.40.193.155" in d.daemon_log(), "address records logged")
    # a one-value range: handed out once, never again in this session
    mp.eipspi_cmd(5, 2, lo=0x500, hi=0x500)
    mp.eipspi_cmd(6, 3, lo=0x500, hi=0x500)
    d.pump(lambda: len(mp.rsps()) >= 4, time.monotonic() + 3)
    rs = mp.rsps()
    check(len(rs) == 4 and struct.unpack("<I", rs[2][3][0x0102])[0] == 0x500 and rs[2][3][0x0101] == b"\x00",
          "0x500 handed out")
    check(len(rs) == 4 and rs[3][3][0x0101] == b"\x01", "0x500 not handed out twice (result 1)")

    # registration
    d.mark()
    modem.urc("+CIREGU: 1,5")
    check(d.wait_event(r"^reg ims registered$"), "CIREGU -> reg ims registered")
    check(d.wait_event(r"^ims services voice,sms ext=0x5$"), "CIREGU ext 5 -> voice,sms")
    rc, out = d.client("status")
    check("reg ims: registered (voice,sms, ext 0x5)" in out, "status reg ims line: %r" % out)
    check("ims pdn: type ims aid 1 up, link wwan0-1, addr 10.40.193.155, transIntfId 101" in out, "status ims pdn line")
    check("data: up aid 0" in out, "internet still up in status")
    modem.handlers["AT+CIREG?"] = "+CIREG: 2,1,1\r\nOK"
    n = len(modem.cmds)
    rc, out = d.client("net")
    check("AT+CIREG?" in modem.cmds[n:] and "reg ims: registered (voice, ext 0x1)" in out, "net re-reads CIREG: %r" % out)
    d.mark()
    d.monitor()
    d.wait(0.3)
    snap = d.events[d.events_mark:]
    check("ims on" in snap and "ims pdn up link=wwan0-1 transIntfId=101 addr=10.40.193.155 type=ims aid=1" in snap,
          "snapshot: %r" % snap)

    # internet down leaves the IMS link alone
    os.unlink(d.iplog)
    rc, out = d.client("data", "down")
    check(rc == 0, "data down")
    ips = d.ip_calls()
    check("link del dev wwan0-2" in ips and "link del dev wwan0-1" not in ips, "only the internet link removed: %r" % ips)
    rc, out = d.client("status")
    check("ims pdn: type ims aid 1 up" in out and "data: down" in out, "IMS PDN survives data down")
    # internet up again; meanwhile the modem raises another interface (103)
    os.unlink(d.iplog)
    mp.eif_during_act = (103, b"\x0a\x01\x02\x09")
    rc, out = d.client("data", "up")
    check(rc == 0 and "aid 0, transIntfId 202 -> linkid 2" in out, "internet up again next to IMS: %r" % out)
    d.wait(0.2)
    ips = d.ip_calls()
    check("link add dev wwan0-2 parentdev wwan0 type wwan linkid 2" in ips and
          "link add dev wwan0-3 parentdev wwan0 type wwan linkid 3" in ips, "own and unrequested interface told apart: %r" % ips)
    check("addr add 10.20.31.221/32 dev wwan0-2" in ips and "addr add 10.1.2.9/32 dev wwan0-3" in ips,
          "addresses kept apart")
    check("route replace default dev wwan0-2 metric 600" in ips and not any("route" in c and "wwan0-3" in c for c in ips),
          "default route only on the internet link")

    # IMS_PDN_IND action 0 removes the IMS link
    os.unlink(d.iplog)
    d.mark()
    mp.ims_pdn_ind(aid=1, action=0, ptype=2, iface=101)
    check(d.wait_event(r"^ims pdn down link=wwan0-1 aid=1 type=ims ims pdn ind action 0$"), "action 0 -> down")
    check(d.ip_calls() == ["link del dev wwan0-1"], "IMS link removed: %r" % d.ip_calls())
    # EIF_IND down for the other one
    d.mark()
    mp.eif_ind(1, tid=103)
    check(d.wait_event(r"^ims pdn down link=wwan0-3 aid=-1 type=- eif ifdown$"), "EIF_IND down -> down")
    check(not any(e.startswith("data down") for e in d.events[d.events_mark:]), "internet not taken down")
    rc, out = d.client("data", "status")
    check("data: up aid 0" in out, "internet untouched by the IMS teardown")

    # IMS_PDN_IND before its EIF_IND: labelled first, link with the EIF_IND
    d.mark()
    mp.ims_pdn_ind(aid=1, action=1, ptype=2, iface=101)
    d.wait(0.2)
    rc, out = d.client("ims", "status")
    check("announced for transIntfId 101, no interface indication yet" in out, "labelled before the EIF_IND: %r" % out)
    mp.eif_ind(0, tid=101, addr=b"\x0a\x28\xc1\x9b")
    check(d.wait_event(r"^ims pdn up link=wwan0-1 transIntfId=101"), "up with the EIF_IND")
    rc, out = d.client("ims", "status")
    check("ims pdn: type ims aid 1 up, link wwan0-1" in out, "label kept")
    # short form: the labels go, the link stays
    d.mark()
    mp.ims_pdn_reset()
    check(d.wait_event(r"^ims pdn init$"), "short form -> init")
    rc, out = d.client("ims", "status")
    check("ims pdn: type - aid -1 up, link wwan0-1" in out, "labels reset, link kept: %r" % out)

    # AT fallback: notify answered with confirm 0, nothing else
    n = len(modem.cmds)
    nact = len([x for x in mp.msgs if x[0] == MC["MIPC_DATA_ACT_CALL_REQ"]])
    os.unlink(d.iplog)
    modem.urc('+EIMSPDN: "notify",1,1,"ims"')
    check(modem.wait_cmd(r'^AT\+EIMSPDN="confirm",1,0$', d, 5.0, start=n), "AT notify -> confirm 0")
    modem.urc('+EIMSPDN: "notify",1,0,"ims"')
    check(modem.wait_cmd(r'^AT\+EIMSPDN="confirm",1,0$', d, 5.0, start=n + 1), "AT notify action 0 -> confirm 0")
    d.wait(0.2)
    check(len([x for x in mp.msgs if x[0] == MC["MIPC_DATA_ACT_CALL_REQ"]]) == nact and not d.ip_calls(),
          "AT notify: no activation, no link change")
    # reject and out-of-service are events
    d.mark()
    modem.urc('+EIMSPDN: "reject",33,"ims"')
    check(d.wait_event(r"^ims pdn reject cause=33 type=ims$"), "reject event")
    modem.urc('+EIMSPDNOOS: "ims",1,0')
    check(d.wait_event(r'^ims pdn oos "ims",1,0$'), "oos event")

    # ims off: onoff 0, our links removed, nothing over MIPC, internet untouched
    n = len(modem.cmds)
    nmsg = len(mp.msgs)
    d.mark()
    rc, out = d.client("ims", "off")
    check(rc == 0, "ims off: %r" % out)
    check('AT+EIMSPDN= "onoff",0' in modem.cmds[n:], "onoff 0 sent")
    check("AT+EIMS=0" not in modem.cmds[n:], "AT+EIMS=0 not sent (we never sent AT+EIMS=1)")
    check(not any(x[0] == MC["MIPC_DATA_DEACT_CALL_REQ"] for x in mp.msgs[nmsg:]), "nothing deactivated over MIPC")
    check(d.ip_calls() == ["link del dev wwan0-1"], "IMS link removed: %r" % d.ip_calls())
    rc, out = d.client("status")
    check("ims: off" in out and "data: up aid 0" in out and "ims pdn:" not in out, "status after ims off")
    os.unlink(d.iplog)
    mp.eif_ind(0, tid=101, addr=b"\x0a\x28\xc1\x9b")
    d.wait(0.3)
    check(not d.ip_calls(), "no IMS link while ims is off")
    # ims on: the start-up again
    n = len(modem.cmds)
    rc, out = d.client("ims", "on")
    check(rc == 0 and "ims: on, setup done" in out, "ims on: %r" % out)
    check(onoff in modem.cmds[n:] and "AT+EIMSCFG=1,0,0,0,1,1" in modem.cmds[n:], "start-up sequence again")
    mp.eif_ind(0, tid=101, addr=b"\x0a\x28\xc1\x9b")
    check(d.wait_event(r"^ims pdn up link=wwan0-1", fresh=True), "IMS link after ims on")
    # modem loss takes both down; after ready the start-up runs again
    os.unlink(d.iplog)
    d.mark()
    radio["cfun"] = 4                # the restarted modem comes back with the radio off
    d.set_state("exception")
    check(d.wait_event(r"^ims pdn down link=wwan0-1 .* modem lost$"), "IMS link down on modem loss")
    check(d.wait_event(r"^data down modem lost$"), "internet down on modem loss")
    ips = d.ip_calls()
    check("link del dev wwan0-1" in ips and "link del dev wwan0-2" in ips, "both links removed: %r" % ips)
    d.mark()
    d.set_state("ready")
    nr = len(modem.cmds)
    check(d.wait_event(r"^ims setup ok$", 6.0), "ims setup again after recovery")
    check(d.wait_event(r"^mipc ready$", 6.0), "mipc ready again")
    check(d.wait_event(r"^radio start on$", 6.0) and modem.wait_cmd(r"^AT\+CFUN=1$", d, 3.0, start=nr),
          "radio on again after the modem came back")
    check(radio["cfun"] == 1, "radio restored")
    # SIGTERM with the IMS link up: removed
    mp.eif_ind(0, tid=101, addr=b"\x0a\x28\xc1\x9b")
    check(d.wait_event(r"^ims pdn up link=wwan0-1"), "IMS link before exit")
    os.unlink(d.iplog)
    rc = d.stop()
    check(rc == 0, "clean exit")
    check("link del dev wwan0-1" in d.ip_calls(), "IMS link removed at exit")
    log = d.daemon_log()
    check("AddressSanitizer" not in log and "runtime error" not in log, "no sanitizer report")
    modem.close()
    mp.close()

    # the address from the +EIF URC, the AT+CIREG? poll, the internet profile later
    wd = mkdtemp("ims6")
    modem = FakeModem()
    modem.handlers["AT+CGDCONT?"] = "OK"            # no attach context while the radio is off
    modem.handlers["AT+CEREG?"] = "+CEREG: 2,0\r\nOK"
    radio_fake(modem, 4)
    mp = FakeMipc()
    d = Daemon(wd, modem, mipc=mp, extra_cfg={"ims_reg_poll_ms": "300", "ims_reg_idle_poll_ms": "3000"})
    d.monitor()
    check(wait_init(d, modem), "init")
    check(wait_ims_setup(d, "APN list without internet, re-sent later"), "APN list without internet first")
    apn = [x for x in mp.msgs if x[0] == MC["MIPC_APN_ADD_PROFILE_LIST_REQ"]]
    check(len(apn) == 1 and apn[0][4] == apn_list([("ims", 0x02, 3, 0), ("sos", 0x100, 3, 0)]),
          "list without the internet profile (2 profiles)")
    # registered, attach APN known -> the full list again
    modem.handlers["AT+CGDCONT?"] = '+CGDCONT: 0,"IP","internet.carrier1","10.20.30.47",0,0\r\nOK'
    modem.urc('+CEREG: 1,"1234","0001ABCD",7')
    check(d.wait_event(r"^apn profiles sent with internet$", 8.0), "full list re-sent once registered")
    apn = [x for x in mp.msgs if x[0] == MC["MIPC_APN_ADD_PROFILE_LIST_REQ"]]
    check(len(apn) == 2 and apn[1][4] == apn_list([("internet.carrier1", 0x29, 1, 1), ("ims", 0x02, 3, 0),
                                                   ("sos", 0x100, 3, 0)]), "second list with internet")
    d.wait(0.5)
    check(len([x for x in mp.msgs if x[0] == MC["MIPC_APN_ADD_PROFILE_LIST_REQ"]]) == 2, "not re-sent again")
    # EIF_IND without an address record, then the +EIF URC on the AT port
    os.unlink(d.iplog) if os.path.exists(d.iplog) else None
    d.mark()
    mp.eif_ind(0, tid=202, addr=None)
    check(d.wait_event(r"^ims pdn up link=wwan0-2 transIntfId=202 addr=-$"), "link without an address first")
    modem.urc("+EIF: 202, ifup, 1, 10.41.22.115, 0")
    check(d.wait_event(r"^ims pdn change link=wwan0-2 transIntfId=202 addr=10.41.22.115$"), "address from the +EIF URC")
    check("addr add 10.41.22.115/32 dev wwan0-2" in d.ip_calls(), "address applied: %r" % d.ip_calls())
    check(not any("route" in c for c in d.ip_calls()), "still no route")
    # registration by polling AT+CIREG? (no +CIREGU URC)
    n = len(modem.cmds)
    check(modem.wait_cmd(r"^AT\+CIREG\?$", d, 2.0, start=n), "AT+CIREG? polled after the IMS PDN")
    modem.handlers["AT+CIREG?"] = "+CIREG: 2,1,5\r\nOK"
    check(d.wait_event(r"^reg ims registered$", 3.0), "registered from the poll")
    rc, out = d.client("status")
    check("reg ims: registered (voice,sms, ext 0x5)" in out, "status registered: %r" % out)
    n = len(modem.cmds)
    d.wait(1.0)
    check(not any(c == "AT+CIREG?" for c in modem.cmds[n:]), "slow poll once registered")
    # the URC first (cache), then an EIF_IND without address; via the AT tunnel
    mp.eif_ind(1, tid=202)
    d.mark()
    mp.ind(MC["MIPC_SYS_AT_IND"], tlv_str(0x8100, "+EIF: 203, ifup, 1, 10.41.22.116, 0"))
    d.wait(0.2)
    mp.eif_ind(0, tid=203, addr=None)
    check(d.wait_event(r"^ims pdn up link=wwan0-3 transIntfId=203 addr=10.41.22.116$"), "tunnel URC address used")
    # the record as an array element
    mp.eif_ind(1, tid=203)
    d.mark()
    mp.eif_ind(0, tid=204, addr=b"\x0a\x29\x16\x75", array=True)
    check(d.wait_event(r"^ims pdn up link=wwan0-4 transIntfId=204 addr=10.41.22.117$"), "array-form record")
    d.stop()
    modem.close()
    mp.close()

    # ims_mipc_key=0: only the flags form; a tunnel ERROR is noted; ims_cfg flags
    wd = mkdtemp("ims5")
    modem = FakeModem()
    mp = FakeMipc()
    mp.at_reply["AT+EIMSRCS=0,0"] = "+CME ERROR: unknown"
    radio = radio_fake(modem, 1)
    d = Daemon(wd, modem, mipc=mp, extra_cfg={"ims_mipc_key": "0", "ims_cfg": "0,0,0,0,1,1",
                                              "mms_apn": "mms.carrier1", "data_apn": "internet",
                                              "radio_mask": "3"})
    d.monitor()
    check(wait_init(d, modem), "init")
    check(wait_ims_setup(d), "ims setup done (no key form)")
    apn = [x for x in mp.msgs if x[0] == MC["MIPC_APN_ADD_PROFILE_LIST_REQ"]]
    exp = apn_list([("internet", 0x29, 1, 1), ("mms.carrier1", 0x04, 1, 1), ("ims", 0x02, 3, 0),
                    ("sos", 0x100, 3, 0)])
    check(len(apn) == 1 and apn[0][4] == exp and len(exp) == 1176,
          "4 profiles with mms (1176 bytes, elements 0xFFFC..0xFFFF, data_apn)")
    check(apn and struct.unpack_from("<HHIIHH", apn[0][4], 8) == (0x8101, 0, 0, 0, 0xFFFC, 4),
          "descriptor 0x8101 first 0xFFFC count 4")
    check(modem.wait_cmd(r"^AT\+EFUN=3$", d, 5.0, start=0), "radio_mask 3 -> AT+EFUN=3 after the start-up")
    log = d.daemon_log()
    check(0 <= log.find("mipc: > msg 0x0113") < log.find("at: > AT+EFUN=3"), "EFUN after the APN list")
    # radio off by the user: not turned on again after a modem loss
    rc, out = d.client("radio", "off")
    check(rc == 0, "radio off")
    d.mark()
    d.set_state("exception")
    check(d.wait_event(r"^modem lost"), "lost")
    n = len(modem.cmds)
    d.set_state("ready")
    check(d.wait_event(r"^ims setup ok$", 6.0), "setup after recovery")
    d.wait(0.3)
    check(not any(c in ("AT+EFUN=3", "AT+CFUN=1") for c in modem.cmds[n:]), "radio left off after radio off")
    check("radio left off (radio off was requested)" in d.daemon_log(), "logged")
    cfg = [x for x in mp.msgs if x[0] == MC["MIPC_IMS_SET_CONFIG_REQ"]]
    # one flags form per connection (the modem was lost and came back above)
    check(len(cfg) == 2 and all(x[4] == tlv(0x0100, 0) + tlv(0x0102, bytes([0, 0, 0, 0, 1, 1])) for x in cfg),
          "ims_mipc_key=0: flags form only, from ims_cfg: %r" % [x[4] for x in cfg])
    rc, out = d.client("ims", "status")
    check("AT+EIMSRCS=0,0 refused" in out, "tunnel error noted: %r" % out)
    d.stop()
    modem.close()
    mp.close()

    # AT backend: +EIMS: 0 -> AT+EIMS=1; the plain onoff form when the
    # observed one is refused; ims off -> AT+EIMS=0
    wd = mkdtemp("ims2")
    modem = FakeModem()
    modem.handlers["AT+EIMS?"] = "+EIMS: 0\r\nOK"
    modem.handlers['AT+EIMSPDN= "onoff",1'] = "+CME ERROR: unknown"
    busy = {"n": 2}

    def cpin(c):
        if busy["n"]:
            busy["n"] -= 1
            return "+CME ERROR: SIM busy"
        return "+CPIN: READY\r\nOK"
    modem.handlers["AT+CPIN?"] = cpin
    d = Daemon(wd, modem, extra_cfg={"data_backend": "at", "ims_cfg": "0,0,0,0,1,1"})
    d.monitor()
    check(wait_init(d, modem), "init")
    check(wait_ims_setup(d), "ims setup done (at backend)")
    check(d.wait_event(r"^sim ready READY$", 5.0), "SIM busy retried until READY")
    check(not any(e.startswith("sim other") for e in d.events), "SIM busy is not a SIM state")
    rc, out = d.client("ims", "config")
    check(rc == 1 and "MIPC session not ready" in out, "ims config refused without MIPC")
    check('AT+EIMSPDN="onoff",1' in modem.cmds, "plain onoff form after a refusal")
    check("AT+EIMSCFG=0,0,0,0,1,1" in modem.cmds, "ims_cfg used")
    check("AT+EIMS=1" in modem.cmds, "AT+EIMS=1 after +EIMS: 0")
    n = len(modem.cmds)
    rc, out = d.client("ims", "off")
    check(rc == 0 and "AT+EIMS=0" in modem.cmds[n:], "ims off -> AT+EIMS=0 (we enabled it)")
    d.stop()
    modem.close()

    # ims = 0: nothing IMS is sent; onoff refused in both forms -> setup failed
    wd = mkdtemp("ims3")
    modem = FakeModem()
    d = Daemon(wd, modem, extra_cfg={"data_backend": "at", "ims": "0"})
    check(wait_init(d, modem), "init")
    d.wait(0.3)
    check(not any(c.startswith("AT+EIMS") for c in modem.cmds), "ims=0: no AT+EIMS* command")
    rc, out = d.client("ims", "status")
    check(rc == 0 and "ims: off" in out, "ims status off")
    modem.handlers['AT+EIMSPDN="onoff",1'] = "ERROR"
    modem.handlers['AT+EIMSPDN= "onoff",1'] = "ERROR"
    rc, out = d.client("ims", "on")
    check(rc == 1 and "error: ims setup failed" in out, "setup failure reported: %r" % out)
    rc, out = d.client("ims", "frob")
    check(rc == 1, "bad ims subcommand")
    d.stop()
    modem.close()


def test_calls():
    print("calls: ECPI/EAIC model, speech trigger")
    wd = mkdtemp("calls")
    modem = FakeModem()
    sp = FakeSpeechd(os.path.join(wd, "speechd.sock"))
    d = Daemon(wd, modem, speechd=sp)
    d.monitor()
    check(wait_init(d, modem), "init")
    # outgoing: ATD -> OK -> call-start right away (speech_on=dial)
    t0 = time.monotonic()
    rc, out = d.client("call", "dial", "+79001234567")
    check(rc == 0 and "ATD+79001234567;" in modem.cmds, "dial: %r" % out)
    check(sp.cmds and sp.cmds[0] == "call-start --rate 48000", "call-start after ATD OK: %r" % sp.cmds)
    check(sp.times and sp.times[0] - t0 < 1.0, "call-start promptly")
    check(d.wait_event(r"^audio on \(dial\)$"), "audio on event")
    d.mark()
    modem.set_call(1, 0, 2, 0, "+79001234567")
    modem.urc('+ECPI: 1,130,0,0,0,0,"+79001234567",145,""')
    check(d.wait_event(r"^call 1 dialing \+79001234567$"), "ECPI 130 -> dialing")
    modem.set_call(1, 0, 3, 0, "+79001234567")
    modem.urc('+ECPI: 1,2,1,1,0,0,"+79001234567",145,""')
    check(d.wait_event(r"^call 1 alerting \+79001234567$"), "ECPI 2 -> alerting")
    check(d.wait_event(r"^call 1 ringback in-band$"), "in-band ringback event")
    modem.urc("+ESPEECH: 1,0,0")
    check(d.wait_event(r"^call speech 1$"), "ESPEECH event")
    modem.set_call(1, 0, 0, 0, "+79001234567")
    modem.urc('+ECPI: 1,6,0,1,0,0,"+79001234567",145,""')
    check(d.wait_event(r"^call 1 active \+79001234567$"), "ECPI 6 -> active")
    check(len([c for c in sp.cmds if c.startswith("call-start")]) == 1, "call-start sent once")
    rc, out = d.client("call", "list")
    check(rc == 0 and "call 1: active MO +79001234567" in out and "audio on" in out, "call list: %r" % out)
    rc, out = d.client("call", "dtmf", "12#")
    check(rc == 0 and "AT+VTS=1" in modem.cmds and "AT+VTS=2" in modem.cmds and "AT+VTS=#" in modem.cmds, "dtmf")
    rc, out = d.client("call", "volume", "5")
    check(rc == 0 and "volume 5" in sp.cmds, "volume pass-through")
    rc, out = d.client("call", "volume", "9")
    check(rc == 1, "speechd error passed through")
    rc, out = d.client("call", "device", "speaker")
    check(rc == 0 and "device speaker" in sp.cmds, "device pass-through")
    # remote disconnect: ECPI 133 -> ended with cause, call-stop
    nsp = len(sp.cmds)
    modem.clear_call(1)
    modem.urc('+ECPI: 1,133,0,0,0,0,"+79001234567",145,"",16')
    check(d.wait_event(r"^call 1 ended \+79001234567 cause 16$"), "ECPI 133 -> ended with cause")
    check(sp.wait_cmd("call-stop", d, start=nsp), "call-stop after the last call ended")
    check(d.wait_event(r"^audio off \(ecpi 133 last call\)$"), "audio off event")
    modem.urc("+ESPEECH: 0,0,0")
    d.wait(0.3)
    check(sp.cmds.count("call-stop") == 1, "no second call-stop on ESPEECH 0: %r" % sp.cmds)

    # incoming: +EAIC approved before ringing, ATA -> call-start, hangup -> CHLD=11, call-stop
    nsp = len(sp.cmds)
    d.mark()
    modem.urc('+EAIC: 2,"+79005550000",145,0,7,"","",0')
    check(modem.wait_cmd(r"^AT\+EAIC=0,2,7$", d), "EAIC accepted with AT+EAIC=0,2,7")
    check(d.wait_event(r"^call 2 incoming \+79005550000$"), "incoming event from EAIC")
    modem.set_call(2, 1, 4, 0, "+79005550000")
    modem.urc("RING")
    modem.urc('+CLIP: "+79005550000",145,"",0,"",0')
    modem.urc('+ECPI: 2,0,0,0,1,0,"+79005550000",145,""')
    check(d.wait_event(r"^call ring \+79005550000$"), "ring event")
    d.wait(0.3)
    check(len(sp.cmds) == nsp, "no call-start while ringing")
    rc, out = d.client("call", "answer")
    check(rc == 0 and "ATA" in modem.cmds, "answer")
    check(sp.cmds[nsp:] == ["call-start --rate 48000"], "call-start after ATA: %r" % sp.cmds[nsp:])
    modem.set_call(2, 1, 0, 0, "+79005550000")
    modem.urc('+ECPI: 2,6,0,1,1,0,"+79005550000",145,""')
    check(d.wait_event(r"^call 2 active \+79005550000$"), "connected")
    # the call leaves +CLCC with the hang-up (not before: a poll would drop it)
    modem.handlers["AT+CHLD=12"] = lambda c: (modem.clear_call(2), "OK")[1]
    nsp = len(sp.cmds)
    rc, out = d.client("call", "hangup")
    check(rc == 0 and "AT+CHLD=12" in modem.cmds, "hangup single call -> AT+CHLD=12: %r" % out)
    check(sp.wait_cmd("call-stop", d, start=nsp), "call-stop after hangup with empty list")
    check(d.wait_event(r"^call 2 ended"), "ended event")
    # rejected incoming (eaic_auto_accept=0 is tested below), CLIR and emergency dial forms
    rc, out = d.client("call", "dial", "123", "--clir")
    check(rc == 0 and "ATD#31#123;" in modem.cmds, "CLIR invoke prefix")
    sp.wait_cmd("call-start", d)
    modem.urc("+ECPI: 1,7,0,0,0,0,\"\",0,\"\"")
    check(sp.wait_cmd("call-stop", d), "ECPI 7 -> call-stop")
    rc, out = d.client("call", "dial", "456", "--no-clir")
    check(rc == 0 and "ATD*31#456;" in modem.cmds, "CLIR suppress prefix")
    modem.urc("+ECPI: 1,7,0,0,0,0,\"\",0,\"\"")
    sp.wait_cmd("call-stop", d)
    rc, out = d.client("call", "dial", "112", "--emergency")
    check(rc == 0 and "ATDE112;" in modem.cmds, "emergency dial ATDE")
    modem.urc("+ECPI: 1,7,0,0,0,0,\"\",0,\"\"")
    sp.wait_cmd("call-stop", d)
    rc, out = d.client("call", "dial", "12345678901234567890123456789012345678901")
    check(rc == 1, "number longer than 40 refused")
    rc, out = d.client("call", "dial", "12;34")
    check(rc == 1, "bad number refused")
    # dial refused by the modem: no call-start
    nsp = len(sp.cmds)
    modem.handlers["ATD999;"] = "+CME ERROR: 3"
    rc, out = d.client("call", "dial", "999")
    check(rc == 1 and len(sp.cmds) == nsp, "no call-start when ATD fails")
    # hangup all with two calls -> AT+CHLD=6; hangup by id
    d.mark()
    modem.set_call(1, 0, 0, 0, "111")
    modem.set_call(2, 1, 1, 0, "222")
    modem.urc('+ECPI: 1,6,0,1,0,0,"111",129,""')
    check(d.wait_event(r"^call 1 active 111$"), "call 1 active")
    check(d.wait_event(r"^call 2 held 222$"), "call 2 held from CLCC poll")
    rc, out = d.client("call", "swap")
    check(rc == 0 and "AT+CHLD=2" in modem.cmds, "swap -> AT+CHLD=2")
    n = len(modem.cmds)
    modem.clear_call(2)
    rc, out = d.client("call", "hangup", "2")
    check(rc == 0 and "AT+CHLD=12" in modem.cmds[n:], "hangup by id")
    n = len(modem.cmds)
    modem.clear_call(1)
    rc, out = d.client("call", "hangup")
    check(rc == 0 and ("AT+CHLD=11" in modem.cmds[n:]), "last call hangup by id")
    # codec, EVADSREP events
    modem.urc("+EVOCD: 2")
    check(d.wait_event(r"^call codec 2$"), "codec event")
    modem.urc("+EVADSREP: 1")
    check(d.wait_event(r"^call EVADSREP: 1$"), "EVADSREP event")
    # modem loss during a call: call-stop
    d.mark()
    rc, out = d.client("call", "dial", "777")
    sp.wait_cmd("call-start", d)
    modem.set_call(1, 0, 0, 0, "777")
    modem.urc('+ECPI: 1,6,0,1,0,0,"777",129,""')
    check(d.wait_event(r"^call 1 active 777$"), "call active before loss")
    nsp = len(sp.cmds)
    d.set_state("exception")
    check(d.wait_event(r"^modem lost"), "modem lost")
    check(sp.wait_cmd("call-stop", d, start=nsp), "call-stop on modem loss")
    check(d.wait_event(r"^call 1 ended 777$"), "call ended on loss")
    modem.calls = []
    d.mark()
    d.set_state("ready")
    check(d.wait_event(r"^modem ready$", 5.0), "modem back")
    check(modem.cmds.count("ATE0") >= 2, "re-init after recovery")
    # SIGTERM with a call up: hang up, call-stop
    d.mark()
    rc, out = d.client("call", "dial", "888")
    modem.set_call(1, 0, 0, 0, "888")
    modem.urc('+ECPI: 1,6,0,1,0,0,"888",129,""')
    check(d.wait_event(r"^call 1 active 888$"), "call active before exit")
    nsp = len(sp.cmds)
    n = len(modem.cmds)
    rc = d.stop()
    check(rc == 0 and "AT+CHLD=6" in modem.cmds[n:] and "call-stop" in sp.cmds[nsp:], "SIGTERM: hangup + call-stop")
    modem.close()
    sp.close()

    # speech_on=espeech: call-start only on +ESPEECH: 1; eaic_auto_accept=0 rejects
    wd = mkdtemp("calls2")
    modem = FakeModem()
    sp = FakeSpeechd(os.path.join(wd, "speechd.sock"))
    d = Daemon(wd, modem, speechd=sp, extra_cfg={"speech_on": "espeech", "eaic_auto_accept": "0", "call_rate": "16000"})
    d.monitor()
    check(wait_init(d, modem), "init")
    rc, out = d.client("call", "dial", "123")
    check(rc == 0, "dial")
    modem.set_call(1, 0, 3, 0, "123")
    modem.urc('+ECPI: 1,2,0,0,0,0,"123",129,""')
    check(d.wait_event(r"^call 1 alerting 123$"), "alerting")
    d.wait(0.3)
    check(not sp.cmds, "no call-start before ESPEECH")
    modem.urc("+ESPEECH: 1,4,0")
    check(sp.wait_cmd("call-start --rate 16000", d), "call-start on ESPEECH 1 with the configured rate")
    modem.urc("+ESPEECH: 0,4,0")
    check(sp.wait_cmd("call-stop", d), "call-stop on ESPEECH 0")
    modem.clear_call(1)
    modem.urc('+ECPI: 1,133,0,0,0,0,"123",129,"",16')
    d.wait(0.3)
    check(sp.cmds.count("call-stop") == 1, "single call-stop")
    modem.urc('+EAIC: 3,"555",129,0,9')
    check(modem.wait_cmd(r"^AT\+EAIC=1,3,9,21$", d), "EAIC rejected with cause 21")
    check(d.wait_event(r"^call 3 rejected 555$"), "rejected event")
    d.stop()
    modem.close()
    sp.close()


def test_sms():
    print("sms")
    wd = mkdtemp("sms")
    modem = FakeModem()
    d = Daemon(wd, modem)
    d.monitor()
    check(wait_init(d, modem), "init")
    # receive: the classic "hellohello" DELIVER PDU, acked with AT+CNMA
    n = len(modem.cmds)
    modem.write("\r\n+CMT: ,30\r\n07917238010010F5040BC87238880900F10000993092516195800AE8329BFD4697D9EC37\r\n")
    check(d.wait_event(r"^sms 27838890001 hellohello$"), "CMT decoded (7-bit)")
    check(modem.wait_cmd(r"^AT\+CNMA$", d, start=n), "AT+CNMA sent")
    # UCS-2 with a newline, escaped in the event; multipart in two +CMT
    rc, out = d.client("sms", "send", "+79001234567", "\u041f\u0440\u0438\u0432\u0435\u0442\\n\u043c\u0438\u0440")
    check(rc == 0 and "part 1/1 sent, mr 1" in out, "send UCS-2 single: %r" % out)
    check(modem.cmds[-1].startswith("AT+CMGS="), "AT+CMGS used")
    pdu = modem.pdus[-1]
    check(pdu.startswith("0001000B919700214365F70008"), "SUBMIT header: smsc 00, fo 01, mr 00, da +79001234567, pid 00, dcs 08: %s" % pdu[:30])
    # deliver that same text back through the fake modem as two parts
    import binascii
    text = "\u041f\u0440\u0438\u0432\u0435\u0442 \u043c\u0438\u0440"
    ud = text.encode("utf-16-be")
    half = len(ud) // 2
    half -= half % 2
    parts = [ud[:half], ud[half:]]
    for i, p in enumerate(parts):
        udh = bytes([5, 0, 3, 0x42, 2, i + 1])
        tpdu = bytes([0x44, 0x0B, 0x91]) + binascii.unhexlify("9700214365F7") + bytes([0, 8]) + b"\x99\x30\x92\x51\x61\x95\x80" + bytes([len(udh) + len(p)]) + udh + p
        modem.write("\r\n+CMT: ,%d\r\n00%s\r\n" % (len(tpdu), binascii.hexlify(tpdu).decode().upper()))
        if i == 0:
            check(d.wait_event(r"^sms-part \+79001234567 1/2$"), "first part held")
    check(d.wait_event(r"^sms \+79001234567 \u041f\u0440\u0438\u0432\u0435\u0442 \u043c\u0438\u0440$"), "two parts concatenated")
    # send a long 7-bit text: two parts, each AT+CMGS with its own PDU
    long = "x" * 200
    npdus = len(modem.pdus)
    rc, out = d.client("sms", "send", "1234", long)
    check(rc == 0 and "part 2/2 sent" in out, "multipart send: %r" % out)
    check(len(modem.pdus) == npdus + 2, "two PDUs written")
    p1 = modem.pdus[npdus]
    check(p1.startswith("004100048121430000A0050003") and p1[28:32] == "0201", "multipart SUBMIT with UDHI, 160 septets, part 1/2: %s" % p1[:34])
    check(modem.pdus[npdus + 1][28:32] == "0202", "part 2/2")
    # 7-bit extension character and a bad PDU
    rc, out = d.client("sms", "send", "1234", "price {5} €")
    check(rc == 0, "extension chars send")
    modem.write("\r\n+CMT: ,5\r\nZZZZ\r\n")
    check(d.wait_event(r"^sms-bad 4$"), "bad PDU reported")
    # +CMTI: stored message read with AT+CMGR
    d.mark()
    modem.handlers["AT+CMGR=3"] = '+CMGR: 0,,30\r\n07917238010010F5040BC87238880900F10000993092516195800AE8329BFD4697D9EC37\r\nOK'
    modem.urc('+CMTI: "SM",3')
    check(d.wait_event(r"^sms 27838890001 hellohello$"), "CMTI -> CMGR -> decoded")
    check(not any(c.startswith("AT+CMGD") for c in modem.cmds), "stored message not deleted by default")
    # list
    modem.handlers["AT+CMGL=4"] = ('+CMGL: 1,1,,30\r\n07917238010010F5040BC87238880900F10000993092516195800AE8329BFD4697D9EC37\r\n'
                                  '+CMGL: 2,3,,20\r\n' + modem.pdus[0] + '\r\nOK')
    rc, out = d.client("sms", "list")
    check(rc == 0 and "1: read from 27838890001 hellohello" in out and "2: sent to +79001234567" in out and "2 messages" in out,
          "sms list: %r" % out)
    rc, out = d.client("sms", "send", "12a", "x")
    check(rc == 1 and "bad number" in out, "bad number refused")
    rc, out = d.client("sms", "send", "123")
    check(rc == 1, "missing text refused")
    d.stop()
    modem.close()


def test_loss_and_misc():
    print("modem loss, port missing, config")
    wd = mkdtemp("loss")
    modem = FakeModem()
    d = Daemon(wd, modem, extra_cfg={"data_backend": "at"})
    d.monitor()
    check(wait_init(d, modem), "init")
    rc, out = d.client("data", "up")
    check(rc == 0, "data up")
    os.unlink(d.iplog)
    # EOF on the port = loss
    d.set_state("exception")
    check(d.wait_event(r"^modem lost state file not ready$"), "lost by state file")
    check(d.wait_event(r"^data down modem lost$"), "data down on loss")
    check("link del dev wwan0-2" in d.ip_calls(), "link removed on loss")
    rc, out = d.client("status")
    check(rc == 0 and "modem: closed" in out, "status while lost")
    rc, out = d.client("data", "up")
    check(rc == 1 and "not ready" in out, "data up refused while lost")
    n = len(modem.cmds)
    d.mark()
    d.set_state("ready")
    check(d.wait_event(r"^modem ready$", 5.0), "ready again")
    check("ATE0" in modem.cmds[n:], "re-init")
    # a new monitor gets the snapshot
    d.mark()
    s = d.monitor()
    d.wait(0.3)
    snap = d.events[d.events_mark:]
    check("modem ready" in snap and "sim ready READY" in snap and "reg eps home LTE" in snap and
          "signal -89 dBm rssi=12 ber=99" in snap, "monitor snapshot: %r" % snap)
    # allow_raw_at=0
    d.stop()
    modem.close()

    wd = mkdtemp("noraw")
    modem = FakeModem()
    d = Daemon(wd, modem, extra_cfg={"allow_raw_at": "0", "init_cmds": "AT+EXTRA=1; AT+MORE=2", "radio_mask": "3"})
    check(wait_init(d, modem), "init")
    check("AT+EXTRA=1" in modem.cmds and "AT+MORE=2" in modem.cmds, "init_cmds run")
    check("AT+EFUN=3" in modem.cmds, "radio_mask -> AT+EFUN=3 at init")
    rc, out = d.client("at", "AT+CSQ")
    check(rc == 1 and "disabled" in out, "raw AT disabled")
    d.stop()
    modem.close()

    # bad configuration values are rejected at start
    wd = mkdtemp("badcfg")
    conf = os.path.join(wd, "bad.conf")
    with open(conf, "w") as f:
        f.write("data_backend = ppp\n")
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.dup2(w, 2)
        os.execv(BIN, [BIN, "-d", "/dev/null", "-c", conf, "-s", os.path.join(wd, "s")])
        os._exit(127)
    os.close(w)
    err = os.read(r, 4096)
    _, st = os.waitpid(pid, 0)
    check(os.waitstatus_to_exitcode(st) == 2 and b"data_backend" in err, "bad config refused: %r" % err)
    # /dev/null is not a character device the daemon accepts? It is a chr dev: the daemon
    # opens it, gets EOF -> modem lost -> retries; just check it starts and stops cleanly.
    modem = FakeModem()
    d = Daemon(wd, modem, extra_cfg={"data_backend": "at"})
    rc = d.stop()
    check(rc == 0, "stop before ready")
    modem.close()


def main():
    global BIN, TMP, SRC_MIPC_H
    if len(sys.argv) != 2:
        print("usage: test_modemd.py path/to/t2-modemd")
        return 2
    BIN = os.path.abspath(sys.argv[1])
    here = os.path.dirname(os.path.abspath(__file__))
    SRC_MIPC_H = os.path.join(here, "..", "src", "mipc.h")
    TMP = os.environ.get("TMPDIR") or "/tmp"
    TMP = os.path.join(TMP, "t2-modemd-test-%d" % os.getpid())
    os.mkdir(TMP, 0o700)
    global MC
    MC = mipc_consts()
    try:
        test_init_and_engine()
        test_radio_sim()
        test_data_at()
        test_data_mipc()
        test_ims()
        test_calls()
        test_sms()
        test_loss_and_misc()
    finally:
        pass
    print("%d checks, %d failures" % (checks, failures))
    if failures:
        print("logs kept in %s" % TMP)
        return 1
    # clean up
    for root, dirs, files in os.walk(TMP, topdown=False):
        for f in files:
            os.unlink(os.path.join(root, f))
        for dd in dirs:
            os.rmdir(os.path.join(root, dd))
    os.rmdir(TMP)
    return 0


if __name__ == "__main__":
    sys.exit(main())
