#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Update Linux on the phone's linux partition over SSH (the phone is booted into that Linux, USB
# network): the kernel, all DTBs, the initramfs and the modules of a scripts/build-mainline.sh
# build, and /boot/extlinux/extlinux.conf, the entries of the titan2-boot "Linux partition" menu.
# The previous kernel, DTBs and initramfs stay in /boot/prev (entry "previous"); the modules of
# the previous version are not removed. Flashes nothing, boot_a and the Android partitions are
# not touched.
#   scripts/linux-update.sh
# Variables:
#   KO=out/mainline-build      kernel build
#   HOST=root@172.16.42.1      the phone
#   VARIANTS="all all-fb full drm ..."  order of the entries (default: all, all-fb, full, the rest)
#   DEFAULT=all                default entry on the partition
#   CMDLINE                    as for make-bootmenu.sh (t2.root=part:linux is added), plus the
#                              content of /boot/extlinux/cmdline.extra on the phone if it exists
#                              (parameters that must survive an update, e.g. mtk_ccci.*)
#   FIRMWARE=1                 install firmware files of YOUR phone (taken from your own backups,
#                              never distributed with this repository):
#                              FW_DIR=<dir>      the vendor firmware directory of your Android
#                                                (holds frsm-spk1.bin, the speaker amplifier)
#                              NVDATA_IMG=<img>  your nvdata partition backup: mediatek/mt6878/WIFI
#                                                (MAC/RF) and BT_Addr (Bluetooth address) are read
#                                                from it with debugfs
#   REBOOT=1                   reboot the phone after the update
#   SSH_OPTS="-S /tmp/sock"    more ssh options (e.g. a ControlMaster socket)
# Foreign entries in /boot/extlinux/extlinux.conf (LINUX neither /boot/Image.gz nor /boot/prev/...,
# e.g. test kernels in /boot/<name>/) are carried over into the new file as they are.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=$ROOT/${KO:-out/mainline-build}
B=$KO/arch/arm64/boot
HOST=${HOST:-root@172.16.42.1}
DEFAULT=${DEFAULT:-all}
CMDLINE=${CMDLINE:-console=tty0 loglevel=7 ignore_loglevel log_buf_len=16M}
REL=$(cat "$KO/include/config/kernel.release")
O=$ROOT/out/linux-update
SSH=(ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 ${SSH_OPTS:-} "$HOST")
[ -f "$B/Image.gz" ] && [ -d "$KO/modules/lib/modules/$REL" ] || { echo "no build of $REL in $KO" >&2; exit 1; }
"${SSH[@]}" 'grep -q "^T2_ROOT=part:linux" /run/t2-root' ||
	{ echo "$HOST: not the Linux of the linux partition (no /run/t2-root with part:linux)" >&2; exit 1; }
EXTRA=$("${SSH[@]}" 'cat /boot/extlinux/cmdline.extra 2>/dev/null' | tr '\n' ' ' | sed 's/  */ /g;s/^ //;s/ $//')
if [ -n "$EXTRA" ]; then
	CMDLINE="$CMDLINE $EXTRA"
	echo "cmdline.extra from the phone: $EXTRA"
fi

rm -rf "$O"
mkdir -p "$O/boot/dtbs" "$O/boot/extlinux"
"$ROOT/tools/initramfs/build.sh" "$O/boot/initramfs" >/dev/null
cp "$B/Image.gz" "$O/boot/Image.gz"
cp "$B"/dts/mediatek/mt6878-unihertz-titan2*.dtb "$O/boot/dtbs/"
# titan2-boot shows at most CONF_MAX_ENTRIES of the partition's entries (12 in packs before
# 2026-11-12, 24 in current ones): the useful variants first, the old one-subsystem tests after them
if [ -z "${VARIANTS:-}" ]; then
	VARIANTS="all all-fb full $(cd "$B/dts/mediatek" && ls mt6878-unihertz-titan2-*.dtb |
		sed 's/^mt6878-unihertz-titan2-\(.*\)\.dtb$/\1/' | grep -vx 'all\|all-fb' | sort | tr '\n' ' ')"
fi
C=$O/boot/extlinux/extlinux.conf
{
	echo "# linux partition: entries of the titan2-boot menu (\"Linux partition\"), scripts/linux-update.sh"
	echo "DEFAULT $DEFAULT"
	for v in $VARIANTS; do
		if [ "$v" = full ]; then dtb=mt6878-unihertz-titan2.dtb label=full
		else dtb=mt6878-unihertz-titan2-$v.dtb label=$v
		fi
		[ -f "$O/boot/dtbs/$dtb" ] || { echo "no $dtb" >&2; exit 1; }
		printf 'LABEL %s\n  MENU LABEL Linux %s -%s\n  LINUX /boot/Image.gz\n  FDT /boot/dtbs/%s\n' "$label" "$REL" "$label" "$dtb"
		printf '  INITRD /boot/initramfs\n  APPEND %s t2.root=part:linux\n' "$CMDLINE"
	done
	# the previous version (moved to /boot/prev on the phone): in case the new kernel does not boot
	printf 'LABEL previous\n  MENU LABEL Linux previous (full)\n  LINUX /boot/prev/Image.gz\n'
	printf '  FDT /boot/prev/dtbs/mt6878-unihertz-titan2.dtb\n  INITRD /boot/prev/initramfs\n'
	printf '  APPEND %s t2.root=part:linux\n' "$CMDLINE"
} >"$C"
# root-owned archives: the build user's uid must not end up on /, /etc, /usr or /lib/modules
TAR_ROOT=(--owner=0 --group=0 --numeric-owner)
tar "${TAR_ROOT[@]}" -C "$KO/modules" -czf "$O/modules.tar.gz" "lib/modules/$REL"

if [ -n "${FIRMWARE:-}" ]; then
	mkdir -p "$O/firmware/mediatek/mt6878"
	fw=${FW_DIR:+$FW_DIR/frsm-spk1.bin}
	[ -n "$fw" ] && [ -f "$fw" ] && cp "$fw" "$O/firmware/frsm-spk1.bin" ||
		echo "no frsm-spk1.bin (FW_DIR=): sound without the amplifier firmware"
	NV=${NVDATA_IMG:-}
	[ -n "$NV" ] || echo "no NVDATA_IMG=: no WIFI/BT_Addr"
	if [ -n "$NV" ]; then
		debugfs -R "dump /APCFG/APRDEB/WIFI $O/firmware/mediatek/mt6878/WIFI" "$NV" 2>/dev/null
		[ -s "$O/firmware/mediatek/mt6878/WIFI" ] || { rm -f "$O/firmware/mediatek/mt6878/WIFI"; echo "WIFI could not be read from $NV"; }
		# Bluetooth address (btmtkbgf reads the first 6 bytes)
		debugfs -R "dump /APCFG/APRDEB/BT_Addr $O/firmware/mediatek/mt6878/BT_Addr" "$NV" 2>/dev/null
		[ -s "$O/firmware/mediatek/mt6878/BT_Addr" ] || { rm -f "$O/firmware/mediatek/mt6878/BT_Addr"; echo "BT_Addr could not be read from $NV"; }
	fi
	tar "${TAR_ROOT[@]}" -C "$O/firmware" -czf "$O/firmware.tar.gz" .
fi

echo "to the phone: kernel $REL, $(ls "$O/boot/dtbs" | wc -l) DTBs, entries: $VARIANTS previous"
tar "${TAR_ROOT[@]}" -C "$O" -cf - boot modules.tar.gz $( [ -f "$O/firmware.tar.gz" ] && echo firmware.tar.gz ) |
	"${SSH[@]}" 'set -e
	T=/tmp/t2-update; rm -rf $T; mkdir -p $T; tar -C $T -xf -
	# the previous version -> /boot/prev (only if /boot holds a kernel)
	if [ -f /boot/Image.gz ]; then
		rm -rf /boot/prev; mkdir -p /boot/prev
		cp -a /boot/Image.gz /boot/initramfs /boot/prev/ 2>/dev/null || true
		[ -d /boot/dtbs ] && cp -a /boot/dtbs /boot/prev/
	fi
	# the whole tree of this release is replaced: a module that became built-in must not stay behind
	rm -rf /lib/modules/'"$REL"'
	tar -C / -xzf $T/modules.tar.gz
	[ -f $T/firmware.tar.gz ] && mkdir -p /lib/firmware && tar -C /lib/firmware -xzf $T/firmware.tar.gz
	rm -rf /boot/dtbs.new && cp -a $T/boot/dtbs /boot/dtbs.new && rm -rf /boot/dtbs && mv /boot/dtbs.new /boot/dtbs
	cp $T/boot/Image.gz /boot/Image.gz.new && mv /boot/Image.gz.new /boot/Image.gz
	cp $T/boot/initramfs /boot/initramfs.new && mv /boot/initramfs.new /boot/initramfs
	# belt and braces: whatever the archives said, system files belong to root
	chown -R 0:0 /lib/modules /lib/firmware /boot
	for d in / /etc /usr /usr/local /usr/local/bin /usr/share /lib; do [ "$(stat -c %u:%g $d)" = 0:0 ] || { echo "owner of $d is $(stat -c %u:%g $d), fixing"; chown 0:0 $d; }; done
	mkdir -p /boot/extlinux
	# keep foreign entries (kernels in /boot/<name>/)
	[ -f /boot/extlinux/extlinux.conf ] && awk '"'"'
		function flush() { if (b != "" && index(b, "LINUX /boot/Image.gz\n") == 0 && index(b, "LINUX /boot/prev/") == 0) printf "\n%s", b; b = "" }
		/^LABEL /{ flush(); b = $0 "\n"; next } { if (b != "" && $0 != "") b = b $0 "\n" } END { flush() }
		'"'"' /boot/extlinux/extlinux.conf >>$T/boot/extlinux/extlinux.conf
	cp $T/boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.new &&
		mv /boot/extlinux/extlinux.conf.new /boot/extlinux/extlinux.conf
	depmod -a '"$REL"' 2>/dev/null || true
	sync; rm -rf $T
	echo "on the phone: $(ls /lib/modules | tr "\n" " ")"; ls -l /boot /boot/prev 2>/dev/null | head -20'
if [ -n "${REBOOT:-}" ]; then
	"${SSH[@]}" 't2-reboot' || true
	echo "rebooting: menu \"Linux partition\" -> entry $DEFAULT"
fi
