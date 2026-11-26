#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Titan 2 "USB console" initramfs: USB gadget NCM+ACM, dropbear, getty on ttyGS0.
#   tools/initramfs/build.sh [out.cpio.gz]     (default out/initramfs-usb/initramfs.cpio.gz)
# Alpine (aarch64) packages are downloaded to out/initramfs-dl and verified (APKINDEX signature,
# control sha1, data sha256). OFFLINE=1: no network, only verify what is already downloaded.
# Python runs in the titan2-kbuild container (python on the host is broken).
# CLANG_DIR=<dir with bin/clang>: the clang for t2-reboot, as for scripts/build-mainline.sh
# (default toolchains/clang-r574158); it runs in the container too.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REL=${ALPINE_RELEASE:-v3.24}
OUT=${1:-$ROOT/out/initramfs-usb/initramfs.cpio.gz}
OUT=$(realpath -m "$OUT")
case "$OUT" in "$ROOT"/*) ;; *) echo "out must be inside $ROOT" >&2; exit 1 ;; esac
# i2c-tools and evtest come from community (hardware checks: i2cdetect/i2cget/i2cdump, evtest);
# e2fsck with its libraries checks the root on the linux partition (t2.root=part:linux)
PKGS="busybox-static busybox-extras dropbear dropbear-dbclient dropbear-scp musl zlib utmps-libs skalibs-libs i2c-tools evtest
e2fsprogs e2fsprogs-libs libcom_err libblkid libuuid libeconf"
mkdir -p "$ROOT/out/initramfs-dl" "$(dirname "$OUT")"
# SSH keys for root, as scripts/netroot-serve.sh takes them: T2_AUTHORIZED_KEYS or AUTHORIZED_KEYS,
# else tools/initramfs/authorized_keys, else ~/.ssh/id_ed25519.pub of the user who builds. The file
# is copied next to the downloads (the build container only sees this repository); without any
# key root has an EMPTY password (USB network only).
KEYS=
for k in "${T2_AUTHORIZED_KEYS:-}" "${AUTHORIZED_KEYS:-}" "$ROOT/tools/initramfs/authorized_keys" "$HOME/.ssh/id_ed25519.pub"; do
	if [ -n "$k" ] && [ -s "$k" ]; then KEYS=$k; break; fi
done
T2_AUTHORIZED_KEYS=
if [ -n "$KEYS" ]; then
	install -m 600 "$KEYS" "$ROOT/out/initramfs-dl/authorized_keys"
	T2_AUTHORIZED_KEYS=/work/out/initramfs-dl/authorized_keys
	echo "initramfs authorized_keys: $KEYS" >&2
else
	rm -f "$ROOT/out/initramfs-dl/authorized_keys"
	T2_AUTHORIZED_KEYS=/work/out/initramfs-dl/none
fi
export T2_AUTHORIZED_KEYS
CLANG_DIR=${CLANG_DIR:-$ROOT/toolchains/clang-r574158}
CLANG_DIR=$(realpath -m "$CLANG_DIR")
# mounted only when it exists (docker would create a missing directory as root)
CLANG_MOUNT=()
[ ! -d "$CLANG_DIR" ] || CLANG_MOUNT=(-v "$CLANG_DIR:/clang:ro")
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" "${CLANG_MOUNT[@]}" -e T2_AUTHORIZED_KEYS -e SOURCE_DATE_EPOCH titan2-kbuild "$@"; }
run python3 -I /work/tools/initramfs/apk.py "$([ -n "${OFFLINE:-}" ] && echo list || echo fetch)" \
	/work/out/initramfs-dl "$REL" $PKGS
# t2-reboot: no libc (raw syscalls), clang from CLANG_DIR (as for the kernel); rebuilt when the
# source is newer
T2R=$ROOT/tools/t2-reboot
if [ ! -x "$T2R/t2-reboot" ] || [ "$T2R/t2-reboot.c" -nt "$T2R/t2-reboot" ]; then
	[ -x "$CLANG_DIR/bin/clang" ] || { echo "CLANG_DIR: no $CLANG_DIR/bin/clang (clang 21, see scripts/build-mainline.sh)" >&2; exit 1; }
	run /clang/bin/clang --target=aarch64-linux-gnu -Os -ffreestanding \
		-fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib -static -fuse-ld=lld \
		-Wl,--build-id=none -s -Wall -Werror -o /work/tools/t2-reboot/t2-reboot /work/tools/t2-reboot/t2-reboot.c
fi
run python3 -I /work/tools/initramfs/build.py /work/out/initramfs-dl "$REL" "/work${OUT#$ROOT}"
ls -l "$OUT"
