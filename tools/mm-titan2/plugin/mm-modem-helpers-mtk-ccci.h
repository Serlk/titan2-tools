/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Response helpers of the mtk-ccci plugin (pure string handling, unit tested).
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

#ifndef MM_MODEM_HELPERS_MTK_CCCI_H
#define MM_MODEM_HELPERS_MTK_CCCI_H

#include <glib.h>
#include "mm-modem-helpers.h"

G_BEGIN_DECLS

/* +CLCC <mode> values of IMS (VoLTE) calls on this modem:
 * 20 voice, 21 video, 22/23 conference host, 24/25 participant */
#define MM_MTK_CCCI_CLCC_MODE_IMS_FIRST 20
#define MM_MTK_CCCI_CLCC_MODE_IMS_LAST  25

/* Rewrites the <mode> field of every "+CLCC: <id>,<dir>,<stat>,<mode>,..."
 * line from 20..25 to 0 so that ModemManager's generic +CLCC parser, which
 * accepts voice calls (mode 0) only, lists the IMS calls too. Everything else
 * is returned unchanged. Never returns NULL for a non-NULL input. */
gchar *mm_mtk_ccci_clcc_normalize (const gchar *response);

/* Append one UNIX-stream fragment. 0: wait, 1: final ok, -1: error/EOF/limit. */
gint mm_mtk_ccci_speech_reply_feed (gchar **reply, const guint8 *data, gsize len, GError **error);

/* Index namespace: SIM1 1..7; SIM2 101..107. Wire ids are never modified. */
guint mm_mtk_ccci_call_index (guint8 slot, guint id);
gboolean mm_mtk_ccci_call_parse (guint8 slot, const guint8 *payload, gsize len, MMCallInfo *info);

/* RAT mode: the <rat>,<prefer_rat> pair of
 * AT+ERAT / MIPC NW_SET_RAT_REQ. <rat> is a RAT bit mask minus one with
 * GSM 1, UMTS 2, LTE 4, C2K 8, NR 16 (0..6 as in ModemManager's mtk-legacy
 * plugin; 22 = GSM|UMTS|LTE|NR is what the factory firmware's RIL sends with Android's
 * allowed types GSM|UMTS|LTE|NR). <prefer_rat> 0 none, 1 GSM, 2 UMTS, 3 LTE
 * (mtk-legacy); 128 is the factory value sent with 22 (meaning unverified; we
 * call it "NR first" like Android's NR/LTE/WCDMA/GSM mode). */
#define MM_MTK_CCCI_RAT_STOCK_DEFAULT     22
#define MM_MTK_CCCI_RAT_PREFER_NONE       0
#define MM_MTK_CCCI_RAT_PREFER_STOCK      128

/* the mode combinations offered for the modes the modem reports (@mask:
 * the union of 2G/3G/4G/5G from the generic probing) */
GArray  *mm_mtk_ccci_rat_build_combinations (MMModemMode mask);
/* the factory-like default: NR first when 5G is in @mask (or @mask is NONE:
 * not known yet), else LTE first */
void     mm_mtk_ccci_rat_default (MMModemMode mask, MMModemMode *allowed, MMModemMode *preferred);
/* MM combination -> rat/prefer; ANY/NONE gives the default; FALSE when
 * the combination is not one of the offered ones */
gboolean mm_mtk_ccci_rat_from_modes (MMModemMode mask, MMModemMode allowed, MMModemMode preferred,
                                     guint8 *rat, guint8 *prefer);
/* rat/prefer -> MM modes; FALSE for an impossible rat */
gboolean mm_mtk_ccci_rat_to_modes (guint rat, guint prefer, MMModemMode *allowed, MMModemMode *preferred);
/* "+ERAT: <curr_rat>,<gprs_status>,<rat>,<prefer_rat>" (anywhere in @reply) */
gboolean mm_mtk_ccci_parse_erat (const gchar *reply, guint *curr_rat, guint *rat, guint *prefer);

G_END_DECLS

#endif /* MM_MODEM_HELPERS_MTK_CCCI_H */
