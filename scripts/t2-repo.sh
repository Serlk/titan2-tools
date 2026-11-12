#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# The Titan 2 apk repository: one signed index over everything we build or pin ourselves, so the
# phone image (scripts/netroot-serve.sh LOCAL_REPO=out/t2-repo LOCAL_KEY=out/t2-repo/t2-repo.rsa.pub)
# and the phone itself (apk add --repository) get them from one place.
#
#   scripts/t2-repo.sh keygen        create the signing key out/t2-repo-keys/t2-repo.rsa(.pub)
#                                    (once; keep it, the phone trusts its public half)
#   scripts/t2-repo.sh aport DIR...  build our own aports (DIR = a directory with an APKBUILD, e.g.
#                                    tools/aports/titan2-vendor-runtime, or the aports of another
#                                    repository, e.g. external/titan2-gnss/aports/titan2-gnss)
#                                    for aarch64 -> out/t2-packages/<arch>/*.apk
#   scripts/t2-repo.sh build         collect the packages -> out/t2-repo/aarch64/*.apk + APKINDEX.tar.gz
#                                    signed with the key above
#   scripts/t2-repo.sh list          what the index contains
#
# An aport may have a file "tarball" naming a directory, relative to the root of the git
# repository that holds the aport (e.g. tools/titan2-vendor-runtime); it is packed as
# <pkgname>.tar.gz next to the APKBUILD and its sha512 is written into the APKBUILD.
# "aport" runs abuild in an alpine aarch64 container (docker --platform linux/arm64): the host
# needs qemu-user binfmt support for aarch64 (e.g. the qemu-user-static package).
#
# Sources of "build" (all optional; missing ones are skipped with a note):
#   out/t2-packages/aarch64, out/t2-packages/noarch   our aports ("aport" above)
#   EXTRA_APK_DIRS="dir ..."   more directories with *.apk inside this repository, e.g. a Mesa or
#                              libcamera build
# Package signatures stay as they are (Alpine's key for pinned Alpine binaries, ours for our
# builds); only the index is signed with our key. apk picks the highest version of a name, so
# rebuilds with a higher pkgrel win over Alpine copies of the same name.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/t2-repo
PKG=$ROOT/out/t2-packages
KEYS=$ROOT/out/t2-repo-keys
KEY=t2-repo.rsa
REL=${ALPINE_RELEASE:-v3.24}
IMG=alpine:${REL#v}
SRC_DIRS="out/t2-packages/aarch64 out/t2-packages/noarch ${EXTRA_APK_DIRS:-}"

need_key() {
	[ -f "$KEYS/$KEY" ] && [ -f "$KEYS/$KEY.pub" ] ||
		{ echo "no signing key $KEYS/$KEY(.pub): scripts/t2-repo.sh keygen" >&2; exit 1; }
}

cmd_keygen() {
	[ ! -e "$KEYS/$KEY" ] || { echo "$KEYS/$KEY exists: not replaced" >&2; exit 1; }
	mkdir -p "$KEYS"
	docker run --rm -v "$KEYS:/keys" -e UIDGID="$(id -u):$(id -g)" "$IMG" sh -euc '
		apk add -q openssl >/dev/null
		openssl genrsa -out /keys/'"$KEY"' 4096 2>/dev/null
		openssl rsa -in /keys/'"$KEY"' -pubout -out /keys/'"$KEY"'.pub 2>/dev/null
		chmod 600 /keys/'"$KEY"'
		chown -R "$UIDGID" /keys'
	echo "key: $KEYS/$KEY (+ .pub)"
}

# one aport -> a staging copy (with the tarball) in out/t2-packages/src/<name>
stage_aport() {
	local a=$1 p d src top sum
	a=$(realpath -m "$a")
	[ -f "$a/APKBUILD" ] || { echo "$a: no APKBUILD" >&2; exit 1; }
	p=$(basename "$a")
	d=$PKG/src/$p
	rm -rf "$d" && mkdir -p "$d"
	cp "$a"/* "$d/"
	if [ -f "$a/tarball" ]; then
		top=$(git -C "$a" rev-parse --show-toplevel 2>/dev/null) ||
			{ echo "$a: not inside a git checkout, cannot resolve the tarball path" >&2; exit 1; }
		src=$(cat "$a/tarball")
		[ -d "$top/$src" ] || { echo "aport $p: no $top/$src" >&2; exit 1; }
		rm -f "$d/tarball"
		tar -C "$top/$(dirname "$src")" --exclude='*.o' --exclude='__pycache__' --exclude='*.pyc' \
			--exclude='*.orig' --exclude='*.rej' --exclude='*~' --exclude='build' \
			--owner=0 --group=0 --mtime=@0 --sort=name \
			-czf "$d/$p.tar.gz" "$(basename "$src")"
		sum=$(sha512sum "$d/$p.tar.gz" | cut -d' ' -f1)
		awk -v f="$p.tar.gz" -v s="$sum" '/^sha512sums="/ {print; print s "  " f; skip=1; next}
			skip && /^"/ {skip=0} !skip {print}' "$d/APKBUILD" >"$d/APKBUILD.new" &&
			mv "$d/APKBUILD.new" "$d/APKBUILD"
	fi
	echo "$p"
}

cmd_aport() {
	[ $# -ge 1 ] || { echo "aport DIR..." >&2; exit 2; }
	need_key
	local names=() a
	for a in "$@"; do names+=("$(stage_aport "$a")"); done
	mkdir -p "$PKG"
	# abuild runs as an unprivileged user inside the container; the source checksums of
	# downloaded sources are verified by abuild against the APKBUILD
	docker run --rm --platform linux/arm64 -v "$PKG:/pkg" -v "$KEYS:/keys:ro" \
		-v "$ROOT/tools/initramfs/keys:/akeys:ro" -e UIDGID="$(id -u):$(id -g)" -e NAMES="${names[*]}" \
		"$IMG" sh -euc '
		apk add -q alpine-sdk >/dev/null
		adduser -D -h /build b && addgroup b abuild
		mkdir -p /build/.abuild /out
		cp /keys/'"$KEY"' /keys/'"$KEY"'.pub /build/.abuild/
		cp /keys/'"$KEY"'.pub /etc/apk/keys/
		echo "PACKAGER_PRIVKEY=/build/.abuild/'"$KEY"'" >/build/.abuild/abuild.conf
		echo "REPODEST=/out" >>/build/.abuild/abuild.conf
		chown -R b:b /build /out
		for p in $NAMES; do
			cp -r /pkg/src/$p /build/$p && chown -R b:b /build/$p
			su b -c "cd /build/$p && abuild -r"
		done
		for a in /out/*/*; do
			[ -d "$a" ] || continue
			mkdir -p /pkg/${a##*/}
			cp "$a"/*.apk /pkg/${a##*/}/
		done
		chown -R "$UIDGID" /pkg'
	ls -l "$PKG"/*/*.apk
}

cmd_build() {
	need_key
	local d n=0 stage=$OUT/aarch64
	rm -rf "$OUT" && mkdir -p "$stage"
	for d in $SRC_DIRS; do
		d=$(realpath -m "$ROOT/$d")
		case "$d" in "$ROOT"/*) ;; *) echo "$d: outside the repository" >&2; exit 1 ;; esac
		if ! ls "$d"/*.apk >/dev/null 2>&1; then
			echo "skip: no packages in ${d#"$ROOT"/}"
			continue
		fi
		cp -n "$d"/*.apk "$stage/"
		echo "${d#"$ROOT"/}: $(ls "$d"/*.apk | wc -l) package(s)"
	done
	n=$(ls "$stage"/*.apk 2>/dev/null | wc -l)
	[ "$n" -gt 0 ] || { echo "nothing to index" >&2; exit 1; }
	# index + signature in a throwaway container (apk-tools 3 + abuild-sign); files owned by the user.
	# apk index verifies every package signature: the Alpine aarch64 key and ours are the only
	# trusted keys
	docker run --rm -v "$stage:/repo" -v "$KEYS:/keys:ro" -v "$ROOT/tools/initramfs/keys:/akeys:ro" \
		-e UIDGID="$(id -u):$(id -g)" "$IMG" sh -euc '
		apk add -q abuild >/dev/null
		mkdir -p /tmp/keys && cp /akeys/*.rsa.pub /keys/*.rsa.pub /tmp/keys/
		cd /repo
		rm -f APKINDEX.tar.gz
		apk index --keys-dir /tmp/keys --rewrite-arch aarch64 -o APKINDEX.tar.gz *.apk 2>&1 | grep -v "^WARNING: " || :
		[ -s APKINDEX.tar.gz ] || { echo "apk index failed" >&2; exit 1; }
		cp /keys/'"$KEY"' /tmp/ && cp /keys/'"$KEY"'.pub /tmp/
		abuild-sign -k /tmp/'"$KEY"' APKINDEX.tar.gz
		chown -R "$UIDGID" /repo'
	cp "$KEYS/$KEY.pub" "$OUT/"
	echo "out/t2-repo/aarch64: $n package(s), index signed"
	cmd_list
}

cmd_list() {
	[ -f "$OUT/aarch64/APKINDEX.tar.gz" ] || { echo "no index: scripts/t2-repo.sh build" >&2; exit 1; }
	tar -xzOf "$OUT/aarch64/APKINDEX.tar.gz" APKINDEX |
		sed -n '/^P:/{s/^P://;h};/^V:/{s/^V://;H;x;s/\n/ /p}' | sort
}

case "${1:-}" in
keygen) cmd_keygen ;;
aport) shift; cmd_aport "$@" ;;
build) cmd_build ;;
list) cmd_list ;;
*) sed -n '5,30p' "$0"; exit 2 ;;
esac
