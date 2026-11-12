#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Builds the ModemManager package of Alpine 3.24 WITH the mtk-ccci plugin, on
# the phone (or on any Alpine 3.24 aarch64 machine), using Alpine's own
# packaging (abuild). Result: a set of apk files, drop-in replacements for the
# repository's modemmanager-1.25.95_git20260527-r1, plus a tarball of the
# standalone plugin .so and the speech-trigger helper.
#
# What it needs on the phone: network access to dl-cdn.alpinelinux.org,
# gitlab.alpinelinux.org (Alpine's aports, for the packaging files) and
# gitlab.freedesktop.org (the ModemManager source tarball, ~5 MB), about 1 GB
# of free space, 15-30 minutes of CPU. Run as a normal user who may use sudo
# (abuild refuses root), e.g.:
#
#   scp -r tools/mm-titan2 phone:mm-titan2
#   ssh phone 'sh ~/mm-titan2/build-on-phone.sh'
#
# Alternative without abuild ("plugin only", MODE=plugin): builds the tree
# with meson and installs just the plugin .so and the udev rule into the
# installed modemmanager package. Same source commit, same options.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
MODE=${MODE:-apk}
work=${WORK:-$HOME/mm-titan2-build}
mm_commit=fcc9d19f5ed63eb6ca63e0f9af6d36d755c59a27
mm_tarball="ModemManager-$mm_commit.tar.gz"
mm_url="https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/archive/$mm_commit/$mm_tarball"

[ -f "$here/mm-mtk-ccci.patch" ] || { echo "mm-mtk-ccci.patch missing next to this script" >&2; exit 1; }
[ "$(id -u)" != 0 ] || { echo "run as a normal user (abuild refuses root); sudo is used where needed" >&2; exit 1; }

mkdir -p "$work"

case "$MODE" in
apk)
	echo "== installing the build tools"
	sudo apk add alpine-sdk sudo doas-sudo-shim 2>/dev/null || sudo apk add alpine-sdk
	sudo addgroup "$(id -un)" abuild 2>/dev/null || true
	[ -f "$HOME/.abuild/abuild.conf" ] || abuild-keygen -a -i -n

	pkg="$work/modemmanager"
	rm -rf "$pkg"; mkdir -p "$pkg"
	# Alpine's own packaging of modemmanager (aports, branch 3.24-stable). Not
	# copied into this repository: APORTS=<aports checkout> uses an existing one,
	# otherwise it is cloned (git clone -b 3.24-stable
	# https://gitlab.alpinelinux.org/alpine/aports.git).
	aports=${APORTS:-$work/aports}
	[ -d "$aports/community/modemmanager" ] ||
		git clone --depth 1 -b 3.24-stable https://gitlab.alpinelinux.org/alpine/aports.git "$aports"
	cp "$aports"/community/modemmanager/* "$pkg/"
	grep -q "^pkgver=1.25.95_git20260527$" "$pkg/APKBUILD" ||
		echo "warning: aports modemmanager is not 1.25.95_git20260527; mm-mtk-ccci.patch targets commit $mm_commit" >&2
	cp "$here/mm-mtk-ccci.patch" "$pkg/"
	cd "$pkg"

	# our patch on top of Alpine's, a new pkgrel, the plugin enabled explicitly,
	# the two plugin tests run with the others; the udev rule lands in the
	# -udev subpackage with the rest of the rules automatically
	sed -i 's/^pkgrel=.*/pkgrel=99/' APKBUILD
	sed -i 's|^\t0001-intel-XMM7360-fix-big-endian-handling-of-ASN-integer.patch$|\t0001-intel-XMM7360-fix-big-endian-handling-of-ASN-integer.patch\n\tmm-mtk-ccci.patch|' APKBUILD
	sed -i 's|-Dsystemd_suspend_resume=true \\|-Dsystemd_suspend_resume=true \\\n\t\t-Dplugin_mtk_ccci=enabled \\|' APKBUILD
	grep -q "mm-mtk-ccci.patch" APKBUILD || { echo "APKBUILD patch list edit failed" >&2; exit 1; }
	grep -q "plugin_mtk_ccci" APKBUILD || { echo "APKBUILD option edit failed" >&2; exit 1; }

	echo "== fetching sources and updating checksums"
	abuild checksum
	echo "== building (this takes a while)"
	abuild -r
	echo
	echo "== packages:"
	ls -1 "$HOME"/packages/*/aarch64/modemmanager-*.apk "$HOME"/packages/*/aarch64/libmm-glib-*.apk
	echo
	echo "install with (keeps the repository's version pinned out by the higher pkgrel):"
	echo "  sudo apk add --allow-untrusted \$HOME/packages/*/aarch64/{modemmanager,libmm-glib,modemmanager-udev,modemmanager-openrc}-1.25.95_git20260527-r99.apk"
	;;

plugin)
	echo "== installing the build tools and the dependencies"
	sudo apk add build-base meson ninja pkgconf python3 glib-dev dbus-dev libgudev-dev libmbim-dev \
		libqmi-dev libqrtr-glib-dev polkit-dev elogind-dev gettext-dev linux-headers curl \
		modemmanager libmm-glib modemmanager-udev
	cd "$work"
	[ -f "$mm_tarball" ] || curl -fL -o "$mm_tarball" "$mm_url"
	rm -rf "ModemManager-$mm_commit"
	tar xf "$mm_tarball"
	cd "ModemManager-$mm_commit"
	patch -p1 < "$here/mm-mtk-ccci.patch"
	# Alpine's options (ABI-relevant: suspend_resume, qmi, mbim, qrtr, udev, polkit)
	meson setup build --prefix=/usr --libdir=lib --localstatedir=/var --sysconfdir=/etc \
		-Dsystemdsystemunitdir=/usr/lib/systemd/system -Ddbus_policy_dir=/usr/share/dbus-1/system.d \
		-Dgtk_doc=false -Dpolkit=permissive -Dsystemd_journal=false -Dsystemd_suspend_resume=true \
		-Dvapi=false -Dintrospection=false -Dbash_completion=false -Dman=false \
		-Dplugin_mtk_ccci=enabled
	ninja -C build src/plugins/libmm-plugin-mtk-ccci.so src/plugins/test-plugin-mtk-ccci \
		src/plugins/test-plugin-mtk-ccci-port-mipc
	./build/src/plugins/test-plugin-mtk-ccci
	./build/src/plugins/test-plugin-mtk-ccci-port-mipc
	echo "== installing the plugin into the system ModemManager"
	sudo install -m 0755 build/src/plugins/libmm-plugin-mtk-ccci.so /usr/lib/ModemManager/
	sudo install -m 0644 src/plugins/mtk-ccci/77-mm-mtk-ccci.rules /usr/lib/udev/rules.d/
	sudo udevadm control --reload
	echo "installed: /usr/lib/ModemManager/libmm-plugin-mtk-ccci.so, /usr/lib/udev/rules.d/77-mm-mtk-ccci.rules"
	;;
*)
	echo "MODE must be apk or plugin" >&2; exit 2;;
esac

echo "== the speech-trigger helper"
sudo apk add build-base pkgconf libmm-glib-dev glib-dev 2>/dev/null || sudo apk add build-base pkgconf modemmanager-dev glib-dev
make -C "$here/speech-trigger" BIN="$work/t2-mm-speech-trigger"
echo "built $work/t2-mm-speech-trigger (install: sudo install -m 0755 $work/t2-mm-speech-trigger /usr/local/bin/)"
