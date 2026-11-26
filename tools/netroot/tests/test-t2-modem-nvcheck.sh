#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# t2-modem-nvcheck against a fake NVRAM sandbox (no phone, no root): the same file is left
# alone; a differing file is quarantined and logged with the CRC-32 of its record units;
# a file without an ro/ copy is left alone with a warning; T2_MODEM_NVCHECK=no does nothing;
# a second run does nothing more; the quarantine is bounded.
#
#   sh tools/netroot/tests/test-t2-modem-nvcheck.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
check=$here/../t2-modem-nvcheck
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

export T2_MODEM_NVCHECK_NOCONF=1 T2_MODEM_NVCHECK_SYSLOG=no
export T2_MODEM_ROOT=$tmp/sandbox T2_MODEM_LOG=$tmp/log
d=nvdata/md/NVRAM/NVD_DATA
f=$d/D2A0_007
fails=0

ok() { echo "ok - $*"; }
fail() { echo "not ok - $*"; fails=$((fails + 1)); }
expect() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

# zlib's CRC-32 of a byte range, for the expected log text
crc() { python3 -I -c 'import sys, zlib; d = open(sys.argv[1], "rb").read(); o, n = int(sys.argv[2]), int(sys.argv[3]); print("%08x" % zlib.crc32(d[o:o + n]))' "$@"; }

reset() {
	rm -rf "$T2_MODEM_ROOT" "$T2_MODEM_LOG"
	mkdir -p "$T2_MODEM_ROOT/ro/$d" "$T2_MODEM_ROOT/rw/$d"
	# a 2432-byte file: 192-byte header, two 1120-byte record units
	head -c 2432 /dev/urandom >"$T2_MODEM_ROOT/ro/$f"
}

# 1. the same file: left in place
reset
cp "$T2_MODEM_ROOT/ro/$f" "$T2_MODEM_ROOT/rw/$f"
out=$(sh "$check")
expect "same file left in place" '[ -f "$T2_MODEM_ROOT/rw/$f" ] && [ ! -d "$T2_MODEM_ROOT/rw.quarantine" ]'
expect "same file logged" 'echo "$out" | grep -q "D2A0_007: same as ro/, left in place"'

# 2. a differing file: quarantined, CRC-32 of both units of both copies logged
reset
cp "$T2_MODEM_ROOT/ro/$f" "$T2_MODEM_ROOT/rw/$f"
printf 'B' | dd of="$T2_MODEM_ROOT/rw/$f" bs=1 seek=1400 conv=notrunc 2>/dev/null
cp "$T2_MODEM_ROOT/rw/$f" "$tmp/bad"
out=$(sh "$check")
q=$(ls -1 "$T2_MODEM_ROOT/rw.quarantine")
expect "differing file moved out of rw/" '[ ! -e "$T2_MODEM_ROOT/rw/$f" ]'
expect "differing file kept in the dated quarantine" 'cmp -s "$T2_MODEM_ROOT/rw.quarantine/$q/$f" "$tmp/bad"'
expect "quarantine directory name is a date" 'echo "$q" | grep -Eq "^[0-9]{8}-[0-9]{6}$"'
expect "quarantine mode 0700" '[ "$(stat -c %a "$T2_MODEM_ROOT/rw.quarantine")" = 700 ]'
expect "ro/ untouched" '[ -f "$T2_MODEM_ROOT/ro/$f" ] && [ "$(wc -c <"$T2_MODEM_ROOT/ro/$f")" -eq 2432 ]'
want="rw/ @0xc0=$(crc "$tmp/bad" 192 1120) @0x520=$(crc "$tmp/bad" 1312 1120); ro/ @0xc0=$(crc "$T2_MODEM_ROOT/ro/$f" 192 1120) @0x520=$(crc "$T2_MODEM_ROOT/ro/$f" 1312 1120)"
expect "unit CRC-32s logged" 'echo "$out" | grep -qF "$want"'
expect "only the changed unit differs" '[ "$(crc "$tmp/bad" 192 1120)" = "$(crc "$T2_MODEM_ROOT/ro/$f" 192 1120)" ]'
expect "log file written" 'grep -q "moved to rw.quarantine/$q/$f" "$T2_MODEM_LOG/nvcheck.log"'

# 3. a second run does nothing more
out=$(sh "$check")
expect "second run: nothing moved" '[ "$(ls -1 "$T2_MODEM_ROOT/rw.quarantine" | wc -l)" -eq 1 ] && [ -z "$out" ]'

# 4. no ro/ copy: left in place with a warning
reset
rm "$T2_MODEM_ROOT/ro/$f"
head -c 2432 /dev/urandom >"$T2_MODEM_ROOT/rw/$f"
out=$(sh "$check")
expect "no ro/ copy: left in place" '[ -f "$T2_MODEM_ROOT/rw/$f" ] && [ ! -d "$T2_MODEM_ROOT/rw.quarantine" ]'
expect "no ro/ copy: warning" 'echo "$out" | grep -q "no ro/ copy to compare with, rw/ copy left in place"'

# 5. switched off: nothing
reset
head -c 2432 /dev/urandom >"$T2_MODEM_ROOT/rw/$f"
out=$(T2_MODEM_NVCHECK=no sh "$check")
expect "off: differing file left in place" '[ -f "$T2_MODEM_ROOT/rw/$f" ] && [ ! -d "$T2_MODEM_ROOT/rw.quarantine" ]'
expect "off: logged" 'echo "$out" | grep -q "off (T2_MODEM_NVCHECK=no)"'

# 6. no rw/ copy: nothing, silently
reset
out=$(sh "$check")
expect "no rw/ copy: nothing" '[ -z "$out" ] && [ ! -d "$T2_MODEM_ROOT/rw.quarantine" ]'

# 7. a further file from the list without a unit layout: whole-file CRC-32
reset
printf 'x' >"$T2_MODEM_ROOT/ro/$d/OTHER_001"
printf 'y' >"$T2_MODEM_ROOT/rw/$d/OTHER_001"
out=$(T2_MODEM_NVCHECK_FILES="$f:192:1120:2 $d/OTHER_001" sh "$check")
expect "listed file without layout quarantined" '[ ! -e "$T2_MODEM_ROOT/rw/$d/OTHER_001" ]'
expect "whole-file CRC-32 logged" 'echo "$out" | grep -qF "rw/ file=$(crc "$tmp/sandbox/rw.quarantine/$(ls -1 "$T2_MODEM_ROOT/rw.quarantine")/$d/OTHER_001" 0 1)"'

# 8. bounded: only the newest T2_MODEM_QUARANTINE_KEEP dated directories stay
reset
mkdir -p "$T2_MODEM_ROOT/rw.quarantine/20260101-000000" "$T2_MODEM_ROOT/rw.quarantine/20260102-000000"
head -c 2432 /dev/urandom >"$T2_MODEM_ROOT/rw/$f"
out=$(T2_MODEM_QUARANTINE_KEEP=2 sh "$check")
expect "bounded: oldest removed" '[ ! -d "$T2_MODEM_ROOT/rw.quarantine/20260101-000000" ] && [ -d "$T2_MODEM_ROOT/rw.quarantine/20260102-000000" ]'
expect "bounded: two kept" '[ "$(ls -1 "$T2_MODEM_ROOT/rw.quarantine" | wc -l)" -eq 2 ]'

# 9. the same under busybox sh, if present (the phone's shell)
if command -v busybox >/dev/null; then
	reset
	head -c 2432 /dev/urandom >"$T2_MODEM_ROOT/rw/$f"
	out=$(busybox sh "$check")
	expect "busybox sh: differing file quarantined" '[ ! -e "$T2_MODEM_ROOT/rw/$f" ] && echo "$out" | grep -q "moved to rw.quarantine"'
fi

[ "$fails" -eq 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
