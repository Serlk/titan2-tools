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
