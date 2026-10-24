#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Test of titan2-boot in qemu-system-aarch64 (virt, cortex-a76, EL1), without the phone.
# The test build (make qemu) differs in the UART (PL011 of virt), the load address 0x40200000 (qemu
# keeps its own DTB at 0x40000000), in taking the FDT from 0x47c80000, where LK puts it, when x0 is
# empty, and in a disk in RAM (0xc0000000) instead of the UFS. Loaded into memory: the pack (as LK
# would unpack it), LK's FDT of your phone (LK_FDT=: /sys/firmware/fdt read on Android; the copy gets earlycon on the PL011 and 4 GB of memory) and
# a disk image with a GPT (boot_a, boot_b = Android boot.img, linux = ext4 with /boot/extlinux).
# Scenarios:
#   android: menu from the pack, 1 s countdown, Android kernel (lz4) with LK's FDT as it is
#   linux:   mainline from the pack right away: Image.gz, LK data merged into the DTB, initrd from the pack
#   disk:    DEFAULT ufs, countdown 0: the "Linux partition" item brings up the UFS itself and
#            boots the default entry of the extlinux.conf on the linux partition (ext4)
#   bootb:   ANDROID part:boot_b: the kernel from a boot.img on a partition
#   recovery: LK booted into recovery (atag,boot: mode 2), part:boot_b comes first: the Android
#            kernel still comes from the pack, the UFS is not brought up
# Success: a kernel line from earlycon (Android: Linux version 6.1; mainline: the Titan 2 model).
# After that the kernels crash on virt (it is not the phone), as expected.
#   ANDROID_BOOT=<boot.img> LK_FDT=<LK FDT> scripts/test-bootmenu-qemu.sh [android linux disk bootb recovery]
#   (TITAN2_BOOT=<dir in the repo>, default titan2-boot; the mainline build in out/mainline-build)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
Q=out/bootmenu-qemu
O=$ROOT/$Q
SCEN=${*:-android linux disk bootb recovery}
mkdir -p "$O" "$ROOT/out/initramfs-dl/debian-apt/archives/partial" "$ROOT/out/initramfs-dl/debian-apt/lists/partial"
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" titan2-kbuild "$@"; }

TB=$ROOT/${TITAN2_BOOT:-titan2-boot}
[ -n "${ANDROID_BOOT:-}" ] && [ -n "${LK_FDT:-}" ] || { echo "ANDROID_BOOT=<boot.img> and LK_FDT=<FDT of your phone> are required" >&2; exit 1; }
make -s -C "$TB" qemu TOOLCHAIN="$ROOT/toolchains/clang-r574158" >/dev/null
ksize=$(od -An -t u4 -j 8 -N 4 "$ANDROID_BOOT" | tr -d ' ')
dd if="$ANDROID_BOOT" of="$O/android.Image" iflag=skip_bytes,count_bytes skip=4096 count="$ksize" status=none
: | cpio -o -H newc --quiet | gzip -9n >"$O/empty.cpio.gz"
B=$ROOT/out/mainline-build/arch/arm64/boot
DTB=$B/dts/mediatek/mt6878-unihertz-titan2.dtb

# LK's FDT: earlycon on the PL011 of virt instead of the phone's 8250, memory: qemu's 4 GB
cp "$LK_FDT" "$O/lk.dtb"
args=$(fdtget -t s "$O/lk.dtb" /chosen bootargs | sed 's/earlycon=[^ ]*/earlycon=pl011,mmio32,0x9000000/')
fdtput -t s "$O/lk.dtb" /chosen bootargs "$args"
fdtput -t x "$O/lk.dtb" /memory reg 0 0x40000000 1 0
# the same, but LK booted into recovery (boot mode = 2)
cp "$O/lk.dtb" "$O/lk-recovery.dtb"
boot=$(fdtget -t x "$O/lk.dtb" /chosen atag,boot)
fdtput -t x "$O/lk-recovery.dtb" /chosen atag,boot $(echo "$boot" | awk '{ $3 = "2000000"; print }')

# disk: GPT with 4 KiB sectors (like the UFS), boot_a, boot_b (Android boot.img), linux (ext4)
D=$O/disk
rm -rf "$D"
mkdir -p "$D/root/boot/extlinux" "$D/root/boot/dtbs"
cp "$B/Image.gz" "$D/root/boot/Image-7.2.gz"
ln -s Image-7.2.gz "$D/root/boot/Image.gz"
cp "$DTB" "$D/root/boot/dtbs/titan2.dtb"
cp "$O/empty.cpio.gz" "$D/root/boot/initrd"
cat >"$D/root/boot/extlinux/extlinux.conf" <<'EOF'
TIMEOUT 0
DEFAULT disk
LABEL disk
  MENU LABEL Linux from the linux partition
  LINUX /boot/Image.gz
  FDT /boot/dtbs/titan2.dtb
  INITRD /boot/initrd
  APPEND console=ttyAMA0 loglevel=8
EOF
mke2fs -q -t ext4 -d "$D/root" "$D/linux.img" 48M
truncate -s 0 "$O/disk.img"
truncate -s 160M "$O/disk.img"
sfdisk -q --sector-size 4096 "$O/disk.img" <<'EOF'
label: gpt
size=1MiB, name="boot_a"
size=64MiB, name="boot_b"
size=48MiB, name="linux"
EOF
part_start() { sfdisk -q --sector-size 4096 -d "$O/disk.img" | awk -F'[=,]' -v n="name=\"$1\"" 'index($0, n) {gsub(/ /,"",$2); print $2}'; }
dd if="$ANDROID_BOOT" of="$O/disk.img" bs=4096 seek="$(part_start boot_b)" conv=notrunc status=none
dd if="$D/linux.img" of="$O/disk.img" bs=4096 seek="$(part_start linux)" conv=notrunc status=none

for s in $SCEN; do
	conf=$O/$s.conf
	echo "UFS on" >"$conf"
	case $s in
	android) printf 'TIMEOUT 10\nDEFAULT android\n' >>"$conf" ;;
	linux) printf 'TIMEOUT 0\nDEFAULT linux\n' >>"$conf" ;;
	disk) printf 'TIMEOUT 0\nDEFAULT ufs\n' >>"$conf" ;;
	bootb|recovery) printf 'TIMEOUT 0\nDEFAULT b\nLABEL b\n  ANDROID part:boot_b\n' >>"$conf" ;;
	*) echo "scenario: android|linux|disk|bootb|recovery" >&2; exit 1 ;;
	esac
	cat >>"$conf" <<'EOF'
LABEL android
  MENU LABEL Android
  ANDROID /android/Image
LABEL linux
  LINUX /linux/Image.gz
  FDT /linux/titan2.dtb
  INITRD /linux/initramfs
  APPEND console=ttyAMA0 loglevel=8
EOF
	run python3 -I "/work${TB#$ROOT}/tools/mkpack.py" --loader "/work${TB#$ROOT}/out-qemu/titan2-boot.bin" \
		--conf "/work/$Q/$s.conf" --file "android/Image=/work/$Q/android.Image" \
		--file "linux/Image.gz=/work${B#$ROOT}/Image.gz" --file "linux/titan2.dtb=/work${DTB#$ROOT}" \
		--file "linux/initramfs=/work/$Q/empty.cpio.gz" \
		--load-addr 0x40200000 --out "/work/$Q/boot-$s.img" --raw "/work/$Q/pack-$s.bin" >/dev/null
done

docker run --rm -v "$ROOT:/work" -e UIDGID="$(id -u):$(id -g)" -e SCEN="$SCEN" debian:trixie bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
D=/work/out/initramfs-dl/debian-apt
trap "chown -R $UIDGID $D /work/'"$Q"'" EXIT
apt-get -qq -o Dir::Cache::archives=$D/archives -o Dir::State::lists=$D/lists update
apt-get -qq -y --no-install-recommends -o Dpkg::Use-Pty=0 -o Dir::Cache::archives=$D/archives -o Dir::State::lists=$D/lists \
	install qemu-system-arm >/dev/null
qemu-system-aarch64 --version | head -1
cd /work/'"$Q"'
for s in $SCEN; do
	lk=lk.dtb
	[ $s != recovery ] || lk=lk-recovery.dtb
	timeout 60 qemu-system-aarch64 -M virt -cpu cortex-a76 -m 4096 -nographic -no-reboot -nic none \
		-device loader,file=pack-$s.bin,addr=0x40200000,force-raw=on \
		-device loader,file=$lk,addr=0x47c80000,force-raw=on \
		-device loader,file=disk.img,addr=0xc0000000,force-raw=on \
		-device loader,addr=0x40200000,cpu-num=0 >$s.log 2>&1 </dev/null || true
done
'
rc=0
for s in $SCEN; do
	log=$O/$s.log
	echo "== $s ($Q/$s.log)"
	grep -a "titan2-boot" "$log" | grep -av "\] mem 0x" | head -40
	case $s in
	android|bootb|recovery) want='Linux version 6\.1' ;;
	linux|disk) want='Machine model: Unihertz Titan 2' ;;
	esac
	if [ $s = recovery ] && grep -a "titan2-boot" "$log" | grep -qi "ufs"; then
		grep -ai "ufs" "$log" | head -5
		echo "FAIL $s: UFS touched in recovery mode"
		rc=1
	elif grep -aqE "$want" "$log"; then
		grep -aE "$want" "$log" | head -1 | cut -c1-110
		echo "PASS $s"
	else
		tail -20 "$log"
		echo "FAIL $s"
		rc=1
	fi
done
exit $rc
