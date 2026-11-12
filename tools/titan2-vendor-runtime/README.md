<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# titan2-vendor-runtime

The shared base for running closed components of the Titan 2 factory firmware (Android 14,
bionic) on mainline Linux with an Alpine/musl user space, without Android and without
containers:

- closed **daemons** (for example the TEE supplicant or the GNSS daemon) run natively in a
  bionic chroot of the vendor root `/var/lib/titan2-vendor`;
- closed **HAL modules** (legacy `hw_module_t`) are loaded in-process by our musl programs
  through [bionic_translation](https://gitlab.com/android_translation_layer/bionic_translation)
  with the vendor profile of the patches in `patches/bionic_translation/`.

[titan2-fingerprint](https://github.com/Serlk/titan2-fingerprint) and
[titan2-gnss](https://github.com/Serlk/titan2-gnss) depend on it.

## What is not here

No vendor binaries, libraries, firmware, keys, trusted applications or configuration data:
nothing from the factory firmware is in this repository or in the package. The file lists
only **name** files; `t2-vendor-setup` copies them from the user's own phone partitions
(mounted or extracted) when it runs on the user's device.

## Pieces

| file | role |
|---|---|
| `t2-vendor-setup` | builds the vendor root from the user's partitions, driven by drop-in lists (below) |
| `t2-vendor-env` | what every daemon in the chroot needs: binds of `/dev`, `/proc`, `/sys`, the property service and the log collector (`up`, `prop NAME VALUE`, `status`) |
| `container/t2-propd.c` | a minimal Android property service (`/dev/__properties__`, `/dev/socket/property_service`, initial values from `<root>/t2-props`) |
| `container/t2-logd.c` | the writer side of Android's logd: liblog output of every bionic process into `/var/log/t2-vendor/logcat.log` |
| `container/t2-vendor-container`, `openrc/t2-vendor` | optional: binderfs and the binder service managers of the factory firmware in the same chroot, the factory SELinux policy loaded permissive |
| `container/vm-test.sh` | smoke test of the container in an aarch64 qemu VM |
| `t2-hal.[ch]`, `bionic_exe.c`, `include/` | loading a HAL module through bionic_translation and the re-exec into the vendor profile; what a host executable must provide for the bionic linker (`_r_debug`, the bionic TLS slots); AOSP `hardware.h` (Apache-2.0) |
| `titan2-vendor.cfg` | bionic_translation library overrides for the vendor profile |
| `needed-closure.py` | prints the DT_NEEDED closure of a binary of the user's own firmware in the list format (helper for writing lists) |
| `patches/bionic_translation/` | the vendor profile of bionic_translation: patches 0001–0005 ([README](patches/bionic_translation/README.md)) |

## Drop-in interface

Packages that need the vendor root install:

- `/usr/share/titan2-vendor-runtime/files.d/<name>.txt`: one file per line,
  `<source path below the partitions> <path below the vendor root>`. `#` starts a comment
  line, a leading `?` marks an optional file, a destination ending in `/` copies a
  directory, `@dir <path>` only creates a runtime directory. Source paths start with
  `system/`, `system_ext/`, `vendor/` or `product/`; `system_a/system/…`, `vendor_a/…` and
  so on are found as well.
- `/usr/share/titan2-vendor-runtime/props.d/<name>.props`: `name=value` lines merged into
  `<root>/t2-props`. A property already in `t2-props` is never overwritten, so manual
  changes survive.

This package installs `files.d/vendor-container.txt` (binder service managers) and
`props.d/00-base.props` (build and board properties). titan2-fingerprint installs
`fingerprint.txt`/`fingerprint.props`, titan2-gnss `gnss.txt`/`gnss.props`.

```
t2-vendor-setup SRC [DEST] [LIST...]
# SRC: the factory partitions, e.g. /mnt/factory with system_a/, system_ext_a/, vendor_a/
# DEST: default /var/lib/titan2-vendor; LIST: default every files.d/*.txt
```

`T2_VENDOR_FILES_D`, `T2_VENDOR_PROPS_D` override the drop-in directories.

## Build

```
make                                   # t2-propd, t2-logd (C11, libc only)
make DESTDIR=… PREFIX=/usr install
make dist                              # tarball for tools/aports/titan2-vendor-runtime
```

The Alpine package is `tools/aports/titan2-vendor-runtime` (`-dev` carries the t2-hal and
bionic_exe sources and headers under `/usr/share/titan2-vendor-runtime/src`, which the
Makefiles of titan2-fingerprint and titan2-gnss use as `VENDOR_RUNTIME`). It needs
`bionic_translation` with the patches above (Alpine's package rebuilt with 0001–0005).

## Related repositories

- [titan2-linux](https://github.com/Serlk/titan2-linux): the kernel, branch `titan2` (the mainline port). The optional camera, fingerprint and GNSS kernel parts are not in it: each lives as a patch series in `kernel/patches/` of its own repository below, applied with `git am` on `titan2`.
- [titan2-boot](https://github.com/Serlk/titan2-boot): the boot menu in `boot_a` (Android and Linux side by side).
- [titan2-tools](https://github.com/Serlk/titan2-tools): image builders, initramfs, network and disk root, modem, power and sensor services, and the shared `titan2-vendor-runtime`.
- [titan2-docs](https://github.com/Serlk/titan2-docs): documentation and hardware notes.
- [titan2-camera](https://github.com/Serlk/titan2-camera): cameras (libcamera patches, configuration, tests).
- [titan2-fingerprint](https://github.com/Serlk/titan2-fingerprint): the fingerprint reader (FocalTech sensor through the TEE).
- [titan2-gnss](https://github.com/Serlk/titan2-gnss): GNSS (navigation daemon, GPS HAL bridge, EPO assistance data).
- [titan2-android](https://github.com/Serlk/titan2-android): Android builds for the Titan 2.
