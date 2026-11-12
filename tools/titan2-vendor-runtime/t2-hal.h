/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * t2-hal: loading a legacy Android HAL module (hw_module_t) in-process through
 * bionic_translation, and the re-exec into the bionic_translation vendor
 * profile. Used by t2-fp/t2-fpd (titan2-fingerprint) and t2-gnssd
 * (titan2-gnss).
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#ifndef T2_HAL_H
#define T2_HAL_H

#include <stddef.h>
#include <stdint.h>
#include <hardware/hardware.h>

#define T2_DEFAULT_ROOT   "/var/lib/titan2-vendor"

/* log prefix of the calling program ("t2-fp", "t2-gnssd", ...) */
extern const char *t2_prog;

uint64_t t2_now_ms(void);
void t2_mkdirs(const char *path, unsigned mode);

/*
 * libdl_bio reads BIONIC_VENDOR_ROOT in a constructor, before main(): put the
 * profile into the environment and exec ourselves again. Returns only when the
 * environment already matches (or -1 when the exec failed).
 */
int t2_vendor_reexec(const char *root, int diag, char **argv);

/* Load a legacy HAL module through bionic_translation and open device "id".
 * Returns the dlopen handle (never closed); *devp receives the device. */
void *t2_open_hal(const char *lib, const char *id, struct hw_device_t **devp);

#endif
