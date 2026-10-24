#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# boot.img for boot_a with the titan2-boot menu: Android and mainline Linux (several DT variants).
# Flashes nothing. vendor_boot_a, init_boot_a, dtbo_a stay Android's: LK builds the stock FDT,
# titan2-boot hands it to the Android kernel unchanged and, for mainline, carries LK's data over
# into the mainline DTB (titan2-boot: src/fdtfix.c). The menu entries are an extlinux.conf in the
# pack (titan2-boot: src/conf.c). The Android kernel is always in the pack: recovery (TWRP),
# charging and a normal Android boot do not depend on titan2-boot reading the UFS.
#   scripts/make-bootmenu.sh
#   ANDROID_BOOT: the Android boot.img the kernel is taken from (required; the boot.img of the
#                  Android that is installed, i.e. what boot_a holds before the menu). New Android
#                  kernel = rebuild the pack with ANDROID_BOOT=<boot.img> and flash boot_a
#   UFS=1        "Linux partition" item ("UFS on"): only when it is chosen (or is the default,
#                  DEFAULT=ufs) does titan2-boot bring up the UFS and show the entries of
#                  /boot/extlinux/extlinux.conf on the linux partition. Without UFS=1 the UFS is untouched
#   LINUX_ROOT=part:linux: root of the pack's Linux entries (t2.root=part:linux, tools/initramfs);
#                  NETROOT=1: root over the network (NBD); with neither: the initramfs only
#   VARIANTS="full drm wifi": mainline DT variants, one menu entry each (default: all built
#                  mt6878-unihertz-titan2[-*].dtb; full = without a suffix)
#   KO, NETROOT, CMDLINE as for scripts/make-mainline-images.sh; INITRD: your own initramfs
#   KEEP_INITRD=1: do not rebuild an existing $OUT/initramfs.cpio.gz
#   DEFAULT=android|linux|linux-<variant>|ufs, TIMEOUT=5 (seconds; 0: the default entry at once)
#   NO_LINUX=1   Android only
#   OUT=out/bootmenu
# Output: $OUT/boot.img (for boot_a), $OUT/pack.bin (unpacked image), $OUT/extlinux.conf.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
O=$ROOT/${OUT:-out/bootmenu}
KO=$ROOT/${KO:-out/mainline-build}
B=$KO/arch/arm64/boot
[ -n "${ANDROID_BOOT:-}" ] || { echo "ANDROID_BOOT=<boot.img of your Android> is required" >&2; exit 1; }
# titan2-boot (github.com/Serlk/titan2-boot) cloned into the repo root, or TITAN2_BOOT=<dir in the repo>
TB=$ROOT/${TITAN2_BOOT:-titan2-boot}
[ -f "$TB/tools/mkpack.py" ] || { echo "no titan2-boot in $TB (git clone it there, or set TITAN2_BOOT)" >&2; exit 1; }
ANDROID_BOOT=$(realpath -m "$ANDROID_BOOT")
case "$ANDROID_BOOT" in "$ROOT"/*) ;; *) echo "ANDROID_BOOT must be inside $ROOT" >&2; exit 1 ;; esac
TC=$ROOT/toolchains/clang-r574158
DEFAULT=${DEFAULT:-android}
TIMEOUT=${TIMEOUT:-5}
case "$O" in "$ROOT"/*) ;; *) echo "OUT must be inside $ROOT" >&2; exit 1 ;; esac
mkdir -p "$O/files/android" "$O/files/linux"
rm -f "$O"/files/android/* "$O"/files/linux/*
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" titan2-kbuild "$@"; }
w() { echo "/work${1#$ROOT}"; }

make -s -C "$TB" TOOLCHAIN="$TC"
C=$O/extlinux.conf
cat >"$C" <<EOF
# titan2-boot menu, made by scripts/make-bootmenu.sh
MENU TITLE Titan 2
TIMEOUT $((TIMEOUT * 10))
DEFAULT $DEFAULT
EOF
[ -z "${UFS:-}" ] || echo "UFS on" >>"$C"
args=(--loader "$(w "$TB/out/titan2-boot.bin")" --conf "$(w "$C")"
	--out "$(w "$O/boot.img")" --raw "$(w "$O/pack.bin")")

# Android: the kernel from the boot.img as it is (lz4/gzip, titan2-boot unpacks it)
[ -z "${ANDROID_PART:-}" ] || { echo "ANDROID_PART is gone: the Android kernel is always in the pack" >&2; exit 1; }
[ -f "$ANDROID_BOOT" ] || { echo "no $ANDROID_BOOT" >&2; exit 1; }
[ "$(head -c 8 "$ANDROID_BOOT")" = "ANDROID!" ] || { echo "$ANDROID_BOOT: not a boot.img" >&2; exit 1; }
ksize=$(od -An -t u4 -j 8 -N 4 "$ANDROID_BOOT" | tr -d ' ')
dd if="$ANDROID_BOOT" of="$O/files/android/Image" iflag=skip_bytes,count_bytes skip=4096 \
	count="$ksize" status=none
printf 'LABEL android\n  MENU LABEL Android\n  ANDROID /android/Image\n' >>"$C"
args+=(--file "android/Image=$(w "$O/files/android/Image")" --android-boot "$(w "$ANDROID_BOOT")")

if [ -z "${NO_LINUX:-}" ]; then
	[ -f "$B/Image.gz" ] || { echo "no $B/Image.gz (scripts/build-mainline.sh)" >&2; exit 1; }
	NR_SPEC=${NETROOT:-}
	[ "$NR_SPEC" != 1 ] || NR_SPEC=nbd:172.16.42.2:10809:t2root
	if [ -n "${LINUX_ROOT:-}" ]; then
		[ -z "$NR_SPEC" ] || { echo "LINUX_ROOT and NETROOT cannot be combined" >&2; exit 1; }
		case "$LINUX_ROOT" in part:?*) NR_SPEC=$LINUX_ROOT ;; *) echo "LINUX_ROOT=part:<partition>" >&2; exit 1 ;; esac
	fi
	CMDLINE=${CMDLINE:-console=tty0 loglevel=7 ignore_loglevel}${NR_SPEC:+ t2.root=$NR_SPEC}
	INITRD=${INITRD:-$O/initramfs.cpio.gz}
	if [ "$INITRD" = "$O/initramfs.cpio.gz" ] && { [ ! -f "$INITRD" ] || [ -z "${KEEP_INITRD:-}" ]; }; then
		"$ROOT/tools/initramfs/build.sh" "$INITRD"
	fi
	REL=$(cat "$KO/include/config/kernel.release" 2>/dev/null || echo mainline)
	cp "$B/Image.gz" "$O/files/linux/Image.gz"
	cp "$INITRD" "$O/files/linux/initramfs"
	args+=(--file "linux/Image.gz=$(w "$O/files/linux/Image.gz")"
		--file "linux/initramfs=$(w "$O/files/linux/initramfs")")
	if [ -z "${VARIANTS:-}" ]; then
		# full first, the others in alphabetical order
		VARIANTS=$(cd "$B/dts/mediatek" && ls mt6878-unihertz-titan2*.dtb |
			sed 's/^mt6878-unihertz-titan2\.dtb$/full/; s/^mt6878-unihertz-titan2-\(.*\)\.dtb$/\1/' |
			sort | awk '$0 == "full" { f = 1; next } { r = r " " $0 } END { if (f) printf "full"; print r }')
	fi
	# with UFS=1 the same versions are on the linux partition too: the pack entries are marked as fallbacks
	SFX=${UFS:+ (boot_a)}
	for v in $VARIANTS; do
		if [ "$v" = full ]; then dtb=mt6878-unihertz-titan2.dtb label=linux title="Linux $REL$SFX"
		else dtb=mt6878-unihertz-titan2-$v.dtb label=linux-$v title="Linux $REL -$v$SFX"
		fi
		[ -f "$B/dts/mediatek/$dtb" ] || { echo "no $dtb" >&2; exit 1; }
		cp "$B/dts/mediatek/$dtb" "$O/files/linux/$dtb"
		args+=(--file "linux/$dtb=$(w "$O/files/linux/$dtb")")
		cat >>"$C" <<EOF
LABEL $label
  MENU LABEL $title
  LINUX /linux/Image.gz
  FDT /linux/$dtb
  INITRD /linux/initramfs
  APPEND $CMDLINE
EOF
	done
fi

run python3 -I "$(w "$TB/tools/mkpack.py")" "${args[@]}"
echo "Flash: fastboot flash boot_a $O/boot.img (vendor_boot_a, init_boot_a, dtbo_a stay Android's)"
echo "Back to Android without the menu: PARTS=boot_a scripts/restore-stock-boot.sh <your backup>"
