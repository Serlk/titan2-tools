# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Shared parts of attaching the root (sourced via ". /etc/t2/rootlib.sh" after lib.sh):
# /etc/t2/netroot (NBD/NFS) and /etc/t2/diskroot (the linux partition on the UFS). The new root
# is $NR.

NR=/newroot
T2_ADDR=172.16.42.1

# t2.root_if=<if> (qemu only): bring this interface up instead of the USB gadget, like
# /etc/t2/usb-gadget
t2_root_if_setup() {
	ip link set "$1" up
	ip addr add $T2_ADDR/24 dev "$1" 2>/dev/null
	echo "$1" >/run/t2-usb-if
}

# First boot: the package scripts that apk on the PC could not run (aarch64).
# The pseudo file systems for the chroot are temporary; /etc/t2/init moves /dev /proc /sys /run
# for good. Returns 1 if the pseudo file systems could not be mounted (the caller undoes
# everything and exits).
t2_firstboot() {
	[ -e $NR/etc/t2-netroot/firstboot ] || return 0
	t2_screen "titan2: first boot, running package scripts"
	mount -t proc proc $NR/proc && mount --bind /dev $NR/dev && mount -t sysfs sysfs $NR/sys ||
		return 1
	if chroot $NR /bin/busybox sh /etc/t2-netroot/firstboot >/run/t2-firstboot.log 2>&1; then
		rm -f $NR/etc/t2-netroot/firstboot
		t2_log "root: first boot done"
	else
		t2_log "root: first boot script failed (/var/log/t2-firstboot.log)"
	fi
	mkdir -p $NR/var/log && cp /run/t2-firstboot.log $NR/var/log/t2-firstboot.log
	umount $NR/sys $NR/dev $NR/proc
}

# UFS protection and the USB network link in the new system (on every boot: the versions from
# the initramfs).
#   t2_install_guard nbd|nfs|part
# nbd: t2-killprocs instead of killprocs at shutdown (SIGSTOP breaks NBD); otherwise the stock
# killprocs. part: t2-growroot (ext4 over the whole partition, once).
t2_install_guard() {
	local proto=$1 d=/etc/t2/netroot.d s
	mkdir -p $NR/usr/lib/t2 $NR/etc/init.d $NR/etc/runlevels/sysinit $NR/etc/runlevels/default \
		$NR/etc/runlevels/shutdown
	cp /etc/t2/lib.sh $NR/usr/lib/t2/lib.sh
	cp /sbin/t2-hotplug $NR/sbin/t2-hotplug
	cp $d/mount-wrapper $d/roguard-loop $NR/usr/lib/t2/
	cp $d/t2-roguard $d/t2-usbnet $d/t2-killprocs $d/t2-growroot $NR/etc/init.d/
	# reboot with an argument (bootloader/recovery): t2-reboot + the last ::shutdown action
	mkdir -p $NR/usr/bin
	cp /usr/bin/t2-reboot $NR/usr/bin/t2-reboot
	cp /usr/lib/t2/reboot-mode $NR/usr/lib/t2/reboot-mode
	for s in sbin/t2-hotplug usr/lib/t2/mount-wrapper usr/lib/t2/roguard-loop usr/lib/t2/reboot-mode \
		usr/bin/t2-reboot etc/init.d/t2-roguard etc/init.d/t2-usbnet etc/init.d/t2-killprocs \
		etc/init.d/t2-growroot; do
		chmod 755 $NR/$s
	done
	# after "openrc shutdown" (mount-ro): busybox init runs the ::shutdown actions in order
	if [ -f $NR/etc/inittab ] && ! grep -q '^::shutdown:/usr/lib/t2/reboot-mode' $NR/etc/inittab; then
		printf '%s\n' '# titan2: t2-reboot <mode> (installed by the initramfs), last' \
			'::shutdown:/usr/lib/t2/reboot-mode' >>$NR/etc/inittab
	fi
	# /bin/mount: busybox --install does not overwrite the link, a busybox update neither
	rm -f $NR/bin/mount
	ln -s /usr/lib/t2/mount-wrapper $NR/bin/mount
	ln -sf /etc/init.d/t2-roguard $NR/etc/runlevels/sysinit/t2-roguard
	ln -sf /etc/init.d/t2-usbnet $NR/etc/runlevels/default/t2-usbnet
	# shutdown: killprocs (kill_all sends SIGSTOP to everything, breaking NBD) -> t2-killprocs.
	# The same image may boot over NBD and from the partition: the right one is set every time.
	if [ "$proto" = nbd ]; then
		rm -f $NR/etc/runlevels/shutdown/killprocs
		ln -sf /etc/init.d/t2-killprocs $NR/etc/runlevels/shutdown/t2-killprocs
	else
		rm -f $NR/etc/runlevels/shutdown/t2-killprocs
		[ -e $NR/etc/init.d/killprocs ] && ln -sf /etc/init.d/killprocs $NR/etc/runlevels/shutdown/killprocs
	fi
	if [ "$proto" = part ]; then
		ln -sf /etc/init.d/t2-growroot $NR/etc/runlevels/default/t2-growroot
	else
		rm -f $NR/etc/runlevels/default/t2-growroot
	fi
	# udev (eudev): read-only for every new block device from udevd (the udev service clears
	# the uevent helper; roguard-loop puts it back, this is one more line of defence)
	mkdir -p $NR/etc/udev/rules.d
	cat >$NR/etc/udev/rules.d/00-t2-roguard.rules <<'RULES'
# titan2 (installed by the initramfs on every boot): read-only for the UFS except the root partition, see /sbin/t2-hotplug
ACTION=="add|change", SUBSYSTEM=="block", KERNEL!="nbd*|loop*|zram*|ram*", RUN+="/sbin/t2-hotplug %k"
RULES
	# mdev (if someone enables it) takes over /proc/sys/kernel/hotplug: the same rule in mdev.conf
	if [ -f $NR/etc/mdev.conf ] && ! grep -q t2-hotplug $NR/etc/mdev.conf; then
		sed -i '1i # titan2: read-only for the UFS (sd*, mmcblk*), see /sbin/t2-hotplug\n-(sd[a-z]|mmcblk)[0-9a-z]* root:disk 0660 */sbin/t2-hotplug' \
			$NR/etc/mdev.conf
	fi
}
