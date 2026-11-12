#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Inside part of scripts/build-mesa.sh: runs as root in an alpine arm64 container and builds the
# prepared aports given as arguments, in order, with abuild.
# Mounts: /src (ro) the prepared aports (aports-<name>/APKBUILD + files), /keys (ro) the signing
# key $KEY(.pub), /distfiles the source download cache (the Mesa archive is already there),
# /apk-cache the apk package cache, /packages REPODEST (-> /packages/aports/aarch64).
# Environment: UIDGID (owner of the output on the host), KEY, JOBS, PACKAGER (optional).
set -eu
[ $# -gt 0 ] || { echo "inner.sh: no packages given" >&2; exit 2; }
trap 'chown -R "$UIDGID" /packages /distfiles /apk-cache 2>/dev/null || :' EXIT

# a persistent apk cache, so a retried build does not download everything again
ln -sfn /apk-cache /etc/apk/cache
apk add -q alpine-sdk

# the signing key: packages, the REPODEST index, and trust for that index (the later packages
# install the earlier ones from it)
mkdir -p /root/.abuild
cp "/keys/$KEY" "/keys/$KEY.pub" /root/.abuild/
cp "/keys/$KEY.pub" /etc/apk/keys/
{
	echo "PACKAGER_PRIVKEY=/root/.abuild/$KEY"
	[ -z "${PACKAGER:-}" ] || echo "PACKAGER=\"$PACKAGER\""
	echo "SRCDEST=/distfiles"
	echo "REPODEST=/packages"
	echo "JOBS=$JOBS"
	echo "MAKEFLAGS=-j$JOBS"
} >/root/.abuild/abuild.conf

for p in "$@"; do
	d=/build/aports/$p
	rm -rf "$d" && mkdir -p "$d"
	cp -r "/src/aports-$p/." "$d/"
	echo "=== $p: $(sed -n 's/^pkgver=//p' "$d/APKBUILD")-r$(sed -n 's/^pkgrel=//p' "$d/APKBUILD")"
	# -F: abuild as root (throwaway container); -r: install the build dependencies; abuild
	# fetches and checks every source against sha512sums before it builds
	(cd "$d" && abuild -F -r)
done
find /packages -name '*.apk' | sort
