#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Inner part of scripts/build-atl.sh and scripts/atl-vm.sh: runs in an alpine:3.24 (x86_64)
# container. Mounts: /work = the repository (ro), /out = out/atl (or out/atl-vm in the vm-*
# modes), /dl = out/atl-dl, /atl = out/atl (ro, vm-* modes only).
#
# aarch64 runs through qemu-aarch64 (static-pie from Alpine's qemu-aarch64 package) and a
# binfmt_misc entry registered in the OWN binfmt_misc instance of a new user namespace
# (kernel >= 6.7: binfmt_misc is bound to the user namespace). The host's global
# /proc/sys/fs/binfmt_misc is not touched: the entry lives as long as the namespace and only its
# processes see it. uid 0 in the namespace = the host user's uid (HOST_UID), 1..65535 -> 100000..,
# so the rootfs in out/atl/rootfs belongs to the user, not to the host's root.
set -eu
set -o pipefail
MODE=${1:?mode}
shift || true
REL=${ALPINE_RELEASE:-v3.24}
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
R=/out/rootfs
QEMU=/usr/bin/qemu-aarch64

if [ -z "${T2_IN_NS:-}" ]; then
	apk add -q --cache-dir /dl/apk-cache-x86 qemu-aarch64 util-linux-misc python3 openssl e2fsprogs >/dev/null
	exec env T2_IN_NS=1 unshare --user --mount \
		--map-users=0:"${HOST_UID:?}":1 --map-users=1:100000:65535 \
		--map-groups=0:"${HOST_GID:?}":1 --map-groups=1:100000:65535 \
		--setuid=0 --setgid=0 sh "$0" "$MODE" "$@"
fi

# --- private binfmt_misc ---
mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
# flags: F = open the interpreter now (works inside the chroot), O = open the binary by its
# original path, C = credentials from the binary (no suid is used, but this is the honest choice)
printf '%s' ':qemu-aarch64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:'"$QEMU"':FOC' \
	>/proc/sys/fs/binfmt_misc/register

mounts() {
	local r=${1:-$R}
	mkdir -p $r/proc $r/dev $r/tmp $r/run $r/sys $r/etc
	mount --rbind /proc $r/proc
	mount --rbind /dev $r/dev
	mount -t tmpfs -o mode=1777 tmpfs $r/tmp
	mount -t tmpfs -o mode=755 tmpfs $r/run
	mkdir -p $r/dev/shm 2>/dev/null || true
	mount -t tmpfs -o mode=1777 tmpfs $r/dev/shm
	cp /etc/resolv.conf $r/etc/resolv.conf
}

# --- packages of the test/target rootfs ---
# ATL and its dependencies are only in edge/testing (vixl, libandroidfw, art_standalone,
# bionic_translation, libopensles-standalone, android-translation-layer); every so: dependency
# of these packages is satisfied by v3.24 main/community (checked against the APKINDEX 2026-10-20).
ATL_PKGS="android-translation-layer@testing art_standalone@testing bionic_translation@testing
libopensles-standalone@testing libandroidfw@testing vixl@testing"
BASE_PKGS="alpine-baselayout alpine-release busybox busybox-binsh musl-utils apk-tools ca-certificates"
GFX_PKGS="mesa-dri-gallium mesa-egl mesa-gles font-dejavu font-noto dbus dbus-x11 cage grim wlr-randr
sway foot fuzzel adwaita-icon-theme hicolor-icon-theme xkeyboard-config shared-mime-info"

# Titan 2 overlay (APK start, sway, keymap): a fresh copy on every entry
sync_overlay() {
	cp -a /work/tools/atl/overlay/. $R/
	chmod 755 $R/usr/local/bin/*
}

cmd_rootfs() {
	mkdir -p $R/etc/apk/keys /dl/apk-cache
	cp /work/tools/initramfs/keys/*.rsa.pub $R/etc/apk/keys/
	printf '%s\n' "$MIRROR/$REL/main" "$MIRROR/$REL/community" "@testing $MIRROR/edge/testing" \
		>$R/etc/apk/repositories
	# package scripts run (chroot + qemu through the private binfmt)
	mounts
	apk add --root $R --arch aarch64 --initdb --no-interactive --keys-dir etc/apk/keys \
		--repositories-file $R/etc/apk/repositories --cache-dir /dl/apk-cache \
		$BASE_PKGS $GFX_PKGS $ATL_PKGS ${EXTRA_PKGS:-}
	sed -n '/^P:/{s/^P://;h};/^V:/{s/^V://;H;x;s/\n/ /p}' $R/lib/apk/db/installed >/out/packages.txt
	# a second check of every package, independent of apk-tools (index signature, sha1 control, sha256 data)
	python3 -I /work/tools/netroot/verify-cache.py /dl/apk-cache /out/packages.txt
	# the ATL packages separately, for the phone's rootfs
	rm -rf /out/apks && mkdir -p /out/apks
	for p in android-translation-layer art_standalone bionic_translation libopensles-standalone libandroidfw vixl; do
		v=$(awk -v p="$p" '$1==p{print $2}' /out/packages.txt)
		cp /dl/apk-cache/"$p-$v".*.apk /out/apks/"$p-$v.apk"
	done
	(cd /out/apks && sha256sum *.apk >SHA256SUMS)
	sync_overlay
	chroot $R /bin/sh -c 'echo "chroot aarch64: $(uname -m), $(cat /etc/alpine-release)"'
	du -sh $R | sed 's/^/rootfs: /'
}

cmd_shell() {
	mounts
	sync_overlay
	chroot $R /usr/bin/env -i HOME=/root TERM="${TERM:-xterm}" PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin /bin/sh -l "$@"
}

cmd_test() {
	mounts
	sync_overlay
	mkdir -p $R/root/apks $R/root/out
	mount --bind /dl/test-apks $R/root/apks
	mount --bind /out/shots $R/root/out
	cp /work/tools/atl/test-session.sh $R/root/test-session.sh
	chroot $R /usr/bin/env -i HOME=/root PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
		TIMEOUT="${TIMEOUT:-900}" SHOTS="${SHOTS:-60}" GSK="${GSK:-}" ATL_LOG="${ATL_LOG:-*:i}" \
		/bin/sh /root/test-session.sh "$@"
}

# --- rebuild from aports for v3.24 (emulated: slow) ---
# /dl/src/aports-<pkg>/APKBUILD (fetched by build-atl.sh from aports at the pinned commit), our
# patches patches/<pkg>/*.patch are added to source= (the checksums are recomputed only after
# abuild verify of the original APKBUILD: the upstream archive matched the sha512 from aports).
# The packages are signed with the local key out/atl/build-keys (on the phone: add its .pub to
# /etc/apk/keys); result: /out/packages/<arch-repo>/aarch64/*.apk + APKINDEX.
cmd_build() {
	local B=/out/buildroot p d
	mkdir -p $B/etc/apk/keys /out/build-keys /out/packages
	cp /work/tools/initramfs/keys/*.rsa.pub $B/etc/apk/keys/
	# edge/testing without a tag: it has none of the main/community packages, only the missing -dev
	printf '%s\n' "$MIRROR/$REL/main" "$MIRROR/$REL/community" "$MIRROR/edge/testing" \
		>$B/etc/apk/repositories
	mounts $B
	apk add --root $B --arch aarch64 --initdb --no-interactive --keys-dir etc/apk/keys \
		--repositories-file $B/etc/apk/repositories --cache-dir /dl/apk-cache \
		alpine-base alpine-sdk sudo
	mkdir -p $B/keys
	mount --bind /out/build-keys $B/keys
	mkdir -p $B/home/build/packages
	mount --bind /out/packages $B/home/build/packages
	if ! ls /out/build-keys/*.rsa >/dev/null 2>&1; then
		chroot $B sh -c 'cd /keys && openssl genrsa -out titan2-atl.rsa 4096 2>/dev/null &&
			openssl rsa -in titan2-atl.rsa -pubout -out titan2-atl.rsa.pub 2>/dev/null'
	fi
	cp /out/build-keys/*.rsa.pub $B/etc/apk/keys/
	chroot $B apk update -q
	mkdir -p $B/root/.abuild
	printf 'PACKAGER="serlk <22211586+Serlk@users.noreply.github.com>"\nPACKAGER_PRIVKEY=/keys/titan2-atl.rsa\nREPODEST=/home/build/packages\nJOBS=%s\nMAKEFLAGS=-j%s\n' \
		"$(nproc)" "$(nproc)" >$B/root/.abuild/abuild.conf
	for p in "$@"; do
		d=$B/home/build/aports/$p
		rm -rf "$d" && mkdir -p "$d"
		cp /dl/src/aports-$p/* "$d/"
		# the original APKBUILD: fetch and verify the upstream archive
		chroot $B sh -c "cd /home/build/aports/$p && abuild -F fetch verify"
		if [ -n "${ATL_COMMIT:-}" ] && [ "$p" = android-translation-layer ]; then
			# fresh upstream: trust = HTTPS to gitlab.com (ATL has no signatures)
			sed -i "s/^_commit=.*/_commit=\"$ATL_COMMIT\"/; s/^pkgver=.*/pkgver=0_git$(date -u +%Y%m%d)/" "$d/APKBUILD"
			# tools/jni_upcall_gen (ATL since 2026-10-25) builds on asm/asm-tree/java-getopt: the jars
			# from /dl/jars (build-atl.sh cmd_jars, sha256 checked) are passed to meson; build.py needs python3;
			# libsecret — AccountManager (HEAD 2026-11-03)
			mkdir -p $B/jars && mount --bind /dl/jars $B/jars
			jar_opts=$(cd /dl/jars && printf -- "-Dasm_jar_path=/jars/%s -Dasm_tree_jar_path=/jars/%s -Dgetopt_jar_path=/jars/%s" asm-[0-9]*.jar asm-tree-*.jar java-getopt-*.jar)
			sed -i -e "/abuild-meson/s|abuild-meson|abuild-meson $jar_opts|" \
				-e 's/^\tmeson$/\tmeson\n\tpython3\n\tlibsecret-dev/' "$d/APKBUILD"
		fi
		if ls /work/patches/$p/*.patch >/dev/null 2>&1; then
			cp /work/patches/$p/*.patch "$d/"
			# pkgrel +100+N (N = number of our patches): the Titan 2 package is newer than the one of the
			# same name in edge/testing (apk picks it), and each new patch gives a new version; otherwise apk
			# takes the rebuilt package for "already installed" and does not reinstall it
			npatch=$(ls /work/patches/$p/*.patch | wc -l)
			awk -v n="$npatch" '/^pkgrel=/ {sub(/=.*/, "=" substr($0, 8) + 100 + n)} {print}' "$d/APKBUILD" >"$d/APKBUILD.new" &&
				mv "$d/APKBUILD.new" "$d/APKBUILD"
			# add our patches to source= (before the closing quote; the aports' own patches are there already)
			awk -v list="$(cd /work/patches/$p && ls *.patch | tr '\n' ' ')" '
				/^source="/ {insrc=1}
				insrc && /"$/ {sub(/"$/, "\n\t" list "\""); insrc=0}
				{print}' "$d/APKBUILD" >"$d/APKBUILD.new" && mv "$d/APKBUILD.new" "$d/APKBUILD"
		fi
		# MAKEDEPENDS_PIN="name=version ...": build against exactly these versions (our own rebuilds in
		# out/atl/packages) instead of whatever is newest in edge/testing
		for pin in ${MAKEDEPENDS_PIN:-}; do
			sed -i "s|^\([[:space:]]*\)${pin%%=*}\$|\1$pin|" "$d/APKBUILD"
			grep -q "^[[:space:]]*$pin\$" "$d/APKBUILD" || echo "MAKEDEPENDS_PIN: ${pin%%=*} is not a makedepends line of $p" >&2
		done
		chroot $B sh -c "cd /home/build/aports/$p && abuild -F checksum && abuild -F -r -c -P /home/build/packages" \
			2>&1 | tee /out/build-$p.log
	done
	find /out/packages -name '*.apk' | sort
}

# --- built packages (out/packages, key out/build-keys) -> test rootfs ---
cmd_local() {
	mounts
	cp /out/build-keys/*.rsa.pub $R/etc/apk/keys/
	mkdir -p $R/var/local-pkgs
	mount --bind /out/packages $R/var/local-pkgs
	grep -qx /var/local-pkgs/aports $R/etc/apk/repositories ||
		echo /var/local-pkgs/aports >>$R/etc/apk/repositories
	chroot $R apk update -q
	[ $# -gt 0 ] || set -- android-translation-layer
	chroot $R apk add --upgrade "$@"
	chroot $R apk info -v android-translation-layer
}

# --- VM (scripts/atl-vm.sh): guest rootfs and disk image ---
# Mounts in these modes: /out = out/atl-vm, /dl = out/atl-dl, /atl = out/atl (ro: our built
# packages and key), /work = the repository. Guest: kernel linux-virt (virtio-gpu, 9p),
# niri + seatd + eudev, the ATL stack (edge/testing with out/atl/packages on top), the Titan 2
# overlay and the guest part of the bench, tools/atl/vm/guest.
VM_PKGS="alpine-base linux-virt eudev seatd dbus dbus-x11 e2fsprogs
niri rofi-wayland foot grim wtype swaybg xkeyboard-config xkbcli evtest python3
mesa-dri-gallium mesa-egl mesa-gles mesa-gbm mesa-vulkan-virtio mesa-vulkan-swrast vulkan-loader vulkan-tools
mesa-utils wayland-utils font-dejavu font-noto adwaita-icon-theme hicolor-icon-theme shared-mime-info
alsa-utils alsa-lib"

cmd_vm_rootfs() {
	mkdir -p $R/etc/apk/keys /dl/apk-cache $R/boot
	cp /work/tools/initramfs/keys/*.rsa.pub $R/etc/apk/keys/
	cp /atl/build-keys/*.rsa.pub $R/etc/apk/keys/ 2>/dev/null || true
	printf '%s\n' "$MIRROR/$REL/main" "$MIRROR/$REL/community" "@testing $MIRROR/edge/testing" \
		>$R/etc/apk/repositories
	# our rebuilt packages (out/atl/packages/aports, pkgrel+100), seen by apk at the container path
	local local_repo=
	[ ! -d /atl/packages/aports ] || local_repo=/atl/packages/aports
	mounts
	apk add --root $R --arch aarch64 --initdb --no-interactive --keys-dir etc/apk/keys \
		--repositories-file $R/etc/apk/repositories ${local_repo:+--repository $local_repo} \
		--cache-dir /dl/apk-cache $VM_PKGS $ATL_PKGS ${EXTRA_PKGS:-}
	sed -n '/^P:/{s/^P://;h};/^V:/{s/^V://;H;x;s/\n/ /p}' $R/lib/apk/db/installed >/out/packages.txt
	# our r1xx ATL package is not in Alpine's signed indexes: one "error" is expected
	python3 -I /work/tools/netroot/verify-cache.py /dl/apk-cache /out/packages.txt || true
	vm_configure
	du -sh $R/usr $R/lib $R/boot 2>/dev/null | sed 's/^/vm rootfs: /'
}

# guest configuration (idempotent; also run on update)
vm_configure() {
	sync_overlay
	cp -a /work/tools/atl/vm/guest/. $R/
	chmod 755 $R/etc/init.d/atl-vm $R/usr/local/bin/atl-vm-*
	# session user: atl (1000), groups for seatd/DRM/input/sound
	chroot $R sh -c 'id atl >/dev/null 2>&1 || adduser -D -u 1000 atl
		for g in seat video input audio; do addgroup atl $g 2>/dev/null || true; done
		passwd -d root >/dev/null; passwd -d atl >/dev/null
		rc-update -q add devfs sysinit; rc-update -q add dmesg sysinit
		rc-update -q add udev sysinit; rc-update -q add udev-trigger sysinit; rc-update -q add udev-settle sysinit
		rc-update -q add hostname boot; rc-update -q add bootmisc boot; rc-update -q add sysctl boot
		rc-update -q add dbus default; rc-update -q add seatd default; rc-update -q add atl-vm default
		rc-update -q add killprocs shutdown; rc-update -q add mount-ro shutdown'
	echo titan2-vm >$R/etc/hostname
	# VM console: root shell without a password on ttyAMA0 (local bench)
	grep -q '^ttyAMA0' $R/etc/inittab || echo 'ttyAMA0::respawn:/sbin/getty -L -n -l /bin/sh 115200 ttyAMA0 vt100' >>$R/etc/inittab
	mkdir -p $R/home/atl/.config/niri $R/usr/share/titan2/apks
	cp /work/tools/atl/vm/guest-niri.kdl $R/home/atl/.config/niri/config.kdl
	cp /dl/test-apks/*.apk $R/usr/share/titan2/apks/ 2>/dev/null || true
	chroot $R chown -R atl:atl /home/atl
	# kernel and initramfs go outside for -kernel/-initrd (the ext4 image boots without a boot loader)
	mkdir -p /out/boot
	cp $R/boot/vmlinuz-virt $R/boot/initramfs-virt /out/boot/
	cp $R/boot/config-*-virt /out/boot/config-virt 2>/dev/null || true
}

# update the guest files [+ packages; our repository out/atl/packages/aports is seen in the chroot
# as /var/local-pkgs/aports]
cmd_vm_update() {
	mounts
	vm_configure
	mkdir -p $R/var/local-pkgs
	mount --bind /atl/packages $R/var/local-pkgs
	# the same version after a rebuild (pkgrel+100 does not change): add installs or downgrades to
	# the wanted version, fix --reinstall reinstalls it from the local repository by its new checksum
	# (no del: apk del does not take a name with a version, and without one the dependants hold it)
	[ $# -eq 0 ] || {
		local names= a
		for a; do names="$names ${a%%=*}"; done
		chroot $R apk add --no-interactive --repository /var/local-pkgs/aports "$@"
		chroot $R apk fix --no-interactive --reinstall --repository /var/local-pkgs/aports $names
	}
	umount $R/var/local-pkgs
}

# ext4 image from the rootfs: mke2fs -d inside the user namespace -> owners 0/1000.. (as the guest sees them)
cmd_vm_image() {
	local size=${VM_SIZE:-6G}
	rm -f /out/disk.img
	truncate -s "$size" /out/disk.img
	mke2fs -q -t ext4 -L titan2-vm -d $R -E root_owner=0:0 /out/disk.img
	ls -la /out/disk.img
}

case "$MODE" in
rootfs) cmd_rootfs ;;
local) cmd_local "$@" ;;
build) cmd_build "$@" ;;
shell) cmd_shell "$@" ;;
test) cmd_test "$@" ;;
vm-rootfs) cmd_vm_rootfs ;;
vm-update) cmd_vm_update "$@" ;;
vm-image) cmd_vm_image ;;
*) echo "inner.sh: unknown mode $MODE" >&2; exit 2 ;;
esac
