# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# titan2-vendor-runtime: the shared base of titan2-fingerprint and titan2-gnss.
#   make                  t2-propd, t2-logd (C11, libc only)
#   make dist             titan2-vendor-runtime.tar.gz for tools/aports/titan2-vendor-runtime
#   make install          programs, OpenRC service, drop-in lists, bionic_translation profile;
#                         the sources of t2-hal and bionic_exe plus the headers go to
#                         $(PREFIX)/share/titan2-vendor-runtime/src (VENDOR_RUNTIME for the
#                         Makefiles of titan2-fingerprint and titan2-gnss)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall
PREFIX ?= /usr
DESTDIR ?=
SHARE = $(DESTDIR)$(PREFIX)/share/titan2-vendor-runtime

BINS = t2-propd t2-logd

all: $(BINS)

t2-propd: container/t2-propd.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $<

t2-logd: container/t2-logd.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $<

install: all
	install -Dm755 t2-propd $(DESTDIR)$(PREFIX)/bin/t2-propd
	install -Dm755 t2-logd $(DESTDIR)$(PREFIX)/bin/t2-logd
	install -Dm755 t2-vendor-setup $(DESTDIR)$(PREFIX)/bin/t2-vendor-setup
	install -Dm755 t2-vendor-env $(DESTDIR)$(PREFIX)/bin/t2-vendor-env
	install -Dm755 container/t2-vendor-container $(DESTDIR)$(PREFIX)/bin/t2-vendor-container
	install -Dm755 openrc/t2-vendor $(DESTDIR)/etc/init.d/t2-vendor
	install -Dm644 openrc/t2-vendor.confd $(DESTDIR)/etc/conf.d/t2-vendor
	install -Dm644 titan2-vendor.cfg $(DESTDIR)/etc/bionic_translation/cfg.d/titan2-vendor.cfg
	install -Dm644 files.d/vendor-container.txt $(SHARE)/files.d/vendor-container.txt
	install -Dm644 props.d/00-base.props $(SHARE)/props.d/00-base.props
	install -Dm755 needed-closure.py $(SHARE)/needed-closure.py
	install -Dm644 t2-hal.c $(SHARE)/src/t2-hal.c
	install -Dm644 t2-hal.h $(SHARE)/src/t2-hal.h
	install -Dm644 bionic_exe.c $(SHARE)/src/bionic_exe.c
	install -Dm644 include/hardware/hardware.h $(SHARE)/src/include/hardware/hardware.h
	install -Dm644 include/sys/cdefs.h $(SHARE)/src/include/sys/cdefs.h

# source tarball for tools/aports/titan2-vendor-runtime (abuild checksum after it)
dist:
	tar -C .. --exclude='*.o' --exclude=t2-propd --exclude=t2-logd --exclude='*.tar.gz' \
		--exclude=__pycache__ --owner=0 --group=0 --mtime=@0 --sort=name \
		-czf ../aports/titan2-vendor-runtime/titan2-vendor-runtime.tar.gz titan2-vendor-runtime

clean:
	rm -f $(BINS) *.o

.PHONY: all install dist clean
