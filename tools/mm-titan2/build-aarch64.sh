#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Cross-builds the mtk-ccci ModemManager plugin (and t2-mm-speech-trigger) for
# the phone: Alpine 3.24 aarch64 (musl), against a sysroot populated with
# Alpine's own packages so that the result matches the phone's
# modemmanager-1.25.95_git20260527 (commit fcc9d19f) binary.
#
# Host tools: meson, ninja, make, pkg-config, patchelf, and the toolchain below.
# Sysroot prerequisites (done once, needs docker and the network):
#   mkdir -p out/mm-aarch64/sysroot
#   docker run --rm --platform linux/amd64 -v $PWD/out/mm-aarch64:/sr alpine:3.24 sh -c '
#     apk --arch aarch64 --root /sr/sysroot --allow-untrusted --initdb \
#         -X https://dl-cdn.alpinelinux.org/alpine/v3.24/main \
#         -X https://dl-cdn.alpinelinux.org/alpine/v3.24/community --no-scripts \
#         add musl-dev linux-headers gcc glib-dev dbus-dev libgudev-dev libmbim-dev \
#             libqmi-dev libqrtr-glib-dev polkit-dev elogind-dev gettext-dev \
#             mobile-broadband-provider-info modemmanager libmm-glib modemmanager-dev
#     chown -R 1000:1000 /sr'
#   a ModemManager checkout at fcc9d19f (Alpine's _commit), default src/ModemManager:
#     git clone https://gitlab.freedesktop.org/mobile-broadband/ModemManager.git src/ModemManager
#     git -C src/ModemManager checkout fcc9d19f
#   AOSP clang r574158 in toolchains/clang-r574158 (or TC=<dir with clang, lld, llvm-ar>).
#
# Usage: tools/mm-titan2/build-aarch64.sh [MM_TREE]   (or MM_TREE=..., SR=..., TC=..., OUT=...)
# Output: $OUT/dist/ (default out/mm-aarch64/dist/) (plugin .so, helper, udev rules, sha256sums)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
mm=${1:-${MM_TREE:-$repo/src/ModemManager}}
OUT=${OUT:-$repo/out/mm-aarch64}
SR=${SR:-$OUT/sysroot}
TC=${TC:-$repo/toolchains/clang-r574158/bin}
out=$OUT
dist=$out/dist

command -v patchelf >/dev/null || { echo "patchelf is required to remove build-tree library paths" >&2; exit 1; }
[ -d "$SR/usr/lib/pkgconfig" ] || { echo "sysroot $SR is not populated (see the header of this script)" >&2; exit 1; }
[ -x "$TC/clang" ] || { echo "clang not found in $TC" >&2; exit 1; }
gccdir=$(ls -d "$SR"/usr/lib/gcc/aarch64-alpine-linux-musl/* 2>/dev/null | head -1)
[ -n "$gccdir" ] || { echo "the sysroot needs the gcc package (crt files, libgcc)" >&2; exit 1; }

"$here/apply.sh" "$mm" >/dev/null

cross=$out/cross.ini
cat > "$cross" <<EOF
[binaries]
c = ['$TC/clang', '--target=aarch64-alpine-linux-musl', '--sysroot=$SR', '--gcc-toolchain=$SR/usr', '-fuse-ld=lld']
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
pkg-config = 'pkg-config'

[properties]
sys_root = '$SR'
pkg_config_libdir = ['$SR/usr/lib/pkgconfig', '$SR/usr/share/pkgconfig']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

build=$mm/build-aarch64
if [ ! -f "$build/build.ninja" ]; then
	# the same options as Alpine's APKBUILD (abuild-meson defaults: prefix /usr,
	# libdir /usr/lib, localstatedir /var), documentation and bindings off
	meson setup "$build" "$mm" --cross-file "$cross" \
		--prefix=/usr --libdir=lib --localstatedir=/var --sysconfdir=/etc \
		-Dsystemdsystemunitdir=/usr/lib/systemd/system \
		-Ddbus_policy_dir=/usr/share/dbus-1/system.d \
		-Dgtk_doc=false -Dpolkit=permissive -Dsystemd_journal=false \
		-Dsystemd_suspend_resume=true -Dvapi=false -Dintrospection=false \
		-Dbash_completion=false -Dman=false \
		-Dplugin_mtk_ccci=enabled
fi

ninja -C "$build" src/plugins/libmm-plugin-mtk-ccci.so

# the helper, against the sysroot's libmm-glib
helper=$out/t2-mm-speech-trigger
PKG_CONFIG_SYSROOT_DIR=$SR PKG_CONFIG_LIBDIR=$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig \
	make -C "$here/speech-trigger" BIN="$helper" \
	CC="$TC/clang --target=aarch64-alpine-linux-musl --sysroot=$SR --gcc-toolchain=$SR/usr -fuse-ld=lld"

mkdir -p "$dist"
cp -f "$build/src/plugins/libmm-plugin-mtk-ccci.so" "$dist/"
cp -f "$helper" "$dist/"
cp -f "$here/plugin/77-mm-mtk-ccci.rules" "$dist/"
cp -f "$here/60-titan2-wwan.conf" "$dist/"
# These files are copied from the build tree rather than through meson install.
# The installed plugin must resolve Alpine's libmm-glib, with no build RUNPATH.
patchelf --remove-rpath "$dist/libmm-plugin-mtk-ccci.so"
patchelf --remove-rpath "$dist/t2-mm-speech-trigger"
"$TC/llvm-strip" --strip-unneeded "$dist/libmm-plugin-mtk-ccci.so" "$dist/t2-mm-speech-trigger"
(cd "$dist" && sha256sum libmm-plugin-mtk-ccci.so t2-mm-speech-trigger 77-mm-mtk-ccci.rules 60-titan2-wwan.conf > SHA256SUMS)
echo "aarch64 build in $dist:"
cat "$dist/SHA256SUMS"
file "$dist/libmm-plugin-mtk-ccci.so" "$dist/t2-mm-speech-trigger" 2>/dev/null || true
