# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# The t2-niri service in the rootfs; sourced from scripts/netroot-serve.sh build ($R = image root).
# Not added to a runlevel and no user is created: create the user named in /etc/conf.d/t2-niri
# (T2_NIRI_USER, default "user"; groups video, input, seat, audio), then rc-update add t2-niri default.
install -m 755 /work/tools/netroot/t2-niri.initd $R/etc/init.d/t2-niri
[ -e $R/etc/conf.d/t2-niri ] || install -m 644 /work/tools/netroot/t2-niri.conf $R/etc/conf.d/t2-niri
install -D -m 755 /work/tools/netroot/t2-niri-session $R/usr/local/bin/t2-niri-session
