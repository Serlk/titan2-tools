#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Return the slot A boot partitions to a working Android after mainline experiments.
# Waits for the phone in fastboot (LK), flashes boot_a, init_boot_a, vendor_boot_a, dtbo_a from the backup,
# then reboots. Backup directory: boot_a.img, init_boot_a.img, vendor_boot_a.img,
# dtbo_a.img and SHA256SUMS (sha256sum *.img > SHA256SUMS), taken from YOUR phone before the experiments.
#   scripts/restore-stock-boot.sh BACKUP-DIR      (FASTBOOT=path: use your own fastboot)
#
# Restores slot A boot partitions (boot_a, init_boot_a, vendor_boot_a, dtbo_a) from your own
# backup directory (with SHA256SUMS), waits for the phone in fastboot (LK) and reboots it.
set -euo pipefail
B=${1:?usage: $0 BACKUP-DIR (boot_a.img init_boot_a.img vendor_boot_a.img dtbo_a.img SHA256SUMS)}
F=${FASTBOOT:-fastboot}

(cd "$B" && sha256sum -c --quiet SHA256SUMS) || { echo "backup is corrupt: $B"; exit 1; }
echo "Waiting for the phone in fastboot. If it is in a boot loop: hold volume UP + POWER,"
echo "and in the «Select Boot Mode» menu choose «Fastboot Mode» (volume up selects, volume down confirms)."
until "$F" devices | grep -q fastboot; do sleep 1; done
for p in boot_a init_boot_a vendor_boot_a dtbo_a; do
	"$F" flash "$p" "$B/$p.img"
done
"$F" reboot
echo "Done: the phone is booting into Android."
