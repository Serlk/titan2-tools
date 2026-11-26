#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Fake IIO sysfs tree for testing the ATL sensor backend (patch 0007) without hardware:
#   fake-iio.sh DIR            create DIR/iio:device1..6 (hub-accel/gyro/magn/light/prox as the
#                              Titan 2 bridge mtk-hf-iio exposes them + a generic accelerometer)
#   fake-iio.sh DIR set NAME ATTR VALUE   change one value, e.g. set hub-prox in_proximity_raw 5
# Run the app with ATL_IIO_ROOT=DIR. Values: readings of a Titan 2 lying on a table.
set -eu
d=${1:?DIR}
if [ "${2:-}" = set ]; then
	for n in "$d"/*/name; do
		[ "$(cat "$n")" = "${3:?NAME}" ] && { printf '%s\n' "${5:?VALUE}" >"$(dirname "$n")/${4:?ATTR}"; exit 0; }
	done
	echo "fake-iio: no device $3" >&2; exit 1
fi
mkdir -p "$d"
dev() { mkdir -p "$d/$1"; echo "$2" >"$d/$1/name"; }
w() { printf '%s\n' "$3" >"$d/$1/$2"; }
dev iio:device1 hub-accel
w iio:device1 in_accel_x_raw -560; w iio:device1 in_accel_y_raw -8460; w iio:device1 in_accel_z_raw 1110
w iio:device1 in_accel_scale 0.001000; w iio:device1 in_mount_matrix "1, 0, 0; 0, 1, 0; 0, 0, 1"
dev iio:device2 hub-gyro
w iio:device2 in_anglvel_x_raw 60000; w iio:device2 in_anglvel_y_raw 50000; w iio:device2 in_anglvel_z_raw -30000
w iio:device2 in_anglvel_scale 0.000001
dev iio:device3 hub-magn
w iio:device3 in_magn_x_raw -22500; w iio:device3 in_magn_y_raw 7600; w iio:device3 in_magn_z_raw 3600
w iio:device3 in_magn_scale 0.000100
dev iio:device4 hub-light
w iio:device4 in_illuminance_raw 20; w iio:device4 in_illuminance_scale 1
dev iio:device5 hub-prox
w iio:device5 in_proximity_raw 0; w iio:device5 in_proximity_nearlevel 4
# generic accelerometer (e.g. a laptop's), axes swapped by its mount matrix
dev iio:device0 generic-accel
w iio:device0 in_accel_x_raw 0; w iio:device0 in_accel_y_raw 100; w iio:device0 in_accel_z_raw 0
w iio:device0 in_accel_scale 0.0980665; w iio:device0 in_accel_mount_matrix "0, 1, 0; 1, 0, 0; 0, 0, 1"
mkdir -p "$d/trigger0"; echo sysfstrig0 >"$d/trigger0/name"
echo "fake IIO tree in $d: $(cat "$d"/*/name | tr '\n' ' ')"
