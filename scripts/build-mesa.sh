#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Mesa 26.3 for the Titan 2 (Mali-G615, Valhall v11, panthor kernel driver): aarch64 apk packages
# for Alpine v3.24. Mesa 26.3.0 is the first Mesa release that supports this GPU (panfrost for
# OpenGL/OpenGL ES, panvk for Vulkan); Alpine v3.24 ships an older Mesa.
#
#   scripts/build-mesa.sh fetch    download the pinned Alpine aports of mesa and wayland-protocols
#                                  and the Mesa source archive (sha512 checked), and prepare the
#                                  APKBUILDs -> out/mesa/src, out/mesa/distfiles
#   scripts/build-mesa.sh build    fetch, then build in an alpine:3.24 arm64 container (hours under
#                                  emulation) -> out/mesa/packages/aports/aarch64 (signed apks +
#                                  APKINDEX); the Mesa packages are also copied to
#                                  out/t2-packages/aarch64, where scripts/t2-repo.sh build finds them
#   scripts/build-mesa.sh status   what has been fetched and built
#   scripts/build-mesa.sh clean    remove out/mesa except the download caches
#
# What is built: Alpine's aports main/mesa APKBUILD at APORTS_COMMIT with
#   - tools/mesa/APKBUILD.diff: only the drivers the phone uses (panfrost, panvk) plus the software
#     fallback (llvmpipe, softpipe, lavapipe); LLVM 22 of Alpine v3.24; no VA-API, no rusticl;
#   - pkgver set to MESA_VER and the archive checksum to MESA_SHA512;
#   - the patches in patches/mesa/*.patch (git format-patch files; see their headers for authors
#     and upstream commits), pkgrel = aports pkgrel + 100 + number of patches, so the packages are
#     newer than any Alpine mesa of the same version and every added patch gives a new version.
# Mesa 26.3 needs wayland-protocols >= 1.49 at build time (Alpine v3.24 has 1.48), so the aports
# main/wayland-protocols APKBUILD at WLP_COMMIT is built first. It is a build dependency only and
# stays in out/mesa/packages; it is not copied to out/t2-packages.
#
# Trust: the aports files come from gitlab.alpinelinux.org over HTTPS at pinned commits; the Mesa
# archive is checked against MESA_SHA512 (the value published in docs/relnotes/<version>.rst of
# Mesa's git repository) before the build, and abuild checks every source against the sha512sums
# of the APKBUILD again inside the container; the Alpine packages installed in the container are
# checked by apk with Alpine's keys.
#
# Signing: the packages and the index are signed with your own key out/t2-repo-keys/t2-repo.rsa,
# the same key as scripts/t2-repo.sh uses (created with "scripts/t2-repo.sh keygen" when it does
# not exist yet). Keep it: the phone trusts its public half. No key is shipped in this repository.
#
# Host requirements: docker, and either an arm64 host or qemu-user binfmt support for aarch64
# (e.g. the qemu-user-static package of your distribution, registered with the "F" flag, as
# docker --platform linux/arm64 needs). About 25 GB of free disk; under emulation the build takes
# several hours. The arm64 container image is a local image of its own, ARM_IMG (default
# titan2-alpine-arm64:3.24, made once from alpine:3.24 with docker build --platform linux/arm64):
# pulling the arm64 variant under the alpine:3.24 tag itself would replace the host's own image
# of that tag, which the other scripts run (docker rmi titan2-alpine-arm64:3.24 to refresh it).
#
# Variables: JOBS (default: nproc), PACKAGER (written into the packages, e.g.
#   PACKAGER="Your Name <you@example.org>"), ALPINE_RELEASE (default v3.24), and for a different
#   pin APORTS_COMMIT, WLP_COMMIT, MESA_VER, MESA_SHA512; NO_BINFMT_CHECK=1 skips the check for
#   a qemu-aarch64 binfmt handler (e.g. Docker Desktop, which has its own emulation).
#
# Next steps: scripts/t2-repo.sh build (one signed index in out/t2-repo), then
# LOCAL_REPO=out/t2-repo scripts/make-linux-root.sh; the image build upgrades the Mesa packages
# from LOCAL_REPO.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/mesa
KEYS=$ROOT/out/t2-repo-keys
KEY=t2-repo.rsa
PKG=$ROOT/out/t2-packages
REL=${ALPINE_RELEASE:-v3.24}
IMG=alpine:${REL#v}
ARM_IMG=${ARM_IMG:-titan2-alpine-arm64:${REL#v}}
# aports main/mesa (mesa 26.2.6-r0 of edge); its pkgver is replaced by MESA_VER
APORTS_COMMIT=${APORTS_COMMIT:-82a2f41f324a}
# aports main/wayland-protocols 1.49 (edge)
WLP_COMMIT=${WLP_COMMIT:-b474ee5904f9}
MESA_VER=${MESA_VER:-26.3.0}
MESA_SHA512=${MESA_SHA512:-b4abd266ecaf813ef078a0461bcdf7c03d2ecddeb5b6e1c51054fb0afe0d830c0f27fc78f0effe68df5ea7b48f76f4e6ca36ed9afe98513f0e4bf8e0bbbe96dd}
MESA_URL=https://mesa.freedesktop.org/archive/mesa-$MESA_VER.tar.xz

die() { echo "build-mesa.sh: $*" >&2; exit 1; }

# aports main/<package> at commit <c> -> $OUT/src/aports-<package>
fetch_aport() {
	local pkg=$1 c=$2 d=$OUT/src/aports-$1 f
	local api="https://gitlab.alpinelinux.org/api/v4/projects/alpine%2Faports/repository"
	rm -rf "$d" && mkdir -p "$d"
	curl -sfL "$api/tree?path=main/$pkg&ref=$c&per_page=100" -o "$d/.tree.json" ||
		die "cannot list aports main/$pkg at $c"
	for f in $(grep -o '"name":"[^"]*"' "$d/.tree.json" | cut -d'"' -f4); do
		case "$f" in */* | .*) die "aports main/$pkg: unexpected file name $f" ;; esac
		curl -sfL -o "$d/$f" "https://gitlab.alpinelinux.org/alpine/aports/-/raw/$c/main/$pkg/$f" ||
			die "cannot download aports main/$pkg/$f at $c"
	done
	rm -f "$d/.tree.json"
	[ -s "$d/APKBUILD" ] || die "aports main/$pkg at $c: no APKBUILD"
	echo "$c" >"$d/.aports-commit"
}

# append our patches to source= and their sha512 to sha512sums= of the APKBUILD in $1
add_patches() {
	local d=$1 list sums p
	list=$(cd "$ROOT/patches/mesa" && ls ./*.patch | sed 's|^\./||')
	cp "$ROOT"/patches/mesa/*.patch "$d/"
	sums=$(cd "$d" && for p in $list; do sha512sum "$p"; done)
	awk -v list="$list" -v sums="$sums" '
		/^source="/ { insrc = 1 }
		insrc && /"$/ { n = split(list, l, "\n"); s = ""; for (i = 1; i <= n; i++) s = s "\t" l[i] "\n"
			sub(/"$/, ""); if ($0 !~ /^[ \t]*$/) print; printf "%s\"\n", s; insrc = 0; next }
		/^sha512sums="/ { insum = 1 }
		insum && /^"$/ { print sums; insum = 0 }
		{ print }' "$d/APKBUILD" >"$d/APKBUILD.new"
	mv "$d/APKBUILD.new" "$d/APKBUILD"
	for p in $list; do
		grep -q "^	$p\$" "$d/APKBUILD" && grep -q "^[0-9a-f]\{128\}  $p\$" "$d/APKBUILD" ||
			die "APKBUILD: could not add $p"
	done
}

cmd_fetch() {
	local d=$OUT/src/aports-mesa old rel n tmp
	command -v curl >/dev/null || die "needs curl"
	mkdir -p "$OUT/distfiles"
	fetch_aport mesa "$APORTS_COMMIT"
	patch -s -d "$d" -p0 --no-backup-if-mismatch <"$ROOT/tools/mesa/APKBUILD.diff" ||
		die "tools/mesa/APKBUILD.diff does not apply to aports main/mesa at $APORTS_COMMIT"
	# Mesa 26.3.0 already contains the LLVM 23 fix of aports; APKBUILD.diff drops that patch
	rm -f "$d/llvm23.patch"
	! grep -q llvm23.patch "$d/APKBUILD" || die "APKBUILD still lists llvm23.patch"
	# version and archive checksum
	[ ${#MESA_SHA512} -eq 128 ] || die "MESA_SHA512 is not a sha512"
	old=$(sed -n 's/^pkgver=//p' "$d/APKBUILD")
	sed -i -e "s/^pkgver=$old\$/pkgver=$MESA_VER/" \
		-e "s/^[0-9a-f]\{128\}  mesa-$old.tar.xz\$/$MESA_SHA512  mesa-$MESA_VER.tar.xz/" "$d/APKBUILD"
	grep -q "^pkgver=$MESA_VER\$" "$d/APKBUILD" && grep -q "^$MESA_SHA512  mesa-$MESA_VER.tar.xz\$" "$d/APKBUILD" ||
		die "could not set mesa $MESA_VER in the APKBUILD (aports had $old)"
	# our patches and pkgrel
	ls "$ROOT"/patches/mesa/*.patch >/dev/null 2>&1 || die "no patches/mesa/*.patch"
	n=$(ls "$ROOT"/patches/mesa/*.patch | wc -l)
	rel=$(sed -n 's/^pkgrel=//p' "$d/APKBUILD")
	sed -i "s/^pkgrel=$rel\$/pkgrel=$((rel + 100 + n))/" "$d/APKBUILD"
	add_patches "$d"
	# the Mesa archive, checked on the host (abuild checks it again)
	if ! echo "$MESA_SHA512  $OUT/distfiles/mesa-$MESA_VER.tar.xz" | sha512sum -c --quiet - 2>/dev/null; then
		tmp=$OUT/distfiles/mesa-$MESA_VER.tar.xz.part
		curl -fL --retry 3 -o "$tmp" "$MESA_URL" || die "cannot download $MESA_URL"
		echo "$MESA_SHA512  $tmp" | sha512sum -c --quiet - || { rm -f "$tmp"; die "mesa-$MESA_VER.tar.xz: sha512 mismatch"; }
		mv "$tmp" "$OUT/distfiles/mesa-$MESA_VER.tar.xz"
	fi
	# wayland-protocols >= 1.49; its test suite is not run (build dependency only)
	fetch_aport wayland-protocols "$WLP_COMMIT"
	d=$OUT/src/aports-wayland-protocols
	grep -q '^pkgver=1\.\(49\|[5-9][0-9]\)$' "$d/APKBUILD" || die "aports wayland-protocols at $WLP_COMMIT is not >= 1.49"
	grep -q '^options=.*!check' "$d/APKBUILD" ||
		sed -i 's/^makedepends=.*$/&\noptions="!check"/' "$d/APKBUILD"
	echo "mesa $(sed -n 's/^pkgver=//p;' "$OUT/src/aports-mesa/APKBUILD")-r$(sed -n 's/^pkgrel=//p' "$OUT/src/aports-mesa/APKBUILD") (aports $APORTS_COMMIT, $n patches)"
	echo "wayland-protocols $(sed -n 's/^pkgver=//p' "$d/APKBUILD") (aports $WLP_COMMIT)"
	echo "mesa-$MESA_VER.tar.xz: sha512 OK"
}

cmd_build() {
	command -v docker >/dev/null || die "needs docker"
	# checked before the arm64 image $ARM_IMG is made: without emulation it could not run
	# (NO_BINFMT_CHECK=1 skips this, e.g. for Docker Desktop, which brings its own emulation)
	[ -n "${NO_BINFMT_CHECK:-}" ] || [ "$(uname -m)" = aarch64 ] ||
		grep -qs '^enabled' /proc/sys/fs/binfmt_misc/qemu-aarch64 ||
		die "no qemu-aarch64 binfmt handler on this host (install qemu-user-static, or use an arm64 host)"
	docker image inspect "$ARM_IMG" >/dev/null 2>&1 ||
		printf 'FROM %s\n' "$IMG" | docker build -q --platform linux/arm64 -t "$ARM_IMG" - >/dev/null ||
		die "cannot make the arm64 image $ARM_IMG from $IMG"
	docker run --rm --platform linux/arm64 "$ARM_IMG" true 2>/dev/null ||
		die "cannot run arm64 containers: the host needs qemu-user binfmt support for aarch64 (or an arm64 host)"
	[ -f "$KEYS/$KEY" ] || "$ROOT/scripts/t2-repo.sh" keygen
	[ -f "$KEYS/$KEY.pub" ] || die "no $KEYS/$KEY.pub"
	cmd_fetch
	mkdir -p "$OUT/packages" "$OUT/apk-cache"
	# apk inside an emulated container sometimes loses a download ("I/O error"); the apk cache
	# keeps what was already fetched, and the whole build is retried up to three times
	local try rc
	for try in 1 2 3; do
		rc=0
		docker run --rm --platform linux/arm64 \
			-v "$ROOT/tools/mesa:/tools:ro" -v "$KEYS:/keys:ro" \
			-v "$OUT/src:/src:ro" -v "$OUT/distfiles:/distfiles" -v "$OUT/apk-cache:/apk-cache" \
			-v "$OUT/packages:/packages" \
			-e UIDGID="$(id -u):$(id -g)" -e KEY="$KEY" -e JOBS="${JOBS:-$(nproc)}" \
			-e PACKAGER="${PACKAGER:-}" \
			"$ARM_IMG" sh /tools/inner.sh wayland-protocols mesa 2>&1 | tee "$OUT/build.log" || rc=$?
		[ "$rc" -ne 0 ] && grep -q 'I/O error' "$OUT/build.log" || break
		echo "attempt $try: apk download error, retrying" >&2
	done
	[ "$rc" -eq 0 ] || die "build failed (log: out/mesa/build.log)"
	# the Mesa packages -> out/t2-packages/aarch64 (replacing older Mesa builds there)
	mkdir -p "$PKG/aarch64"
	rm -f "$PKG"/aarch64/mesa-*.apk
	cp "$OUT"/packages/aports/aarch64/mesa-*.apk "$PKG/aarch64/"
	ls -l "$OUT"/packages/aports/aarch64/
	echo "Mesa packages copied to out/t2-packages/aarch64; next: scripts/t2-repo.sh build"
}

cmd_status() {
	local d=$OUT/src/aports-mesa
	if [ -f "$d/APKBUILD" ]; then
		echo "fetched: mesa $(sed -n 's/^pkgver=//p' "$d/APKBUILD")-r$(sed -n 's/^pkgrel=//p' "$d/APKBUILD") (aports $(cat "$d/.aports-commit"))"
	else
		echo "not fetched"
	fi
	find "$OUT/packages" -name '*.apk' 2>/dev/null | sort
}

case "${1:-}" in
fetch) cmd_fetch ;;
build) cmd_build ;;
status) cmd_status ;;
clean) rm -rf "$OUT/src" "$OUT/packages" "$OUT/build.log" ;;
*) sed -n '5,17p' "$0"; exit 2 ;;
esac
