# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Common functions of the Titan 2 initramfs (sourced via ". /etc/t2/lib.sh").
#
# SAFETY. This initramfs NEVER mounts UFS partitions read-write and never touches
# nvdata/nvram/nvcfg/persist/protect*/userdata/metadata or other partitions with calibration data
# and keys. The scripts here do not mount block devices at all; in addition
#   - every UFS block device (t2_protected_blk: its sysfs path is behind the UFS controller,
#     */112b0000.ufshci/*; in qemu t2.protect=<glob>[,<glob>...]) is marked read-only right away
#     (BLKROSET, t2_ro_sweep and /sbin/t2-hotplug), so the kernel refuses writes even if someone
#     mounts it. Other devices (SD card, USB storage, loop, zram, nbd) are left alone: they can
#     be mounted, partitioned and written as usual;
#   - /bin/mount is a wrapper: UFS only read-only, and never the partitions listed in
#     T2_PROTECTED (on any device).
# The one exception is a root on the linux partition (t2.root=part:linux, /etc/t2/diskroot):
# that partition is not marked read-only (t2_ro_exempt, file /run/t2-rw-part "partition disk
# name").
#
# Lock (kernel with CONFIG_BLK_DEV_RO_LOCK, parameter /sys/module/block/parameters/ro_lock,
# t2_ro_lock): the read-only flag of a disk is NOT inherited by its partitions, so the disk (sdc)
# stays read-only too (the disk field of /run/t2-rw-part is "-") and only the root partition is
# writable. "1" in ro_lock freezes the devices that are read-only AT THAT MOMENT: until the next
# boot not even root can make them writable (BLKROSET -> EPERM), writes to them (also through
# dm/md, discard) fail, and for a frozen disk (and its partitions) BLKPG, BLKRRPART and SCSI
# pass-through (SG_IO, bsg, ufs-bsg) are refused; new partitions of a frozen disk and new disks
# behind the same controller (UFS) appear read-only and frozen. So t2_ro_lock first runs
# t2_ro_sweep: everything except the root partition and virtual devices is read-only. The
# initramfs sets the lock in every mode: diskroot and netroot before switch_root, without t2.root=
# at the end of rc.init, on failure before busybox init. Kernel-level holes remain: own modules
# (module signing is off), kexec, /dev/mem — needed for debugging, an accepted risk.
# Kernel without the lock: diskroot refuses (only with t2.root_unsafe the old scheme: the disk
# flag is inherited by its partitions, so it is cleared on the disk and raw writes to /dev/sdc
# are possible).

T2_LOG_TAG=titan2-initramfs
# PARTNAME (GPT) of the partitions that must not be mounted at all, not even read-only
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

# parameter t2.<name>=<value> from the kernel command line
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

# Virtual devices (network root over NBD, loop, zram, ramdisk) are not UFS: leave them alone,
# otherwise ro on /dev/nbd0 under a mounted read-write root (netroot mode) brings the system down.
t2_virtual_blk() {
	case "$1" in nbd*|loop*|zram*|ram*) return 0 ;; esac
	return 1
}

# Protected devices: the path /sys/class/block/<name> (realpath) is behind the UFS controller.
# A comma-separated list of globs; t2.protect=<glob>[,<glob>...] on the command line replaces it
# (qemu).
T2_PROTECT_DEFAULT='*/112b0000.ufshci/*'
T2_PROTECT=

# 0: the device <name> (disk or partition) is protected (UFS): read-only, mounted only read-only
t2_protected_blk() {
	local p g IFS
	t2_virtual_blk "$1" && return 1
	[ -n "$T2_PROTECT" ] || T2_PROTECT=$(t2_cmdline protect) || T2_PROTECT=$T2_PROTECT_DEFAULT
	p=$(readlink -f "/sys/class/block/$1" 2>/dev/null) || return 1
	[ -n "$p" ] || return 1
	IFS=,
	set -f # patterns for case, not file names
	for g in $T2_PROTECT; do
		case "$p" in $g) set +f; return 0 ;; esac
	done
	set +f
	return 1
}

# Root partition on the UFS (t2.root=part:<name>): the only writable partition.
# /run/t2-rw-part: "<partition> <disk> <PARTNAME>", e.g. "sdc78 sdc linux"; written by
# /etc/t2/diskroot. The entry is valid only while the partition's PARTNAME and disk match.
T2_RW_FILE=/run/t2-rw-part

# T2_RW_PART, T2_RW_DISK from /run/t2-rw-part; 1 (and empty): no exception.
# Disk "-": kernel with the lock, the disk stays read-only, the exception is the partition only.
t2_rw_get() {
	local p= d= n=
	T2_RW_PART= T2_RW_DISK=
	read -r p d n 2>/dev/null <$T2_RW_FILE || return 1
	[ -n "$p" ] && [ -n "$d" ] && [ -n "$n" ] || return 1
	grep -qx "PARTNAME=$n" "/sys/class/block/$p/uevent" 2>/dev/null || return 1
	if [ "$d" = - ]; then
		d=
	else
		[ "$(t2_blk_parent "$p")" = "$d" ] || return 1
	fi
	T2_RW_PART=$p T2_RW_DISK=$d
}

T2_RO_LOCK=/sys/module/block/parameters/ro_lock

# the kernel has the read-only lock (CONFIG_BLK_DEV_RO_LOCK)
t2_ro_lock_supported() {
	[ -e $T2_RO_LOCK ]
}

# one-way read-only lock until the next boot (see the header); 1: not supported or not set.
# Idempotent.
t2_ro_lock() {
	local v=
	t2_ro_lock_supported || { t2_log "ro_lock: not supported by this kernel"; return 1; }
	read -r v 2>/dev/null <$T2_RO_LOCK
	[ "$v" = Y ] && return 0
	# only what is read-only now gets frozen: one last pass right before the lock
	t2_ro_sweep
	echo 1 >$T2_RO_LOCK 2>/dev/null
	read -r v 2>/dev/null <$T2_RO_LOCK
	[ "$v" = Y ] || { t2_log "ro_lock: could not set"; return 1; }
	t2_log "ro_lock: read-only state of all block devices locked until reboot"
}

# the disk a partition belongs to (sdc78 -> sdc); empty: not a partition
t2_blk_parent() {
	[ -f "/sys/class/block/$1/partition" ] || return 1
	basename "$(readlink -f "/sys/class/block/$1/..")"
}

# not marked read-only: virtual devices, the root partition and (only without the lock) its disk.
# Call t2_rw_get first (t2_ro_sweep does it itself).
t2_ro_exempt() {
	t2_virtual_blk "$1" && return 0
	[ -n "$T2_RW_PART" ] && { [ "$1" = "$T2_RW_PART" ] || [ "$1" = "${T2_RW_DISK:--}" ]; }
}

# read-only for all protected devices (UFS: disks and partitions) except t2_ro_exempt; idempotent
t2_ro_sweep() {
	local b n r done=
	t2_rw_get
	for b in /sys/class/block/*; do
		[ -e "$b" ] || continue
		n=${b##*/}
		t2_ro_exempt "$n" && continue
		t2_protected_blk "$n" || continue
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

# Parse mount arguments (the /bin/mount wrappers of the initramfs and of the installed system).
# Result:
#   T2M_SRC      source: the first of two positional arguments; with one positional argument
#                looked up in /etc/fstab (device or mount point), for remount in /proc/mounts
#   T2M_DEV      the block device the source resolves to (/dev/sd*, /dev/block/..., links in
#                /dev/disk/by-*, UUID=/LABEL=/PARTLABEL=/PARTUUID=); empty: the source is not a
#                block device (tmpfs, devpts, proc, sysfs, cgroup, a directory for --bind, ...)
#   T2M_RW       write access requested: -w/--rw or option rw
#   T2M_REMOUNT  option remount
#   T2M_ALL      -a/--all
# Returns 1: a label source (UUID=... and the like) that could not be resolved.
# The file system type (-t) is ignored on purpose: a block device with "-t tmpfs" is still one.
t2_mount_parse() {
	local a skip= opts= p1= np=0
	T2M_SRC= T2M_DEV= T2M_RW= T2M_REMOUNT= T2M_ALL=
	for a in "$@"; do
		if [ -n "$skip" ]; then
			[ "$skip" = o ] && opts="$opts,$a"
			skip=
			continue
		fi
		case "$a" in
		-t|--types|-O|--test-opts) skip=x ;;
		-o|--options) skip=o ;;
		-t?*|-O?*) ;;
		-o?*) opts="$opts,${a#-o}" ;;
		--options=*) opts="$opts,${a#--options=}" ;;
		-w|--rw|--read-write) T2M_RW=1 ;;
		-a|--all) T2M_ALL=1 ;;
		--*) ;;
		-*)
			# grouped short flags of busybox mount (-nw, -av, ...)
			case "$a" in *w*) T2M_RW=1 ;; esac
			case "$a" in *a*) T2M_ALL=1 ;; esac
			;;
		*)
			np=$((np + 1))
			[ $np = 1 ] && p1=$a
			;;
		esac
	done
	case ",$opts," in *,rw,*) T2M_RW=1 ;; esac
	case ",$opts," in *,remount,*) T2M_REMOUNT=1 ;; esac
	if [ $np -ge 2 ]; then
		T2M_SRC=$p1
	elif [ $np = 1 ]; then
		if [ -n "$T2M_REMOUNT" ]; then
			T2M_SRC=$(awk -v m="$p1" '$2 == m { d = $1 } END { print d }' /proc/mounts)
		else
			T2M_SRC=$(awk -v m="$p1" '$1 !~ /^#/ && ($1 == m || $2 == m) { print $1; exit }' \
				/etc/fstab 2>/dev/null)
		fi
		[ -n "$T2M_SRC" ] || T2M_SRC=$p1
	fi
	t2_blk_resolve "$T2M_SRC"
}

# T2M_DEV from a mount source (see t2_mount_parse); 1: a label could not be resolved
t2_blk_resolve() {
	local s=$1 d= b n v
	T2M_DEV=
	case "$s" in
	UUID=*|LABEL=*)
		d=$(findfs "$s" 2>/dev/null)
		;;
	PARTLABEL=*|PARTUUID=*)
		v=${s#*=}
		[ "${s%%=*}" = PARTLABEL ] && n=by-partlabel || n=by-partuuid
		[ -e "/dev/disk/$n/$v" ] && d=/dev/disk/$n/$v
		if [ -z "$d" ] && [ $n = by-partlabel ]; then
			for b in /sys/class/block/*; do
				grep -qx "PARTNAME=$v" "$b/uevent" 2>/dev/null && { d=/dev/${b##*/}; break; }
			done
		fi
		if [ -z "$d" ] && [ $n = by-partuuid ]; then
			d=$(blkid 2>/dev/null | awk -v u="PARTUUID=\"$v\"" 'index($0, u) { sub(/:.*/, ""); print; exit }')
		fi
		;;
	esac
	case "$s" in
	UUID=*|LABEL=*|PARTLABEL=*|PARTUUID=*) [ -n "$d" ] || return 1 ;;
	*) d=$s ;;
	esac
	# a pseudo file system name (tmpfs, proc, none, ...) is not a path to an existing file
	[ -e "$d" ] || return 0
	d=$(readlink -f "$d") || return 0
	[ -b "$d" ] && T2M_DEV=$d
	return 0
}
