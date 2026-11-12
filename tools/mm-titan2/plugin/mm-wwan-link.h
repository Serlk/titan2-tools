/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WWAN link creation over rtnetlink: the equivalent of
 *   ip link add dev <ifname> parentdev <wwan device> type wwan linkid <N>
 *   ip link del dev <ifname>
 * for drivers that register wwan_ops (here the mtk_ccci driver of the MT6878
 * modem, which creates no default link: every data interface is a link).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

#ifndef MM_WWAN_LINK_H
#define MM_WWAN_LINK_H

#include <glib.h>

G_BEGIN_DECLS

/* Creates the link; a link with that name that already exists is accepted
 * (returns TRUE with *out_existed = TRUE) because a crashed daemon may have
 * left it behind. Synchronous: one request and its acknowledgement. */
gboolean mm_wwan_link_add (const gchar  *parent_dev,
                           guint         link_id,
                           const gchar  *ifname,
                           gboolean     *out_existed,
                           GError      **error);

gboolean mm_wwan_link_del (const gchar  *ifname,
                           GError      **error);

/* IMS links: set MTU/up and an optional address; never routes or DNS. */
gboolean mm_wwan_link_configure (const gchar *ifname, guint mtu, const gchar *address, GError **error);

G_END_DECLS

#endif /* MM_WWAN_LINK_H */
