# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# NetworkManager for Wi-Fi (and the modem); sourced from scripts/netroot-serve.sh build
# ($R = image root). NetworkManager leaves the USB link to the PC (usb*/rndis*/ncm*) alone: the
# initramfs, t2-usbnet and t2-defroute set it up (the route via the PC has metric 750, below
# Wi-Fi). No network profiles with passwords go into the image: use nmcli on the phone.
mkdir -p $R/etc/NetworkManager/conf.d
install -m 644 /work/tools/netroot/NetworkManager-titan2.conf $R/etc/NetworkManager/conf.d/titan2.conf
rl default dbus
rl default networkmanager
