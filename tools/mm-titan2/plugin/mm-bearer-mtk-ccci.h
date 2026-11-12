/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMBearerMtkCcci: a mobile-data bearer of the MT6878 integrated modem. The
 * PDN is activated with a MIPC DATA_ACT_CALL request on the MIPC port; the
 * modem names the data channel (transIntfId), the bearer creates the wwan link
 * netdev "<wwan device>-<transIntfId mod 100>" for it and reports a static
 * IPv4 configuration to the connection manager.
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

#ifndef MM_BEARER_MTK_CCCI_H
#define MM_BEARER_MTK_CCCI_H

#include <glib.h>
#include <glib-object.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-bearer.h"
#include "mm-broadband-modem-mtk-ccci.h"

#define MM_TYPE_BEARER_MTK_CCCI            (mm_bearer_mtk_ccci_get_type ())
#define MM_BEARER_MTK_CCCI(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BEARER_MTK_CCCI, MMBearerMtkCcci))
#define MM_BEARER_MTK_CCCI_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BEARER_MTK_CCCI, MMBearerMtkCcciClass))
#define MM_IS_BEARER_MTK_CCCI(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BEARER_MTK_CCCI))
#define MM_IS_BEARER_MTK_CCCI_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BEARER_MTK_CCCI))
#define MM_BEARER_MTK_CCCI_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BEARER_MTK_CCCI, MMBearerMtkCcciClass))

typedef struct _MMBearerMtkCcci MMBearerMtkCcci;
typedef struct _MMBearerMtkCcciClass MMBearerMtkCcciClass;
typedef struct _MMBearerMtkCcciPrivate MMBearerMtkCcciPrivate;

struct _MMBearerMtkCcci {
    MMBaseBearer parent;
    MMBearerMtkCcciPrivate *priv;
};

struct _MMBearerMtkCcciClass {
    MMBaseBearerClass parent;
};

GType mm_bearer_mtk_ccci_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMBearerMtkCcci, g_object_unref)

MMBaseBearer *mm_bearer_mtk_ccci_new (MMBroadbandModemMtkCcci *modem,
                                      MMBearerProperties      *config);

#endif /* MM_BEARER_MTK_CCCI_H */
