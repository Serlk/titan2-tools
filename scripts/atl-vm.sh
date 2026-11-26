#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# ATL in full-system emulation: qemu-system-aarch64 (TCG) + virtio-gpu-gl (virgl/venus on the host
# GPU), Alpine's linux-virt kernel, niri on DRM through seatd, and the same Titan 2 overlay
# (tools/atl/overlay) that goes into the phone image. This exercises the real path GTK4 -> GSK
# (gl/vulkan) -> Mesa -> dmabuf -> niri -> KMS, input through evdev/libinput (uinput injector) and
# the xkb keymap.
#
#   scripts/atl-vm.sh rootfs             guest rootfs out/atl-vm/rootfs (Alpine v3.24 aarch64 + linux-virt +
#                                        niri + ATL from edge/testing and out/atl/packages), through qemu-user
#   scripts/atl-vm.sh update [pkg..]     update the guest files (overlay, tools/atl/vm/guest) [+ packages]
#   scripts/atl-vm.sh image              ext4 image out/atl-vm/disk.img from the rootfs (VM_SIZE=6G)
#   scripts/atl-vm.sh start              start the VM in the background (container titan2-atl-vm; VNC 127.0.0.1:5900)
#   scripts/atl-vm.sh stop | status | console (ttyAMA0 log) | qmp '{"execute":...}'
#   scripts/atl-vm.sh root 'sh ...'      command as root (root shell of the console), output as for cmd
#   scripts/atl-vm.sh cmd 'command'      run in the niri session (user atl), output here (CMD_TIMEOUT=120)
#   scripts/atl-vm.sh shot [name]        screenshot (grim) -> out/atl-vm/shots/name.png
#   scripts/atl-vm.sh key CHORD..        keys through uinput (evdev codes: 158=Back, 580=Menu, 202=Sym, 203=Fn, 100=Alt)
#   scripts/atl-vm.sh tap X Y | swipe X1 Y1 X2 Y2 [ms]   touches (virtual 1440x1440 touchscreen)
#   scripts/atl-vm.sh test APK [sec] [-- ATL arguments]  atl-run in the session, screenshots every SHOTS s,
#                                        log -> out/atl-vm/shots/APK.log (ATL_GSK=gl|vulkan|cairo, ATL_LOG)
#   scripts/atl-vm.sh kill               stop the running ATL application in the guest
#
# A second, independent bench instance (own rootfs, image and container; the other is untouched):
#   VM_INSTANCE=apps VM_VNC=127.0.0.1:5901 scripts/atl-vm.sh rootfs|image|start|...   -> out/atl-vm-apps
# VM_KERNEL_IMAGE=<arm64 Image>: boot that kernel instead of linux-virt (no initramfs; it needs
# virtio, 9p and ext4 built in).
#
# Host: docker, a GPU with a render node (/dev/dri/renderD128, virgl through egl-headless); kernel
# >= 6.7 to build the rootfs (binfmt in a user namespace, as for build-atl.sh). No KVM needed
# (aarch64 on x86 runs under TCG). The host image: docker build -t titan2-atl-qemu docker/atl-vm
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/atl-vm${VM_INSTANCE:+-$VM_INSTANCE}
DL=$ROOT/out/atl-dl
ATL=$ROOT/out/atl
REL=${ALPINE_RELEASE:-v3.24}
IMG=alpine:${REL#v}
QIMG=titan2-atl-qemu
NAME=titan2-atl-vm${VM_INSTANCE:+-$VM_INSTANCE}
RENDER=${RENDER_NODE:-/dev/dri/renderD128}
mkdir -p "$OUT/share/cmd" "$OUT/share/out" "$OUT/shots" "$OUT/boot" "$DL/apk-cache" "$DL/apk-cache-x86"

# rootfs/image build: the same container and user namespace as build-atl.sh (tools/atl/inner.sh)
dk() {
	docker run --rm ${DK_TTY:-} --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
		--cap-add SYS_ADMIN -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
		-e ALPINE_RELEASE="$REL" -e EXTRA_PKGS="${EXTRA_PKGS:-}" -e VM_SIZE="${VM_SIZE:-6G}" -e TERM="${TERM:-xterm}" \
		-v "$ROOT:/work:ro" -v "$OUT:/out" -v "$DL:/dl" -v "$ATL:/atl:ro" "$IMG" sh /work/tools/atl/inner.sh "$@"
}

qimage() {
	docker image inspect "$QIMG" >/dev/null 2>&1 || docker build -q -t "$QIMG" "$ROOT/docker/atl-vm"
}

running() { [ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" = true ]; }

cmd_start() {
	qimage
	! running || { echo "VM already running ($NAME)"; return 0; }
	docker rm -f "$NAME" >/dev/null 2>&1 || true
	[ -s "$OUT/disk.img" ] || { echo "no $OUT/disk.img: scripts/atl-vm.sh rootfs && image" >&2; exit 1; }
	[ -e "$RENDER" ] || { echo "no $RENDER (host GPU)" >&2; exit 1; }
	rm -f "$OUT/share/cmd"/* "$OUT/console.log"
	: >"$OUT/console.log"
	local mem=${VM_MEM:-6G} cpus=${VM_CPUS:-8} audio=() kernel=()
	# VM_KERNEL_IMAGE: an arm64 Image of your own, without initramfs (virtio/9p/ext4 built in);
	# otherwise Alpine's linux-virt and its initramfs
	if [ -n "${VM_KERNEL_IMAGE:-}" ]; then
		[ -s "$VM_KERNEL_IMAGE" ] || { echo "no $VM_KERNEL_IMAGE" >&2; exit 1; }
		cp "$VM_KERNEL_IMAGE" "$OUT/boot/Image-custom"
		kernel=(-kernel /out/boot/Image-custom)
	else
		kernel=(-kernel /out/boot/vmlinuz-virt -initrd /out/boot/initramfs-virt)
	fi
	# sound: virtio-sound -> a wav file on the host (checks that OpenSL/SDL2/ALSA produce a signal)
	[ -z "${VM_AUDIO:-}" ] || audio=(-audiodev "wav,id=snd0,path=/out/audio.wav" -device virtio-sound-pci,audiodev=snd0)
	docker run -d --name "$NAME" -u "$(id -u):$(id -g)" --device "$RENDER" -e HOME=/tmp \
		-p "${VM_VNC:-127.0.0.1:5900}:5900" \
		-v "$OUT:/out" -v "$DL/test-apks:/apks:ro" "$QIMG" \
		qemu-system-aarch64 -M virt,gic-version=3 -cpu "${VM_CPU:-cortex-a76}" -smp "$cpus" -m "$mem" \
		-accel tcg,thread=multi \
		"${kernel[@]}" \
		-append "root=/dev/vda rw rootfstype=ext4 console=ttyAMA0 modules=ext4,virtio_blk,virtio_pci,virtio_gpu quiet ${VM_APPEND:-}" \
		-drive if=none,file=/out/disk.img,format=raw,id=hd0 -device virtio-blk-pci,drive=hd0 \
		-device "virtio-gpu-gl-pci,xres=1440,yres=1440,${VM_GPU:-venus=on,blob=on,hostmem=4G}" -display egl-headless,rendernode="$RENDER" -vnc :0 \
		-device virtio-keyboard-pci -device virtio-tablet-pci \
		-virtfs local,path=/out/share,mount_tag=host,security_model=none,id=host \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
		-chardev socket,id=con,path=/out/console.sock,server=on,wait=off,logfile=/out/console.log -serial chardev:con \
		-qmp unix:/out/qmp.sock,server,nowait -monitor none -nodefaults -rtc base=utc "${audio[@]}" >/dev/null
	echo "VM started: container $NAME, VNC ${VM_VNC:-127.0.0.1:5900}, console $OUT/console.log"
	echo "waiting for the agent (niri)..."
	local t=0
	rm -f "$OUT/share/out/agent.log"
	while [ $t -lt "${BOOT_TIMEOUT:-600}" ]; do
		sleep 5; t=$((t + 5))
		running || { echo "the container exited:"; docker logs "$NAME" 2>&1 | tail -20; return 1; }
		[ ! -s "$OUT/share/out/agent.log" ] || { echo "agent ready after ${t} s: $(cat "$OUT/share/out/agent.log")"; return 0; }
	done
	echo "no answer from the agent within ${t} s; console:"; tail -30 "$OUT/console.log"; return 1
}

cmd_stop() {
	if running; then
		cmd_qmp '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
		local t=0
		while running && [ $t -lt 30 ]; do sleep 1; t=$((t + 1)); done
	fi
	docker rm -f "$NAME" >/dev/null 2>&1 || true
	echo "VM stopped"
}

cmd_qmp() {
	running || { echo "VM not running" >&2; return 1; }
	docker exec -i "$NAME" python3 - "$1" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("/out/qmp.sock")
f = s.makefile("rw")
f.readline()
f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
f.write(sys.argv[1] + "\n"); f.flush()
print(f.readline().strip())
PY
}

# command in the niri session: a file in share/cmd, the agent writes .out/.rc
cmd_cmd() {
	running || { echo "VM not running" >&2; return 1; }
	local id b
	id=$(date +%s%N)
	b=$OUT/share/cmd/$id
	printf '%s\n' "$1" >"$b.sh.tmp" && mv "$b.sh.tmp" "$b.sh"
	local t=0 lim=${CMD_TIMEOUT:-120}
	while [ ! -s "$b.rc" ]; do
		sleep 1; t=$((t + 1))
		[ $t -lt "$lim" ] || { echo "cmd: no answer within ${lim} s" >&2; cat "$b.out" 2>/dev/null; return 124; }
	done
	cat "$b.out"
	local rc
	rc=$(cat "$b.rc")
	rm -f "$b.run" "$b.out" "$b.rc"
	return "$rc"
}

# command as root: through the root shell on the ttyAMA0 console (console.sock), output in share/root
cmd_root() {
	running || { echo "VM not running" >&2; return 1; }
	local id b
	id=$(date +%s%N)
	mkdir -p "$OUT/share/root"
	b=$OUT/share/root/$id
	printf '%s\n' "$1" >"$b.sh"
	printf 'sh /host/root/%s.sh >/host/root/%s.out 2>&1; echo $? >/host/root/%s.rc\n' "$id" "$id" "$id" |
		docker exec -i "$NAME" socat -t 1 - UNIX-CONNECT:/out/console.sock >/dev/null
	local t=0 lim=${CMD_TIMEOUT:-120}
	while [ ! -s "$b.rc" ]; do
		sleep 1; t=$((t + 1))
		[ $t -lt "$lim" ] || { echo "root: no answer within ${lim} s" >&2; cat "$b.out" 2>/dev/null; return 124; }
	done
	cat "$b.out"
	local rc
	rc=$(cat "$b.rc")
	rm -f "$b.sh" "$b.out" "$b.rc"
	return "$rc"
}

cmd_shot() {
	local n=${1:-shot-$(date +%H%M%S)}
	cmd_cmd "grim /host/out/$n.png" && mv "$OUT/share/out/$n.png" "$OUT/shots/$n.png" && echo "$OUT/shots/$n.png"
}

cmd_test() {
	local apk=$1 secs=${2:-120}
	shift; [ $# -eq 0 ] || shift
	[ "${1:-}" != "--" ] || shift
	[ -s "$DL/test-apks/$apk" ] || { echo "no $DL/test-apks/$apk" >&2; return 1; }
	local name=${apk%.apk} shots=${SHOTS:-30}
	cmd_cmd "pkill -f android-translation-layer; sleep 1; rm -f /host/out/$name.log /host/out/$name.pid
		ATL_LOGFILE=0 ATL_GSK=${ATL_GSK:-gl} ATL_LOG=${ATL_LOG:-*:i} ${TEST_ENV:-} nohup atl-run /usr/share/titan2/apks/$apk $* >/host/out/$name.log 2>&1 &
		echo \$! >/host/out/$name.pid; echo started pid \$!" >/dev/null
	local t=0 n=0
	while [ $t -lt "$secs" ]; do
		sleep "$shots"; t=$((t + shots)); n=$((n + 1))
		if ! cmd_cmd "kill -0 \$(cat /host/out/$name.pid)" >/dev/null 2>&1; then
			echo "== the application ended before ${t} s"; break
		fi
		cmd_cmd "grim /host/out/$name-$(printf %03d $n).png" >/dev/null && mv "$OUT/share/out/$name-$(printf %03d $n).png" "$OUT/shots/" &&
			echo "== screenshot $OUT/shots/$name-$(printf %03d $n).png (${t} s)"
	done
	cp "$OUT/share/out/$name.log" "$OUT/shots/$name.log"
	echo "== log $OUT/shots/$name.log:"; grep -iE 'llvmpipe|virgl|renderer|fatal|exception|error|not found|unimplemented|NoSuch' "$OUT/shots/$name.log" | sort | uniq -c | sort -rn | head -25
}

case "${1:-}" in
rootfs) dk vm-rootfs ;;
update) shift; dk vm-update "$@" ;;
image) dk vm-image ;;
start) cmd_start ;;
stop) cmd_stop ;;
status) running && { echo "VM running ($NAME)"; tail -3 "$OUT/share/out/agent.log" 2>/dev/null; } || echo "VM not running" ;;
console) tail -${2:-40} "$OUT/console.log" ;;
qmp) cmd_qmp "$2" ;;
cmd) cmd_cmd "$2" ;;
root) cmd_root "$2" ;;
shot) cmd_shot "${2:-}" ;;
key) shift; cmd_cmd "atl-vm-input key $*" ;;
tap) shift; cmd_cmd "atl-vm-input tap $*" ;;
swipe) shift; cmd_cmd "atl-vm-input swipe $*" ;;
test) shift; cmd_test "$@" ;;
kill) cmd_cmd "pkill -f android-translation-layer; echo killed" ;;
*) sed -n '5,33p' "$0"; exit 2 ;;
esac
