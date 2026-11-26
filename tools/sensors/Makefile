# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# titan2-sensors: sensor hub calibration (t2-sensors-cali + OpenRC services) and udev rules.
#
#   make                 host build
#   make test            sanitizer build + the fake sysfs/nvcfg test
#   SR=out/musl-aarch64/sysroot; make BIN=t2-sensors-cali-aarch64 \
#        CC="clang --target=aarch64-alpine-linux-musl --sysroot=$SR -fuse-ld=lld --gcc-toolchain=$SR/usr"
#                        build for the phone (musl libc only; netroot install.d/titan2-sensors.sh)
#   make DESTDIR=... PREFIX=/usr install

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral
LDFLAGS ?=
PREFIX  ?= /usr
DESTDIR ?=
BIN     ?= t2-sensors-cali
TBIN    := build/t2-sensors-cali-asan
SRC     := src/t2-sensors-cali.c

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)

$(TBIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) -o $@ $(SRC)

test: $(TBIN)
	sh tests/cali-test.sh $(TBIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/t2-sensors-cali
	install -Dm755 openrc/t2-sensors-cali $(DESTDIR)/etc/init.d/t2-sensors-cali
	install -Dm755 openrc/t2-sensors-calisave $(DESTDIR)/etc/init.d/t2-sensors-calisave
	install -Dm644 openrc/t2-sensors-cali.confd $(DESTDIR)/etc/conf.d/t2-sensors-cali
	install -Dm644 90-titan2-sensors.rules $(DESTDIR)/usr/lib/udev/rules.d/90-titan2-sensors.rules
	install -Dm644 CALIBRATION.md $(DESTDIR)$(PREFIX)/share/doc/titan2-sensors/CALIBRATION.md

clean:
	rm -f $(BIN) t2-sensors-cali t2-sensors-cali-aarch64
	rm -rf build

.PHONY: all test install clean
