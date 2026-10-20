#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Test the USB console initramfs in qemu-system-aarch64 (virt) with the same kernel as for the
# phone (arm64 defconfig enables virt). UDC is dummy_hcd (t2.udc=dummy_udc.0), the gadget host
# side is cdc_ncm/cdc-acm in the same kernel (modules only in the test add-on).
#   [O=out/mainline-usb] [SCEN='nokey key noudc'] tools/initramfs/qemu/run.sh
# Scenarios: nokey (empty password), key (authorized_keys), noudc (no UDC -> message and
# automatic reboot; qemu with -no-reboot exits). Logs: out/initramfs-usb/qemu/*.log
# qemu is installed in a throwaway debian:trixie container (.deb cache in out/initramfs-dl/debian-apt).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
O=${O:-out/mainline-usb}
Q=out/initramfs-usb/qemu
mkdir -p "$ROOT/$Q" "$ROOT/out/initramfs-dl/debian-apt/archives/partial" "$ROOT/out/initramfs-dl/debian-apt/lists/partial"
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" titan2-kbuild "$@"; }

# test key (qemu only)
[ -f "$ROOT/$Q/id_test" ] || ssh-keygen -q -t ed25519 -N '' -C t2-qemu-test -f "$ROOT/$Q/id_test"
OFFLINE=1 "$ROOT/tools/initramfs/build.sh" "$ROOT/$Q/nokey.cpio.gz" >/dev/null
OFFLINE=1 T2_AUTHORIZED_KEYS=/work/$Q/id_test.pub "$ROOT/tools/initramfs/build.sh" "$ROOT/$Q/key.cpio.gz" >/dev/null
run python3 -I /work/tools/initramfs/qemu/overlay.py /work/$O /work/$Q/overlay.cpio.gz
run python3 -I /work/tools/initramfs/apk.py "$([ -n "${OFFLINE:-}" ] && echo list || echo fetch)" /work/out/initramfs-dl v3.24 dropbear-convert >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" -e T2_DL=/work/out/initramfs-dl titan2-kbuild \
	python3 -I /work/tools/initramfs/qemu/overlay.py /work/$O /work/$Q/overlay-key.cpio.gz /work/$Q/id_test
cat "$ROOT/$Q/nokey.cpio.gz" "$ROOT/$Q/overlay.cpio.gz" >"$ROOT/$Q/t-nokey.cpio.gz"
cat "$ROOT/$Q/key.cpio.gz" "$ROOT/$Q/overlay-key.cpio.gz" >"$ROOT/$Q/t-key.cpio.gz"
cp "$ROOT/$Q/nokey.cpio.gz" "$ROOT/$Q/t-noudc.cpio.gz"
truncate -s 16M "$ROOT/$Q/disk.img"

docker run --rm -v "$ROOT:/work" -e UIDGID="$(id -u):$(id -g)" -e SCEN="${SCEN:-nokey key noudc}" debian:trixie bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
D=/work/out/initramfs-dl/debian-apt
trap "chown -R $UIDGID $D /work/'"$Q"'" EXIT
apt-get -qq -o Dir::Cache::archives=$D/archives -o Dir::State::lists=$D/lists update
apt-get -qq -y --no-install-recommends -o Dpkg::Use-Pty=0 -o Dir::Cache::archives=$D/archives -o Dir::State::lists=$D/lists \
	install qemu-system-arm >/dev/null
qemu-system-aarch64 --version | head -1
cd /work/'"$Q"'
q() {
	local name=$1; shift
	timeout 420 qemu-system-aarch64 -M virt -cpu cortex-a76 -smp 2 -m 1024 -nographic -no-reboot -nic none \
		-kernel /work/'"$O"'/arch/arm64/boot/Image -initrd t-$name.cpio.gz \
		-drive if=virtio,file=disk.img,format=raw \
		-append "console=ttyAMA0 loglevel=6 $*" </dev/null >$name.log 2>&1 || echo "qemu $name: exit $?"
	grep -a "T2TEST\|titan2:" $name.log | tail -40 | sed "s/^/[$name] /" || true
}
for s in $SCEN; do
	case $s in
	nokey|key) q $s t2.udc=dummy_udc.0 ;;
	noudc) q noudc t2.reboot_after=45 ;;
	esac
done
'
