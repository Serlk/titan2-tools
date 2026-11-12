# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Installs the sleep broker t2-powerd into the rootfs; sourced from scripts/netroot-serve.sh build
# ($R = image root). The binary tools/powerd/t2-powerd-aarch64 (cross build, see
# tools/powerd/README.md; dynamic: needs the dbus-libs package) is installed if it was built.
# Not added to a runlevel: rc-update add t2-powerd default.
install -m 755 /work/tools/powerd/t2-powerd.initd $R/etc/init.d/t2-powerd
[ -e $R/etc/t2-powerd.conf ] || install -m 644 /work/tools/powerd/t2-powerd.conf $R/etc/t2-powerd.conf
mkdir -p $R/etc/t2-powerd/hooks/maintenance.d $R/etc/t2-powerd/hooks/post-resume.d
mkdir -p $R/etc/t2-powerd/hooks/pre-suspend.d
for h in screen-off screen-on maintenance.d/10-time-sync post-resume.d/10-log pre-suspend.d/10-wifi-wowlan; do
	[ -e $R/etc/t2-powerd/hooks/$h ] || install -m 755 /work/tools/powerd/hooks/$h $R/etc/t2-powerd/hooks/$h
done
if [ -x /work/tools/powerd/t2-powerd-aarch64 ]; then
	install -m 755 /work/tools/powerd/t2-powerd-aarch64 $R/usr/local/bin/t2-powerd
	ln -sf t2-powerd $R/usr/local/bin/t2-powerctl
fi
