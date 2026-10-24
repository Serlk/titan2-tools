#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Dual boot (titan2-docs: docs/DUALBOOT.md): the linux partition in place of the end of userdata.
# The phone is in TWRP (adb). Only the GPT of the disk with userdata (UFS LU2, sdc) changes: the
# primary and backup headers and entry arrays; the partitions' data is not touched, except that
# userdata gets shorter (Android's data is wiped anyway in the next step, fastboot -w).
#   scripts/dualboot-repartition.sh check          read the GPT, check it, show it (writes nothing)
#   scripts/dualboot-repartition.sh run            save the GPT in backup/<date>-pre-repartition,
#                                                  carve linux, write it after "yes"
#   scripts/dualboot-repartition.sh restore <dir>  write the GPT from the backup back (after "yes")
# Variables: SIZE=256GiB (size of linux), MIN_USERDATA=64GiB, CONFIRM=yes (no question), ADB=adb;
# for scripts/test-gpt-carve.sh: ADB (a fake adb) and BACKUP (where the backup goes).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A=${ADB:-adb}
BACKUP=${BACKUP:-$ROOT/backup}
SIZE=${SIZE:-256GiB}
MIN_USERDATA=${MIN_USERDATA:-64GiB}
mkdir -p "$ROOT/out"
GRAB=64 # sectors from the start and from the end of the disk: a GPT with 4 KiB sectors takes 6 and 5
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" titan2-kbuild "$@"; }
w() { echo "/work${1#$ROOT}"; }
sh_() { "$A" shell "$@" | tr -d '\r'; }
die() { echo "ERROR: $*" >&2; exit 1; }

# phone in TWRP, /data and /metadata unmounted, userdata not held by anything mounted
probe() {
	[ "$("$A" get-state 2>/dev/null)" = recovery ] || die "the phone is not in recovery (adb get-state)"
	TWRP=$(sh_ getprop ro.twrp.version)
	[ -n "$TWRP" ] || die "this is not TWRP (no ro.twrp.version)"
	sh_ 'umount /data /sdcard /metadata 2>/dev/null; true' >/dev/null
	! sh_ cat /proc/mounts | grep -qE ' /(data|metadata|sdcard) ' ||
		die "/data or /metadata are mounted (TWRP: Unmount), could not unmount them"
	PART=$(sh_ readlink -f /dev/block/by-name/userdata)
	PART=${PART##*/}
	case "$PART" in sd[a-z][0-9]*) ;; *) die "userdata -> '$PART': not an sdX partition" ;; esac
	DISK=${PART%%[0-9]*}
	# dm on top of userdata (TWRP attaches the metadata decryption at start and has no tool to
	# remove it): accepted only if it is not mounted anywhere; then nobody writes to userdata,
	# and the GPT is at the start and end of the disk, not in userdata
	for h in $(sh_ ls "/sys/class/block/$PART/holders"); do
		! sh_ cat /proc/mounts | grep -q "^/dev/block/$h " ||
			die "userdata is busy: $h on top of $PART is mounted (TWRP: Unmount Data)"
		echo "$h ($(sh_ cat /sys/block/$h/dm/name 2>/dev/null)) on top of $PART, not mounted: accepted"
	done
	SS=$(sh_ cat "/sys/block/$DISK/queue/logical_block_size")
	N=$(($(sh_ cat "/sys/block/$DISK/size") * 512 / SS))
	echo "TWRP $TWRP; userdata = /dev/block/$PART, disk $DISK: $N sectors of $SS B"
}

# start and end of the disk into $1/head.bin, $1/tail.bin
pull() {
	mkdir -p "$1"
	# 2>/dev/null inside the phone command: otherwise the dd statistics end up in the data
	"$A" exec-out "dd if=/dev/block/$DISK bs=$SS count=$GRAB 2>/dev/null" >"$1/head.bin"
	"$A" exec-out "dd if=/dev/block/$DISK bs=$SS skip=$((N - GRAB)) count=$GRAB 2>/dev/null" >"$1/tail.bin"
	[ "$(stat -c %s "$1/head.bin")" = $((GRAB * SS)) ] && [ "$(stat -c %s "$1/tail.bin")" = $((GRAB * SS)) ] ||
		die "could not read the start/end of the disk"
	cat >"$1/disk.env" <<EOF
DISK=$DISK
PART=$PART
SS=$SS
N=$N
EOF
	sh_ "cat /proc/partitions; ls -l /dev/block/by-name/" >"$1/partitions.txt"
	(cd "$1" && sha256sum head.bin tail.bin >SHA256SUMS)
}

carve() {
	run python3 -I /work/scripts/gpt-carve.py --head "$(w "$1/head.bin")" --tail "$(w "$1/tail.bin")" \
		--disk-sectors "$N" --sector-size "$SS" "${@:2}"
}

confirm() {
	echo
	echo "$1"
	# CONFIRM=yes: confirmed without a question (no terminal); otherwise a question on the
	# terminal: stdin cannot be used for it, the adb shell calls read it
	if [ "${CONFIRM:-}" = yes ]; then
		ans=yes
		echo "CONFIRM=yes"
	else
		read -r -p "Type yes: " ans </dev/tty
	fi
	[ "$ans" = yes ] || die "cancelled"
}

# write $1 to LBA $2 ($3 sectors) and read it back
put() {
	local f=$1 lba=$2 cnt=$3 tmp
	[ "$(stat -c %s "$f")" -ge $((cnt * SS)) ] || die "$f is shorter than $cnt sectors"
	tmp=$(mktemp -d "$ROOT/out/repartition.XXXXXX")
	head -c $((cnt * SS)) "$f" >"$tmp/w.bin"
	"$A" push "$tmp/w.bin" /tmp/t2-gpt.bin >/dev/null
	sh_ "dd if=/tmp/t2-gpt.bin of=/dev/block/$DISK bs=$SS seek=$lba count=$cnt conv=notrunc,fsync && sync && rm /tmp/t2-gpt.bin" >/dev/null
	"$A" exec-out "dd if=/dev/block/$DISK bs=$SS skip=$lba count=$cnt 2>/dev/null" >"$tmp/r.bin"
	cmp -s "$tmp/w.bin" "$tmp/r.bin" || die "write check of LBA $lba failed (compare $tmp)"
	rm -rf "$tmp"
	echo "written and verified: LBA $lba..$((lba + cnt - 1))"
}

next_steps() {
	cat <<'EOF'

Next (one step at a time):
  adb reboot bootloader
  fastboot getvar partition-size:linux     # (this LK answers only for a few partitions; flash tells)
  fastboot -w                               # formats userdata and metadata: Android data is wiped
  fastboot flash linux out/linux-root/linux.img
  fastboot reboot
Back to the old layout: scripts/dualboot-repartition.sh restore <backup dir> (from TWRP), then fastboot -w.
EOF
}

case "${1:-}" in
check)
	probe
	T=$(mktemp -d "$ROOT/out/repartition-check.XXXXXX")
	pull "$T"
	carve "$T" --check
	echo "snapshot: $T"
	;;
run)
	probe
	B=$BACKUP/$(date +%Y%m%d-%H%M)-pre-repartition
	[ ! -e "$B" ] || die "$B exists already"
	pull "$B"
	echo "GPT saved: $B"
	carve "$B" --size "$SIZE" --min-userdata "$MIN_USERDATA" --out "$(w "$B/new")"
	. "$B/new/carve.env"
	[ "$SECTOR_SIZE" = "$SS" ] || die "sector size"
	confirm "Write the new GPT to /dev/block/$DISK? userdata gets shorter, Android data will be wiped at the fastboot -w step."
	# backup copy first, then the primary: if interrupted half way, the kernel/LK take the intact one
	put "$B/new/tail.bin" "$TAIL_LBA" "$TAIL_SECTORS"
	put "$B/new/head.bin" 0 "$HEAD_SECTORS"
	pull "$B/after"
	carve "$B/after" --check || die "the new GPT read back from the phone does not pass the check: restore $B"
	next_steps
	;;
restore)
	B=${2:-}
	[ -n "$B" ] && [ -f "$B/disk.env" ] && [ -f "$B/new/carve.env" ] ||
		die "give the backup directory backup/*-pre-repartition"
	(cd "$B" && sha256sum -c --quiet SHA256SUMS) || die "the backup is corrupt"
	probe
	OLD_N=$(sed -n 's/^N=//p' "$B/disk.env")
	OLD_DISK=$(sed -n 's/^DISK=//p' "$B/disk.env")
	[ "$OLD_N" = "$N" ] && [ "$OLD_DISK" = "$DISK" ] || die "another disk: the backup has $OLD_DISK/$OLD_N, now $DISK/$N"
	carve "$B" --check
	confirm "Write the GPT from $B back to /dev/block/$DISK?"
	# GPT sectors only (their location does not change), not the data of neighbouring partitions
	. "$B/new/carve.env"
	tail -c $((TAIL_SECTORS * SS)) "$B/tail.bin" >"$B/restore-tail.bin"
	put "$B/restore-tail.bin" "$TAIL_LBA" "$TAIL_SECTORS"
	put "$B/head.bin" 0 "$HEAD_SECTORS"
	echo "GPT restored. Next: adb reboot bootloader; fastboot -w."
	;;
*)
	sed -n '5,14p' "$0"
	exit 1
	;;
esac
