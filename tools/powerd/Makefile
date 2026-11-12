# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
#   make                 host build (needs dbus-1 headers: pkg-config dbus-1)
#   make DBUS=0          without the ModemManager watch (no libdbus)
#   make test            host build with sanitizers + the fake-sysfs test
#   make BIN=t2-powerd-aarch64 CC="clang --target=aarch64-alpine-linux-musl --sysroot=$SR -fuse-ld=lld" \
#        DBUS_CFLAGS="-I$SR/usr/include/dbus-1.0 -I$SR/usr/lib/dbus-1.0/include" DBUS_LIBS="-ldbus-1"
#                        build for the phone (dynamic: libdbus-1.so.3 from the dbus package)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral -Wno-format-truncation
LDFLAGS ?=
DBUS    ?= 1
BIN     ?= t2-powerd
TBIN    := build/t2-powerd-asan
SRC     := src/t2-powerd.c

ifeq ($(DBUS),1)
DBUS_CFLAGS ?= $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS   ?= $(shell pkg-config --libs dbus-1 2>/dev/null || echo -ldbus-1)
CFLAGS  += -DHAVE_DBUS $(DBUS_CFLAGS)
LDLIBS  += $(DBUS_LIBS)
endif

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

$(TBIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

test: $(TBIN)
	sh tests/fake-sysfs-test.sh $(TBIN)

clean:
	rm -f $(BIN) $(TBIN)

.PHONY: all test clean
