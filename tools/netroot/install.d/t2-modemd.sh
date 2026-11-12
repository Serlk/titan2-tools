# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Installs the t2-modemd service (modem control: radio/SIM/data/calls/SMS over the AT and
# MIPC WWAN ports) into the rootfs; sourced from scripts/netroot-serve.sh build ($R = image
# root). Not added to a runlevel (rc-update add t2-modemd default). The binary
# (tools/modemd/t2-modemd-aarch64, static) goes to /usr/local/bin when present.
install -m 755 /work/tools/modemd/t2-modemd.initd $R/etc/init.d/t2-modemd
[ -e $R/etc/t2-modemd.conf ] || install -m 644 /work/tools/modemd/t2-modemd.conf $R/etc/t2-modemd.conf
[ -e $R/etc/conf.d/t2-modemd ] || printf '# /etc/conf.d/t2-modemd\n#T2_MODEMD_ARGS="-v"\n#T2_MODEMD_LOG=/var/log/t2-modemd.log\n#T2_MODEMD_AT=/dev/wwan0at0\n#T2_MODEMD_CONF=/etc/t2-modemd.conf\n' > $R/etc/conf.d/t2-modemd
if [ -x /work/tools/modemd/t2-modemd-aarch64 ]; then
	install -m 755 /work/tools/modemd/t2-modemd-aarch64 $R/usr/local/bin/t2-modemd
fi
