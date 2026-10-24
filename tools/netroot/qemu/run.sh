#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Test of the network root in qemu-system-aarch64 (virt) with the phone kernel:
# initramfs -> netroot (NBD) -> switch_root -> Alpine (openrc) -> SSH login from the host.
#   [O=out/mainline-netroot] tools/netroot/qemu/run.sh
# Modules: if $O/modules exists (scripts/build-mainline.sh), they go into the image (MODULES=$O).
# KEEP_IMG=1: do not rebuild the image, take the existing QIMG (default qemu-rootfs.img),
# e.g. after adding to it (netroot-serve.sh build on an existing image).
# virtio-net instead of the USB gadget (t2.root_if=eth0): the gadget part is tested separately
# (tools/initramfs/qemu/run.sh). qemu network: user (slirp) with net=172.16.42.0/24,
# host=172.16.42.2: guest connections to 172.16.42.2:10809 go to 127.0.0.1:10809 of the
# host, where scripts/netroot-serve.sh serve listens (LISTEN=127.0.0.1); guest SSH on
# 127.0.0.1:10822. A separate test key and image (out/netroot/qemu, IMG=.../qemu-rootfs.img).
# The virtio disk vda plays the UFS (t2.protect=*/4010000000.pcie/*: the devices behind the qemu
# PCIe controller are protected): in the new system it must be read-only and locked (block.ro_lock).
# Scenarios: 1) first boot (firstboot) + SSH + checks + poweroff; 2) second boot
# (data kept); 3) no server -> after t2.root_wait stays in the initramfs, SSH.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
O=${O:-out/mainline-netroot}
Q=out/netroot/qemu
QA=$ROOT/$Q
mkdir -p "$QA" "$ROOT/out/initramfs-dl/debian-apt/archives/partial" "$ROOT/out/initramfs-dl/debian-apt/lists/partial"
[ -f "$QA/id_test" ] || ssh-keygen -q -t ed25519 -N '' -C t2-netroot-qemu-test -f "$QA/id_test"
OFFLINE=${OFFLINE:-} T2_AUTHORIZED_KEYS=/work/$Q/id_test.pub "$ROOT/tools/initramfs/build.sh" "$QA/initramfs.cpio.gz" >/dev/null
QIMG=$ROOT/out/netroot/${QIMG:-qemu-rootfs.img}
MODS=
[ -d "$ROOT/$O/modules/lib/modules" ] && MODS=$O
if [ -z "${KEEP_IMG:-}" ]; then
	FORCE=1 SIZE=1G IMG=$QIMG AUTHORIZED_KEYS=$QA/id_test.pub MODULES=$MODS \
		"$ROOT/scripts/netroot-serve.sh" build >"$QA/build.log" 2>&1 || { tail "$QA/build.log"; exit 1; }
fi
# "UFS": ext4 labelled t2ufs (checks the mount wrapper by device and by LABEL=)
rm -f "$QA/ufs.img"
truncate -s 64M "$QA/ufs.img"
docker run --rm -u "$(id -u):$(id -g)" -v "$QA:/q" titan2-kbuild mke2fs -q -F -t ext4 -L t2ufs /q/ufs.img

FAIL=
ok() { echo "NRTEST PASS: $*"; }
bad() { echo "NRTEST FAIL: $*"; FAIL=1; }
SSH="ssh -i $QA/id_test -p 10822 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o BatchMode=yes -o LogLevel=ERROR root@127.0.0.1"

serve_start() {
	CTR=titan2-netroot-qemu-nbd LISTEN=127.0.0.1 IMG=$QIMG \
		"$ROOT/scripts/netroot-serve.sh" serve >"$QA/serve.log" 2>&1 &
	local i=0
	until grep -q "nbd-server" "$QA/serve.log" 2>/dev/null && ss -ltn | grep -q "127.0.0.1:10809 "; do
		i=$((i + 1)); [ $i -lt 120 ] || { cat "$QA/serve.log"; return 1; }
		sleep 1
	done
}
serve_stop() { docker rm -f titan2-netroot-qemu-nbd >/dev/null 2>&1 || true; wait 2>/dev/null || true; }

# qemu in a throwaway container (host network: slirp goes to 127.0.0.1 of the host)
qemu_start() { # <name> <extra cmdline>
	local name=$1; shift
	docker rm -f titan2-netroot-qemu >/dev/null 2>&1 || true
	docker run -d --rm --name titan2-netroot-qemu --network host -v "$ROOT:/work" -e UIDGID="$(id -u):$(id -g)" \
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
	-drive if=virtio,file=ufs.img,format=raw \
	-netdev user,id=n0,net=172.16.42.0/24,host=172.16.42.2,dhcpstart=172.16.42.100,hostfwd=tcp:127.0.0.1:10822-172.16.42.1:22 \
	-device virtio-net-pci,netdev=n0,romfile= \
	-append "console=ttyAMA0 loglevel=6 t2.root_if=eth0 t2.noreboot t2.protect=*/4010000000.pcie/* '"$*"'" </dev/null >'"$name"'.log 2>&1 || echo "qemu: exit $?" >>'"$name"'.log
' >/dev/null
}
qemu_wait_ssh() { # <seconds>
	local i=0
	until $SSH true 2>/dev/null; do
		docker ps -q -f name=^titan2-netroot-qemu\$ | grep -q . || return 1
		i=$((i + 5)); [ $i -lt "$1" ] || return 1
		sleep 5
	done
}
qemu_end() { # <name>: wait for qemu to exit (poweroff), otherwise kill it
	local i=0
	while docker ps -q -f name=^titan2-netroot-qemu\$ | grep -q .; do
		i=$((i + 2)); [ $i -lt "${2:-120}" ] || { docker rm -f titan2-netroot-qemu >/dev/null; echo "(killed)"; break; }
		sleep 2
	done
	grep -a "titan2\|Kernel panic\|EXT4-fs error\|nbd" "$QA/$1.log" | grep -av "^\[.*audit" | tail -25 | sed "s/^/[$1] /" || true
}

trap 'docker rm -f titan2-netroot-qemu titan2-netroot-qemu-nbd >/dev/null 2>&1 || true' EXIT
serve_start || { bad "serve did not start"; exit 1; }
ok "nbd-server on 127.0.0.1:10809 ($(grep -m1 "version" "$QA/serve.log"))"

# --- 1. first boot ---
qemu_start boot1 t2.root=nbd:172.16.42.2:10809:t2root
if qemu_wait_ssh 600; then
	ok "boot1: ssh into the netroot system"
	out=$($SSH 'cat /etc/alpine-release; uname -r; awk "\$2==\"/\"" /proc/mounts; cat /run/t2-netroot;
		rc-status -a 2>&1 | grep -E "t2-|dropbear|networking"; ls -l /bin/sh /bin/mount /sbin/init;
		cat /proc/sys/kernel/hotplug; echo vda_ro=$(cat /sys/block/vda/ro) nbd0_ro=$(cat /sys/block/nbd0/ro);
		echo ro_lock=$(cat /sys/module/block/parameters/ro_lock 2>/dev/null); blockdev --setrw /dev/vda 2>&1 && echo VDA-SETRW-OK;
		dd if=/dev/zero of=/dev/vda bs=512 count=1 2>&1 && echo VDA-WRITE-OK;
		mkdir -p /mnt/x; mount -o rw /dev/vda /mnt/x 2>&1; mount -t ext4 /dev/vda /mnt/x 2>&1; umount /mnt/x 2>/dev/null;
		echo persist-$(date +%s) >/root/persist.txt; sync; cat /root/persist.txt;
		test -e /etc/t2-netroot/firstboot && echo FIRSTBOOT-PENDING; tail -3 /var/log/t2-firstboot.log;
		echo nbd_oom=$(cat /proc/$(pidof nbd-client)/oom_score_adj); ip -4 addr show eth0 | grep inet; rc-service t2-usbnet status;
		for m in /dev/shm /dev/pts /dev/mqueue /run /proc /sys; do awk -v m=$m "\$2==m {print \"MNT\", \$2, \$3, substr(\$4, 1, 2)}" /proc/mounts; done;
		touch /dev/shm/t2test && echo SHM-WRITE-OK; rm -f /dev/shm/t2test;
		mkdir -p /mnt/t; mount -t tmpfs -o size=1m t2tmp /mnt/t && touch /mnt/t/x && echo TMPFS-RW-OK; umount /mnt/t;
		mount /dev/vda /mnt/x 2>&1 && awk "\$2==\"/mnt/x\" {print \"VDAMNT\", \$1, substr(\$4, 1, 2)}" /proc/mounts; umount /mnt/x 2>/dev/null;
		mount LABEL=t2ufs /mnt/x 2>&1 && awk "\$2==\"/mnt/x\" {print \"LABELMNT\", \$1, substr(\$4, 1, 2)}" /proc/mounts; umount /mnt/x 2>/dev/null;
		mount -o rw LABEL=t2ufs /mnt/x 2>&1; mount -o remount,rw /dev/shm && echo SHM-REMOUNT-OK;
		echo hotplug=$(cat /proc/sys/kernel/hotplug); pidof udevd >/dev/null && echo UDEVD-RUNNING;
		rc-status -a 2>&1 | grep -E "udev|seatd|chronyd|t2-defroute";
		ip route show default; cat /etc/resolv.conf; ls /lib/modules/$(uname -r)/modules.dep 2>&1;
		ls -l /lib/firmware/arm/mali/arch11.8/; ls -l /etc/udev/rules.d/' 2>&1) || true
	echo "$out" | sed 's/^/[boot1] /'
	echo "$out" | grep -q '^3\.24' && ok "Alpine 3.24 userspace" || bad "alpine-release"
	echo "$out" | grep -q '^/dev/nbd0 / ext4 rw' && ok "root is /dev/nbd0 ext4 rw" || bad "root mount"
	echo "$out" | grep -q 'vda_ro=1 nbd0_ro=0' && ok "vda (UFS stand-in) read-only, nbd0 writable" || bad "ro flags"
	echo "$out" | grep -q VDA-WRITE-OK && bad "write to vda succeeded" || ok "write to vda refused"
	echo "$out" | grep -q '^ro_lock=Y' && ok "block.ro_lock set" || bad "block.ro_lock not set"
	echo "$out" | grep -q VDA-SETRW-OK && bad "BLKROSET 0 on vda succeeded" || ok "BLKROSET 0 on vda refused (locked)"
	echo "$out" | grep -q 'is on the UFS: read-only only' && ok "mount wrapper refuses rw on UFS" || bad "mount wrapper"
	echo "$out" | grep -q '^/sbin/t2-hotplug' && ok "uevent helper /sbin/t2-hotplug" || bad "hotplug helper"
	echo "$out" | grep -q 't2-roguard.*started' && ok "t2-roguard started" || bad "t2-roguard"
	echo "$out" | grep -q 'dropbear.*started' && ok "dropbear (openrc) started" || bad "dropbear"
	echo "$out" | grep -q FIRSTBOOT-PENDING && bad "firstboot still pending" || ok "firstboot done"
	echo "$out" | grep -q "^nbd_oom=-1000" && ok "nbd-client oom_score_adj -1000" || bad "nbd-client oom_score_adj"
	echo "$out" | grep -q 'first boot OK' && ok "firstboot log OK" || bad "firstboot log"
	for m in /dev/shm /dev/pts /dev/mqueue; do
		echo "$out" | grep -q "^MNT $m [a-z]* rw" && ok "$m rw" || bad "$m not rw"
	done
	echo "$out" | grep -q SHM-WRITE-OK && ok "write to /dev/shm" || bad "write to /dev/shm"
	echo "$out" | grep -q TMPFS-RW-OK && ok "mount -t tmpfs: rw" || bad "mount -t tmpfs rw"
	echo "$out" | grep -q SHM-REMOUNT-OK && ok "remount,rw /dev/shm allowed" || bad "remount,rw /dev/shm"
	echo "$out" | grep -q '^VDAMNT /dev/vda ro' && ok "mount /dev/vda: forced ro" || bad "mount /dev/vda not ro"
	echo "$out" | grep -q '^LABELMNT /dev/vda ro' && ok "mount LABEL=t2ufs: forced ro" || bad "mount LABEL= not ro"
	echo "$out" | grep -q '^LABELMNT /dev/vda rw\|^VDAMNT /dev/vda rw' && bad "vda mounted rw"
	[ "$(echo "$out" | grep -c 'is on the UFS: read-only only')" -ge 2 ] &&
		ok "rw on vda refused (device and LABEL=)" || bad "rw on vda via LABEL= not refused"
	echo "$out" | grep -q '^hotplug=/sbin/t2-hotplug' && ok "uevent helper kept with udev" || bad "uevent helper lost (udev)"
	echo "$out" | grep -q UDEVD-RUNNING && ok "udevd running" || bad "udevd"
	for sv in udev seatd chronyd t2-defroute; do
		echo "$out" | grep -q " $sv .*started" && ok "$sv started" || bad "$sv not started"
	done
	echo "$out" | grep -q '^default via 172.16.42.2' && ok "default route via 172.16.42.2" || bad "default route"
	echo "$out" | grep -q '^nameserver ' && ok "resolv.conf" || bad "resolv.conf"
	echo "$out" | grep -q 'mali_csffw.bin$' && ok "mali_csffw.bin (uncompressed)" || bad "mali_csffw.bin"
	echo "$out" | grep -q '00-t2-roguard.rules' && ok "udev rule 00-t2-roguard.rules" || bad "udev rule"
	if [ -n "$MODS" ]; then
		echo "$out" | grep -q '^/lib/modules/.*/modules.dep$' && ok "modules for $(echo "$out" | sed -n 's|^/lib/modules/\([^/]*\)/modules.dep$|\1|p')" || bad "modules"
	fi
	$SSH poweroff >/dev/null 2>&1 || true
else
	bad "boot1: no ssh"
fi
qemu_end boot1 180
grep -aq "Power down\|reboot: Power" "$QA/boot1.log" && ok "boot1: clean poweroff" || bad "boot1: poweroff"
grep -aq "mount-ro failed\|ERROR: mount-ro" "$QA/boot1.log" && bad "boot1: mount-ro failed at shutdown" || ok "boot1: mount-ro (root remounted ro) at shutdown"

# --- 2. second boot ---
qemu_start boot2 t2.root=nbd:172.16.42.2:10809:t2root
if qemu_wait_ssh 300; then
	out=$($SSH 'cat /root/persist.txt; test -e /etc/t2-netroot/firstboot && echo FIRSTBOOT-PENDING; dmesg | grep "nbd0" | grep -i "recovery\|error\|shutting down\|failed"; ls /etc/runlevels/shutdown' 2>&1) || true
	echo "$out" | sed 's/^/[boot2] /'
	echo "$out" | grep -q '^persist-' && ok "boot2: data persisted across reboot" || bad "boot2: persistence"
	echo "$out" | grep -qi "recovery\|error\|shutting down\|failed" && bad "boot2: nbd0/ext4 errors or journal recovery (unclean boot1 shutdown)" || ok "boot2: clean ext4 (boot1 shut down cleanly, no NBD errors)"
	echo "$out" | grep -q "^t2-killprocs" && ok "shutdown runlevel uses t2-killprocs" || bad "t2-killprocs not in shutdown runlevel"
	# s2idle with the root on NBD: the freezer wakes nbd-client (ioctl NBD_DO_IT) with a fake signal;
	# without the kernel fix (nbd: freezable wait) the socket was closed and the root returned EIO.
	# pm_test freezer and devices (virtio-net goes through suspend/resume), then write/read on the root.
	for lvl in freezer devices; do
		out=$($SSH "echo $lvl >/sys/power/pm_test && echo freeze >/sys/power/state; echo rc=\$?" 2>&1) || true
		echo "$out" | sed "s/^/[boot2 pm_test=$lvl] /"
		echo "$out" | grep -q '^rc=0' && ok "pm_test=$lvl + freeze: suspend cycle done" || bad "pm_test=$lvl + freeze"
	done
	out=$($SSH 'echo none >/sys/power/pm_test; dd if=/dev/urandom of=/root/pmtest bs=64k count=64 2>/dev/null;
		a=$(md5sum </root/pmtest); sync; echo 3 >/proc/sys/vm/drop_caches; b=$(md5sum </root/pmtest);
		[ -s /root/pmtest ] && [ -n "$a" ] && [ "$a" = "$b" ] && echo PM-RW-OK; rm -f /root/pmtest; sync;
		dmesg | grep -iE "nbd0|I/O error|EXT4-fs (error|warning)|Restarting tasks|suspend entry|shutting down" | tail -12;
		awk "\$2==\"/\"" /proc/mounts; ls -l /usr/bin/t2-reboot; grep reboot-mode /etc/inittab' 2>&1) || true
	echo "$out" | sed 's/^/[boot2 pm] /'
	echo "$out" | grep -q PM-RW-OK && ok "root on NBD read/write after s2idle cycles" || bad "root I/O after s2idle"
	echo "$out" | grep -qiE "shutting down sockets|I/O error|EXT4-fs error|Connection timed out" && bad "nbd0 errors after s2idle" || ok "no nbd/ext4 errors after s2idle"
	echo "$out" | grep -q '^/dev/nbd0 / ext4 rw' && ok "root still rw after s2idle" || bad "root not rw after s2idle"
	echo "$out" | grep -q '::shutdown:/usr/lib/t2/reboot-mode' && ok "inittab: reboot-mode hook" || bad "inittab hook"
	# reboot with an argument: properly through init (openrc shutdown, mount-ro), then reboot(2) RESTART2
	$SSH t2-reboot bootloader >/dev/null 2>&1 || true
else
	bad "boot2: no ssh"
fi
qemu_end boot2 180
grep -aq "reboot: Restarting system with command 'bootloader'" "$QA/boot2.log" && ok "boot2: t2-reboot bootloader -> RESTART2 'bootloader'" || bad "boot2: no restart with 'bootloader'"
grep -aq "mount-ro failed\|ERROR: mount-ro" "$QA/boot2.log" && bad "boot2: mount-ro failed before t2-reboot" || ok "boot2: mount-ro before t2-reboot"
serve_stop

# --- 3. no server: stay in the initramfs ---
qemu_start nosrv t2.root=nbd:172.16.42.2:10809:t2root t2.root_wait=20
if qemu_wait_ssh 300; then
	out=$($SSH 'test -e /etc/t2/netroot && echo IN-INITRAMFS; dmesg | grep "netroot"; awk "\$2==\"/\"" /proc/mounts' 2>&1) || true
	echo "$out" | sed 's/^/[nosrv] /'
	echo "$out" | grep -q IN-INITRAMFS && ok "nosrv: stayed in initramfs with SSH" || bad "nosrv"
	$SSH t2-reboot recovery >/dev/null 2>&1 || true
else
	bad "nosrv: no ssh to initramfs"
fi
qemu_end nosrv 60
grep -aq "reboot: Restarting system with command 'recovery'" "$QA/nosrv.log" && ok "nosrv: t2-reboot recovery from initramfs" || bad "nosrv: no restart with 'recovery'"
[ -z "$FAIL" ] && echo "NRTEST RESULT: ALL PASS" || { echo "NRTEST RESULT: FAILURES"; exit 1; }
