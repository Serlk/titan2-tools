/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 *
 * musl has no <sys/cdefs.h>; the AOSP headers only need the declaration guards.
 * On a libc that has one (glibc, for a build on the host) defer to it.
 */
#ifndef _T2_SYS_CDEFS_H
#define _T2_SYS_CDEFS_H
#if defined(__has_include_next) && __has_include_next(<sys/cdefs.h>)
#include_next <sys/cdefs.h>
#else
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif
#endif
