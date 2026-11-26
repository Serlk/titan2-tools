# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Android Translation Layer in the rootfs; sourced from scripts/netroot-serve.sh build ($R = image
# root, /work = the repository) when ATL=1. The packages themselves come from PKGS (ATL_PKGS in
# netroot-serve.sh, served by LOCAL_REPO = scripts/t2-repo.sh); here: the Titan 2 overlay
# (atl-run, atl-check, xkb keymap, niri/sway snippets) and the test APKs.
if [ -n "${ATL:-}" ]; then
	cp -a /work/tools/atl/overlay/. $R/
	chmod 755 $R/usr/local/bin/atl-run $R/usr/local/bin/atl-install $R/usr/local/bin/atl-check
	chmod 644 $R/etc/profile.d/titan2-panvk.sh
	# test APKs fetched and verified by scripts/build-atl.sh apks (blob id / F-Droid index signature)
	mkdir -p $R/usr/share/titan2/apks
	for a in /work/out/atl-dl/test-apks/*.apk; do
		[ -e "$a" ] && cp -f "$a" $R/usr/share/titan2/apks/
	done
	# no portal backend for DynamicLauncher yet: atl-install writes the .desktop files itself
	echo "ATL: overlay + $(ls $R/usr/share/titan2/apks 2>/dev/null | wc -l) test APK(s)"
fi
