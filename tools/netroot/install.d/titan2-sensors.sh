# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Sensors (SCP sensor hub → mtk-hf-iio → IIO): udev rules for iio-sensor-proxy and its
# service. Sourced from scripts/netroot-serve.sh build ($R = image root, /work = repo).
# Packages: iio-sensor-proxy iio-sensor-proxy-openrc (GFX_PKGS). Kernel modules load
# by themselves (scp, sensorhub, mtk-hf-iio via /etc/modules-load.d below).
install -D -m 644 /work/tools/sensors/90-titan2-sensors.rules $R/etc/udev/rules.d/90-titan2-sensors.rules
mkdir -p $R/etc/modules-load.d
printf '%s\n' scp hf_manager sensorhub mtk-hf-iio > $R/etc/modules-load.d/titan2-sensors.conf
[ -e $R/etc/init.d/iio-sensor-proxy ] && ln -sf /etc/init.d/iio-sensor-proxy $R/etc/runlevels/default/iio-sensor-proxy
# the niri session is not a logind "active" session: let members of group input claim sensors
install -D -m 644 /work/tools/netroot/polkit/50-titan2-sensors.rules $R/etc/polkit-1/rules.d/50-titan2-sensors.rules
# Calibration records of the hub (tools/sensors/CALIBRATION.md): the boot step t2-sensors-cali
# (read-only import from nvcfg with debugfs into /var/lib/titan2-sensors, records -> hub_cali)
# in the boot runlevel, the writer t2-sensors-calisave (hub-learned values -> the local copy,
# never nvcfg) in default. Needs debugfs (e2fsprogs-extra) in the image. The binary is the
# cross build tools/sensors/t2-sensors-cali-aarch64 (Makefile), unless the package
# titan2-sensors already put /usr/bin/t2-sensors-cali into the image.
if [ ! -x $R/usr/bin/t2-sensors-cali ] && [ -x /work/tools/sensors/t2-sensors-cali-aarch64 ]; then
	install -D -m 755 /work/tools/sensors/t2-sensors-cali-aarch64 $R/usr/bin/t2-sensors-cali
	install -D -m 755 /work/tools/sensors/openrc/t2-sensors-cali $R/etc/init.d/t2-sensors-cali
	install -D -m 755 /work/tools/sensors/openrc/t2-sensors-calisave $R/etc/init.d/t2-sensors-calisave
	[ -e $R/etc/conf.d/t2-sensors-cali ] ||
		install -D -m 644 /work/tools/sensors/openrc/t2-sensors-cali.confd $R/etc/conf.d/t2-sensors-cali
fi
if [ -e $R/etc/init.d/t2-sensors-cali ]; then
	mkdir -p $R/etc/runlevels/boot $R/etc/runlevels/default
	ln -sf /etc/init.d/t2-sensors-cali $R/etc/runlevels/boot/t2-sensors-cali
	ln -sf /etc/init.d/t2-sensors-calisave $R/etc/runlevels/default/t2-sensors-calisave
fi
