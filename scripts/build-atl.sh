#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Android Translation Layer (ATL) for mainline Titan 2 (Alpine aarch64): build pipeline on an
# x86 host.
#
#   scripts/build-atl.sh rootfs          aarch64 rootfs out/atl/rootfs: Alpine v3.24 + ATL from
#                                        edge/testing + cage/sway/grim/mesa; ATL packages -> out/atl/apks
#   scripts/build-atl.sh apks            test APKs -> out/atl-dl/test-apks (hashes checked)
#   scripts/build-atl.sh test APK [-- ATL arguments]
#                                        run in the rootfs: cage headless 1440x1440 + llvmpipe,
#                                        grim screenshots -> out/atl/shots (TIMEOUT=900 SHOTS=60 GSK=cairo|gl)
#   scripts/build-atl.sh shell [-c COMMAND] aarch64 shell in out/atl/rootfs
#   scripts/build-atl.sh build [pkg|DIR..]
#                                        rebuild from the aports APKBUILD (+ our patches, below) for
#                                        v3.24 under emulation -> out/atl/packages/aports/aarch64
#                                        (slow; default: android-translation-layer). A package with
#                                        its own recipe in tools/aports/<pkg> (titan2-atl-tools,
#                                        titan2-sensors, titan2-vendor-runtime, ...) is built from that
#                                        recipe; DIR (a path with "/") is any directory with an
#                                        APKBUILD, e.g. ../titan2-gnss/aports/titan2-gnss. The packages
#                                        are built in the order given; each one sees the packages built
#                                        before it (out/atl/packages/aports is a local repository), so
#                                        give dependencies first.
#                                        Our patches of an Alpine package: patches/<pkg>/*.patch, or
#                                        tools/titan2-vendor-runtime/patches/<pkg>/*.patch
#                                        (bionic_translation); pkgrel becomes 100 + its own + the number
#                                        of patches.
#                                        MAKEDEPENDS_PIN="bionic_translation-dev=0_git20260317-r108": build
#                                        against exactly this version of a dependency (our rebuild, not edge)
#   scripts/build-atl.sh local [pkg..]   install what was built (out/atl/packages) into out/atl/rootfs
#   scripts/build-atl.sh status | clean
#
# Host: nothing is changed. Needs docker and a kernel >= 6.7 (binfmt_misc in a user namespace).
# The alpine:3.24 container runs with --cap-add SYS_ADMIN and without a seccomp/apparmor profile
# (needed for unshare(CLONE_NEWUSER) and mount inside); qemu-aarch64 is registered in the private
# binfmt_misc of that user namespace (tools/atl/inner.sh), the host's global binfmt is not touched.
#
# Trust: apk-tools installs the Alpine packages with --keys-dir holding only Alpine's aarch64 key
# (tools/initramfs/keys), then tools/netroot/verify-cache.py checks every package in the cache.
# Test APKs: atl_test_apks (ATL on GitLab), checked by the git blob sha1 from the GitLab API;
# F-Droid: the sha256 from index-v1.jar, whose signature tools/atl/fdroid-verify.py checks
# (the F-Droid repository certificate by its known fingerprint). No APK is part of this repository.
# The packages are signed with the key of the Titan 2 apk repository, KEYS=out/t2-repo-keys
# (t2-repo.rsa; made by scripts/t2-repo.sh keygen when it does not exist yet, KEY=<file name> for
# another one), so scripts/t2-repo.sh build takes them as they are (it indexes
# out/atl/packages/aports/aarch64 by default). PACKAGER="Name <address>" goes into the packages
# (default "titan2 builder <builder@localhost>").
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/atl
DL=$ROOT/out/atl-dl
REL=${ALPINE_RELEASE:-v3.24}
IMG=alpine:${REL#v}
PYIMG=titan2-atl-py:${REL#v}  # alpine + python3 + openssl (local image)
KEYS=$(realpath -m "${KEYS:-$ROOT/out/t2-repo-keys}")
KEY=${KEY:-t2-repo.rsa}
mkdir -p "$OUT/shots" "$DL/apk-cache-x86" "$DL/apk-cache" "$DL/test-apks"

dk() {
	docker run --rm ${DK_TTY:-} --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
		--cap-add SYS_ADMIN -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
		-e ALPINE_RELEASE="$REL" -e EXTRA_PKGS="${EXTRA_PKGS:-}" -e TIMEOUT="${TIMEOUT:-900}" \
		-e SHOTS="${SHOTS:-60}" -e GSK="${GSK:-}" -e ATL_COMMIT="${ATL_COMMIT:-}" -e MAKEDEPENDS_PIN="${MAKEDEPENDS_PIN:-}" -e ATL_LOG="${ATL_LOG:-}" -e TERM="${TERM:-xterm}" \
		-e KEY="$KEY" -e PACKAGER="${PACKAGER:-titan2 builder <builder@localhost>}" \
		${KEYS_MOUNT:+-v "$KEYS:/keys:ro"} \
		-v "$ROOT:/work:ro" -v "$OUT:/out" -v "$DL:/dl" "$IMG" sh /work/tools/atl/inner.sh "$@"
}

# the signing key for "build" (scripts/t2-repo.sh keygen makes it when it does not exist)
need_key() {
	if [ ! -f "$KEYS/$KEY" ]; then
		[ "$KEYS" = "$ROOT/out/t2-repo-keys" ] && [ "$KEY" = t2-repo.rsa ] ||
			{ echo "no signing key $KEYS/$KEY" >&2; exit 1; }
		"$ROOT/scripts/t2-repo.sh" keygen
	fi
	[ -f "$KEYS/$KEY.pub" ] || { echo "no $KEYS/$KEY.pub" >&2; exit 1; }
	KEYS_MOUNT=1
}

# the host's python3 is not used: the checks run in the container (python3 -I, /work read-only)
py() {
	docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/work:ro" -v "$DL:/dl" "$PYIMG" python3 -I "$@"
}

py_image() {
	docker image inspect "$PYIMG" >/dev/null 2>&1 || printf "FROM %s\nRUN apk add --no-cache python3 openssl\n" "$IMG" |
		docker build -q -t "$PYIMG" - >/dev/null
}

need_kernel() {
	local v
	v=$(uname -r | awk -F. '{print $1*100+$2}')
	[ "$v" -ge 607 ] || { echo "host kernel $(uname -r) < 6.7: no binfmt_misc in a user namespace" >&2; exit 1; }
}

# --- test APKs ---
# atl_test_apks: file name in that repository -> checked by its blob id (git sha1) from the GitLab API
GL_APKS="org.happysanta.gd_29.apk gles3jni.apk demo_app.apk"
# F-Droid: packages (newest version from the signed index)
FDROID_APKS="org.billthefarmer.editor org.fdroid.fdroid"

cmd_apks() {
	py_image
	local api="https://gitlab.com/api/v4/projects/android_translation_layer%2Fatl_test_apks/repository"
	local tree="$DL/test-apks/atl_test_apks.tree.json" f
	curl -sfL -o "$tree" "$api/tree?per_page=100"
	for f in $GL_APKS; do
		[ -s "$DL/test-apks/$f" ] || curl -sfL -o "$DL/test-apks/$f" \
			"https://gitlab.com/android_translation_layer/atl_test_apks/-/raw/master/$f"
		py /work/tools/atl/gl-verify.py /dl/test-apks/atl_test_apks.tree.json /dl/test-apks/$f
	done
	local jar="$DL/test-apks/fdroid-index-v1.jar"
	curl -sfL -o "$jar" https://f-droid.org/repo/index-v1.jar
	local p
	for p in $FDROID_APKS; do
		py /work/tools/atl/fdroid-verify.py /dl/test-apks/fdroid-index-v1.jar "$p" /dl/test-apks
	done
}

# jar dependencies of ATL's generator tools/jni_upcall_gen (ATL since 2026-10-25): Alpine does not
# package them (no xmvn); fetched from Maven Central and checked against the sha256 pinned here
# (2026-11-04)
JARS="org/ow2/asm/asm/9.11.1/asm-9.11.1.jar ae4e6e10d59d0a1500317764b21881ad883f8550e33c66e6142f7032c6fb566c
org/ow2/asm/asm-tree/9.11.1/asm-tree-9.11.1.jar 2b15c3c1b288a9fad3bb2cd6d988ba9521554914ed1f2a2029141fa3f483154a
gnu/getopt/java-getopt/1.0.13/java-getopt-1.0.13.jar f09dc6369ffc4fe4362cbd49b6ac6c4a7471375ac5d884c71b0a625a889c38c4"

cmd_jars() {
	mkdir -p "$DL/jars"
	printf '%s\n' "$JARS" | while read -r path sum; do
		f=$DL/jars/$(basename "$path")
		[ -s "$f" ] || curl -sfL -o "$f" "https://repo1.maven.org/maven2/$path"
		echo "$sum  $f" | sha256sum -c - >/dev/null || { echo "sha256 mismatch: $f" >&2; exit 1; }
	done
	ls "$DL/jars"/*.jar | xargs -n1 basename | tr '\n' ' '; echo
}

# aports: the testing/<pkg> directories at the commits the edge/testing binary packages were built
# from (field c: of the APKINDEX, 2026-10-20): the rebuild reproduces them + our patches/<pkg>
declare -A APORTS_COMMIT=(
	[android-translation-layer]=cd3d398515548fc0dd70bddcbccc3ba28db05df6
	[art_standalone]=8ac8f5cb2e5c2206a93de5498f6368190625b3cc
	[bionic_translation]=136126b9dc634e1f23a6b46e0b83ce5d721079ac
	[libopensles-standalone]=3044a81377d3319506d54650c3773856a053c84c
	[vixl]=0285ee09fc8770308350f13af231f66f970db67f
)

fetch_aports() {
	local p=$1 c=${APORTS_COMMIT[$1]:?no aports commit for $1} d=$DL/src/aports-$1 f t
	local api="https://gitlab.alpinelinux.org/api/v4/projects/alpine%2Faports/repository"
	local gh="https://api.github.com/repos/alpinelinux/aports/contents/testing/$p?ref=$c"
	local cu="curl -sfL --connect-timeout 10 --max-time 90 --retry 2"
	# cache: the directory was fetched from this commit already, no network needed
	# (gitlab.alpinelinux.org is unreachable at times)
	if [ -s "$d/APKBUILD" ] && [ -f "$d/.aports-complete" ] && [ "$(cat "$d/.aports-commit" 2>/dev/null)" = "$c" ]; then
		echo "aports testing/$p @ ${c:0:12}: from the cache $d"
		return 0
	fi
	t=$d.tmp; rm -rf "$t" && mkdir -p "$t"
	if $cu "$api/tree?path=testing/$p&ref=$c&per_page=100" -o "$t/.tree.json" && [ -s "$t/.tree.json" ]; then
		for f in $(grep -o '"name":"[^"]*"' "$t/.tree.json" | cut -d'"' -f4); do
			$cu -o "$t/$f" "https://gitlab.alpinelinux.org/alpine/aports/-/raw/$c/testing/$p/$f" ||
				{ echo "aports $p: $f not fetched" >&2; rm -rf "$t"; return 1; }
		done
	elif $cu "$gh" -o "$t/.tree.json" && [ -s "$t/.tree.json" ]; then
		# fallback: the aports mirror on GitHub, same commit
		for f in $(grep -o '"name": *"[^"]*"' "$t/.tree.json" | cut -d'"' -f4); do
			$cu -o "$t/$f" "https://raw.githubusercontent.com/alpinelinux/aports/$c/testing/$p/$f" ||
				{ echo "aports $p: $f not fetched (GitHub)" >&2; rm -rf "$t"; return 1; }
		done
		echo "aports testing/$p: taken from the GitHub mirror"
	else
		rm -rf "$t"
		echo "aports testing/$p @ ${c:0:12}: neither gitlab.alpinelinux.org nor the GitHub mirror answers" >&2
		return 1
	fi
	[ -s "$t/APKBUILD" ] || { echo "aports $p: no APKBUILD" >&2; rm -rf "$t"; return 1; }
	echo "$c" >"$t/.aports-commit"; : >"$t/.aports-complete"
	rm -rf "$d" && mv "$t" "$d"
	echo "aports testing/$p @ ${c:0:12}: $(ls "$d" | tr '\n' ' ')"
}

# our own packages: an aport directory <a> (tools/aports/<pkg>, or the aports of another
# repository: ../titan2-gnss/aports/titan2-gnss). A file "tarball" in it names the directory to
# pack as <pkg>.tar.gz, relative to the root of the git checkout that holds <a>: a bare name is a
# directory under tools/, a path with "/" is relative to that root (tools/atl/overlay). Caches
# and editor/patch leftovers never go into the tarball. The sha512 of every local *.tar.gz (that
# tarball, or archives made beforehand: "make dist" in titan2-fingerprint and titan2-gnss) is
# written into the staged APKBUILD, as "abuild checksum" would.
local_aports() {
	local a=$1 p d src sum base top
	a=$(realpath -m "$a")
	[ -f "$a/APKBUILD" ] || { echo "$a: no APKBUILD" >&2; exit 1; }
	p=$(basename "$a")
	d=$DL/src/aports-$p
	rm -rf "$d" && mkdir -p "$d"
	cp "$a"/* "$d/"
	if [ -f "$a/tarball" ]; then
		top=$(git -C "$a" rev-parse --show-toplevel 2>/dev/null) ||
			{ echo "local aports $p: $a is not inside a git checkout (tarball path)" >&2; exit 1; }
		src=$(cat "$a/tarball")
		case "$src" in
		*/*) base=$top/$(dirname "$src"); src=$(basename "$src") ;;
		*) base=$top/tools ;;
		esac
		[ -d "$base/$src" ] || { echo "local aports $p: no $base/$src" >&2; exit 1; }
		rm -f "$d/tarball"
		tar -C "$base" --exclude='*.o' --exclude='__pycache__' --exclude='*.pyc' \
			--exclude='*.orig' --exclude='*.orig[0-9]' --exclude='*.rej' --exclude='*.pre-silence*' \
			--exclude='*~' --owner=0 --group=0 --mtime=@0 --sort=name \
			-czf "$d/$p.tar.gz" "$src"
	fi
	sum=$(cd "$d" && for f in *.tar.gz; do [ ! -f "$f" ] || sha512sum "$f"; done)
	if [ -n "$sum" ]; then
		awk -v s="$sum" '/^sha512sums="/ {print; print s; skip=1; next} skip && /^"/ {skip=0} !skip {print}' \
			"$d/APKBUILD" >"$d/APKBUILD.new" && mv "$d/APKBUILD.new" "$d/APKBUILD"
	fi
	grep -q '^sha512sums=' "$d/APKBUILD" || { echo "local aports $p: no sha512sums= in the APKBUILD" >&2; exit 1; }
	echo "local aports $p: $(ls "$d" | tr '\n' ' ')"
	STAGED=$p
}

cmd_status() {
	[ -d "$OUT/rootfs" ] && echo "rootfs: $OUT/rootfs ($(cat "$OUT/rootfs/etc/alpine-release" 2>/dev/null))" || echo "rootfs: none"
	[ -f "$OUT/packages.txt" ] && grep -E '^(android-translation-layer|art_standalone|bionic_translation|libopensles-standalone|libandroidfw|vixl) ' "$OUT/packages.txt"
	ls "$OUT/apks" "$OUT/packages" "$OUT/shots" 2>/dev/null || true
}

case "${1:-}" in
rootfs) need_kernel; dk rootfs ;;
apks) cmd_apks ;;
test)
	need_kernel
	shift
	[ $# -ge 1 ] || { echo "test APK [-- arguments]" >&2; exit 2; }
	apk=$(basename "$1"); shift
	[ "${1:-}" != "--" ] || shift
	[ -s "$DL/test-apks/$apk" ] || { echo "no $DL/test-apks/$apk (scripts/build-atl.sh apks)" >&2; exit 1; }
	dk test "$apk" "$@" ;;
shell)
	need_kernel
	shift
	[ ! -t 0 ] || DK_TTY=-it
	dk shell "$@" ;;
build)
	need_kernel
	shift
	[ $# -ge 1 ] || set -- android-translation-layer
	need_key
	names=()
	for p in "$@"; do
		case "$p" in
		*/*) local_aports "$p"; names+=("$STAGED") ;;
		*)
			if [ -f "$ROOT/tools/aports/$p/APKBUILD" ]; then local_aports "$ROOT/tools/aports/$p"
			else fetch_aports "$p"
			fi
			names+=("$p") ;;
		esac
	done
	[ -z "${ATL_COMMIT:-}" ] || cmd_jars
	dk build "${names[@]}" ;;
local) need_kernel; shift; dk local "$@" ;;
status) cmd_status ;;
clean)
	# the rootfs files belong to the user and to uid 100000+ (user namespace): remove them in a container
	docker run --rm -v "$OUT:/out" "$IMG" sh -c 'rm -rf /out/rootfs /out/buildroot' ;;
*) sed -n '5,49p' "$0"; exit 2 ;;
esac
