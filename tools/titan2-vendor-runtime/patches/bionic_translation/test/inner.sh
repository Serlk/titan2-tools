#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# in the container: user namespace + private binfmt (as tools/atl/inner.sh), chroot into the
# aarch64 buildroot, compile the test with each tree's libpthread.c and run it
set -eu
if [ -z "${T2_IN_NS:-}" ]; then
	apk add -q --cache-dir /dl/apk-cache-x86 qemu-aarch64 util-linux-misc >/dev/null
	exec env T2_IN_NS=1 unshare --user --mount \
		--map-users=0:"${HOST_UID:?}":1 --map-users=1:100000:65535 \
		--map-groups=0:"${HOST_GID:?}":1 --map-groups=1:100000:65535 \
		--setuid=0 --setgid=0 sh "$0" "$@"
fi
mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
printf '%s' ':qemu-aarch64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64:FOC' \
	>/proc/sys/fs/binfmt_misc/register
B=/out/buildroot
mount -t tmpfs tmpfs $B/tmp
mount --rbind /proc $B/proc
rc=0
for t in "$@"; do
	n=$(basename "$t")
	mkdir -p $B/tmp/$n/test
	cp -r "$t/pthread_wrapper" $B/tmp/$n/
	cp /test/*-test.c $B/tmp/$n/test/
	echo "== $t ($(cd "$t" && grep -c lazy_init_lock pthread_wrapper/libpthread.c || true) lazy_init_lock refs," \
		"$(cd "$t" && grep -c '^int bionic_sem_getvalue' pthread_wrapper/libpthread.c || true) bionic_sem_getvalue)"
	for c in /test/*-test.c; do
		c=$(basename "$c" .c)
		# a tree without the entry point a test calls does not compile: reported, not fatal
		if ! chroot $B /usr/bin/gcc -O2 -w -o /tmp/$n/test/$c /tmp/$n/test/$c.c -lpthread; then
			echo "$c: does not build against $t"; rc=1
			continue
		fi
		# the CPU model the qemu interpreter emulates: QEMU_CPU passes through the chroot
		chroot $B /usr/bin/env QEMU_CPU=cortex-a76 /tmp/$n/test/$c || { echo "$c: exit $?"; rc=1; }
	done
done
exit $rc
