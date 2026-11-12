#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Build the mainline kernel for Titan 2 (titan2-linux, titan2 branch):
# arm64 defconfig + configs/mainline-titan2.config [+ FRAGMENTS] -> out/mainline-build/arch/arm64/boot/Image.gz
# and the DTBs.
# Flashes nothing. nice/ionice, JOBS=4 by default.
# SRC/O (relative to the repo root): the kernel tree (clone of titan2-linux, titan2 branch) and the build
# directory. Requires the titan2-kbuild docker image (docker/Dockerfile) and a clang 21 toolchain:
# CLANG_DIR=<dir with bin/clang> (default toolchains/clang-r574158). Either the official LLVM 21
# release from GitHub (llvm/llvm-project, LLVM-21.1.8-Linux-X64.tar.xz) or AOSP clang r574158;
# clang 17 does not build kernel 7.2 (asm goto). See README.
#   SRC=linux O=out/mainline-build [MODULES=0] scripts/build-mainline.sh   (MODULES=0: no modules)
# FRAGMENTS="file ..." (or CONFIG_FRAGMENTS): more config fragments, merged after
# configs/mainline-titan2.config in the order given (scripts/kconfig/merge_config.sh: a later
# value of a symbol wins). A relative path is relative to the current directory; the files may be
# outside this repository (each one is mounted read-only into the build container). The kernel
# tree must carry the matching patch series, e.g. the camera (git am of
# titan2-camera/kernel/patches/*.patch on the titan2 branch):
#   SRC=linux FRAGMENTS=../titan2-camera/kernel/titan2-camera.config scripts/build-mainline.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-linux}
O=${O:-out/mainline-build}
FRAGMENTS=${FRAGMENTS:-${CONFIG_FRAGMENTS:-}}
CLANG_DIR=${CLANG_DIR:-$ROOT/toolchains/clang-r574158}
[ -x "$CLANG_DIR/bin/clang" ] || { echo "CLANG_DIR: no $CLANG_DIR/bin/clang (clang 21: see README)" >&2; exit 1; }
CLANG_DIR=$(realpath "$CLANG_DIR")
echo "clang: $("$CLANG_DIR/bin/clang" --version | head -1)"
FRAG_MOUNTS=() FRAG_FILES=
i=0
for f in $FRAGMENTS; do
	[ -f "$f" ] || { echo "FRAGMENTS: no file $f" >&2; exit 1; }
	i=$((i + 1))
	FRAG_MOUNTS+=(-v "$(realpath "$f"):/fragments/$i-$(basename "$f"):ro")
	FRAG_FILES="$FRAG_FILES /fragments/$i-$(basename "$f")"
	echo "config fragment $i: $f"
done
nice -n 19 ionice -c 3 docker run --rm --name "titan2-mainline-$$" --cpu-shares 128 \
	-u "$(id -u):$(id -g)" -v "$ROOT:/work" -v "$CLANG_DIR:/clang:ro" "${FRAG_MOUNTS[@]}" titan2-kbuild bash -euo pipefail -c "
# kernel 7.2 does not build with clang 17 (asm goto): clang 21 from CLANG_DIR
export PATH=/clang/bin:\$PATH
cd /work/$SRC
M='make O=/work/$O ARCH=arm64 LLVM=1 -j${JOBS:-4}'
\$M defconfig
scripts/kconfig/merge_config.sh -m -O /work/$O /work/$O/.config /work/configs/mainline-titan2.config$FRAG_FILES
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
