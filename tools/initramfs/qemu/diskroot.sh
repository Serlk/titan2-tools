#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Test of the root on a partition (t2.root=part:linux, /etc/t2/diskroot) in qemu-system-aarch64 (virt)
# with the phone kernel. A virtio disk vda with a GPT plays the UFS: boot_a (ext4, label t2boot),
# userdata (ext4), linux (Alpine from netroot-serve.sh build, partition larger than the image -> t2-growroot).
#   [O=out/mainline-build] [BASE=out/netroot/qemu-rootfs.img] tools/initramfs/qemu/diskroot.sh
# BASE: a ready Alpine image (the test image of tools/netroot/qemu/run.sh): copied and
# extended (netroot-serve.sh build: e2fsprogs-extra -> firstboot on the partition).
# Image copy: out/netroot/diskroot-qemu-root.img. Network: slirp (t2.root_if=eth0), guest SSH on 127.0.0.1:10823. Logs: out/diskroot-qemu/*.log.
# Disks: vda is the "UFS" (virtio-blk on the virtio-mmio transport a003c00, a platform device of its own,
# like the UFS controller), sda is another protected one (AHCI -> sd, controller 4010000000.pcie; SCSI,
# like the UFS): misc, nvdata, for SG_IO/bsg/BLKPG through sd, delete and rescan; vdb is the
# "SD card" (virtio-mmio a003e00, not under t2.protect): sdcard, userdata, must stay a normal disk.
# t2.protect: which sysfs paths are protected (on the phone the default is */112b0000.ufshci/*).
# Scenarios: boot1 (first boot from the partition, protection, read-only lock and write attempts as
# root: qemu/diskroot-attacks.sh, file system growth, poweroff), boot2 (data kept, file system clean),
# bad (t2.root=part:boot_a refused, stays in the initramfs, the lock is set anyway).
# The kernel needs CONFIG_BLK_DEV_RO_LOCK; the modules (dm-mod) go into the image from $O.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
O=${O:-out/mainline-build}
Q=out/diskroot-qemu
QA=$ROOT/$Q
BASE=$ROOT/${BASE:-out/netroot/qemu-rootfs.img}
KEY=$ROOT/out/netroot/qemu/id_test
mkdir -p "$QA" "$ROOT/out/initramfs-dl/debian-apt/archives/partial" "$ROOT/out/initramfs-dl/debian-apt/lists/partial"
[ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -C t2-netroot-qemu-test -f "$KEY"
OFFLINE=${OFFLINE:-} T2_AUTHORIZED_KEYS=/work/${KEY#$ROOT/}.pub "$ROOT/tools/initramfs/build.sh" "$QA/initramfs.cpio.gz" >/dev/null
dk() { docker run --rm -u "$(id -u):$(id -g)" -v "$QA:/q" titan2-kbuild "$@"; }

# root: copy of BASE + e2fsprogs-extra (resize2fs)
RIMG=$ROOT/out/netroot/diskroot-qemu-root.img # netroot-serve.sh: images only in out/netroot
cp --sparse=always "$BASE" "$RIMG"
# sg3_utils (sg_raw), util-linux-misc (addpart/delpart/blkdiscard), device-mapper (dmsetup),
# losetup: for the write attempts (qemu/diskroot-attacks.sh)
IMG=$RIMG AUTHORIZED_KEYS=$KEY.pub MODULES=$O \
	EXTRA_PKGS="e2fsprogs-extra sg3_utils util-linux-misc device-mapper losetup" \
	"$ROOT/scripts/netroot-serve.sh" build >"$QA/build.log" 2>&1 || { tail "$QA/build.log"; exit 1; }
ROOTSZ=$(stat -c %s "$RIMG")
# disk: GPT, 512-byte sectors (like virtio); linux 512 MB larger than the image
rm -f "$QA/disk.img"
truncate -s $((ROOTSZ + 768 * 1048576)) "$QA/disk.img"
sfdisk -q "$QA/disk.img" <<EOF
label: gpt
size=16MiB, name="boot_a"
size=64MiB, name="userdata"
size=$(((ROOTSZ + 512 * 1048576) / 512)), name="linux"
EOF
part_start() { sfdisk -q -d "$QA/disk.img" | awk -F'[=,]' -v n="name=\"$1\"" 'index($0, n) {gsub(/ /,"",$2); print $2}'; }
rm -f "$QA/boot_a.img" "$QA/userdata.img"
dk mke2fs -q -F -t ext4 -L t2boot /q/boot_a.img 16M
dk mke2fs -q -F -t ext4 -L t2data /q/userdata.img 64M
dd if="$QA/boot_a.img" of="$QA/disk.img" bs=512 seek="$(part_start boot_a)" conv=notrunc,sparse status=none
dd if="$QA/userdata.img" of="$QA/disk.img" bs=512 seek="$(part_start userdata)" conv=notrunc,sparse status=none
dd if="$RIMG" of="$QA/disk.img" bs=1M oflag=seek_bytes seek=$(($(part_start linux) * 512)) conv=notrunc,sparse status=none
# sda: random data (any write shows), GPT misc + nvdata
head -c $((64 * 1048576)) /dev/urandom >"$QA/sata.img"
sfdisk -q "$QA/sata.img" <<EOF
label: gpt
size=8MiB, name="misc"
size=32MiB, name="nvdata"
EOF
# vdb: "SD card"
truncate -s 0 "$QA/sdcard.img"; truncate -s 64M "$QA/sdcard.img"
sfdisk -q "$QA/sdcard.img" <<EOF
label: gpt
size=32MiB, name="sdcard"
size=16MiB, name="userdata"
EOF

FAIL=
ok() { echo "DRTEST PASS: $*"; }
bad() { echo "DRTEST FAIL: $*"; FAIL=1; }
SSH="ssh -i $KEY -p 10823 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o BatchMode=yes -o LogLevel=ERROR root@127.0.0.1"
CT=titan2-diskroot-qemu
# protected: the virtio-mmio transport of the "UFS" (vda; the second -device -> a003c00, the kernel probes it first)
# and the AHCI controller (sda)
PROTECT='*/a003c00.virtio_mmio/*,*/4010000000.pcie/*'

qemu_start() { # <name> <extra cmdline>
	local name=$1; shift
	docker rm -f $CT >/dev/null 2>&1 || true
	# an old log confuses the wait for lines in it (the bad scenario found them before the boot)
	rm -f "$QA/$name.log"
	docker run -d --rm --name $CT --network host -v "$ROOT:/work" -e UIDGID="$(id -u):$(id -g)" -e PROTECT="$PROTECT" \
		debian:trixie bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
D=/work/out/initramfs-dl/debian-apt
trap "chown -R $UIDGID $D /work/'"$Q"'" EXIT
apt-get -qq -o Dir::Cache::archives=$D/archives -o Dir::State::lists=$D/lists update
apt-get -qq -y --no-install-recommends -o Dpkg::Use-Pty=0 -o Dir::Cache::archives=$D/archives -o Dir::State::lists=$D/lists \
	install qemu-system-arm >/dev/null
cd /work/'"$Q"'
timeout 900 qemu-system-aarch64 -M virt -cpu cortex-a76 -smp 2 -m 1024 -nographic -no-reboot \
	-kernel /work/'"$O"'/arch/arm64/boot/Image -initrd initramfs.cpio.gz \
	-drive if=none,id=c0,file=sdcard.img,format=raw -device virtio-blk-device,drive=c0,serial=sdcard \
	-drive if=none,id=u0,file=disk.img,format=raw -device virtio-blk-device,drive=u0,serial=ufs \
	-device ahci,id=ahci -drive if=none,id=s0,file=sata.img,format=raw -device ide-hd,drive=s0,bus=ahci.0 \
	-netdev user,id=n0,net=172.16.42.0/24,host=172.16.42.2,dhcpstart=172.16.42.100,hostfwd=tcp:127.0.0.1:10823-172.16.42.1:22 \
	-device virtio-net-pci,netdev=n0,romfile= \
	-append "console=ttyAMA0 loglevel=6 t2.root_if=eth0 t2.noreboot t2.protect=$PROTECT '"$*"'" </dev/null >'"$name"'.log 2>&1 || echo "qemu: exit $?" >>'"$name"'.log
' >/dev/null
}
qemu_wait_ssh() { # <seconds>
	local i=0
	until $SSH true 2>/dev/null; do
		docker ps -q -f name=^$CT\$ | grep -q . || return 1
		i=$((i + 5)); [ $i -lt "$1" ] || return 1
		sleep 5
	done
}
qemu_end() { # <name> [seconds]: wait for qemu to exit (poweroff), otherwise kill it
	local i=0
	while docker ps -q -f name=^$CT\$ | grep -q .; do
		i=$((i + 2)); [ $i -lt "${2:-120}" ] || { docker rm -f $CT >/dev/null; echo "(killed)"; break; }
		sleep 2
	done
	grep -a "titan2\|Kernel panic\|EXT4-fs error" "$QA/$1.log" | tail -25 | sed "s/^/[$1] /" || true
}
trap 'docker rm -f $CT >/dev/null 2>&1 || true' EXIT

# --- 1. first boot from the partition ---
qemu_start boot1 t2.root=part:linux
if qemu_wait_ssh 600; then
	ok "boot1: ssh into the system on the linux partition"
	out=$($SSH 'awk "\$2==\"/\"" /proc/mounts; cat /run/t2-rw-part; cat /run/t2-root;
		for n in vda vda1 vda2 vda3 sda sda1 sda2; do echo "RO $n $(cat /sys/class/block/$n/ro)"; done;
		echo "LOCK $(cat /sys/module/block/parameters/ro_lock)";
		echo persist-$(date +%s) >/root/persist.txt && sync && echo ROOT-WRITE-OK;
		dd if=/dev/zero of=/dev/vda1 bs=512 count=1 2>&1 && echo VDA1-WRITE-OK;
		dd if=/dev/zero of=/dev/vda2 bs=512 count=1 2>&1 && echo VDA2-WRITE-OK;
		mkdir -p /mnt/x;
		mount /dev/vda2 /mnt/x 2>&1 && echo USERDATA-MOUNTED; umount /mnt/x 2>/dev/null;
		mount -o rw /dev/vda1 /mnt/x 2>&1 && echo BOOTA-RW-MOUNTED; umount /mnt/x 2>/dev/null;
		mount /dev/vda1 /mnt/x 2>&1 && awk "\$2==\"/mnt/x\" {print \"BOOTAMNT\", \$1, substr(\$4, 1, 2)}" /proc/mounts; umount /mnt/x 2>/dev/null;
		mount LABEL=t2boot /mnt/x 2>&1 && awk "\$2==\"/mnt/x\" {print \"LABELMNT\", \$1, substr(\$4, 1, 2)}" /proc/mounts; umount /mnt/x 2>/dev/null;
		mount /dev/vda /mnt/x 2>&1 && echo DISK-MOUNTED; umount /mnt/x 2>/dev/null;
		mount -o remount,rw / 2>&1 && echo ROOT-REMOUNT-OK;
		echo hotplug=$(cat /proc/sys/kernel/hotplug);
		/sbin/t2-hotplug vda; /sbin/t2-hotplug vda3; echo "AFTER-HOTPLUG vda=$(cat /sys/block/vda/ro) vda3=$(cat /sys/class/block/vda3/ro)";
		. /usr/lib/t2/lib.sh; t2_ro_sweep; echo "AFTER-SWEEP vda=$(cat /sys/block/vda/ro) vda3=$(cat /sys/class/block/vda3/ro) vda1=$(cat /sys/class/block/vda1/ro)";
		rc-status -a 2>&1 | grep -E "t2-|dropbear"; ls /etc/runlevels/shutdown;
		test -e /etc/t2-netroot/firstboot && echo FIRSTBOOT-PENDING; tail -2 /var/log/t2-firstboot.log 2>/dev/null;
		dmesg | grep -E "diskroot|e2fsck"' 2>&1) || true
	echo "$out" | sed 's/^/[boot1] /'
	echo "$out" | grep -q '^/dev/vda3 / ext4 rw' && ok "root is /dev/vda3 ext4 rw" || bad "root mount"
	echo "$out" | grep -q '^vda3 - linux$' && ok "/run/t2-rw-part: vda3 - linux (disk not exempt)" || bad "/run/t2-rw-part"
	echo "$out" | grep -q ROOT-WRITE-OK && ok "write to the root" || bad "write to the root"
	echo "$out" | grep -q '^LOCK Y$' && ok "block.ro_lock = Y" || bad "ro_lock not set"
	echo "$out" | grep -q '^RO vda 1$' && ok "disk vda ro=1 (holds the root, stays read-only)" || bad "vda ro flag"
	echo "$out" | grep -q '^RO sda 1$' && echo "$out" | grep -q '^RO sda1 1$' && echo "$out" | grep -q '^RO sda2 1$' &&
		ok "second disk sda and its partitions ro=1" || bad "sda ro flags"
	echo "$out" | grep -q '^RO vda3 0$' && ok "root partition vda3 ro=0" || bad "vda3 ro flag"
	echo "$out" | grep -q '^RO vda1 1$' && echo "$out" | grep -q '^RO vda2 1$' &&
		ok "boot_a (vda1) and userdata (vda2) ro=1" || bad "sibling partitions not ro"
	echo "$out" | grep -q 'VDA[12]-WRITE-OK' && bad "raw write to a sibling partition succeeded" || ok "raw writes to boot_a/userdata refused"
	echo "$out" | grep -q USERDATA-MOUNTED && bad "userdata mounted" || ok "mount userdata refused"
	echo "$out" | grep -q 'partition userdata (/dev/vda2) is protected' && ok "  (protected partition message)" || bad "userdata refusal message"
	echo "$out" | grep -q BOOTA-RW-MOUNTED && bad "boot_a mounted rw" || ok "rw mount of boot_a refused"
	echo "$out" | grep -q '^BOOTAMNT /dev/vda1 ro' && ok "mount boot_a: forced ro" || bad "mount boot_a not ro"
	echo "$out" | grep -q '^LABELMNT /dev/vda1 ro' && ok "mount LABEL=t2boot: forced ro" || bad "mount LABEL= not ro"
	echo "$out" | grep -q DISK-MOUNTED && bad "whole disk mounted" || ok "mount of the whole disk vda refused"
	echo "$out" | grep -q 'whole disk not mountable' && ok "  (whole disk message)" || bad "whole disk message"
	echo "$out" | grep -q ROOT-REMOUNT-OK && ok "remount,rw / allowed" || bad "remount,rw /"
	echo "$out" | grep -q '^hotplug=/sbin/t2-hotplug' && ok "uevent helper /sbin/t2-hotplug" || bad "hotplug helper"
	echo "$out" | grep -q '^AFTER-HOTPLUG vda=1 vda3=0' && ok "t2-hotplug: root partition untouched, disk ro" || bad "t2-hotplug"
	echo "$out" | grep -q '^AFTER-SWEEP vda=1 vda3=0 vda1=1' && ok "t2_ro_sweep: root partition untouched, disk ro" || bad "t2_ro_sweep"
	for sv in t2-roguard t2-usbnet t2-growroot dropbear; do
		echo "$out" | grep -q " $sv .*started" && ok "$sv started" || bad "$sv not started"
	done
	echo "$out" | grep -qx killprocs && ! echo "$out" | grep -qx t2-killprocs && ok "shutdown: killprocs (no t2-killprocs)" || bad "shutdown runlevel"
	echo "$out" | grep -q FIRSTBOOT-PENDING && bad "firstboot pending" || ok "firstboot done"
	echo "$out" | grep -q 'diskroot: e2fsck -p /dev/vda3: 0' && ok "e2fsck -p in the initramfs: clean" || bad "e2fsck in the initramfs"
	# write attempts as root (after the lock)
	out=$($SSH 'sh -s' <"$ROOT/tools/initramfs/qemu/diskroot-attacks.sh" 2>&1) || true
	echo "$out" | sed 's/^/[boot1 attack] /'
	na=$(echo "$out" | grep -c '^ATTACK ') || true
	nr=$(echo "$out" | grep -c '^ATTACK .* REFUSED ') || true
	[ "$na" -ge 20 ] && ok "attacks run: $na" || bad "attacks run: only $na"
	for a in $(echo "$out" | awk '$1 == "ATTACK" && $3 == "ALLOWED" { print $2 }'); do bad "attack $a ALLOWED"; done
	[ "$na" = "$nr" ] && ok "all $na write attempts refused" || bad "$((na - nr)) write attempts allowed"
	for a in $(echo "$out" | awk '$1 == "POS" && $3 == "OK" { print $2 }'); do ok "$a"; done
	for a in $(echo "$out" | awk '$1 == "POS" && $3 != "OK" { print $2 }'); do bad "$a"; done
	echo "$out" | grep -q '^INFO after: ro_lock=Y vda=1 vda2=1 sda1=1' && ok "read-only state unchanged after the attempts" || bad "read-only state changed"
	echo "$out" | grep -q 'Refusing write to read-only block-device' && ok "kernel: write bios to read-only devices refused (dm)" || bad "no bio refusal in dmesg"
	# file system growth: in the background
	i=0
	until $SSH test -e /var/lib/t2/growroot.done 2>/dev/null; do i=$((i + 2)); [ $i -lt 120 ] || break; sleep 2; done
	out=$($SSH 'cat /var/log/t2-growroot.log; echo FS=$(($(stat -f -c "%b*%S" /)/1048576)) PART=$(($(cat /sys/class/block/vda3/size)/2048))' 2>&1) || true
	echo "$out" | sed 's/^/[boot1 grow] /'
	fs=$(echo "$out" | sed -n 's/^FS=\([0-9]*\) .*/\1/p'); pt=$(echo "$out" | sed -n 's/.*PART=\([0-9]*\)$/\1/p')
	[ -n "$fs" ] && [ -n "$pt" ] && [ $((pt - fs)) -lt 64 ] && ok "t2-growroot: fs ${fs} MiB of ${pt} MiB partition" || bad "t2-growroot (fs=$fs part=$pt)"
	$SSH poweroff >/dev/null 2>&1 || true
else
	bad "boot1: no ssh"
fi
qemu_end boot1 180
grep -aq "reboot: Power down" "$QA/boot1.log" && ok "boot1: clean poweroff" || bad "boot1: poweroff"
grep -aq "mount-ro failed\|ERROR: mount-ro" "$QA/boot1.log" && bad "boot1: mount-ro failed" || ok "boot1: mount-ro at shutdown"

# --- 2. second boot ---
qemu_start boot2 t2.root=part:linux
if qemu_wait_ssh 300; then
	out=$($SSH 'cat /root/persist.txt; dmesg | grep -E "diskroot: e2fsck|EXT4-fs.*(recovery|error)"; rc-status -a 2>&1 | grep t2-growroot; ls -l /var/lib/t2/growroot.done' 2>&1) || true
	echo "$out" | sed 's/^/[boot2] /'
	echo "$out" | grep -q '^persist-' && ok "boot2: data persisted" || bad "boot2: persistence"
	echo "$out" | grep -q 'e2fsck -p /dev/vda3: 0' && ok "boot2: e2fsck clean" || bad "boot2: e2fsck"
	echo "$out" | grep -qi "recovery\|EXT4-fs error" && bad "boot2: journal recovery/errors" || ok "boot2: no journal recovery (clean shutdown)"
	$SSH t2-reboot bootloader >/dev/null 2>&1 || true
else
	bad "boot2: no ssh"
fi
qemu_end boot2 180
grep -aq "reboot: Restarting system with command 'bootloader'" "$QA/boot2.log" && ok "boot2: t2-reboot bootloader" || bad "boot2: t2-reboot bootloader"

# --- 3. a foreign partition as the root: refused ---
qemu_start bad t2.root=part:boot_a t2.root_wait=5
i=0
until grep -aq "staying in initramfs\|only partitions named linux" "$QA/bad.log" 2>/dev/null; do
	i=$((i + 2)); [ $i -lt 240 ] || break; sleep 2
done
sleep 3
docker rm -f $CT >/dev/null 2>&1 || true
grep -a "diskroot\|titan2: root" "$QA/bad.log" | sed 's/^/[bad] /' | tail -5
grep -aq "boot_a: only partitions named linux\* may hold the root" "$QA/bad.log" && ok "bad: part:boot_a refused" || bad "bad: part:boot_a not refused"
grep -aq "switch_root" "$QA/bad.log" && bad "bad: switch_root happened" || ok "bad: no switch_root"
grep -aq "ro_lock: read-only state of all block devices locked" "$QA/bad.log" && ok "bad: read-only lock set while staying in initramfs" || bad "bad: no lock in the initramfs"

[ -z "$FAIL" ] && echo "DRTEST RESULT: ALL PASS" || { echo "DRTEST RESULT: FAILURES"; exit 1; }
