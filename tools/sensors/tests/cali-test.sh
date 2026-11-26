#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# t2-sensors-cali against a fake sysfs, a fake local record directory and a
# fake nvcfg (a directory, and a fake debugfs that "reads" it): the import
# rule, the boot load (word lists, missing/short/malformed records, missing
# devices) and the write-back (event mapping, status check, byte format,
# atomic rewrite, rate limit, flush on SIGUSR1/SIGTERM). No phone, no root.
#
#   sh tests/cali-test.sh ./t2-sensors-cali      (make test: the sanitizer build)

set -u
BIN=${1:-./t2-sensors-cali}
case $BIN in /*) ;; *) BIN=$PWD/$BIN ;; esac
T=$(mktemp -d)
PID=
trap 'exec 3>&- 2>/dev/null; [ -n "$PID" ] && kill $PID 2>/dev/null; rm -rf "$T"' EXIT INT TERM
FAILS=0
PASSES=0
ok() { PASSES=$((PASSES + 1)); }
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }

# the record text exactly as the other system writes it (no final newline)
rec() {	# name v...
	local name=$1 sep=
	shift
	printf '{\n  "%s": [\n' "$name"
	for v; do printf '%s      %s' "$sep" "$v"; sep=',
'; done
	printf '\n    ]\n}'
}

S=$T/sys/bus/iio/devices
D=$T/state/cali		# local records
SNAP=$T/state/nvcfg	# what the last import took
NV=$T/nvcfg		# a fake nvcfg file system: NV/sensor, NV/fg
mkdev() {	# index name
	mkdir -p "$S/iio:device$1"
	echo "$2" >"$S/iio:device$1/name"
	: >"$S/iio:device$1/hub_cali"
}
fresh_sys() {
	rm -rf "$T/sys"
	mkdev 0 hub-prox; mkdev 1 hub-accel; mkdev 2 hub-light; mkdev 3 hub-magn; mkdev 4 hub-gyro
	mkdev 5 mt6685-auxadc
}
phone_set() {	# dir: the record set of a phone (no acc_bias, gyro_temp, als_cali)
	mkdir -p "$1"
	rec acc_cali -1341 743 1516 >"$1/acc_cali.json"
	rec gyro_cali 42146 40676 -14498 >"$1/gyro_cali.json"
	rec gyro_bias 14278 19849 -15656 >"$1/gyro_bias.json"
	rec mag_bias -180725 61053 4012 >"$1/mag_bias.json"
	rec mag_cali -180725 61053 4012 3000 0 0 >"$1/mag_cali.json"
	rec ps_cali 5644 4644 >"$1/ps_cali.json"
}
fresh_dir() {	# local records = the phone set
	rm -rf "$T/state"
	phone_set "$D"
}
cali() { cat "$S/iio:device$1/hub_cali"; }
expect_cali() {	# index expected label
	local got
	got=$(cali "$1")
	[ "$got" = "$2" ] && ok || fail "$3: hub_cali of device $1 is '$got', expected '$2'"
}
load() { "$BIN" load -d "$D" -s "$T/sys" "$@" 2>>"$T/log"; }

echo "== load: the record set of a phone"
fresh_sys; fresh_dir
load -w 1; rc=$?
[ $rc = 0 ] && ok || fail "load exit $rc"
expect_cali 1 "0 0 0 -1341 743 1516" accel
expect_cali 4 "14278 19849 -15656 42146 40676 -14498 0 0 0 0 0 0" gyro
expect_cali 3 "-180725 61053 4012 -180725 61053 4012 3000 0 0" magn
expect_cali 2 "0" light
expect_cali 0 "5644 4644" prox
[ -z "$(cat "$S/iio:device5/hub_cali")" ] && ok || fail "a foreign IIO device was written"
# exactly one line per attribute, newline-terminated
[ "$(tail -c1 "$S/iio:device0/hub_cali" | od -An -c | tr -d ' ')" = '\n' ] && ok || fail "hub_cali text without a final newline"
[ "$(wc -l <"$S/iio:device4/hub_cali")" = 1 ] && ok || fail "hub_cali text is not one line"

echo "== load: malformed, short and odd records"
case_rec() {	# description file-content expected-prox-words
	fresh_sys
	printf '%s' "$2" >"$D/ps_cali.json"
	load -w 1
	expect_cali 0 "$3" "$1"
}
fresh_dir
case_rec "valid, compact" '{"ps_cali":[1,2]}' "1 2"
case_rec "valid, extra elements" '{"ps_cali":[1,2,3,4]}' "1 2"
case_rec "valid, other keys first" '{"x":{"a":[1,{"b":null}]},"ps_cali":[7,8],"y":"s\"té"}' "7 8"
case_rec "valid, extra non-integers after N" '{"ps_cali":[7,8,1.5,"a",true]}' "7 8"
case_rec "valid, CRLF and tabs" "$(printf '{\r\n\t"ps_cali" :\t[ -5 , 6 ]\r\n}\r\n')" "-5 6"
case_rec "valid, int32 limits" '{"ps_cali":[-2147483648,2147483647]}' "-2147483648 2147483647"
case_rec "duplicate key: the last counts" '{"ps_cali":[1,2],"ps_cali":[3,4]}' "3 4"
case_rec "short array" '{"ps_cali":[5644]}' "0 0"
case_rec "empty array" '{"ps_cali":[]}' "0 0"
case_rec "fraction" '{"ps_cali":[5644.0,4644]}' "0 0"
case_rec "exponent" '{"ps_cali":[1e3,4644]}' "0 0"
case_rec "out of int32" '{"ps_cali":[2147483648,1]}' "0 0"
case_rec "string element" '{"ps_cali":["5644",4644]}' "0 0"
case_rec "not an array" '{"ps_cali":5644}' "0 0"
case_rec "wrong key" '{"ps_calib":[1,2]}' "0 0"
case_rec "escaped key text matches" '{"ps_cali":[1,2]}' "1 2"
case_rec "truncated" '{"ps_cali":[1,2' "0 0"
case_rec "trailing garbage" '{"ps_cali":[1,2]}x' "0 0"
case_rec "two objects" '{"ps_cali":[1,2]}{}' "0 0"
case_rec "top level array" '[1,2]' "0 0"
case_rec "leading zero" '{"ps_cali":[01,2]}' "0 0"
case_rec "trailing comma" '{"ps_cali":[1,2,]}' "0 0"
case_rec "bare word" '{"ps_cali":[1,2],"z":nul}' "0 0"
case_rec "empty file" '' "0 0"
case_rec "binary junk" "$(printf '\001\002\377')" "0 0"
deep=$(awk 'BEGIN { for (i = 0; i < 40; i++) printf "["; for (i = 0; i < 40; i++) printf "]" }')
case_rec "nesting too deep" "{\"z\":$deep,\"ps_cali\":[1,2]}" "0 0"
fresh_sys
rm -f "$D/ps_cali.json"; ln -s ../../elsewhere.json "$D/ps_cali.json"; rec ps_cali 1 2 >"$T/elsewhere.json"
load -w 1
expect_cali 0 "0 0" "symlink is not followed"
fresh_sys
rm -f "$D/ps_cali.json"; mkdir "$D/ps_cali.json"
load -w 1
expect_cali 0 "0 0" "directory instead of a file"
rmdir "$D/ps_cali.json"
fresh_sys
awk 'BEGIN { printf "{\"ps_cali\":[1,2],\"pad\":\""; for (i = 0; i < 70000; i++) printf "x"; printf "\"}" }' >"$D/ps_cali.json"
load -w 1
expect_cali 0 "0 0" "file larger than 64 KiB"
# one bad record leaves the others of the same sensor alone
fresh_sys; fresh_dir
printf '{"gyro_bias":[1,2]}' >"$D/gyro_bias.json"
load -w 1
expect_cali 4 "0 0 0 42146 40676 -14498 0 0 0 0 0 0" "short gyro_bias, gyro_cali kept"

echo "== load: no records, no devices, some devices"
fresh_sys; rm -rf "$T/state"
load -w 1; rc=$?
[ $rc = 0 ] && ok || fail "load without a record directory: exit $rc"
expect_cali 4 "0 0 0 0 0 0 0 0 0 0 0 0" "no directory: gyro zeros"
expect_cali 1 "0 0 0 0 0 0" "no directory: accel zeros"
rm -rf "$T/sys"; mkdir -p "$S"
load -w 0; rc=$?
[ $rc = 2 ] && ok || fail "no hub devices: exit $rc, expected 2"
fresh_dir
rm -rf "$T/sys"; mkdev 1 hub-accel; mkdev 4 hub-gyro
t0=$(date +%s)
load -w 30; rc=$?
t1=$(date +%s)
[ $rc = 0 ] && ok || fail "two devices only: exit $rc"
[ $((t1 - t0)) -le 5 ] && ok || fail "two devices only: waited $((t1 - t0)) s for the missing ones"
expect_cali 1 "0 0 0 -1341 743 1516" "two devices: accel"
# a device that refuses: exit 1, the others are still sent
fresh_sys
rm "$S/iio:device4/hub_cali"; mkdir "$S/iio:device4/hub_cali"
load -w 1; rc=$?
[ $rc = 1 ] && ok || fail "refused write: exit $rc, expected 1"
expect_cali 0 "5644 4644" "after a refused gyro: prox"
# dry run prints, sends nothing
fresh_sys
out=$("$BIN" load -n -d "$D" -s "$T/sys" 2>>"$T/log")
[ "$out" = "hub-accel 0 0 0 -1341 743 1516
hub-gyro 14278 19849 -15656 42146 40676 -14498 0 0 0 0 0 0
hub-magn -180725 61053 4012 -180725 61053 4012 3000 0 0
hub-light 0
hub-prox 5644 4644" ] && ok || fail "dry run printed: $out"
[ -z "$(cali 1)" ] && ok || fail "dry run wrote hub_cali"

echo "== import: the rule (from a directory)"
imp() { "$BIN" import -d "$D" -N "$SNAP" "$@" 2>>"$T/log"; }
rm -rf "$T/state" "$NV"
phone_set "$NV/sensor"
mkdir -p "$NV/fg"; echo keep >"$NV/fg/old_fg_data"
imp -f "$NV/sensor"; rc=$?
[ $rc = 0 ] && ok || fail "first import: exit $rc"
for f in acc_cali gyro_cali gyro_bias mag_bias mag_cali ps_cali; do
	cmp -s "$NV/sensor/$f.json" "$D/$f.json" && cmp -s "$NV/sensor/$f.json" "$SNAP/$f.json" && ok ||
		fail "first import: $f.json not copied byte for byte"
done
[ ! -e "$D/acc_bias.json" ] && [ ! -e "$D/als_cali.json" ] && ok || fail "import created a record nvcfg does not have"
[ "$(ls -A "$D" | wc -l)" = 6 ] && ok || fail "import left files behind: $(ls -A "$D")"
# values learned here survive an import when nvcfg did not change
rec gyro_bias 1 2 3 >"$D/gyro_bias.json"
imp -f "$NV/sensor"
same() {	# file expected-text label
	if [ "$(od -An -tx1 "$1")" = "$(printf '%s' "$2" | od -An -tx1)" ]; then ok; else
		fail "$3: $(basename "$1") is"; od -c "$1" >&2; fi
}
same "$D/gyro_bias.json" "$(rec gyro_bias 1 2 3)" "local value kept, nvcfg unchanged"
# the other system wrote nvcfg: that record (only) is taken
rec mag_bias 7 8 9 >"$D/mag_bias.json"
rec gyro_bias 4 5 6 >"$NV/sensor/gyro_bias.json"
imp -f "$NV/sensor"
same "$D/gyro_bias.json" "$(rec gyro_bias 4 5 6)" "nvcfg changed: taken"
same "$SNAP/gyro_bias.json" "$(rec gyro_bias 4 5 6)" "nvcfg changed: snapshot updated"
same "$D/mag_bias.json" "$(rec mag_bias 7 8 9)" "other local record kept"
# an unusable nvcfg record never replaces the local one, even forced
printf '{"ps_cali":[1]}' >"$NV/sensor/ps_cali.json"
imp -f "$NV/sensor" -F
same "$D/ps_cali.json" "$(rec ps_cali 5644 4644)" "short nvcfg record not imported"
# forced: every usable record taken
same "$D/mag_bias.json" "$(rec mag_bias -180725 61053 4012)" "-F takes the nvcfg record"
# a local copy that went missing comes back; a record gone from nvcfg leaves the local one
rec ps_cali 5644 4644 >"$NV/sensor/ps_cali.json"
rm "$D/gyro_cali.json" "$NV/sensor/acc_cali.json"
imp -f "$NV/sensor"
same "$D/gyro_cali.json" "$(rec gyro_cali 42146 40676 -14498)" "missing local copy restored"
same "$D/acc_cali.json" "$(rec acc_cali -1341 743 1516)" "record gone from nvcfg: local kept"
# a record that appears on nvcfg after the last import is taken over a local one
rec gyro_temp 9 9 9 9 9 9 >"$D/gyro_temp.json"
rec gyro_temp 1 1 1 1 1 1 >"$NV/sensor/gyro_temp.json"
imp -f "$NV/sensor"
same "$D/gyro_temp.json" "$(rec gyro_temp 1 1 1 1 1 1)" "new on nvcfg: taken"
rm "$NV/sensor/gyro_temp.json" "$D/gyro_temp.json" "$SNAP/gyro_temp.json"
[ "$(cat "$NV/fg/old_fg_data")" = keep ] && ok || fail "nvcfg fg/ touched"
"$BIN" import -d "$D" 2>>"$T/log"; rc=$?
[ $rc = 2 ] && ok || fail "import without a source: exit $rc"

echo "== import: through debugfs (a fake one)"
cat >"$T/fake-debugfs" <<'EOF2'
#!/bin/sh
# fake debugfs: debugfs -R "rdump /sensor DEST" DEVICE, DEVICE = a directory standing for the fs
echo "debugfs 0.0 (fake)" >&2
[ "$1" = -R ] || exit 1
set -- $2 "$3"
[ "$1" = rdump ] && [ -d "$4$2" ] || { echo "$4: Bad magic number in super-block while opening filesystem" >&2; exit 1; }
cp -r "$4$2" "$3/"
EOF2
chmod +x "$T/fake-debugfs"
rm -rf "$T/state"
imp -p "$NV" -X "$T/fake-debugfs"; rc=$?
[ $rc = 0 ] && ok || fail "debugfs import: exit $rc"
same "$D/gyro_bias.json" "$(rec gyro_bias 4 5 6)" "debugfs import"
# nvcfg holds 5 records by now (acc_cali was removed above)
[ "$(ls -A "$D" | wc -l)" = 5 ] && ok || fail "debugfs import left files behind: $(ls -A "$D")"
rec gyro_bias 1 2 3 >"$D/gyro_bias.json"
imp -p "$T/no-such-fs" -X "$T/fake-debugfs"; rc=$?
[ $rc = 1 ] && ok || fail "failing debugfs: exit $rc, expected 1"
same "$D/gyro_bias.json" "$(rec gyro_bias 1 2 3)" "failing debugfs leaves the local records"
grep -q "Bad magic" "$T/log" && ok || fail "the debugfs error is not logged"
[ "$(ls -A "$D" | wc -l)" = 5 ] && ok || fail "failing debugfs left files behind: $(ls -A "$D")"
imp -p "$NV" -X "$T/none/debugfs"; rc=$?
[ $rc = 1 ] && ok || fail "missing debugfs: exit $rc"

echo "== watch: events from a stream, flushed at its end"
ev() { "$BIN" event "$@"; }
fresh_dir
mt_mb=$(stat -c %y "$D/mag_bias.json")
sleep 0.05
{
	ev 4 bias 3 11 22 33
	ev 4 bias 3 12361 20581 -13694		# the last value of a stream counts
	ev 2 bias 3 -180725 61053 4012		# same as the file: no rewrite
	ev 2 cali 3 -170000 60000 5000 3000 0 0
	ev 1 cali 1 9 9 9			# failed calibration: not stored
	ev 1 cali 0 -1300 700 1500
	ev 4 cali 2 1 1 1			# failed
	ev 8 cali 0 6000 5000
	ev 4 temp 0 1 2 3 4 5 6
	ev 1 0 0 999 999 999			# a sample, not a result
	ev 5 bias 0 1				# no light bias record
} >"$T/events"
"$BIN" watch -d "$D" -e "$T/events" -i 600 -S 2>>"$T/log"; rc=$?
[ $rc = 0 ] && ok || fail "watch exit $rc"
same "$D/gyro_bias.json" "$(rec gyro_bias 12361 20581 -13694)" "gyro bias"
same "$D/mag_cali.json" "$(rec mag_cali -170000 60000 5000 3000 0 0)" "mag cali"
same "$D/acc_cali.json" "$(rec acc_cali -1300 700 1500)" "accel cali, status 0"
same "$D/gyro_cali.json" "$(rec gyro_cali 42146 40676 -14498)" "gyro cali kept after a failed run"
same "$D/ps_cali.json" "$(rec ps_cali 6000 5000)" "prox cali"
same "$D/gyro_temp.json" "$(rec gyro_temp 1 2 3 4 5 6)" "gyro temp created"
[ "$(tail -c1 "$D/gyro_bias.json")" = "}" ] && ok || fail "record ends with something else than }"
[ "$(stat -c %a "$D/gyro_bias.json")" = 644 ] && ok || fail "gyro_bias.json has mode $(stat -c %a "$D/gyro_bias.json")"
[ "$(stat -c %a "$D/gyro_temp.json")" = 644 ] && ok || fail "new gyro_temp.json has mode $(stat -c %a "$D/gyro_temp.json")"
[ "$(stat -c %y "$D/mag_bias.json")" = "$mt_mb" ] && ok || fail "mag_bias.json rewritten with the same value"
for f in acc_bias als_cali; do
	[ ! -e "$D/$f.json" ] && ok || fail "$f.json created without a hub report"
done
[ "$(ls -A "$D" | wc -l)" = 7 ] && ok || fail "files left in the record directory: $(ls -A "$D")"
# what was written loads back
fresh_sys
load -w 1
expect_cali 4 "12361 20581 -13694 42146 40676 -14498 1 2 3 4 5 6" "write-back loads back"
expect_cali 0 "6000 5000" "prox loads back"

echo "== watch: rate limit, SIGUSR1, SIGTERM (takes ~6 s)"
fresh_dir
mkfifo "$T/fifo"
"$BIN" watch -d "$D" -e "$T/fifo" -i 3 -S 2>>"$T/log" &
PID=$!
exec 3>"$T/fifo"
ev 4 bias 0 1 1 1 >&3
sleep 1
same "$D/gyro_bias.json" "$(rec gyro_bias 14278 19849 -15656)" "bias held back by the interval"
ev 1 cali 0 5 5 5 >&3
sleep 0.5
same "$D/acc_cali.json" "$(rec acc_cali 5 5 5)" "calibration result written at once"
sleep 2.2
same "$D/gyro_bias.json" "$(rec gyro_bias 1 1 1)" "bias written after the interval"
ev 4 bias 0 2 2 2 >&3
sleep 0.5
same "$D/gyro_bias.json" "$(rec gyro_bias 1 1 1)" "second bias held back"
kill -USR1 $PID
sleep 0.5
same "$D/gyro_bias.json" "$(rec gyro_bias 2 2 2)" "SIGUSR1 flushes"
ev 4 bias 0 3 3 3 >&3
sleep 0.5
kill -TERM $PID
wait $PID; rc=$?
PID=
exec 3>&-
[ $rc = 0 ] && ok || fail "watch after SIGTERM: exit $rc"
same "$D/gyro_bias.json" "$(rec gyro_bias 3 3 3)" "SIGTERM flushes"

echo "== watch: record directory missing"
"$BIN" watch -d "$T/none" -e /dev/null 2>>"$T/log"; rc=$?
[ $rc = 1 ] && ok || fail "watch without a directory: exit $rc"

echo "passed $PASSES, failed $FAILS"
[ -z "${SHOWLOG:-}" ] || cat "$T/log"
[ $FAILS = 0 ] || { echo "--- log"; cat "$T/log"; exit 1; }
