#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Test of scripts/gpt-carve.py without the phone. A sparse disk image like sdc (UFS LU2):
# 4 KiB sectors, 77 entries (as on the phone: exactly as many as partitions), 77 partitions sdc1..sdc77 with the exact layout of
# scripts/testdata/titan2-sdc-layout.txt (the real starts and sizes of the sdc partitions). Read
# the start and end as dd does on the phone, carve, write back, check with sfdisk.
#   scripts/test-gpt-carve.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
O=$ROOT/out/test-gpt-carve
SS=4096
rm -rf "$O"
mkdir -p "$O"
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" titan2-kbuild "$@"; }
w() { echo "/work${1#$ROOT}"; }
fails=0
ok() { echo "PASS: $*"; }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }

# the real sdc layout (read on the phone from mainline, 2026-10-24): scripts/testdata/titan2-sdc-layout.txt,
# starts and sizes in 512-byte sectors
L=$ROOT/scripts/testdata/titan2-sdc-layout.txt
DISK_BYTES=$(($(sed -n 's/.*size512=\([0-9]*\).*/\1/p' "$L" | head -1) * 512))
N=$((DISK_BYTES / SS))
IMG=$O/sdc.img
truncate -s "$DISK_BYTES" "$IMG"
{
	echo "label: gpt"
	echo "table-length: 77"
	echo "sector-size: $SS"
	echo "first-lba: 8"
	awk '$1 ~ /^sdc[0-9]+$/ { sub(/start512=/, "", $3); sub(/size512=/, "", $4); print $3, $4, $2 }' "$L" |
		while read -r st sz name; do
			echo "start=$((st * 512 / SS)), size=$((sz * 512 / SS)), type=0FC63DAF-8483-4772-8E47-3D69D8477DE4, name=\"$name\""
		done
} | sfdisk -q --sector-size $SS --no-reread "$IMG" 2>"$O/sfdisk.log" || { cat "$O/sfdisk.log"; exit 1; }
sfdisk --sector-size $SS -d "$IMG" >"$O/before.dump"
[ "$(grep -c 'name=' "$O/before.dump")" = 77 ] && ok "image: 77 partitions" || bad "image"

grab() {
	dd if="$IMG" of="$O/head.bin" bs=$SS count=64 status=none
	dd if="$IMG" of="$O/tail.bin" bs=$SS skip=$((N - 64)) count=64 status=none
}
carve() {
	run python3 -I /work/scripts/gpt-carve.py --head "$(w "$O/head.bin")" --tail "$(w "$O/tail.bin")" \
		--disk-sectors $N --sector-size $SS "$@"
}

grab
cp "$O/head.bin" "$O/orig-head.bin"
cp "$O/tail.bin" "$O/orig-tail.bin"
carve --check >"$O/check.log" 2>&1 && ok "--check" || { cat "$O/check.log"; bad "--check"; }
carve --out "$(w "$O/new")" | tee "$O/carve.log" || bad "carve"
. "$O/new/carve.env"
dd if="$O/new/head.bin" of="$IMG" bs=$SS count="$HEAD_SECTORS" conv=notrunc status=none
dd if="$O/new/tail.bin" of="$IMG" bs=$SS seek="$TAIL_LBA" count="$TAIL_SECTORS" conv=notrunc status=none
sfdisk --sector-size $SS --verify "$IMG" >"$O/verify.log" 2>&1 && grep -q "^No errors detected" "$O/verify.log" &&
	ok "sfdisk --verify" || { cat "$O/verify.log"; bad "sfdisk --verify"; }
sfdisk --sector-size $SS -d "$IMG" >"$O/after.dump"
sfdisk --sector-size $SS -J "$IMG" >"$O/after.json"

# all partitions except userdata and linux, and the header: unchanged
# (table-length: 77 -> 78: the entry array grew by one entry in the same sectors)
grep -v 'name="userdata"\|^table-length:' "$O/before.dump" >"$O/b.cmp"
grep -v 'name="userdata"\|name="linux"\|^table-length:' "$O/after.dump" >"$O/a.cmp"
diff -u "$O/b.cmp" "$O/a.cmp" && ok "the other 76 partitions and the header unchanged" || bad "other partitions changed"
grep -q '^table-length: 78$' "$O/after.dump" && ok "GPT entries: 78" || bad "table-length"
p() { jq -r --arg n "$1" ".partitiontable.partitions[] | select(.name == \$n) | .$2" "$3"; }
ud_start0=$(awk -F'[=,]' '/name="userdata"/ {gsub(/ /, "", $2); print $2}' "$O/before.dump")
ud_size0=$(awk -F'[=,]' '/name="userdata"/ {gsub(/ /, "", $4); print $4}' "$O/before.dump")
ud_end0=$((ud_start0 + ud_size0 - 1))
ud_start=$(p userdata start "$O/after.json")
ud_size=$(p userdata size "$O/after.json")
l_start=$(p linux start "$O/after.json")
l_size=$(p linux size "$O/after.json")
l_type=$(p linux type "$O/after.json")
[ "$ud_start" = "$ud_start0" ] && ok "userdata: same start ($ud_start)" || bad "userdata: start $ud_start != $ud_start0"
[ "$l_size" = $((256 << 30 >> 12)) ] && ok "linux: exactly 256 GiB" || bad "linux: size $l_size"
[ $((l_start % 256)) = 0 ] && ok "linux: start aligned to 1 MiB" || bad "linux: start $l_start"
[ $((ud_start + ud_size)) = "$l_start" ] && ok "userdata ends right before linux" || bad "userdata/linux boundary"
[ $((l_start + l_size - 1)) -le $ud_end0 ] && ok "linux not beyond the old end of userdata" || bad "linux overruns"
[ "$l_type" = 0FC63DAF-8483-4772-8E47-3D69D8477DE4 ] && ok "linux: type Linux filesystem" || bad "type $l_type"
echo "userdata: $((ud_size * SS >> 30)) GiB (was $((ud_size0 * SS >> 30)))"
[ "$(jq '.partitiontable.partitions | length' "$O/after.json")" = 78 ] && ok "78 partitions" || bad "number of partitions"
[ "$(jq -r '.partitiontable.partitions[77].name' "$O/after.json")" = linux ] && ok "linux is entry 78" || bad "linux entry"
# the backup table equals the primary: sfdisk reads it too; compare the arrays byte by byte
cmp <(dd if="$IMG" bs=$SS skip=2 count=$((HEAD_SECTORS - 2)) status=none) \
	<(dd if="$IMG" bs=$SS skip="$TAIL_LBA" count=$((HEAD_SECTORS - 2)) status=none) &&
	ok "backup array = primary" || bad "arrays differ"

# second run: refused with "already done"
grab
if carve --out "$(w "$O/again")" >"$O/again.log" 2>&1; then bad "second run passed"
else grep -q "already done" "$O/again.log" && ok "second run: already done" || { cat "$O/again.log"; bad "second run"; }; fi

# broken CRC (the primary array, and separately the backup header): refused
cp "$O/head.bin" "$O/head.good"
cp "$O/tail.bin" "$O/tail.good"
printf '\x01' | dd of="$O/head.bin" bs=1 seek=$((2 * SS + 100)) conv=notrunc status=none
if carve --check >"$O/crc1.log" 2>&1; then bad "broken array accepted"
else grep -q "CRC mismatch" "$O/crc1.log" && ok "broken array CRC: refused" || { cat "$O/crc1.log"; bad "crc1"; }; fi
cp "$O/head.good" "$O/head.bin"
printf '\x01' | dd of="$O/tail.bin" bs=1 seek=$((63 * SS + 60)) conv=notrunc status=none
if carve --check >"$O/crc2.log" 2>&1; then bad "broken backup header accepted"
else grep -q "CRC mismatch" "$O/crc2.log" && ok "broken backup header CRC: refused" || { cat "$O/crc2.log"; bad "crc2"; }; fi
cp "$O/tail.good" "$O/tail.bin"
# primary and backup GPT differ (each with a valid CRC): the new primary + the original backup
cp "$O/orig-tail.bin" "$O/tail.bin"
if carve --check >"$O/differ.log" 2>&1; then bad "differing copies accepted"
else grep -q "differ" "$O/differ.log" && ok "differing primary and backup: refused" || { cat "$O/differ.log"; bad "differ"; }; fi

# userdata below 64 GiB after the carve: refused (on the original GPT, linux 450 GiB)
cp "$O/orig-head.bin" "$O/head.bin"
cp "$O/orig-tail.bin" "$O/tail.bin"
if carve --size 450GiB --out "$(w "$O/big")" >"$O/small.log" 2>&1; then bad "too large linux accepted"
else grep -q "less than" "$O/small.log" && ok "userdata < 64 GiB: refused" || { cat "$O/small.log"; bad "small"; }; fi

# End-to-end run of scripts/dualboot-repartition.sh with a fake adb ("TWRP" on top of the original
# image): run, then restore; the GPT comes back byte for byte, neighbouring partitions untouched.
dd if="$O/orig-head.bin" of="$IMG" bs=$SS count=6 conv=notrunc status=none
dd if="$O/orig-tail.bin" of="$IMG" bs=$SS seek=$((N - 5)) skip=59 count=5 conv=notrunc status=none
printf 'MISC-SENTINEL' | dd of="$IMG" bs=$SS seek=6 conv=notrunc status=none
FA=$O/fake-adb
mkdir -p "$O/fake-tmp"
cat >"$FA" <<EOF
#!/usr/bin/env bash
# fake adb for test-gpt-carve.sh: phone in TWRP, /dev/block/sdc = the image
IMG=$IMG; TMP=$O/fake-tmp; BYTES=$DISK_BYTES
EOF
cat >>"$FA" <<'EOF'
map() { sed "s#/dev/block/sdc#$IMG#g; s#/tmp/t2-gpt.bin#$TMP/t2-gpt.bin#g"; }
case "$1" in
get-state) echo recovery ;;
push) cp "$2" "$TMP/t2-gpt.bin" ;;
exec-out) shift; eval "$(echo "$*" | map)" ;;
shell)
	shift
	c="$*"
	case "$c" in
	"getprop ro.twrp.version") echo 3.7.1_12-0 ;;
	"readlink -f /dev/block/by-name/userdata") echo /dev/block/sdc75 ;;
	"ls /sys/class/block/sdc75/holders") ;;
	"cat /sys/block/sdc/queue/logical_block_size") echo 4096 ;;
	"cat /sys/block/sdc/size") echo $((BYTES / 512)) ;;
	"cat /proc/mounts") echo "tmpfs /tmp tmpfs rw 0 0" ;;
	umount*|"cat /proc/partitions"*) ;;
	dd*) eval "$(echo "$c" | map)" ;;
	*) echo "fake-adb: unknown: $c" >&2; exit 1 ;;
	esac
	;;
*) echo "fake-adb: unknown: $*" >&2; exit 1 ;;
esac
EOF
chmod +x "$FA"
md5_before=$(dd if="$IMG" bs=$SS count=4096 status=none | md5sum)
sfdisk --sector-size $SS -d "$IMG" >"$O/e2e-before.dump"
rm -rf "$O/bk" "$O/bk2"
if CONFIRM=yes ADB=$FA BACKUP=$O/bk "$ROOT/scripts/dualboot-repartition.sh" run >"$O/e2e-run.log" 2>&1; then
	sfdisk --sector-size $SS -d "$IMG" | grep -q 'name="linux"' && ok "dualboot-repartition run: linux present" ||
		bad "run: no linux"
else cat "$O/e2e-run.log"; bad "dualboot-repartition run"; fi
[ "$(dd if="$IMG" bs=$SS skip=6 count=1 status=none | head -c 13)" = MISC-SENTINEL ] &&
	ok "run: the first partition after the GPT untouched" || bad "run: LBA 6 overwritten"
sfdisk --sector-size $SS --verify "$IMG" 2>&1 | grep -q "^No errors detected" && ok "run: sfdisk --verify" || bad "run: verify"
BK=$(ls -d "$O"/bk/*-pre-repartition | head -1)
printf 'MISC-CHANGED!' | dd of="$IMG" bs=$SS seek=6 conv=notrunc status=none
if CONFIRM=yes ADB=$FA "$ROOT/scripts/dualboot-repartition.sh" restore "$BK" >"$O/e2e-restore.log" 2>&1; then
	sfdisk --sector-size $SS -d "$IMG" | diff -q - "$O/e2e-before.dump" >/dev/null &&
		ok "restore: table as before run" || bad "restore: table differs"
else cat "$O/e2e-restore.log"; bad "dualboot-repartition restore"; fi
[ "$(dd if="$IMG" bs=$SS skip=6 count=1 status=none | head -c 13)" = MISC-CHANGED! ] &&
		ok "restore: writes GPT sectors only" || bad "restore: overwrote partition data"
printf 'MISC-SENTINEL' | dd of="$IMG" bs=$SS seek=6 conv=notrunc status=none
[ "$(dd if="$IMG" bs=$SS count=4096 status=none | md5sum)" = "$md5_before" ] &&
		ok "restore: start of the disk byte for byte" || bad "restore: start of the disk differs"
if echo no | ADB=$FA BACKUP=$O/bk2 "$ROOT/scripts/dualboot-repartition.sh" run >"$O/e2e-no.log" 2>&1; then
	bad "run without yes succeeded"
elif sfdisk --sector-size $SS -d "$IMG" | grep -q 'name="linux"'; then bad "run without yes: linux appeared"
else ok "run without yes: nothing written"; fi

echo "== result: $fails FAIL"
[ "$fails" = 0 ]
