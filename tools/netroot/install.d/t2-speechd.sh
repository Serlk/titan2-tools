# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Installs the t2-speechd service (call speech) into the rootfs; sourced from
# scripts/netroot-serve.sh build ($R = image root). Not added to a runlevel
# (rc-update add t2-speechd default). The binary (tools/speechd/t2-speechd-aarch64, dynamic:
# needs the alsa-lib package) goes to /usr/local/bin if it was built. The codec routes of a call
# are set by the UCM verb "Voice Call" (tools/ucm/Titan2, README-voice-call.md);
# T2_SPEECHD_CODEC_ROUTES=1 in /etc/conf.d/t2-speechd restores the old behaviour.
install -m 755 /work/tools/speechd/t2-speechd.initd $R/etc/init.d/t2-speechd
[ -e $R/etc/conf.d/t2-speechd ] || install -m 644 /work/tools/speechd/t2-speechd.conf $R/etc/conf.d/t2-speechd
if [ -x /work/tools/speechd/t2-speechd-aarch64 ]; then
	install -m 755 /work/tools/speechd/t2-speechd-aarch64 $R/usr/local/bin/t2-speechd
fi
