/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef MM_MTK_CCCI_IMS_H
#define MM_MTK_CCCI_IMS_H
#include "mm-base-modem.h"
#include "mm-port-mipc.h"
typedef struct _MMMtkCcciIms MMMtkCcciIms;
MMMtkCcciIms *mm_mtk_ccci_ims_new (MMBaseModem *modem, MMPortMipc *port, guint8 slot, const gchar *wwan);
void mm_mtk_ccci_ims_set_radio_owner (MMMtkCcciIms *ims, gboolean owner);
void mm_mtk_ccci_ims_set_profiles (MMMtkCcciIms *ims, const gchar *internet, const gchar *ims_apn, const gchar *sos);
void mm_mtk_ccci_ims_free (MMMtkCcciIms *ims);
void mm_mtk_ccci_ims_stop (MMMtkCcciIms *ims);
void mm_mtk_ccci_ims_setup (MMMtkCcciIms *ims, GAsyncReadyCallback callback, gpointer user_data);
gboolean mm_mtk_ccci_ims_setup_finish (GAsyncResult *res, GError **error);
void mm_mtk_ccci_ims_prepare_data (MMMtkCcciIms *ims, const gchar *apn,
                                  GAsyncReadyCallback callback, gpointer user_data);
gboolean mm_mtk_ccci_ims_prepare_data_finish (GAsyncResult *res, GError **error);
void mm_mtk_ccci_ims_urc (MMMtkCcciIms *ims, const gchar *line);
gboolean mm_mtk_ccci_ims_owns_link (MMMtkCcciIms *ims, guint link_id);
#endif
