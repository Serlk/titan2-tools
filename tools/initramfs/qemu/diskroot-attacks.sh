#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Write attempts to other partitions as root, in the qemu guest (root on the linux partition,
# read-only lock on). Run by tools/initramfs/qemu/diskroot.sh: ssh ... sh -s.
# Disks: vda ("UFS", virtio-mmio: boot_a, userdata, linux = the root), sda (AHCI -> sd: misc,
# nvdata; protected too), vdb ("SD card", not protected: sdcard, userdata).
# Output: "ATTACK <name> REFUSED|ALLOWED <details>", "POS <name> OK|FAIL ...", "INFO ...".
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
r() { # <name> <command...>: the command succeeding = the attack went through
	local name=$1 out rc
	shift
	out=$("$@" 2>&1)
	rc=$?
	out=$(echo "$out" | tr '\n' ' ' | cut -c1-160)
	if [ $rc = 0 ]; then echo "ATTACK $name ALLOWED rc=0 $out"; else echo "ATTACK $name REFUSED rc=$rc $out"; fi
}
sum() { echo 3 >/proc/sys/vm/drop_caches; md5sum <"$1" | cut -d' ' -f1; }
# only into an existing block device (otherwise dd would create a file in /dev)
ddw() { [ -b "$1" ] || { echo "$1: no block device"; return 1; }; dd if=/dev/zero of="$1" bs=4096 count=1 conv=fsync,notrunc; }

echo "INFO ro_lock=$(cat /sys/module/block/parameters/ro_lock)"
for n in vda vda1 vda2 vda3 sda sda1 sda2 vdb vdb1 vdb2; do echo "INFO ro $n=$(cat /sys/class/block/$n/ro 2>/dev/null)"; done
V1=$(sum /dev/vda1) V2=$(sum /dev/vda2) S=$(sum /dev/sda) VD=$(dd if=/dev/vda bs=1M count=1 2>/dev/null | md5sum | cut -d' ' -f1)

# 1. clear read-only and the lock
r setrw-vda2 blockdev --setrw /dev/vda2
r setrw-vda blockdev --setrw /dev/vda
r setrw-sda1 blockdev --setrw /dev/sda1
r ro_lock-0 sh -c 'echo 0 >/sys/module/block/parameters/ro_lock'
echo "INFO after: ro_lock=$(cat /sys/module/block/parameters/ro_lock) vda=$(cat /sys/block/vda/ro) vda2=$(cat /sys/class/block/vda2/ro) sda1=$(cat /sys/class/block/sda1/ro)"

# 2. write to the device
for d in vda vda1 vda2 sda sda1; do r "dd-$d" ddw /dev/$d; done

# 3. partition table
r rereadpt-vda blockdev --rereadpt /dev/vda
r rereadpt-sda blockdev --rereadpt /dev/sda
r addpart-vda addpart /dev/vda 9 4096 2048
r delpart-vda delpart /dev/vda 2
r addpart-sda addpart /dev/sda 9 4096 2048
r delpart-sda delpart /dev/sda 2
echo "INFO partitions vda: $(ls /sys/block/vda | grep -c '^vda[0-9]') sda: $(ls /sys/block/sda | grep -c '^sda[0-9]')"

# 4. discard / zeroout
r blkdiscard-vda2 blkdiscard /dev/vda2
r blkzeroout-vda2 blkdiscard -z /dev/vda2
r blkdiscard-sda1 blkdiscard /dev/sda1

# 5. dm-linear on top of userdata: the write must be refused (by dm or at the bio level)
modprobe dm-mod 2>/dev/null
if echo "0 $(blockdev --getsz /dev/vda2) linear /dev/vda2 0" | dmsetup create t2x 2>/dev/null; then
	dm=$(basename "$(readlink -f /dev/mapper/t2x)")
	echo "INFO dm t2x ($dm) over vda2: ro=$(cat /sys/block/$dm/ro 2>/dev/null) $(dmsetup info t2x | grep -i attributes | tr -s ' ')"
	r dm-linear-write ddw /dev/mapper/t2x
	# the dm disk itself was created after the lock and is not frozen: its ro can be cleared, but a
	# write to vda2 below it must still fail at the bio level
	echo "INFO dm setrw: $(blockdev --setrw /dev/mapper/t2x 2>&1 && echo ok) ro=$(cat /sys/block/$dm/ro)"
	r dm-linear-write2 ddw /dev/mapper/t2x
	dmsetup remove --retry t2x 2>/dev/null
else
	echo "INFO dm: dmsetup create refused or unavailable: $(echo "0 8 linear /dev/vda2 0" | dmsetup create t2y 2>&1 | head -1)"
fi

# 6. SCSI pass-through (sd_ioctl SG_IO, bsg): WRITE(10) of 1 block at LBA 0
BSG=$(ls /sys/class/block/sda/device/bsg 2>/dev/null)
echo "INFO bsg of sda: ${BSG:-none}"
r sgio-write-sda sg_raw -s 512 -i /dev/zero /dev/sda 2a 00 00 00 00 00 00 00 01 00
r sgio-write-sda1 sg_raw -s 512 -i /dev/zero /dev/sda1 2a 00 00 00 00 00 00 00 01 00
[ -n "$BSG" ] && r sgio-write-bsg sg_raw -s 512 -i /dev/zero /dev/bsg/$BSG 2a 00 00 00 00 00 00 00 01 00
r sgio-read-sda sg_raw -r 512 /dev/sda 28 00 00 00 00 00 00 00 01 00

# 7. the data did not change
[ "$(sum /dev/vda1)" = "$V1" ] && echo "POS vda1-unchanged OK" || echo "POS vda1-unchanged FAIL"
[ "$(sum /dev/vda2)" = "$V2" ] && echo "POS vda2-unchanged OK" || echo "POS vda2-unchanged FAIL"
[ "$(sum /dev/sda)" = "$S" ] && echo "POS sda-unchanged OK" || echo "POS sda-unchanged FAIL"
[ "$(dd if=/dev/vda bs=1M count=1 2>/dev/null | md5sum | cut -d' ' -f1)" = "$VD" ] &&
	echo "POS vda-gpt-unchanged OK" || echo "POS vda-gpt-unchanged FAIL"

# 8. delete the SCSI disk and rescan: it must come back read-only and frozen
h=$(basename "$(readlink -f /sys/block/sda/device/../..)")
echo "INFO sda host: $h"
echo 1 >/sys/block/sda/device/delete
sleep 1
echo '- - -' >/sys/class/scsi_host/$h/scan
i=0
while [ ! -e /sys/block/sda ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
sleep 1
echo "INFO rescan: $(ls /sys/block | grep '^sd' | tr '\n' ' ') sda=$(cat /sys/block/sda/ro 2>/dev/null) sda1=$(cat /sys/class/block/sda1/ro 2>/dev/null)"
[ "$(cat /sys/block/sda/ro 2>/dev/null)" = 1 ] && [ "$(cat /sys/class/block/sda1/ro 2>/dev/null)" = 1 ] &&
	echo "POS rescan-sda-ro OK" || echo "POS rescan-sda-ro FAIL"
r rescan-setrw-sda blockdev --setrw /dev/sda
r rescan-setrw-sda1 blockdev --setrw /dev/sda1
r rescan-dd-sda ddw /dev/sda
r rescan-sgio-sda sg_raw -s 512 -i /dev/zero /dev/sda 2a 00 00 00 00 00 00 00 01 00

# 9. "SD card" (vdb, not UFS): a normal device even after the lock
[ "$(cat /sys/block/vdb/ro)" = 0 ] && [ "$(cat /sys/class/block/vdb1/ro)" = 0 ] &&
	echo "POS sdcard-rw OK" || echo "POS sdcard-rw FAIL vdb=$(cat /sys/block/vdb/ro)"
ddw /dev/vdb1 >/dev/null 2>&1 && echo "POS sdcard-dd OK" || echo "POS sdcard-dd FAIL"
if mkfs.ext4 -q -F /dev/vdb1 && mkdir -p /mnt/sd && mount -o rw /dev/vdb1 /mnt/sd &&
	awk '$2 == "/mnt/sd" { exit !($4 ~ /^rw/) }' /proc/mounts && echo z >/mnt/sd/z && sync && umount /mnt/sd
then echo "POS sdcard-mount-rw OK"; else echo "POS sdcard-mount-rw FAIL"; umount /mnt/sd 2>/dev/null; fi
mount /dev/vdb2 /mnt/sd 2>/dev/null && { echo "POS sdcard-userdata-refused FAIL"; umount /mnt/sd; } ||
	echo "POS sdcard-userdata-refused OK"
blockdev --setro /dev/vdb && [ "$(cat /sys/block/vdb/ro)" = 1 ] && blockdev --setrw /dev/vdb &&
	[ "$(cat /sys/block/vdb/ro)" = 0 ] && echo "POS sdcard-setro-setrw OK" || echo "POS sdcard-setro-setrw FAIL"
o=$(addpart /dev/vdb 3 $((50 * 2048)) 2048 2>&1 && sleep 0.5 && [ -e /sys/block/vdb/vdb3 ] && delpart /dev/vdb 3 2>&1) &&
	echo "POS sdcard-blkpg OK" || echo "POS sdcard-blkpg FAIL $(echo $o) $(ls /sys/block/vdb)"
o=$(blockdev --rereadpt /dev/vdb 2>&1) && echo "POS sdcard-rereadpt OK" || echo "POS sdcard-rereadpt FAIL $o"

# 10. what must work
echo ok-$$ >/root/attack-write.txt && sync && echo "POS root-write OK" || echo "POS root-write FAIL"
truncate -s 64M /root/loop.img
L=$(losetup -f --show /root/loop.img 2>&1)
if [ -b "$L" ] && mkfs.ext4 -q -F "$L" && mkdir -p /mnt/l && mount "$L" /mnt/l && echo x >/mnt/l/f && sync &&
	umount /mnt/l; then echo "POS loop-existing OK ($L ro=$(cat /sys/class/block/${L##*/}/ro))"
else echo "POS loop-existing FAIL ($L)"; fi
losetup -d "$L" 2>/dev/null
# a new loop device after the lock (by number: the kernel creates it on open)
[ -e /dev/loop42 ] || mknod /dev/loop42 b 7 42
if losetup /dev/loop42 /root/loop.img 2>/dev/null && [ "$(cat /sys/block/loop42/ro)" = 0 ] &&
	mount /dev/loop42 /mnt/l && echo y >/mnt/l/g && sync && umount /mnt/l; then echo "POS loop-new OK"
else echo "POS loop-new FAIL ro=$(cat /sys/block/loop42/ro 2>/dev/null)"; fi
losetup -d /dev/loop42 2>/dev/null
rm -f /root/loop.img
[ -e /sys/class/zram-control ] || modprobe zram 2>/dev/null
[ -e /sys/class/zram-control ] && echo "INFO zram present" || echo "INFO zram not in this kernel"
dmesg | grep -E "read-only block-device|ro_lock|read-only state" | tail -8 | sed 's/^/DMESG /'
