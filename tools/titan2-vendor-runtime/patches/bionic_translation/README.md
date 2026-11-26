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
| 0006 | `libpthread_bio.so`: lazy host-object init of statically initialized cond/mutex/rwlock/sem under one lock with re-check and release publish (before: two threads at first use could map two host objects → lost wakeups / no mutual exclusion; half-initialized mutex visible) |
| 0007 | `libpthread_bio.so`: `bionic_sem_getvalue`. Without it a bionic library's `sem_getvalue` was bound to the host libc and read the count from the bionic object, where the wrappers keep a pointer to the host semaphore: an empty semaphore read as positive, a caller that posts only while the count is below one never posted, its waiter never woke (a fingerprint touch never reached the HAL's worker thread). `semaphore.sem_getvalue` added to the expected-to-pass list of the bionic test suite |
| 0008 | `libpthread_bio.so`: the lazy init of 0006 published the host pointer with a store-release; bionic's objects are only int-aligned, and on AArch64 without FEAT_LSE2 that instruction faults at a 4-but-not-8-byte aligned address (SIGBUS at the first use of such a statically initialized mutex/cond/rwlock/sem). Now a full barrier and a plain store. On the phone `t2-fpd` died with SIGBUS at every start with 0001–0007 and runs with 0001–0008 |

Status: built for aarch64 with 0001–0008 (Alpine pkgrel 108 = 100 + number of patches) and
exercised on the phone and in a qemu VM with the FocalTech fingerprint and TrustKernel
gatekeeper HALs (titan2-fingerprint).

## Tests

`test/run.sh TREE...` builds every `test/*-test.c` together with the tree's
`pthread_wrapper/libpthread.c` and runs it on aarch64 under qemu with `QEMU_CPU=cortex-a76`
(no FEAT_LSE2, like the Titan 2's cores; wrapper logic only): `pthread-lazy-test.c` (0006,
0008: part 3 dies with SIGBUS with 0006 alone and passes with 0008), `sem-getvalue-test.c`
(0007; does not build against a tree without the entry point, which is the "before" result).
It needs the aarch64 buildroot of `scripts/build-atl.sh` in `out/atl`. The route a bionic
library's import takes through the loader is covered by `t2-fp-semtest` in titan2-fingerprint.

## Version against Alpine's own package

The rebuild keeps Alpine's `pkgver` (the upstream snapshot the recipe is pinned to) and only
raises `pkgrel` to 100 + the number of patches. A newer snapshot in Alpine edge/testing therefore
sorts above ours although it has none of these patches. Packages that need the patched loader
pin it exactly: titan2-fingerprint depends on `bionic_translation=0_git20260317-r108`, so apk
can neither pick the edge package for it nor upgrade past it while it is installed.
