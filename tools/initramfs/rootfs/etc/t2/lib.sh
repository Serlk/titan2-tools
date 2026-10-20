# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Common functions of the Titan 2 initramfs (sourced via ". /etc/t2/lib.sh").
#
# SAFETY. This initramfs NEVER mounts UFS partitions read-write and never touches
# nvdata/nvram/nvcfg/persist/protect*/userdata/metadata or other partitions with calibration data
# and keys. The scripts here do not mount block devices at all; in addition
#   - every block device is marked read-only right away (BLKROSET, t2_ro_sweep and
#     /sbin/t2-hotplug), so the kernel refuses writes even if someone mounts it;
#   - /bin/mount is a wrapper: ro only, and never for partitions listed in T2_PROTECTED.

T2_LOG_TAG=titan2-initramfs
# PARTNAME (GPT) of partitions that must not be mounted even read-only
T2_PROTECTED='nvdata nvram nvcfg persist protect1 protect2 userdata metadata seccfg proinfo frp otp md1img md1img_a md1img_b'

t2_log() {
	# marker in the kernel log (also ends up in ramoops/pstore)
	echo "<5>$T2_LOG_TAG: $*" >/dev/kmsg 2>/dev/null
}

t2_screen() {
	# message on the screen (fbcon) and in the log
	t2_log "$*"
	[ -c /dev/tty0 ] && echo "$*" >/dev/tty0 2>/dev/null
}

# parameter t2.<name>=<value> from the kernel cmdline
t2_cmdline() {
	local a
	for a in $(cat /proc/cmdline); do
		case "$a" in
		"t2.$1="*) echo "${a#*=}"; return 0 ;;
		"t2.$1") echo 1; return 0 ;;
		esac
	done
	return 1
}

# read-only on all block devices (disks and partitions); idempotent
t2_ro_sweep() {
	local b n r done=
	for b in /sys/class/block/*; do
		[ -e "$b" ] || continue
		n=${b##*/}
		[ -b "/dev/$n" ] || continue
		r=
		read -r r 2>/dev/null <"$b/ro"
		[ "$r" = 1 ] && continue
		blockdev --setro "/dev/$n" 2>/dev/null && done="$done $n"
	done
	[ -z "$done" ] || t2_log "read-only:$done"
}

t2_serial() {
	local s
	s=$(tr -d '\0' 2>/dev/null </proc/device-tree/firmware/android/serialno)
	[ -n "$s" ] || s=$(sed -n 's/^androidboot\.serialno *= *"\(.*\)"$/\1/p' /proc/bootconfig 2>/dev/null)
	[ -n "$s" ] || s=titan2-mainline
	echo "$s"
}
