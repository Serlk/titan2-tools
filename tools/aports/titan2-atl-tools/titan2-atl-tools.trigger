#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# the CA bundle changed (/etc/ssl/certs): lay it out again for Android applications
/usr/bin/atl-cacerts >/dev/null 2>&1
exit 0
