#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Images for booting the mainline kernel with the Titan 2 factory LK (titan2-docs: docs/lk-boot-requirements.md).
# Flashes nothing. Output: <dir>/{boot,vendor_boot,init_boot,dtbo}.img, titan2.dtb
#   scripts/make-mainline-images.sh [initramfs.cpio.gz]   (default: tools/mini-init)
#   VARIANT=minimal|full|pd: which DTB goes into vendor_boot:
#     minimal: no SPMI/PMIC and no input devices except the volume gpio-keys (simplefb, UART,
#               UFS, USB): the very first boot; directory ...-minimal
#     full:    (default) mt6878-unihertz-titan2.dtb as is: SPMI/PMIC, PMIC keys,
#               keyboard, touchscreen, touchpad, Func1/Func2; directory without suffix
#     pd:      full + power domains (mt6878-unihertz-titan2-pd.dtb); directory ...-pd
#   MINIMAL=1 / PD=1: old names for VARIANT=minimal / VARIANT=pd
#     <name>:  any other board variant of the tree (dvfs, idle, gpu, wifi, drm, ...):
#               mt6878-unihertz-titan2-<name>.dtb; directory ...-<name>
#   USB=1:    "USB console" initramfs (tools/initramfs: NCM+ACM gadget, dropbear, getty on
#               ttyGS0; titan2-docs: docs/hardware/usb-gadget.md); directory ...-usb[-minimal|-pd]
#   NETROOT=1: the same initramfs (USB=1) + network root: t2.root=nbd:172.16.42.2:10809:t2root on
#               the cmdline (root from the PC over NBD, scripts/netroot-serve.sh); another value is
#               the spec itself (NETROOT=nfs:172.16.42.2:/srv/t2); directory ...-usb-netroot[-...]
#   KO=out/mainline-build: kernel build directory (Image.gz and DTB)
#   OUT=out/mainline-images: output directory prefix (e.g. OUT=out/mainline-stage2/images)
#   CMDLINE:  boot.img cmdline (default 'console=tty0 loglevel=7 ignore_loglevel')
#
# LK requirements met here:
#   - boot/vendor_boot/init_boot v4; addresses kernel 0x40000000, ramdisk 0x66f00000,
#     DTB at tags_addr 0x47c80000; compressed kernel (Image.gz)
#   - init_boot with ANDROID! magic and an empty ramdisk (otherwise its /init would overlay ours)
#   - vendor cmdline with bootopt=64S3,32N2,64N2, as in the factory images
#   - vendor ramdisks of types PLATFORM and RECOVERY, as in the factory images: without RECOVERY, booting
#     to recovery (volume up) stops in LK on an assertion
#   - an all-zero dtbo: LK logs that it cannot load the dtbo and boots without an overlay
#   - this slot's vbmeta must be flashed with --disable-verification (separately, not here)
#
# LK quirks worked around here (titan2-docs: docs/lk-boot-requirements.md §3):
#   - LK reads the boot.img cmdline at header offset 64 (the v0-v2 field), but in v3/v4 the cmdline
#     starts at 44: the first 20 bytes are lost ("console=tty0 loglevel=7" became "l=7"). So the
#     cmdline is prefixed with 20 spaces: LK drops them, and a bootloader that reads the field correctly
#     passes extra spaces to the kernel, which are harmless.
#   - LK places the bootconfig at ramdisk + (whole vendor ramdisk + init_boot), but copies
#     only the PLATFORM fragment: between the init_boot ramdisk and the bootconfig there is a hole the
#     size of the RECOVERY fragment with stale memory contents. The kernel reports «Initramfs unpacking
#     failed: invalid magic» on it: this comes after both our archives, unpacking is already
#     finished, and the kernel finds the bootconfig from the end of the initrd. Harmless; the RECOVERY
#     fragment is kept minimal (empty cpio.gz, 50 bytes).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/${KO:-out/mainline-build}/arch/arm64/boot
VARIANT=${VARIANT:-}
if [ -z "$VARIANT" ]; then
	if [ -n "${MINIMAL:-}" ]; then VARIANT=minimal
	elif [ -n "${PD:-}" ]; then VARIANT=pd
	else VARIANT=full
	fi
fi
case "$VARIANT" in
minimal) SUF=-minimal DTS=mt6878-unihertz-titan2 ;;
full) SUF= DTS=mt6878-unihertz-titan2 ;;
pd) SUF=-pd DTS=mt6878-unihertz-titan2-pd ;;
# the other board variants of the tree: dvfs, idle, gpu, wifi, drm, ...
*) [ -f "$ROOT/${KO:-out/mainline-build}/arch/arm64/boot/dts/mediatek/mt6878-unihertz-titan2-$VARIANT.dtb" ] ||
	{ echo "VARIANT: minimal, full, pd or the name of a board variant (no mt6878-unihertz-titan2-$VARIANT.dtb)" >&2; exit 1; }
   SUF=-$VARIANT DTS=mt6878-unihertz-titan2-$VARIANT ;;
esac
NR_SPEC=
if [ -n "${NETROOT:-}" ]; then
	USB=1
	NR_SPEC=$NETROOT
	[ "$NR_SPEC" != 1 ] || NR_SPEC=nbd:172.16.42.2:10809:t2root
	case "$NR_SPEC" in nbd:?*|nfs:?*:/*) ;; *) echo "NETROOT: nbd:<ip>[:port[:export]] or nfs:<ip>:/path" >&2; exit 1 ;; esac
fi
O=$ROOT/${OUT:-out/mainline-images}${USB:+-usb}${NR_SPEC:+-netroot}$SUF
INITRD=${1:-}
CMDLINE=${CMDLINE:-console=tty0 loglevel=7 ignore_loglevel}${NR_SPEC:+ t2.root=$NR_SPEC}
mkdir -p "$O"
cp "$B/dts/mediatek/$DTS.dtb" "$O/titan2.dtb"
DTB=$O/titan2.dtb
if [ "$VARIANT" = minimal ]; then
	fdtput -t s "$DTB" /soc@0/spmi@1cc04000 status disabled
	# without SPMI the VUSB regulator never appears and mtu3 would wait for it forever; the LDO itself is on
	# (always-on in the factory device tree; the preloader enumerates USB on every boot)
	U=/soc@0/usb0@11201000
	if fdtget "$DTB" $U vusb33-supply >/dev/null 2>&1; then
		fdtput -d "$DTB" $U vusb33-supply
	fi
	# input devices: keyboard and touchscreen are powered from the MT6369 (SPMI); touchpad and Func2
	# are not needed for the first boot either
	for n in /soc@0/i2c@11e01000/keyboard@58 /soc@0/i2c@11c20000/touchscreen@67 \
		/soc@0/i2c@11c22000/touchpad@20 /regulator-touchpad-vdd /gpio-keys/key-func2; do
		if fdtget -p "$DTB" $n >/dev/null 2>&1; then
			fdtput -t s "$DTB" $n status disabled
		else
			echo "minimal: no node $n" >&2
			exit 1
		fi
	done
fi
if [ -n "${USB:-}" ] && [ -z "$INITRD" ]; then
	"$ROOT/tools/initramfs/build.sh" "$O/initramfs.cpio.gz"
	INITRD=$O/initramfs.cpio.gz
fi

if [ -z "$INITRD" ]; then
	# initramfs from tools/mini-init: /init + /dev
	R=$O/rootfs
	rm -rf "$R"
	mkdir -p "$R/dev"
	install -m 0755 "$ROOT/tools/mini-init/init" "$R/init"
	(cd "$R" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet | gzip -9n) >"$O/initramfs.cpio.gz"
	INITRD=$O/initramfs.cpio.gz
fi
: | cpio -o -H newc --quiet | gzip -9n >"$O/empty.cpio.gz"

# the 20 bytes LK skips (see above)
PAD='                    '
R=/work${O#$ROOT}
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" -e "T2_CMDLINE=$PAD$CMDLINE" titan2-kbuild bash -euo pipefail -c "
mk='python3 /work/tools/mkbootimg/mkbootimg.py'
addr='--pagesize 4096 --base 0 --kernel_offset 0x40000000 --ramdisk_offset 0x66f00000 --tags_offset 0x47c80000 --dtb_offset 0x47c80000'
\$mk --header_version 4 --kernel /work${B#$ROOT}/Image.gz \
	--cmdline \"\$T2_CMDLINE\" --os_version 14.0.0 --os_patch_level 2024-04 \
	--output $R/boot.img
\$mk --header_version 4 \$addr --vendor_boot $R/vendor_boot.img \
	--ramdisk_type platform --ramdisk_name '' --vendor_ramdisk_fragment /work${INITRD#$ROOT} \
	--ramdisk_type recovery --ramdisk_name recovery --vendor_ramdisk_fragment $R/empty.cpio.gz \
	--dtb /work${DTB#$ROOT} \
	--vendor_cmdline 'bootopt=64S3,32N2,64N2'
\$mk --header_version 4 --ramdisk $R/empty.cpio.gz --os_version 14.0.0 --os_patch_level 2024-04 \
	--output $R/init_boot.img
python3 /work/tools/mkbootimg/unpack_bootimg.py --boot_img $R/vendor_boot.img --out /tmp/vb >/dev/null
"
# what LK will see: the cmdline from header byte 64
got=$(dd if="$O/boot.img" bs=1 skip=64 count=512 status=none | tr -d '\0')
if [ "$got" != "$CMDLINE" ]; then
	echo "boot.img: LK will read cmdline «$got», expected «$CMDLINE»" >&2
	exit 1
fi
head -c 4096 /dev/zero >"$O/dtbo.img"
# LK: vendor ramdisk + init_boot ramdisk + bootconfig (+0x400) <= 64 MiB (mb_ramdisk)
rd=$(( $(stat -c %s "$INITRD") + $(stat -c %s "$O/empty.cpio.gz") * 2 + 1024 + 0x400 ))
if [ $rd -gt $((64 << 20)) ]; then
	echo "ramdisk ~$rd bytes > 64 MiB: LK will not boot it" >&2
	exit 1
fi
echo "$VARIANT: $O (DTB $DTS)"
ls -l "$O"/*.img
