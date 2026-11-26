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
#   scripts/t2-repo.sh aport PKG|DIR...
#                                    build aarch64 packages with scripts/build-atl.sh build: DIR is
#                                    a directory with an APKBUILD (tools/aports/titan2-sensors, or
#                                    the aports of another repository, ../titan2-gnss/aports/titan2-gnss),
#                                    PKG a name of tools/aports/ or an Alpine aport that build-atl.sh
#                                    knows (bionic_translation, with our patches) ->
#                                    out/atl/packages/aports/aarch64, signed with the key above.
#                                    Built in the order given; every package sees the ones built
#                                    before it (that directory is a local repository), e.g.
#                                    aport bionic_translation tools/aports/titan2-vendor-runtime \
#                                          ../titan2-fingerprint/aports/titan2-fingerprint
#   scripts/t2-repo.sh build         collect the packages -> out/t2-repo/aarch64/*.apk + APKINDEX.tar.gz
#                                    signed with the key above (a new index on every call)
#   scripts/t2-repo.sh list          what the index contains
#
# An aport may have a file "tarball" naming a directory to pack as <pkgname>.tar.gz next to the
# APKBUILD (a bare name is a directory under tools/ of the git checkout that holds the aport, a
# path with "/" is relative to its root). The sha512 of every *.tar.gz next to the APKBUILD (that
# one, or archives made beforehand: "make dist" in titan2-fingerprint and titan2-gnss) is written
# into the staged APKBUILD. "aport" runs under emulation in a container like build-atl.sh (docker
# and a host kernel >= 6.7; qemu-aarch64 in a private binfmt_misc, nothing on the host changes).
#
# Sources of "build" (all optional; missing ones are skipped with a note):
#   out/t2-packages/aarch64, out/t2-packages/noarch   e.g. Mesa (scripts/build-mesa.sh build)
#   out/atl/packages/aports/aarch64                   "aport" above, scripts/build-atl.sh build
#   out/libcamera/packages/aports/aarch64             titan2-camera's scripts/build-libcamera.sh
#                                                     with OUT=../titan2-tools/out/libcamera
#   EXTRA_APK_DIRS="dir ..."   more directories with *.apk inside this repository (all of them in
#                              the same call: each call makes the index anew)
# Package signatures stay as they are (Alpine's key for pinned Alpine binaries, ours for our
# builds); only the index is signed with our key. Every package must be signed by one of the two
# keys (apk on the phone checks them). apk picks the highest version of a name, so rebuilds with
# a higher pkgrel win over Alpine copies of the same name.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/t2-repo
KEYS=$ROOT/out/t2-repo-keys
KEY=t2-repo.rsa
REL=${ALPINE_RELEASE:-v3.24}
IMG=alpine:${REL#v}
SRC_DIRS="out/t2-packages/aarch64 out/t2-packages/noarch out/atl/packages/aports/aarch64
out/libcamera/packages/aports/aarch64 ${EXTRA_APK_DIRS:-}"

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

cmd_aport() {
	[ $# -ge 1 ] || { echo "aport PKG|DIR..." >&2; exit 2; }
	need_key
	KEYS=$KEYS KEY=$KEY "$ROOT/scripts/build-atl.sh" build "$@"
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
*) sed -n '5,43p' "$0"; exit 2 ;;
esac
