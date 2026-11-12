# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# t2-mm-speech-trigger: needs libmm-glib (pkg-config mm-glib). To build against
# an uninstalled ModemManager build tree:
#   make PKG_CONFIG_PATH=/path/to/ModemManager/build/meson-uninstalled
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
PKG_CONFIG ?= pkg-config
BIN ?= t2-mm-speech-trigger

MM_CFLAGS := $(shell $(PKG_CONFIG) --cflags mm-glib gio-2.0 gio-unix-2.0)
MM_LIBS   := $(shell $(PKG_CONFIG) --libs mm-glib gio-2.0 gio-unix-2.0)

$(BIN): t2-mm-speech-trigger.c
	$(CC) $(CFLAGS) $(MM_CFLAGS) -o $@ $< $(MM_LIBS) $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: clean
