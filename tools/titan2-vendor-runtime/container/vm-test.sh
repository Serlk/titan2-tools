#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Smoke test inside an aarch64 qemu VM whose kernel has binderfs: start the vendor
# container from a vendor root and show what the binder managers do.
#   SHARE=/host/vendor sh vm-test.sh
# SHARE: a directory visible in the VM with this directory as $SHARE/src/container and
# a vendor root built by t2-vendor-setup as $SHARE/vendor-root.
set -u
SHARE=${SHARE:-/host/vendor}
C=$SHARE/src/container/t2-vendor-container
# the root is copied onto the VM disk: bind mounts over a 9p share cannot be undone.
# Never rm -rf it: with /dev or /sys still bound below it, rm walks into the real ones.
export T2_VENDOR_ROOT=/root/vendor-root T2_CONTAINER_LOG=/root/t2-vendor-log
sh $C stop >/dev/null 2>&1
mkdir -p /root/vendor-root
for d in system vendor data mnt t2-props; do cp -a $SHARE/vendor-root/$d /root/vendor-root/; done
rm -f /root/t2-vendor-log/*.log
echo "## kernel $(uname -r); binder: $(grep -c binder /proc/filesystems) fs; selinux: $(cat /sys/fs/selinux/enforce 2>/dev/null || echo none)"
sh $C start
sleep 2
echo "## logs"
for f in /root/t2-vendor-log/*.log; do echo "--- $f"; tail -15 "$f"; done
echo "## status again"
sh $C status
