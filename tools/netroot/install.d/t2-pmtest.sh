# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# t2-pmtest: one logged suspend attempt with an RTC alarm.
# Sourced from scripts/netroot-serve.sh build ($R = the image root). Not a service:
# run by hand, `t2-pmtest -n` first.
mkdir -p $R/usr/local/bin
install -m 755 /work/tools/netroot/t2-pmtest $R/usr/local/bin/
# t2-s2idle-test: one real s2idle with WoWLAN, the USB gadget detached and every result on disk
install -m 755 /work/tools/netroot/t2-s2idle-test $R/usr/local/sbin/
# t2-sleep-series: unattended t2-s2idle-test series for the SPM 26 MHz clock experiments
install -m 755 /work/tools/netroot/t2-sleep-series $R/usr/local/sbin/
# ffrumble: FF_RUMBLE test for the haptic (no fftest in Alpine)
install -m 755 /work/tools/netroot/ffrumble $R/usr/local/bin/
