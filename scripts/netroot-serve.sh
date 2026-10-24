#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Alpine root for mainline on the Titan 2, PC side (network root over NBD, and the image for the
# linux partition, scripts/make-linux-root.sh). The root is the ext4 image out/netroot/rootfs.img,
# served over NBD through USB-NCM; the phone boots with t2.root=nbd:172.16.42.2:10809:t2root.
#
#   scripts/netroot-serve.sh build     build the rootfs (Alpine aarch64) and the image; if the image
#                                      exists, add to it (packages, services, modules) without
#                                      overwriting manual edits; FORCE=1: build anew (replaces it)
#   scripts/netroot-serve.sh modules <O>  put the kernel modules from <O>/modules into the image
#                                      (scripts/build-mainline.sh, O=<O>): /lib/modules/<version>
#   scripts/netroot-serve.sh serve     nbd-server in a container (Ctrl-C stops), waits for 172.16.42.2
#   scripts/netroot-serve.sh fsck      e2fsck -f -p of the image (only while serve is not running)
#   scripts/netroot-serve.sh status    what is built and running
#
# Variables:
#   ALPINE_RELEASE=v3.24  ALPINE_MIRROR=https://dl-cdn.alpinelinux.org/alpine
#   SIZE=4G               image size (sparse file; new builds only)
#   EXTRA_PKGS="..."      additional packages in the rootfs
#   GFX=1                 add graphics packages to check DRM/fbcon (GFX_PKGS below)
#   LOCAL_REPO=<dir>      own apk repository (inside the project, with aarch64/APKINDEX.tar.gz),
#                         e.g. a Mesa build: added for the duration of the build, its packages
#                         are newer than the Alpine ones of the same name and apk picks them;
#                         LOCAL_PKGS (default: the Mesa packages, see below) are installed or
#                         upgraded (apk add --upgrade). The key LOCAL_KEY (default
#                         out/atl/build-keys/titan2-atl.rsa.pub) also goes into /etc/apk/keys.
#                         apk checks these packages with that key; verify-cache (Alpine key)
#                         skips them, the list is in out/netroot/stage/packages-local.txt
#   MODULES=<O>           build: also put the kernel modules from <O>/modules (like "modules")
#   AUTHORIZED_KEYS=file  SSH keys for root (otherwise tools/initramfs/authorized_keys, otherwise
#                         ~/.ssh/id_ed25519.pub; without keys root has an EMPTY password,
#                         dropbear -B); the keys are not changed when adding to an image
#   LISTEN=172.16.42.2 PORT=10809 EXPORT=t2root   for serve (LISTEN: the USB network address only)
#   IMG=out/netroot/rootfs.img
#
# Adding to an image (build on an existing image, modules): only while nobody serves the image
# (serve stopped, phone off). A copy is edited (cp --reflink), checked (e2fsck, package check)
# and only then replaces the image; the previous one stays as <image>.prev. The image is
# mounted (loop) in a throwaway container with --privileged, the only place that needs
# privileged; nothing is mounted on the host. The scripts of new packages are run by the first
# boot (/etc/t2-netroot/firstboot + firstboot.pkgs). Manual edits (hostname, keys, conf.d,
# /etc/local.d, world) are left alone; only packages, services in runlevels and our files
# (t2-defroute) are added.
#
# What changes on the host: nothing but the files in out/netroot and alpine:3.24 containers
# (--rm; serve: --network host, listens on LISTEN:PORT only). No NAT, no forwarding, no network
# settings; the host gets the address 172.16.42.2 from the phone over DHCP (as for the initramfs).
#
# Trust: the aarch64 packages are installed by apk-tools (x86_64, from the alpine:3.24 image)
# with --keys-dir holding only the Alpine aarch64 key from tools/initramfs/keys (the same as for
# the initramfs); then every package of the cache is checked independently by
# tools/netroot/verify-cache.py (the code of tools/initramfs/apk.py: APKINDEX signature, control
# sha1, data sha256). The package scripts (--no-scripts: aarch64 cannot run on x86 without
# qemu-user) are run by the initramfs on the phone at the first boot (tools/netroot/firstboot).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REL=${ALPINE_RELEASE:-v3.24}
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
D=$ROOT/out/netroot
IMG=$(realpath -m "${IMG:-$D/rootfs.img}")
case "$IMG" in "$D"/*) ;; *) echo "IMG must be in $D" >&2; exit 1 ;; esac
IMGREL=${IMG#"$D"/}
LISTEN=${LISTEN:-172.16.42.2}
PORT=${PORT:-10809}
EXPORT=${EXPORT:-t2root}
CTR=${CTR:-titan2-netroot-nbd}
ALPINE_IMG=alpine:${REL#v}

# Base set. eudev (udevd + udev-trigger): libinput and module autoloading (panthor); mounts
# nothing, the UFS ro protection is kept by t2-roguard/t2-hotplug and the rule
# 00-t2-roguard.rules (installed by the initramfs). seatd: DRM/input access for compositors.
# chrony: NTP time through the PC (t2-defroute). linux-firmware-arm: the Mali CSF firmware
# (arm/mali/arch11.8/mali_csffw.bin) for panthor. e2fsprogs-extra: resize2fs for t2-growroot
# (a root on the linux partition is grown to the whole partition).
PKGS="alpine-base openrc busybox-extras dropbear dropbear-openrc dropbear-scp kbd evtest i2c-tools
iproute2 e2fsprogs e2fsprogs-extra eudev udev-init-scripts udev-init-scripts-openrc kmod seatd seatd-openrc
chrony chrony-openrc linux-firmware-arm"
# Graphics (GFX=1): check DRM (simpledrm) and fbcon from a full system.
#   cage + foot: wlroots kiosk compositor and terminal (cage -- foot);
#   weston: an alternative (weston --backend=drm); kmscube and libdrm-tests (modetest): quick checks.
GFX_PKGS="cage foot font-dejavu mesa-dri-gallium mesa-egl weston weston-backend-drm
weston-shell-desktop weston-terminal kmscube libdrm-tests"
[ -z "${GFX:-}" ] || PKGS="$PKGS $GFX_PKGS"
PKGS="$PKGS ${EXTRA_PKGS:-}"
# own repository (LOCAL_REPO) -> paths inside /work
LREPO= LKEY= LPKGS=
if [ -n "${LOCAL_REPO:-}" ]; then
	LREPO=$(realpath -m "$ROOT/$LOCAL_REPO")
	LKEY=$(realpath -m "$ROOT/${LOCAL_KEY:-out/atl/build-keys/titan2-atl.rsa.pub}")
	case "$LREPO$LKEY" in "$ROOT"/*"$ROOT"/*) ;; *) echo "LOCAL_REPO/LOCAL_KEY: inside $ROOT" >&2; exit 1 ;; esac
	[ -f "$LREPO/aarch64/APKINDEX.tar.gz" ] && [ -f "$LKEY" ] ||
		{ echo "no $LREPO/aarch64/APKINDEX.tar.gz or $LKEY" >&2; exit 1; }
	LREPO=/work${LREPO#"$ROOT"} LKEY=/work${LKEY#"$ROOT"}
	LPKGS=${LOCAL_PKGS:-mesa mesa-dri-gallium mesa-egl mesa-gles mesa-gbm mesa-gl mesa-vulkan-panfrost}
fi

mkdir -p "$D/apk-cache" "$D/apk-cache-host"
dk() { docker run --rm -e UIDGID="$(id -u):$(id -g)" "$@"; }

keys_file() {
	local k
	for k in "${AUTHORIZED_KEYS:-}" "$ROOT/tools/initramfs/authorized_keys" "$HOME/.ssh/id_ed25519.pub"; do
		[ -n "$k" ] && [ -s "$k" ] && { realpath "$k"; return 0; }
	done
	return 1
}

# module directory <O>/modules/lib/modules/<version> -> path inside /work
modules_src() {
	local o=$1 v
	[ -n "$o" ] || return 0
	o=$(realpath -m "$ROOT/$o")
	case "$o" in "$ROOT"/*) ;; *) echo "MODULES: build directory inside $ROOT" >&2; return 1 ;; esac
	set -- "$o"/modules/lib/modules/*/modules.dep
	[ $# = 1 ] && [ -f "$1" ] || {
		echo "$o/modules does not hold exactly one module version (scripts/build-mainline.sh, MODULES=1)" >&2
		return 1
	}
	v=${1%/modules.dep}
	echo "/work${v#"$ROOT"}"
}

# Somebody serves the image (serve): it must not be changed
img_busy() {
	running && return 0
	command -v fuser >/dev/null && fuser -s "$IMG" 2>/dev/null && return 0
	return 1
}

# Shared part: packages and setup in $R (a new directory or the mounted image).
# MODE=new|update, ONLY_MODULES=1: no apk, MODSRC: module directory.
CONFIGURE='
R=/build/root; [ -z "${T2_DEBUG:-}" ] || set -x
if [ -z "${ONLY_MODULES:-}" ]; then
	mkdir -p $R/etc/apk/keys $R/lib/apk/db
	pv() { sed -n "/^P:/{s/^P://;h};/^V:/{s/^V://;H;x;s/\n/ /p}" $R/lib/apk/db/installed 2>/dev/null; }
	pv | sort >/tmp/pv.before || :
	if [ $MODE = new ]; then
		# only the aarch64 key from tools/initramfs/keys: apk rejects everything not signed by it
		cp /work/tools/initramfs/keys/*.rsa.pub $R/etc/apk/keys/
		printf "%s\n" "$MIRROR/$REL/main" "$MIRROR/$REL/community" >$R/etc/apk/repositories
		INITDB=--initdb
	else
		INITDB=
	fi
	# keys only from tools/initramfs/keys (not from the image), our own repository list
	mkdir -p /tmp/keys
	cp /work/tools/initramfs/keys/*.rsa.pub /tmp/keys/
	printf "%s\n" "$MIRROR/$REL/main" "$MIRROR/$REL/community" >/tmp/repositories
	: >/out/stage/packages-local.txt
	if [ -n "${LREPO:-}" ]; then
		# own repository: its key goes into /tmp/keys (for apk) and into the image (apk on the phone)
		cp $LKEY /tmp/keys/ && cp $LKEY $R/etc/apk/keys/
		echo $LREPO >>/tmp/repositories
		tar -xzOf $LREPO/aarch64/APKINDEX.tar.gz APKINDEX |
			sed -n "/^P:/{s/^P://;h};/^V:/{s/^V://;H;x;s/\n/ /p}" | sort >/tmp/local.pv
	fi
	apk add --root $R --arch aarch64 $INITDB --no-scripts --no-interactive --keys-dir /tmp/keys \
		--repositories-file /tmp/repositories --cache-dir /out/apk-cache $PKGS
	if [ -n "${LREPO:-}" ]; then
		apk add --root $R --arch aarch64 --upgrade --no-scripts --no-interactive --keys-dir /tmp/keys \
			--repositories-file /tmp/repositories --cache-dir /out/apk-cache $LPKGS
	fi
	pv | sort >/tmp/pv.after
	if [ -n "${LREPO:-}" ]; then
		# the packages of the own repository (not in the signed Alpine indexes): separately
		comm -12 /tmp/local.pv /tmp/pv.after >/out/stage/packages-local.txt
		comm -23 /tmp/pv.after /out/stage/packages-local.txt >/tmp/pv.alpine
		sed "s/^/own repository: /" /out/stage/packages-local.txt
	fi
	# to be checked (verify-cache): new and upgraded packages; when adding to an image the others
	# were in it already (including ones installed on the phone, maybe not in the PC cache)
	PVA=/tmp/pv.after; [ -z "${LREPO:-}" ] || PVA=/tmp/pv.alpine
	comm -13 /tmp/pv.before $PVA >/out/stage/packages.txt
	cut -d" " -f1 /tmp/pv.before | sort -u >/tmp/pkgs.before
	cut -d" " -f1 /tmp/pv.after | sort -u | comm -13 /tmp/pkgs.before - >/tmp/pkgs.new
	echo "packages: $(wc -l </tmp/pv.after), new: $(wc -l </tmp/pkgs.new), new and upgraded: $(wc -l </out/stage/packages.txt)"
	# linux-firmware files are zstd-compressed and the kernel has no FW_LOADER_COMPRESS: put plain copies next to them
	find $R/lib/firmware/arm -name "*.zst" 2>/dev/null | while read -r f; do
		[ ! -e "${f%.zst}" ] && [ ! -L "${f%.zst}" ] || continue
		if [ -L "$f" ]; then
			t=$(readlink "$f")
			ln -s "${t%.zst}" "${f%.zst}"
		else
			zstd -q -d "$f" -o "${f%.zst}"
		fi
	done
	# first boot: the package scripts (aarch64) run on the phone
	mkdir -p $R/etc/t2-netroot
	if [ $MODE = new ]; then
		cp /work/tools/netroot/firstboot $R/etc/t2-netroot/firstboot
	elif [ -s /tmp/pkgs.new ]; then
		if [ -e $R/etc/t2-netroot/firstboot ] && [ ! -e $R/etc/t2-netroot/firstboot.pkgs ]; then
			echo "firstboot (full) has not run yet: it will run the new packages too"
		else
			cat /tmp/pkgs.new >>$R/etc/t2-netroot/firstboot.pkgs
		fi
		cp /work/tools/netroot/firstboot $R/etc/t2-netroot/firstboot
	fi
	[ ! -e $R/etc/t2-netroot/firstboot ] || chmod 755 $R/etc/t2-netroot/firstboot
	rl() { mkdir -p $R/etc/runlevels/$1; ln -sfn /etc/init.d/$2 $R/etc/runlevels/$1/$2; }
	if [ $MODE = new ]; then
		echo titan2 >$R/etc/hostname
		printf "127.0.0.1\tlocalhost titan2\n::1\t\tlocalhost titan2\n172.16.42.2\tpc\n" >$R/etc/hosts
		# networking: lo only; the root goes through the USB interface, set up by the initramfs
		mkdir -p $R/etc/network
		printf "auto lo\niface lo inet loopback\n" >$R/etc/network/interfaces
		# console: getty on tty1 (fbcon) exists already; USB serial (ACM, ttyGS0)
		# (getty waits for /dev/ttyGS0 itself: in qemu and without the ACM function there is no device)
		mkdir -p $R/usr/local/sbin
		cat >$R/usr/local/sbin/t2-getty-gs0 <<\EOF
#!/bin/sh
until [ -c /dev/ttyGS0 ]; do sleep 5; done
exec /sbin/getty -L 115200 ttyGS0 vt100
EOF
		chmod 755 $R/usr/local/sbin/t2-getty-gs0
		grep -q t2-getty-gs0 $R/etc/inittab || echo "::respawn:/usr/local/sbin/t2-getty-gs0" >>$R/etc/inittab
		grep -qx ttyGS0 $R/etc/securetty 2>/dev/null || echo ttyGS0 >>$R/etc/securetty
		# openrc services (like setup-alpine, without hwclock: RTC later)
		for s in devfs dmesg; do rl sysinit $s; done
		for s in hostname bootmisc sysctl syslog seedrng; do rl boot $s; done
		for s in networking dropbear local; do rl default $s; done
		for s in killprocs mount-ro savecache; do rl shutdown $s; done
		# SSH: dropbear creates the host keys at the first start (they stay in the image)
		mkdir -p $R/root/.ssh && chmod 700 $R/root/.ssh
		if [ -s /out/stage/authorized_keys ]; then
			cp /out/stage/authorized_keys $R/root/.ssh/authorized_keys
			chmod 600 $R/root/.ssh/authorized_keys
			echo "DROPBEAR_OPTS=\"-s -R\"" >$R/etc/conf.d/dropbear
		else
			echo "DROPBEAR_OPTS=\"-B -R\"" >$R/etc/conf.d/dropbear
		fi
		cat >$R/etc/motd <<MOTD
Titan 2 mainline: Alpine $REL (scripts/netroot-serve.sh, scripts/make-linux-root.sh).
UFS read-only (t2-roguard), /bin/mount: UFS only ro. Internet through the PC (t2-defroute:
default route via 172.16.42.2) if the PC has NAT enabled.
MOTD
	fi
	# our services (every time: updated) and their conf.d (only if missing)
	install -m 755 /work/tools/netroot/t2-defroute $R/etc/init.d/t2-defroute
	[ -e $R/etc/conf.d/t2-defroute ] || install -m 644 /work/tools/netroot/t2-defroute.conf $R/etc/conf.d/t2-defroute
	# other files of ours: tools/netroot/install.d/*.sh (one per service, sourced right here)
	for f in /work/tools/netroot/install.d/*.sh; do if [ -e "$f" ]; then . "$f"; fi; done
	# udev (together with devfs; mdev stays off), seatd, route/DNS, chrony
	for s in udev udev-trigger; do rl sysinit $s; done
	for s in seatd t2-defroute chronyd; do rl default $s; done
fi
if [ -n "${MODSRC:-}" ]; then
	v=${MODSRC##*/}
	rm -rf $R/lib/modules/$v
	mkdir -p $R/lib/modules
	cp -a $MODSRC $R/lib/modules/$v
	# the build/source links point into the build tree on the PC
	rm -f $R/lib/modules/$v/build $R/lib/modules/$v/source
	chown -R 0:0 $R/lib/modules/$v
	echo "modules: /lib/modules/$v ($(find $R/lib/modules/$v -name "*.ko*" | wc -l) files, $(du -sh $R/lib/modules/$v | cut -f1))"
fi
'

cmd_build() {
	local mode=new modsrc
	if [ -e "$IMG" ] && [ -z "${FORCE:-}" ]; then
		mode=update
	fi
	modsrc=$(modules_src "${MODULES:-}")
	if [ $mode = update ]; then
		img_update "$modsrc"
		return
	fi
	local keys=
	if keys=$(keys_file); then
		mkdir -p "$D/stage"
		cp "$keys" "$D/stage/authorized_keys"
		echo "authorized_keys: $keys"
	else
		rm -f "$D/stage/authorized_keys"
		echo "no SSH keys: root with an EMPTY password (USB network only)"
	fi
	mkdir -p "$D/stage"
	docker pull -q "$ALPINE_IMG" >/dev/null
	echo "container: $(docker image inspect "$ALPINE_IMG" --format '{{index .RepoDigests 0}}')"
	# the rootfs is built inside the container (not on the host disk as root), only the image comes out
	dk -v "$ROOT:/work:ro" -v "$D:/out" -e REL="$REL" -e MIRROR="$MIRROR" -e PKGS="$PKGS" -e LREPO="$LREPO" -e LKEY="$LKEY" -e LPKGS="$LPKGS" \
		-e SIZE="${SIZE:-4G}" -e IMGREL="$IMGREL" -e MODE=new -e MODSRC="$modsrc" \
		-e T2_DEBUG="${T2_DEBUG:-}" -e CONFIGURE="$CONFIGURE" "$ALPINE_IMG" sh -euc '
trap "chown -R $UIDGID /out" EXIT
apk add -q --cache-dir /out/apk-cache-host e2fsprogs e2fsprogs-extra zstd >/dev/null
eval "$CONFIGURE"
# --- image ---
rm -f /out/$IMGREL.part
truncate -s $SIZE /out/$IMGREL.part
mke2fs -q -t ext4 -L t2root -E root_owner=0:0 -d $R /out/$IMGREL.part
tune2fs -e remount-ro -c 0 -i 0 /out/$IMGREL.part >/dev/null
e2fsck -fn /out/$IMGREL.part >/dev/null
echo "rootfs: $(du -sh $R | cut -f1)"
'
	# second, independent check of every package that went into the image
	verify_cache
	mv "$IMG.part" "$IMG"
	ls -ls "$IMG"
}

verify_cache() {
	docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work" titan2-kbuild \
		python3 -I /work/tools/netroot/verify-cache.py /work/out/netroot/apk-cache /work/out/netroot/stage/packages.txt
}

# Adding to an existing image: a copy is edited, checked, and replaces the image
img_update() { # <modsrc> ; ONLY_MODULES=1: no packages
	local modsrc=$1
	img_busy && { echo "$IMG is being served (serve/$CTR is running): turn the phone off and stop serve first" >&2; exit 1; }
	[ -f "$IMG" ] || { echo "no $IMG: run build first" >&2; exit 1; }
	mkdir -p "$D/stage"
	docker pull -q "$ALPINE_IMG" >/dev/null
	echo "adding to $IMG (copy: $IMG.part)"
	rm -f "$IMG.part"
	cp --reflink=auto --sparse=always "$IMG" "$IMG.part"
	dk --privileged -v "$ROOT:/work:ro" -v "$D:/out" -e REL="$REL" -e MIRROR="$MIRROR" -e PKGS="$PKGS" -e LREPO="$LREPO" -e LKEY="$LKEY" -e LPKGS="$LPKGS" \
		-e IMGREL="$IMGREL" -e MODE=update -e MODSRC="$modsrc" -e ONLY_MODULES="${ONLY_MODULES:-}" \
		-e T2_DEBUG="${T2_DEBUG:-}" -e CONFIGURE="$CONFIGURE" "$ALPINE_IMG" sh -euc '
LOOP=
# (sh -e: a failing command in the trap aborts it with exit 1, hence the "|| :")
trap "umount /build/root 2>/dev/null || :; [ -z \"\$LOOP\" ] || losetup -d \$LOOP || :; chown -R $UIDGID /out" EXIT
apk add -q --cache-dir /out/apk-cache-host e2fsprogs zstd >/dev/null
# journal/errors before the edit: the image must be clean (phone shut down properly)
e2fsck -fp /out/$IMGREL.part >/dev/null || { e=$?; [ $e -le 1 ] || { echo "e2fsck: $e" >&2; exit 1; }; }
mkdir -p /build/root
# loop device: the container /dev is a snapshot, the node of a new loopN may be missing
LOOP=$(losetup -f)
[ -b $LOOP ] || mknod $LOOP b 7 ${LOOP#/dev/loop}
losetup $LOOP /out/$IMGREL.part
mount -t ext4 $LOOP /build/root
eval "$CONFIGURE"
df -h /build/root | tail -1
umount /build/root
losetup -d $LOOP
LOOP=
sync
e2fsck -fn /out/$IMGREL.part >/tmp/fsck.log 2>&1 || { cat /tmp/fsck.log; exit 1; }
'
	[ -n "${ONLY_MODULES:-}" ] || verify_cache
	img_busy && { echo "the image started being served during the edit: the copy is left in $IMG.part" >&2; exit 1; }
	ln -f "$IMG" "$IMG.prev" 2>/dev/null || cp --reflink=auto --sparse=always "$IMG" "$IMG.prev"
	mv "$IMG.part" "$IMG"
	ls -ls "$IMG" "$IMG.prev"
}

cmd_modules() {
	[ -n "${1:-}" ] || { echo "modules <O>: kernel build directory (O= of scripts/build-mainline.sh)" >&2; exit 1; }
	local modsrc
	modsrc=$(modules_src "$1")
	ONLY_MODULES=1 img_update "$modsrc"
}

running() { [ -n "$(docker ps -q -f "name=^$CTR\$")" ]; }

cmd_serve() {
	[ -f "$IMG" ] || { echo "no $IMG: run build first" >&2; exit 1; }
	running && { echo "$CTR is already running" >&2; exit 1; }
	local t=0
	until ip -4 -o addr show | grep -q " inet $LISTEN/"; do
		[ $t = 0 ] && echo "waiting for the address $LISTEN on the host (phone over USB, DHCP from the initramfs)..."
		t=$((t + 1))
		sleep 1
	done
	echo "$LISTEN is up ($(ip -4 -o addr show | awk -v a="$LISTEN" '$4 ~ "^"a"/" {print $2}')); nbd-server $LISTEN:$PORT export $EXPORT -> $IMG"
	exec docker run --rm --name "$CTR" --network host -v "$D:/srv" -e UIDGID="$(id -u):$(id -g)" \
		-e LISTEN="$LISTEN" -e PORT="$PORT" -e EXPORT="$EXPORT" -e IMGREL="$IMGREL" "$ALPINE_IMG" sh -euc '
apk add -q --cache-dir /srv/apk-cache-host nbd >/dev/null
addgroup -g ${UIDGID#*:} t2 2>/dev/null || true
adduser -D -H -u ${UIDGID%:*} -G t2 t2 2>/dev/null || true
cat >/run/nbd.conf <<CONF
[generic]
	listenaddr = $LISTEN
	port = $PORT
	user = t2
	group = t2
[$EXPORT]
	exportname = /srv/$IMGREL
	flush = true
	fua = true
CONF
echo "nbd-server $(nbd-server -V 2>&1 | head -1)"
# no -d: nbd-server -d serves one connection and exits; here it goes into the background
# (a fork per client), and the shell waits for it and stops it on Ctrl-C/docker stop
mkdir -p /run/nbd && chown t2:t2 /run/nbd
nbd-server -C /run/nbd.conf -p /run/nbd/nbd-server.pid
sleep 1
pid=$(cat /run/nbd/nbd-server.pid)
trap "kill $pid; exit 0" INT TERM
while kill -0 $pid 2>/dev/null; do sleep 2 & wait $!; done
echo "nbd-server exited"
exit 1
'
}

cmd_fsck() {
	running && { echo "$CTR is running: turn the phone off and stop serve first" >&2; exit 1; }
	dk -v "$D:/out" -e IMGREL="$IMGREL" "$ALPINE_IMG" sh -euc '
apk add -q --cache-dir /out/apk-cache-host e2fsprogs >/dev/null
e2fsck -f -p /out/$IMGREL; echo "e2fsck: $?"; chown -R $UIDGID /out/apk-cache-host'
}

cmd_status() {
	[ -f "$IMG" ] && ls -ls "$IMG" || echo "no image: $IMG"
	[ -f "$D/stage/packages.txt" ] && echo "packages: $(wc -l <"$D/stage/packages.txt")"
	running && echo "serve: $CTR is running" || echo "serve: not running"
	ip -4 -o addr show | grep " inet $LISTEN/" || echo "no address $LISTEN on the host (phone not connected?)"
}

case "${1:-}" in
build) cmd_build ;;
modules) cmd_modules "${2:-}" ;;
serve) cmd_serve ;;
fsck) cmd_fsck ;;
status) cmd_status ;;
*) sed -n '5,15p' "$0" >&2; exit 1 ;;
esac
