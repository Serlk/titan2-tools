#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Read-only snapshot; run on Titan 2, e.g. ssh ... sh -s < collect.sh.
set -u
date -u
uname -a
printf '\n--- Power supplies ---\n'
for p in /sys/class/power_supply/*; do
    [ -d "$p" ] || continue
    echo "$p"
    cat "$p/uevent"
done
printf '\n--- ADC ---\n'
for p in /sys/bus/iio/devices/iio:device*; do
    [ -f "$p/name" ] || continue
    [ "$(cat "$p/name")" = mt6375-adc ] || continue
    for f in "$p"/in_*; do
        [ -f "$f" ] || continue
        printf '%s: ' "$f"
        cat "$f"
    done
done
printf '\n--- Type-C ---\n'
ls -l /sys/class/typec
printf '\n--- Interrupts ---\n'
grep -E 'mt6375|pwr_rdy|detach|vbus_ov|chg_tout|chg_batov|bc12' /proc/interrupts || true
printf '\n--- Kernel log ---\n'
dmesg | grep -Ei 'mt6375|bq27|sh366|charger|tcpm|tcpci|i2c.*(error|fail|timeout)' || true
