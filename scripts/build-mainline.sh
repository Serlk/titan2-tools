#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Build the mainline kernel for Titan 2 (titan2-linux, titan2 branch):
# arm64 defconfig + configs/mainline-titan2.config -> out/mainline-build/arch/arm64/boot/Image.gz and the DTB.
# Flashes nothing. nice/ionice, JOBS=4 by default.
# SRC/O (relative to the repo root): the kernel tree (clone of titan2-linux, titan2 branch) and the build
# directory. Requires the titan2-kbuild docker image (docker/Dockerfile) and AOSP clang r574158 in
# toolchains/clang-r574158 (see README).
#   SRC=linux O=out/mainline-build [MODULES=0] scripts/build-mainline.sh   (MODULES=0: no modules)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-linux}
O=${O:-out/mainline-build}
nice -n 19 ionice -c 3 docker run --rm --name "titan2-mainline-$$" --cpu-shares 128 \
	-u "$(id -u):$(id -g)" -v "$ROOT:/work" titan2-kbuild bash -euo pipefail -c "
# kernel 7.2 does not build with clang 17 (asm goto): AOSP clang 21 r574158
export PATH=/work/toolchains/clang-r574158/bin:\$PATH
cd /work/$SRC
M='make O=/work/$O ARCH=arm64 LLVM=1 -j${JOBS:-4}'
\$M defconfig
scripts/kconfig/merge_config.sh -m -O /work/$O /work/$O/.config /work/configs/mainline-titan2.config
\$M olddefconfig
\$M Image.gz mediatek/mt6878-unihertz-titan2.dtb
# every board variant of the tree (pd, dvfs, gpu, drm, wifi, ...)
for d in arch/arm64/boot/dts/mediatek/mt6878-unihertz-titan2-*.dts; do
	[ -f \"\$d\" ] && \$M mediatek/\$(basename \"\$d\" .dts).dtb
done
# modules (MODULES=0: skip) into O/modules (for the Alpine root: scripts/netroot-serve.sh MODULES=)
if [ '${MODULES:-1}' = 1 ]; then
	\$M modules
	rm -rf /work/$O/modules
	\$M INSTALL_MOD_PATH=/work/$O/modules INSTALL_MOD_STRIP=1 modules_install
fi
"
ls -l "$ROOT/$O"/arch/arm64/boot/Image.gz "$ROOT/$O"/arch/arm64/boot/dts/mediatek/mt6878-unihertz-titan2*.dtb
if [ "${MODULES:-1}" = 1 ]; then
	ls -d "$ROOT/$O"/modules/lib/modules/*
fi
