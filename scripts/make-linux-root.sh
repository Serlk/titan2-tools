#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Image of the linux partition (dual boot, titan2-docs: docs/DUALBOOT.md): the same Alpine as the
# network root (scripts/netroot-serve.sh build), plus /boot for the "Linux partition" item of
# the titan2-boot menu:
#   /boot/Image.gz, /boot/initramfs, /boot/dtbs/*.dtb,
#   /boot/extlinux/extlinux.conf: one entry per DT variant, root t2.root=part:linux.
# Flashes nothing. The image is smaller than the partition: at the first boot t2-growroot
# (installed by the initramfs) grows the ext4 to the whole partition.
#   scripts/make-linux-root.sh
# Variables:
#   KO=out/mainline-build   kernel build (scripts/build-mainline.sh): Image.gz, dtb, modules
#   SIZE=8G                 ext4 size (before it is grown on the phone)
#   VARIANTS="full drm"     DT variants (default: all built mt6878-unihertz-titan2[-*].dtb)
#   CMDLINE                 as for scripts/make-bootmenu.sh (t2.root=part:linux is added)
#   REUSE=1                 do not rebuild Alpine, take the existing out/netroot/linux-root.img
#   GFX (default 1), EXTRA_PKGS, LOCAL_REPO (default: a Mesa build in out/mesa if there is one),
#   AUTHORIZED_KEYS: passed on to netroot-serve.sh build
#   OUT=out/linux-root
# Output: $OUT/linux.img (sparse, for fastboot flash linux), $OUT/linux.raw (ext4 as it is).
# Host tools: docker, e2fsck, img2simg (android-tools).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
O=$ROOT/${OUT:-out/linux-root}
KO=$ROOT/${KO:-out/mainline-build}
B=$KO/arch/arm64/boot
NR=$ROOT/out/netroot
BASE=$NR/linux-root.img
SIZE=${SIZE:-8G}
CMDLINE=${CMDLINE:-console=tty0 loglevel=7 ignore_loglevel}
case "$O" in "$ROOT"/*) ;; *) echo "OUT must be inside $ROOT" >&2; exit 1 ;; esac
[ -f "$B/Image.gz" ] || { echo "no $B/Image.gz (scripts/build-mainline.sh)" >&2; exit 1; }
REL=$(cat "$KO/include/config/kernel.release")
mkdir -p "$O/boot/dtbs" "$O/boot/extlinux"
rm -f "$O"/boot/dtbs/* "$O/linux.raw" "$O/linux.img"

# 1. Alpine: a separate image, the network root (out/netroot/rootfs.img) is not touched
if [ -z "${REUSE:-}" ] || [ ! -f "$BASE" ]; then
	# like the network root: graphics, and a local Mesa build if there is one
	[ -n "${LOCAL_REPO+x}" ] || [ ! -f "$ROOT/out/mesa/packages/aports/aarch64/APKINDEX.tar.gz" ] ||
		export LOCAL_REPO=out/mesa/packages/aports
	GFX=${GFX-1} FORCE=1 IMG=$BASE SIZE=$SIZE MODULES=${KO#"$ROOT"/} "$ROOT/scripts/netroot-serve.sh" build
fi

# 2. /boot
"$ROOT/tools/initramfs/build.sh" "$O/boot/initramfs"
cp "$B/Image.gz" "$O/boot/Image.gz"
if [ -z "${VARIANTS:-}" ]; then
	VARIANTS=$(cd "$B/dts/mediatek" && ls mt6878-unihertz-titan2*.dtb |
		sed 's/^mt6878-unihertz-titan2\.dtb$/full/; s/^mt6878-unihertz-titan2-\(.*\)\.dtb$/\1/' |
		sort | awk '$0 == "full" { f = 1; next } { r = r " " $0 } END { if (f) printf "full"; print r }')
fi
C=$O/boot/extlinux/extlinux.conf
cat >"$C" <<EOF
# linux partition: entries of the titan2-boot menu ("Linux partition"), make-linux-root.sh
# New kernel: put Image.gz, initramfs, dtbs/ here (from Linux itself); boot_a does not change.
DEFAULT linux
EOF
for v in $VARIANTS; do
	if [ "$v" = full ]; then dtb=mt6878-unihertz-titan2.dtb label=linux title="Linux $REL"
	else dtb=mt6878-unihertz-titan2-$v.dtb label=linux-$v title="Linux $REL -$v"
	fi
	[ -f "$B/dts/mediatek/$dtb" ] || { echo "no $dtb" >&2; exit 1; }
	cp "$B/dts/mediatek/$dtb" "$O/boot/dtbs/"
	cat >>"$C" <<EOF
LABEL $label
  MENU LABEL $title
  LINUX /boot/Image.gz
  FDT /boot/dtbs/$dtb
  INITRD /boot/initramfs
  APPEND $CMDLINE t2.root=part:linux
EOF
done

# 3. copy of the base image + /boot (loop in a throwaway container, like netroot-serve.sh); e2fsck on the host
cp --reflink=auto --sparse=always "$BASE" "$O/linux.raw"
REL_A=${ALPINE_RELEASE:-v3.24}
docker run --rm --privileged -v "$O:/o" -e UIDGID="$(id -u):$(id -g)" "alpine:${REL_A#v}" sh -euc '
	mkdir -p /mnt/r
	mount -o loop /o/linux.raw /mnt/r
	trap "umount /mnt/r" EXIT
	rm -rf /mnt/r/boot/extlinux /mnt/r/boot/dtbs
	mkdir -p /mnt/r/boot
	cp -r /o/boot/. /mnt/r/boot/
	chown -R 0:0 /mnt/r/boot
	umount /mnt/r
	trap - EXIT
	chown "$UIDGID" /o/linux.raw
'
e2fsck -f -p "$O/linux.raw"
img2simg "$O/linux.raw" "$O/linux.img"
ls -l "$O/linux.img"
echo "Flash (after the repartitioning, scripts/dualboot-repartition.sh): fastboot flash linux $O/linux.img"
