#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Titan 2 "USB console" initramfs: USB gadget NCM+ACM, dropbear, getty on ttyGS0.
#   tools/initramfs/build.sh [out.cpio.gz]     (default out/initramfs-usb/initramfs.cpio.gz)
# Alpine (aarch64) packages are downloaded to out/initramfs-dl and verified (APKINDEX signature,
# control sha1, data sha256). OFFLINE=1: no network, only verify what is already downloaded.
# Python runs in the titan2-kbuild container (python on the host is broken).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REL=${ALPINE_RELEASE:-v3.24}
OUT=${1:-$ROOT/out/initramfs-usb/initramfs.cpio.gz}
OUT=$(realpath -m "$OUT")
case "$OUT" in "$ROOT"/*) ;; *) echo "out must be inside $ROOT" >&2; exit 1 ;; esac
# i2c-tools and evtest come from community (hardware checks: i2cdetect/i2cget/i2cdump, evtest)
PKGS="busybox-static busybox-extras dropbear dropbear-dbclient dropbear-scp musl zlib utmps-libs skalibs-libs i2c-tools evtest"
mkdir -p "$ROOT/out/initramfs-dl" "$(dirname "$OUT")"
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" -e T2_AUTHORIZED_KEYS -e SOURCE_DATE_EPOCH titan2-kbuild "$@"; }
run python3 -I /work/tools/initramfs/apk.py "$([ -n "${OFFLINE:-}" ] && echo list || echo fetch)" \
	/work/out/initramfs-dl "$REL" $PKGS
run python3 -I /work/tools/initramfs/build.py /work/out/initramfs-dl "$REL" "/work${OUT#$ROOT}"
ls -l "$OUT"
