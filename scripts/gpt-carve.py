#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# The linux partition in the Titan 2 GPT (titan2-docs: docs/DUALBOOT.md): userdata is shortened at
# its end, and the linux partition goes into the freed space after it. Works on files only: the
# start and end of the disk read with dd (scripts/dualboot-repartition.sh); writes to no device.
#
#   gpt-carve.py --head H --tail T --disk-sectors N [--sector-size 4096] --check
#   gpt-carve.py --head H --tail T --disk-sectors N [--size 256GiB] [--min-userdata 64GiB] \
#       --out DIR
# H: the first sectors of the disk (from LBA 0), T: the last ones (up to LBA N-1); each must hold
# enough sectors for the GPT. In DIR: head.bin (LBA 0 .. end of the primary table), tail.bin
# (backup table .. LBA N-1) and carve.env (HEAD_SECTORS, TAIL_LBA, TAIL_SECTORS: where to write).
# The start of userdata does not change; linux is exactly --size, its start aligned to 1 MiB, its
# end not beyond the old end of userdata (the partitions after it, mrdump and flashinfo, stay).
import argparse
import os
import re
import struct
import sys
import uuid
import zlib

LINUX_TYPE = uuid.UUID("0FC63DAF-8483-4772-8E47-3D69D8477DE4")
HDR = struct.Struct("<8sIIIIQQQQ16sQIII")
ENT = struct.Struct("<16s16sQQQ72s")
ALIGN = 1 << 20


class Refuse(Exception):
    pass


def parse_size(s):
    m = re.fullmatch(r"(\d+)\s*([KMGT]?)(i?B)?", s.strip(), re.I)
    if not m:
        raise SystemExit(f"bad size: {s}")
    return int(m.group(1)) << {"": 0, "K": 10, "M": 20, "G": 30, "T": 40}[m.group(2).upper()]


def human(b):
    for u, sh in (("TiB", 40), ("GiB", 30), ("MiB", 20), ("KiB", 10)):
        if b >= 1 << sh:
            return f"{b / (1 << sh):.2f} {u}"
    return f"{b} B"


class Gpt:
    def __init__(self, raw, lba, ss):
        if len(raw) < ss:
            raise Refuse(f"LBA {lba}: not read")
        f = HDR.unpack_from(raw, 0)
        (self.sig, self.rev, self.hsize, self.hcrc, _, self.my, self.alt, self.first, self.last,
         self.guid, self.ents_lba, self.n, self.esz, self.ecrc) = f
        if self.sig != b"EFI PART":
            raise Refuse(f"LBA {lba}: no GPT signature")
        if self.my != lba:
            raise Refuse(f"LBA {lba}: header says it is at LBA {self.my}")
        if not 92 <= self.hsize <= ss or self.esz < 128 or self.esz % 8:
            raise Refuse(f"LBA {lba}: odd header (size {self.hsize}, entry {self.esz})")
        h = bytearray(raw[:self.hsize])
        h[16:20] = b"\0" * 4
        if zlib.crc32(h) != self.hcrc:
            raise Refuse(f"LBA {lba}: header CRC mismatch")
        self.raw = bytes(raw[:self.hsize])
        self.sector = bytes(raw[:ss])
        self.arr_sectors = -(-self.n * self.esz // ss)

    def pack(self, ents_crc):
        h = bytearray(self.raw)
        struct.pack_into("<I", h, 80, self.n)
        struct.pack_into("<I", h, 88, ents_crc)
        struct.pack_into("<I", h, 16, 0)
        struct.pack_into("<I", h, 16, zlib.crc32(h))
        return bytes(h) + self.sector[len(h):]


def entries(arr, n, esz):
    out = []
    for i in range(n):
        t, u, s, e, a, name = ENT.unpack_from(arr, i * esz)
        nm = name.decode("utf-16-le").split("\0", 1)[0]
        out.append(dict(i=i, type=t, guid=u, start=s, end=e, attr=a, name=nm, used=t != b"\0" * 16))
    return out


def load(a):
    ss = a.sector_size
    head = open(a.head, "rb").read()
    tail = open(a.tail, "rb").read()
    if len(head) % ss or len(tail) % ss:
        raise Refuse("head/tail are not whole sectors")
    N = a.disk_sectors
    tail0 = N - len(tail) // ss

    def sec(lba, cnt=1):
        if lba + cnt <= len(head) // ss:
            return head[lba * ss:(lba + cnt) * ss]
        if lba >= tail0:
            return tail[(lba - tail0) * ss:(lba - tail0 + cnt) * ss]
        raise Refuse(f"LBA {lba}..{lba + cnt - 1} is neither in the head nor in the tail")

    if head[510:512] != b"\x55\xaa" or head[450] != 0xEE:
        raise Refuse("LBA 0: no protective MBR")
    p = Gpt(sec(1), 1, ss)
    if p.alt != N - 1:
        raise Refuse(f"backup header at LBA {p.alt}, disk has {N} sectors (wrong --disk-sectors?)")
    b = Gpt(sec(N - 1), N - 1, ss)
    if (b.n, b.esz) != (p.n, p.esz):
        raise Refuse("primary and backup GPT differ (entry count or size)")
    parr = sec(p.ents_lba, p.arr_sectors)
    barr = sec(b.ents_lba, b.arr_sectors)
    nb = p.n * p.esz
    if zlib.crc32(parr[:nb]) != p.ecrc:
        raise Refuse("primary entry array CRC mismatch")
    if zlib.crc32(barr[:nb]) != b.ecrc:
        raise Refuse("backup entry array CRC mismatch")
    if (b.alt, b.first, b.last, b.guid, b.n, b.esz) != (1, p.first, p.last, p.guid, p.n, p.esz) \
            or parr[:nb] != barr[:nb]:
        raise Refuse("primary and backup GPT differ")
    if p.ents_lba + p.arr_sectors > p.first or b.ents_lba <= p.last:
        raise Refuse("entry arrays overlap the usable area")
    return ss, N, head, p, b, bytearray(parr[:nb]), sec


def table(ents, ss, names):
    print(f"  {'#':>3} {'name':<12} {'start':>12} {'end':>12} {'size':>12}")
    for e in ents:
        if e["used"] and e["name"] in names:
            print(f"  {e['i'] + 1:>3} {e['name']:<12} {e['start']:>12} {e['end']:>12} "
                  f"{human((e['end'] - e['start'] + 1) * ss):>12}")


def check_layout(ents, p):
    used = sorted((e for e in ents if e["used"]), key=lambda e: e["start"])
    for e in used:
        if e["end"] < e["start"] or e["start"] < p.first or e["end"] > p.last:
            raise Refuse(f"partition {e['name']} outside the usable area")
    for x, y in zip(used, used[1:]):
        if y["start"] <= x["end"]:
            raise Refuse(f"partitions {x['name']} and {y['name']} overlap")
    guids = [e["guid"] for e in used]
    if len(set(guids)) != len(guids):
        raise Refuse("duplicate partition GUIDs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True)
    ap.add_argument("--tail", required=True)
    ap.add_argument("--disk-sectors", type=int, required=True)
    ap.add_argument("--sector-size", type=int, default=4096)
    ap.add_argument("--size", default="256GiB")
    ap.add_argument("--min-userdata", default="64GiB")
    ap.add_argument("--name", default="linux")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    try:
        return run(a)
    except Refuse as e:
        print(f"gpt-carve: REFUSED: {e}", file=sys.stderr)
        return 1


def run(a):
    ss, N, head, p, b, arr, sec = load(a)
    ents = entries(arr, p.n, p.esz)
    check_layout(ents, p)
    used = [e for e in ents if e["used"]]
    print(f"GPT ok: {N} sectors x {ss} B ({human(N * ss)}), {len(used)}/{p.n} entries, "
          f"usable {p.first}..{p.last}")
    names = {"userdata", a.name}
    ud = [e for e in used if e["name"] == "userdata"]
    if [e for e in used if e["name"] == a.name]:
        table(ents, ss, names)
        if a.check:
            print(f"partition '{a.name}' present: already done")
            return 0
        raise Refuse(f"partition '{a.name}' already present: already done")
    if len(ud) != 1:
        raise Refuse("no single 'userdata' partition")
    ud = ud[0]
    after = sorted((e for e in used if e["start"] > ud["end"]), key=lambda e: e["start"])
    names |= {e["name"] for e in after[:2]}
    print("before:")
    table(ents, ss, names)
    if a.check:
        return 0

    al = ALIGN // ss
    L = parse_size(a.size)
    if L % ss:
        raise Refuse("size is not a whole number of sectors")
    L //= ss
    ls = (ud["end"] + 1 - L) // al * al
    if ls - ud["start"] < parse_size(a.min_userdata) // ss:
        raise Refuse(f"userdata would be {human((ls - ud['start']) * ss)}, less than {a.min_userdata}")
    free = [e for e in ents if not e["used"]]
    grown = False
    if not free:
        # The Titan 2 GPT has exactly as many entries as partitions (77/77). One more entry
        # fits if both arrays keep their sector count (77 and 78 entries of 128 B both take
        # 3 sectors of 4 KiB): only NumberOfPartitionEntries in both headers changes.
        if -(-(p.n + 1) * p.esz // ss) != p.arr_sectors:
            raise Refuse("no free entry slot, and one more entry needs another array sector")
        p.n += 1
        b.n += 1
        arr += b"\0" * p.esz
        ents = entries(arr, p.n, p.esz)
        free = [ents[-1]]
        grown = True
        print(f"no free entry slot: entry array grown to {p.n} entries (same {p.arr_sectors} sectors)")
    if not 0 < len(a.name) <= 36:
        raise Refuse("bad name")
    slot = free[0]
    new_ud_end = ls - 1
    struct.pack_into("<Q", arr, ud["i"] * p.esz + 40, new_ud_end)
    name = a.name.encode("utf-16-le").ljust(72, b"\0")
    others = {e["guid"] for e in used} | {p.guid}
    while True:
        g = uuid.uuid4().bytes_le
        if g not in others:
            break
    o = slot["i"] * p.esz
    arr[o:o + p.esz] = b"\0" * p.esz
    ENT.pack_into(arr, o, LINUX_TYPE.bytes_le, g, ls, ls + L - 1, 0, name)

    new = entries(arr, p.n, p.esz)
    check_layout(new, p)
    for x, y in zip(ents, new):
        if x["i"] not in (ud["i"], slot["i"]) and x != y:
            raise Refuse(f"internal: entry {x['i']} changed")
    if grown and len(new) != p.n:
        raise Refuse("internal: entry count")
    print("after:")
    table(new, ss, names)
    print(f"{a.name}: entry {slot['i'] + 1}, GUID {uuid.UUID(bytes_le=g)}, "
          f"gap before {after[0]['name'] if after else 'end'}: "
          f"{human((ud['end'] - (ls + L - 1)) * ss)}")

    crc = zlib.crc32(arr)
    pad = bytes(p.arr_sectors * ss - len(arr))
    hs = p.ents_lba + p.arr_sectors
    nh = bytearray(head[:hs * ss])
    nh[ss:2 * ss] = p.pack(crc)
    nh[p.ents_lba * ss:hs * ss] = bytes(arr) + pad
    # the phone has 4 unused sectors between the backup array and the backup header: they are
    # written back unchanged
    gap = N - 1 - b.ents_lba - b.arr_sectors
    if gap < 0:
        raise Refuse("backup array overlaps the backup header")
    nt = bytes(arr) + pad + (sec(b.ents_lba + b.arr_sectors, gap) if gap else b"") + b.pack(crc)
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, "head.bin"), "wb").write(nh)
    open(os.path.join(a.out, "tail.bin"), "wb").write(nt)
    with open(os.path.join(a.out, "carve.env"), "w") as f:
        f.write(f"SECTOR_SIZE={ss}\nHEAD_SECTORS={hs}\nTAIL_LBA={b.ents_lba}\n"
                f"TAIL_SECTORS={N - b.ents_lba}\nLINUX_ENTRY={slot['i'] + 1}\n"
                f"LINUX_GUID={uuid.UUID(bytes_le=g)}\nENTRIES={p.n}\n")
    print(f"written: {a.out}/head.bin (LBA 0..{hs - 1}), tail.bin (LBA {b.ents_lba}..{N - 1})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
