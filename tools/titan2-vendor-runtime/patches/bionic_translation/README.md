<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# bionic_translation: vendor profile patches

Patches against [bionic_translation](https://gitlab.com/android_translation_layer/bionic_translation)
commit `ee37eb21c91409fe0eed833d0a5a0aa6b931bb7b` (the source of Alpine's
`bionic_translation-0_git20260317`). Apply in order with `git am` on a checkout of that
commit (or `patch -p1`). The new files they add carry their own `SPDX-License-Identifier:
Apache-2.0` lines, the license of bionic_translation.

| patch | what |
|---|---|
| 0001 | `BIONIC_VENDOR_ROOT` / `BIONIC_VNDK_APEX`: `<root>/vendor/lib64`, `vendor/lib64/hw`, `apex/<vndk>/lib64`, `system/lib64` as bionic library directories; absolute Android paths re-rooted; `libm.so -> libc_bio.so.0` |
| 0002 | `__system_property_get/find/read/read_callback/foreach/set/serial/area_serial` from `BIONIC_PROPS_FILE` (default `<root>/t2-props`, `name=value` lines) |
| 0003 | `libvendor_bio.so`: `bionic___android_log_*` to stderr/syslog (`BIONIC_LOG_LEVEL`, `BIONIC_LOG_SYSLOG`); auto-loaded with `RTLD_GLOBAL` when the vendor root is set |
| 0004 | `libvendor_bio.so`: `bionic_ashmem_*` on `memfd_create(2)` |
| 0005 | `libvendor_bio.so`: bionic entry points of Android 11..14 that `libc_bio.so` lacks (fdsan, `__cfi_slowpath`, `__register_atfork`, `__cxa_thread_atexit_impl`, `__system_property_wait`, `__openat_2`, `__pread_chk`, `__pwrite_chk`, `__readlink_chk`, `__strlcpy_chk`, `__strlcat_chk`, `android_get_exported_namespace`); `libpthread_bio.so`: `pthread_cond_clockwait`, `pthread_gettid_np`. Without them the fingerprint/gatekeeper HALs load only with `LINKER_DIE_AT_RUNTIME` (`t2-fp -D`) |

Not included: a sixth patch (lazy initialisation of statically initialised
cond/mutex/rwlock/sem in `libpthread_bio.so`): it is broken on the phone and is kept
back until that is understood.

Status: built for aarch64 with 0001–0005 (Alpine pkgrel 105 = 100 + number of patches)
and exercised in a qemu VM with the FocalTech fingerprint and TrustKernel gatekeeper HALs
(titan2-fingerprint).
