# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Installs the t2-modem service into the rootfs; sourced from scripts/netroot-serve.sh build
# ($R = image root). Not added to a runlevel. The daemons (t2-ccci-fsd, t2-ccci-rpcd) and the
# NVRAM sandbox are installed separately: the sandbox is a copy of your phone's partitions and
# never goes into the image.
install -m 755 /work/tools/netroot/t2-modem.initd $R/etc/init.d/t2-modem
[ -e $R/etc/conf.d/t2-modem ] || install -m 644 /work/tools/netroot/t2-modem.conf $R/etc/conf.d/t2-modem
