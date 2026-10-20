# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Static aarch64 helpers without libc: mini-init (/init for the first boots) and devmem.
#   make -C tools CC=/path/to/clang-r574158/bin/clang
CC ?= clang
CFLAGS := --target=aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector -nostdlib -static \
	-fuse-ld=lld -Wl,--build-id=none

all: mini-init/init devmem/devmem

mini-init/init: mini-init/init.c
	$(CC) $(CFLAGS) -o $@ $<

devmem/devmem: devmem/devmem.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f mini-init/init devmem/devmem
