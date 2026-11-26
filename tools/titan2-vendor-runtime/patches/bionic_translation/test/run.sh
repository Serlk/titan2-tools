#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Build and run every *-test.c here against a bionic_translation tree on aarch64 (the ATL
# buildroot under qemu-aarch64, QEMU_CPU=cortex-a76 = no FEAT_LSE2 like the Titan 2's cores).
#   sh tools/titan2-vendor-runtime/patches/bionic_translation/test/run.sh TREE [TREE...]
#   (TREE: unpacked source with patches; the aarch64 buildroot comes from scripts/build-atl.sh in out/atl)
set -eu
ROOT=$(cd "$(dirname "$0")/../../../../.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 BIONIC_TRANSLATION_TREE..."; exit 2; }
args=""
i=0
for t in "$@"; do
	i=$((i + 1))
	args="$args /t$i"
	vols="${vols:-} -v $(cd "$t" && pwd):/t$i:ro"
done
# shellcheck disable=SC2086
docker run --rm --security-opt seccomp=unconfined --security-opt apparmor=unconfined --cap-add SYS_ADMIN \
	-e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
	-v "$ROOT/out/atl:/out" -v "$ROOT/out/atl-dl:/dl" -v "$ROOT/tools/titan2-vendor-runtime/patches/bionic_translation/test:/test:ro" $vols \
	alpine:3.24 sh /test/inner.sh $args
