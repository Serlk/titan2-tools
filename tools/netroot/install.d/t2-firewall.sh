# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Firewall in the rootfs (sourced from scripts/netroot-serve.sh build; $R = image root, /work = the
# repository): the t2-firewall script (ufw over iptables-nft: deny incoming except lo/wlan0/usb0,
# allow outgoing) and the OpenRC hook that applies it at boot once ufw is installed. The rule set
# itself is applied on the device (`t2-firewall`), because ufw stores its state under /etc/ufw of
# the running system; the local.d hook re-applies it on every boot so that the carrier network
# never sees an open port even after an image update. Packages: ufw iptables (PKGS).
install -D -m 755 /work/tools/netroot/t2-firewall $R/usr/local/sbin/t2-firewall
install -D -m 755 /work/tools/netroot/local.d/t2-firewall.start $R/etc/local.d/t2-firewall.start
echo "firewall: t2-firewall + local.d hook (apply on the phone: t2-firewall)"
