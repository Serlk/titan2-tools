# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
#   make                 host build (needs alsa-lib)
#   make test            sanitizer build + host test (python3)
#   make BIN=t2-speechd-aarch64 CC="clang --target=aarch64-alpine-linux-musl --sysroot=..."
#                        build for the phone (dynamic: libasound.so.2), left as
#                        t2-speechd-aarch64 so the host binary is not overwritten

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wformat=2
LDFLAGS ?=
LDLIBS  ?= -lasound
PYTHON  ?= python3

# speech protocol (libc only) / AP audio path / alsa-lib backend / glue
SRCS := src/proto.c src/shm.c src/engine.c \
        src/audio.c src/audio_alsa.c \
        src/main.c src/replay.c
HDRS := src/speechd.h
BIN  ?= t2-speechd
TBIN := build/t2-speechd-asan

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer \
       -fno-sanitize-recover=all

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

$(TBIN): $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wshadow -Werror \
		$(SAN) -o $@ $(SRCS) $(LDLIBS)

test: $(TBIN)
	$(PYTHON) -I tests/test_replay.py $(abspath $(TBIN))

clean:
	rm -rf $(BIN) build

.PHONY: all test clean
