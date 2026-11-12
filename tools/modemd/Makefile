# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
#   make                 host build
#   make test            sanitizer build + host test (python3)
#   make CC="clang --target=aarch64-alpine-linux-musl --sysroot=..." LDFLAGS=-static
#                        static build for the phone (see README.md)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wformat=2
LDFLAGS ?=
PYTHON  ?= python3

SRCS := src/at.c src/modem.c src/data.c src/ims.c src/call.c src/sms.c src/ctl.c \
        src/vendor.c src/mipc.c src/main.c
HDRS := src/modemd.h src/mipc.h
BIN  ?= t2-modemd
TBIN := build/t2-modemd-asan

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer \
       -fno-sanitize-recover=all

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

$(TBIN): $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wshadow -Werror \
		$(SAN) -o $@ $(SRCS)

test: $(TBIN)
	$(abspath $(TBIN)) -T
	$(PYTHON) -I tests/test_modemd.py $(abspath $(TBIN))

clean:
	rm -rf $(BIN) build

.PHONY: all test clean
