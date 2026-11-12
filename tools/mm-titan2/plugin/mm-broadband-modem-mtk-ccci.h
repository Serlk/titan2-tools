/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMBroadbandModemMtkCcci: the MediaTek MT6878 integrated modem (Unihertz
 * Titan 2) behind the mtk_ccci WWAN driver. 3GPP control is AT on wwan0at0
 * with the MediaTek specifics (AT+EFUN radio power, AT+ESUO stack select,
 * +ECPI/+EAIC/+ESPEECH voice reports); mobile data is a MIPC data call on
 * wwan0mipc1 (MMPortMipc) carried by a wwan link netdev (MMBearerMtkCcci).
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

#ifndef MM_BROADBAND_MODEM_MTK_CCCI_H
#define MM_BROADBAND_MODEM_MTK_CCCI_H

#include "mm-broadband-modem.h"
#include "mm-port-mipc.h"

#define MM_TYPE_BROADBAND_MODEM_MTK_CCCI            (mm_broadband_modem_mtk_ccci_get_type ())
#define MM_BROADBAND_MODEM_MTK_CCCI(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_MTK_CCCI, MMBroadbandModemMtkCcci))
#define MM_BROADBAND_MODEM_MTK_CCCI_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_MODEM_MTK_CCCI, MMBroadbandModemMtkCcciClass))
#define MM_IS_BROADBAND_MODEM_MTK_CCCI(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_MTK_CCCI))
#define MM_IS_BROADBAND_MODEM_MTK_CCCI_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_MODEM_MTK_CCCI))
#define MM_BROADBAND_MODEM_MTK_CCCI_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_MODEM_MTK_CCCI, MMBroadbandModemMtkCcciClass))

/* udev properties read from the AT port's kernel device (set in the rules file) */
#define ID_MM_MTK_CCCI_RADIO_MASK "ID_MM_MTK_CCCI_RADIO_MASK" /* AT+EFUN=<mask>: 1 SIM1, 2 SIM2, 3 both (default 3) */
#define ID_MM_MTK_CCCI_SIM_SLOT   "ID_MM_MTK_CCCI_SIM_SLOT"   /* protocol stack on the AT port and MIPC slot: 1 or 2 (default 1) */
#define ID_MM_MTK_CCCI_MIPC_DATA  "ID_MM_MTK_CCCI_MIPC_DATA"  /* the MIPC port used for data calls */

typedef struct _MMBroadbandModemMtkCcci MMBroadbandModemMtkCcci;
typedef struct _MMBroadbandModemMtkCcciClass MMBroadbandModemMtkCcciClass;
typedef struct _MMBroadbandModemMtkCcciPrivate MMBroadbandModemMtkCcciPrivate;

struct _MMBroadbandModemMtkCcci {
    MMBroadbandModem parent;
    MMBroadbandModemMtkCcciPrivate *priv;
};

struct _MMBroadbandModemMtkCcciClass {
    MMBroadbandModemClass parent;
};

GType mm_broadband_modem_mtk_ccci_get_type (void);

MMBroadbandModemMtkCcci *mm_broadband_modem_mtk_ccci_new (const gchar  *device,
                                                          const gchar  *physdev,
                                                          const gchar **drivers,
                                                          const gchar  *plugin,
                                                          guint16       vendor_id,
                                                          guint16       product_id);

/* the MIPC data port (NULL when the modem has none); not referenced */
MMPortMipc *mm_broadband_modem_mtk_ccci_peek_port_mipc (MMBroadbandModemMtkCcci *self);

/* MIPC header slot bit of the selected SIM stack (0x01 SIM1, 0x02 SIM2) */
guint8 mm_broadband_modem_mtk_ccci_get_mipc_slot (MMBroadbandModemMtkCcci *self);

/* kernel name of the WWAN device the ports belong to ("wwan0"): the parent of
 * the data links */
const gchar *mm_broadband_modem_mtk_ccci_get_wwan_device (MMBroadbandModemMtkCcci *self);

gboolean mm_broadband_modem_mtk_ccci_ims_owns_link (MMBroadbandModemMtkCcci *self, guint link_id);

/* Synchronize the selected SIM's internet APN with the connection manager. */
void mm_broadband_modem_mtk_ccci_prepare_data (MMBroadbandModemMtkCcci *self, const gchar *apn,
                                             GAsyncReadyCallback callback, gpointer user_data);
gboolean mm_broadband_modem_mtk_ccci_prepare_data_finish (MMBroadbandModemMtkCcci *self,
                                                        GAsyncResult *res, GError **error);

#endif /* MM_BROADBAND_MODEM_MTK_CCCI_H */
