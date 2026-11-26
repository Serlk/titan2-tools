<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# Titan 2 sensor hub calibration on Linux

The SCP sensor hub keeps per-sensor calibration words only while it runs. The Android
install of the phone keeps them as small JSON files in `sensor/` of its `nvcfg` partition
(GPT label `nvcfg`, ext4). This system never mounts nvcfg (the mount wrapper refuses it and
the disk is read-only), so the records are **imported read-only with debugfs** into a local
directory, sent to the hub from there, and what the hub learns is written back **only to the
local copy**. Without calibration the gyroscope at rest reads about 0.058 / 0.059 / -0.031
rad/s (the hub then learns the bias itself within seconds), the accelerometer on a flat table
gives |a| about 11.2 m/s² and the magnetometer |B| about 200-240 uT.

| piece | what it does |
|---|---|
| `t2-sensors-cali import` | `debugfs -R "rdump /sensor TMP" /dev/disk/by-partlabel/nvcfg` (opens the device read-only, mounts nothing), then applies the import rule below to every record |
| `t2-sensors-cali load` | reads the local records, writes one complete word list per sensor into `hub_cali` of the `hub-*` IIO devices |
| OpenRC `t2-sensors-cali` (boot runlevel) | import + load, before `iio-sensor-proxy`; `rc-service t2-sensors-cali reimport` = forced import + load |
| `t2-sensors-cali watch`, OpenRC `t2-sensors-calisave` (default runlevel, **on**) | listens on `/dev/hf_manager` for BIAS/TEMP/CALI results and rewrites the local records, at most every 600 s per record (calibration results at once), everything pending on SIGUSR1 and at stop |
| kernel `mtk-hf-iio` (titan2-linux `titan2` since 2026-11-27) | `hub_cali` takes exactly the record length of the sensor and refuses anything else; reading it shows the words last sent; the loader also works with the current driver, since it only sends complete records |

Local state (`T2_SENSORS_CALI_STATE`, default `/var/lib/titan2-sensors`):

- `cali/<name>.json`: the records used here (loaded at boot, updated by the writer);
- `nvcfg/<name>.json`: byte copies of what the last import took from nvcfg.

**Import rule**, per record that is present and usable on nvcfg:

| situation | result |
|---|---|
| no local copy | taken from nvcfg |
| no copy of a previous import (first import of this record) | taken |
| nvcfg file differs from what the last import took (Android wrote it since) | taken |
| nvcfg file unchanged since the last import | local copy kept (it may hold newer values learned here) |
| `import -F` / `rc-service t2-sensors-cali reimport` | taken |

A record that is missing or unusable on nvcfg never replaces a local one. The comparison is
by content, not by timestamps, so clocks and debugfs's handling of times do not matter.
After a session in Android that learned new values, Linux takes them at the next boot;
values learned in Linux stay in Linux (Android never sees them, nvcfg is never written).

Word lists sent at boot (space-separated decimal; a missing or unusable record is zeros):

| device | words | from |
|---|---|---|
| `hub-accel` | 6 | `acc_bias[3]`, `acc_cali[3]` |
| `hub-gyro` | 12 | `gyro_bias[3]`, `gyro_cali[3]`, `gyro_temp[6]` |
| `hub-magn` | 9 | `mag_bias[3]`, `mag_cali[6]` |
| `hub-light` | 1 | `als_cali[1]` |
| `hub-prox` | 2 | `ps_cali[2]` (threshold high, low) |

A record counts only when the file is one valid JSON object whose key (the file name without
`.json`) holds an array with at least that many int32 integers (no fractions or exponents).
The writer keeps the byte format of the nvcfg files (`{\n  "key": [\n      v,\n ...    ]\n}`,
no final newline), replaces a file atomically (temporary file, fsync, rename, mode 0644) and
creates a record only when the hub reported a value for it.

The kernel keeps the first record of a sensor and refuses one of another length until
reboot. A record of the same length can be sent again at any time.

## Build and install

    cd tools/sensors
    make test                      # host: sanitizer build + fake sysfs/nvcfg/debugfs test
    SR=$PWD/../../out/musl-aarch64/sysroot
    make BIN=t2-sensors-cali-aarch64 \
         CC="clang --target=aarch64-alpine-linux-musl --sysroot=$SR -fuse-ld=lld --gcc-toolchain=$SR/usr"

Package: `scripts/build-atl.sh build titan2-sensors` (tools/aports/titan2-sensors; depends on
`e2fsprogs-extra` for debugfs; the post-install adds `t2-sensors-cali` to boot and
`t2-sensors-calisave` to default). Netroot image: `tools/netroot/install.d/titan2-sensors.sh`
installs the cross build and enables both services.

Settings: `/etc/conf.d/t2-sensors-cali` (`T2_SENSORS_CALI_IMPORT`, `..._LOAD`, `..._WAIT`,
`..._WRITEBACK`, `..._INTERVAL`, `..._STATE`). Log: syslog tag `t2-sensors-cali` (and the
console at boot).

## Acceptance checks on the phone

Helper (`iio <name>` gives the device directory, `rd3` prints scaled x y z and the norm):

    iio() { for d in /sys/bus/iio/devices/iio:device*; do [ "$(cat $d/name)" = "$1" ] && echo $d; done; }
    rd3() {	# name channel [multiplier]
        d=$(iio $1); s=$(cat $d/in_$2_scale)
        x=$(cat $d/in_$2_x_raw); y=$(cat $d/in_$2_y_raw); z=$(cat $d/in_$2_z_raw)
        awk -v x=$x -v y=$y -v z=$z -v s=$s -v m=${3:-1} 'BEGIN { x*=s*m; y*=s*m; z*=s*m;
            printf "%.4f %.4f %.4f  |%.4f|\n", x, y, z, sqrt(x*x+y*y+z*z) }'
    }
    # gyro rad/s: rd3 hub-gyro anglvel     magn uT: rd3 hub-magn magn 100     accel m/s2: rd3 hub-accel accel

Results of 26.11.2026 (first loader version, records copied by hand with debugfs, phone flat,
screen up): with the records gyro 0.0001-0.001 rad/s at once, accel 0.20/0.74/9.53 (|a|
9.56, sign right), magn |B| 63 uT; with zeros gyro 0.058/0.059/-0.031 (learned to ~0.001
after about 5 s), accel |a| 11.2, magn |B| 198. The checks below repeat this with the
services.

1. **First boot with the package.** `rc-service t2-sensors-cali status` started; the log
   shows `debugfs` import lines (`<name>.json imported from nvcfg (no local copy)`, six
   records), then one `hub-* : N words sent` line per sensor (5 of 5).
   `ls -l /var/lib/titan2-sensors/cali /var/lib/titan2-sensors/nvcfg`: six records each,
   no `.import-*` directory left. `grep nvcfg /proc/mounts` is empty (nothing mounted).
   `cat $(iio hub-gyro)/hub_cali` shows the 12 words (with the new kernel module).
2. **Readings after boot**: gyro at rest within a few mrad/s of 0; magnetometer |B| in
   25-65 uT and steady while the phone is turned slowly; accel flat: x, y near 0, |a| near
   9.81. Proximity: near/far switches (cover/uncover), `hub_words` of `hub-prox`; light:
   `in_illuminance_raw` as before.
3. **Accelerometer sign again** (phone flat, screen up, not touched):
   `echo 0 0 0 0 0 0 > $(iio hub-accel)/hub_cali; rd3 hub-accel accel`, then
   `t2-sensors-cali load -w 1` (sends all records again, same lengths) and `rd3` again.
   With the record x/y must be closer to 0 and |a| closer to 9.81.
4. **Length lock**: `echo 1 2 3 > $(iio hub-accel)/hub_cali` fails with "Invalid argument",
   `dmesg` says "hub_cali takes 6 words, got 3" (new kernel module only).
5. **Writer**: `rc-service t2-sensors-calisave status` started, log line
   `listening for N kinds of hub results`. Keep the gyroscope running (the bridge turns it off
   3 s after the last read: `while :; do rd3 hub-gyro anglvel >/dev/null; sleep 1; done`) and
   do a figure-eight; after `T2_SENSORS_CALI_INTERVAL` (or at once with
   `kill -USR1 $(pidof t2-sensors-cali)`) the log shows `[a, b, c] ==> gyro_bias.json` and
   `... ==> mag_bias.json` / `mag_cali.json`, and the files in `cali/` change (`od -c`: same
   format as the nvcfg files, no final newline). The copies in `nvcfg/` stay as imported.
6. **Next boot**: the import logs `unchanged on nvcfg, local copy kept` (with `-v`) or no
   "imported" lines, and the load sends the learned values (compare the log words with
   `cali/gyro_bias.json`).
7. **Dual boot**: boot Android (it still reads its untouched files), let it run a while with the compass/gyro in use, boot
   Linux: the records Android rewrote are imported (`changed on nvcfg since the last import`),
   the others keep their Linux values.
8. **Forced import**: `rc-service t2-sensors-cali reimport` stops the writer (it writes what
   it holds), takes every nvcfg record, sends them, restarts the writer.

Turning the writer off: `T2_SENSORS_CALI_WRITEBACK=no` in `/etc/conf.d/t2-sensors-cali` and
`rc-update del t2-sensors-calisave default`. Starting over from Android's records:
`rc-service t2-sensors-cali reimport`, or delete `/var/lib/titan2-sensors` and reboot.
