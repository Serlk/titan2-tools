# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Small Titan 2 items in the rootfs; sourced from scripts/netroot-serve.sh build ($R = image root):
# keyboard backlight at boot (service local), bluetoothd in default, the BT module still loaded by
# hand (loading BT together with Wi-Fi at boot is not tested yet).
mkdir -p $R/etc/local.d $R/etc/modprobe.d
install -m 755 /work/tools/netroot/local.d/t2-kbd-light.start $R/etc/local.d/
install -m 644 /work/tools/netroot/modprobe.d/btmtkbgf-noauto.conf $R/etc/modprobe.d/
install -m 644 /work/tools/netroot/modprobe.d/titan2-display-te-sync.conf $R/etc/modprobe.d/
# Bluetooth works (btmtkbgf EMI preparation + HCI fixes):
# bluetoothd always on; the driver is still loaded by hand until BT + Wi-Fi at boot is tested.
[ -e $R/etc/init.d/bluetooth ] && ln -sf /etc/init.d/bluetooth $R/etc/runlevels/default/bluetooth
# performance/thermal: the uclamp floor of RT tasks (needs
# CONFIG_UCLAMP_TASK), the sampling tool for the measurement plan, the Wi-Fi firmware log off
install -D -m 644 /work/tools/netroot/sysctl.d/60-titan2-sched.conf $R/etc/sysctl.d/60-titan2-sched.conf
install -D -m 755 /work/tools/netroot/t2-perfstat $R/usr/local/bin/t2-perfstat
install -m 755 /work/tools/netroot/local.d/t2-quiet-log.start $R/etc/local.d/
# GPU DVFS policy switches and the measurement run
install -m 644 /work/tools/netroot/modprobe.d/titan2-panthor.conf $R/etc/modprobe.d/
install -m 755 /work/tools/netroot/local.d/t2-gpu-policy.start $R/etc/local.d/
install -D -m 755 /work/tools/netroot/t2-perfrun $R/usr/local/bin/t2-perfrun
