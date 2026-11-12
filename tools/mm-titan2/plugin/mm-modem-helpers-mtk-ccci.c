/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Response helpers of the mtk-ccci plugin.
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

#include <string.h>

#include "mm-modem-helpers-mtk-ccci.h"
#include "mm-mipc-proto.h"

gchar *
mm_mtk_ccci_clcc_normalize (const gchar *response)
{
    g_autoptr(GRegex) regex = NULL;
    gchar            *out;

    if (!response)
        return NULL;

    /* +CLCC: <id>,<dir>,<stat>,<mode>, ... with <mode> 20..25; observed on the
     * phone: "+CLCC: 1,0,2,20,0,"+7...",145" for a dialing VoLTE call */
    regex = g_regex_new ("(\\+CLCC:\\s*\\d+,\\s*\\d+,\\s*\\d+,\\s*)(2[0-5])(\\s*,)",
                         G_REGEX_RAW | G_REGEX_MULTILINE, 0, NULL);
    g_assert (regex);
    out = g_regex_replace (regex, response, -1, 0, "\\g<1>0\\g<3>", 0, NULL);
    return out ? out : g_strdup (response);
}

/* speechd is a stream, not one read per response. */
gint
mm_mtk_ccci_speech_reply_feed (gchar **reply, const guint8 *data, gsize len, GError **error)
{
    gsize previous = *reply ? strlen (*reply) : 0;
    if (len > 4096 || previous > 4096 - len || (len && memchr (data, 0, len))) {
        g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "Invalid or oversized speechd reply");
        return -1;
    }
    *reply = g_realloc (*reply, previous + len + 1);
    if (len)
        memcpy (*reply + previous, data, len);
    (*reply)[previous + len] = 0;
    if (len && (g_str_equal (*reply, "ok\n") || g_str_has_suffix (*reply, "\nok\n")))
        return 1;
    if (!len || strstr (*reply, "error:")) {
        g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                     "speechd: %s", (*reply)[0] ? *reply : "no reply");
        return -1;
    }
    return 0;
}

guint mm_mtk_ccci_call_index (guint8 slot, guint id)
{
    if ((slot != 1 && slot != 2) || id < 1 || id > 7)
        return 0;
    return id + (slot == 2 ? 100 : 0);
}

gboolean mm_mtk_ccci_call_parse (guint8 slot, const guint8 *payload, gsize len, MMCallInfo *info)
{
    guint32 id, event, dir;
    memset (info, 0, sizeof (*info));
    if (!mm_mipc_tlv_find_uint (payload, len, 0x0100, &id) ||
        !mm_mipc_tlv_find_uint (payload, len, 0x0109, &event) ||
        !(info->index = mm_mtk_ccci_call_index (slot, id)))
        return FALSE;
    if (mm_mipc_tlv_find_uint (payload, len, 0x0101, &dir) && dir <= 1)
        info->direction = dir ? MM_CALL_DIRECTION_INCOMING : MM_CALL_DIRECTION_OUTGOING;
    switch (event) {
    case 0: info->state = MM_CALL_STATE_RINGING_IN; info->direction = MM_CALL_DIRECTION_INCOMING; break;
    case 2: info->state = MM_CALL_STATE_RINGING_OUT; info->direction = MM_CALL_DIRECTION_OUTGOING; break;
    case 6: info->state = MM_CALL_STATE_ACTIVE; break;
    case 130: info->state = MM_CALL_STATE_DIALING; info->direction = MM_CALL_DIRECTION_OUTGOING; break;
    case 133: info->state = MM_CALL_STATE_TERMINATED; break;
    default: return FALSE;
    }
    info->number = mm_mipc_tlv_find_str (payload, len, 0x8104);
    return TRUE;
}

/*****************************************************************************/
/* RAT mode */

#define RAT_BIT_GSM  0x01
#define RAT_BIT_UMTS 0x02
#define RAT_BIT_LTE  0x04
#define RAT_BIT_C2K  0x08
#define RAT_BIT_NR   0x10

#define MODES_2345 (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G)

typedef struct {
    MMModemMode allowed;
    MMModemMode preferred;
    guint8      rat;
    guint8      prefer;
} RatMode;

/* offered combinations, most useful first; the rat values follow the mask
 * rule above, the first entry is the captured factory-firmware one */
static const RatMode rat_modes[] = {
    { MODES_2345, MM_MODEM_MODE_5G, MM_MTK_CCCI_RAT_STOCK_DEFAULT, MM_MTK_CCCI_RAT_PREFER_STOCK },
    { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_4G,   6, 3 },
    { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE, 6, 0 },
    { MM_MODEM_MODE_4G | MM_MODEM_MODE_5G, MM_MODEM_MODE_NONE, 19, 0 },
    { MM_MODEM_MODE_4G,                    MM_MODEM_MODE_NONE,  3, 0 },
    { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, MM_MODEM_MODE_3G,    2, 2 },
    { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, MM_MODEM_MODE_NONE,  2, 0 },
    { MM_MODEM_MODE_3G,                    MM_MODEM_MODE_NONE,  1, 0 },
    { MM_MODEM_MODE_2G,                    MM_MODEM_MODE_NONE,  0, 0 },
};

static MMModemMode
rat_mask_normalize (MMModemMode mask)
{
    mask &= MODES_2345;
    /* nothing usable reported: the 2G/3G/4G modem every MT6878 is */
    return mask ? mask : (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G);
}

GArray *
mm_mtk_ccci_rat_build_combinations (MMModemMode mask)
{
    GArray *combinations = g_array_new (FALSE, FALSE, sizeof (MMModemModeCombination));
    guint   i;

    mask = rat_mask_normalize (mask);
    for (i = 0; i < G_N_ELEMENTS (rat_modes); i++) {
        MMModemModeCombination mode;

        if ((rat_modes[i].allowed & mask) != rat_modes[i].allowed)
            continue;
        mode.allowed = rat_modes[i].allowed;
        mode.preferred = rat_modes[i].preferred;
        g_array_append_val (combinations, mode);
    }
    return combinations;
}

void
mm_mtk_ccci_rat_default (MMModemMode mask, MMModemMode *allowed, MMModemMode *preferred)
{
    if (!(mask & MODES_2345) || (mask & MM_MODEM_MODE_5G)) {
        *allowed = rat_modes[0].allowed;
        *preferred = rat_modes[0].preferred;
    } else {
        *allowed = rat_modes[1].allowed;
        *preferred = rat_modes[1].preferred;
    }
}

gboolean
mm_mtk_ccci_rat_from_modes (MMModemMode mask, MMModemMode allowed, MMModemMode preferred,
                            guint8 *rat, guint8 *prefer)
{
    guint i;

    if (allowed == MM_MODEM_MODE_ANY && preferred == MM_MODEM_MODE_NONE)
        mm_mtk_ccci_rat_default (mask, &allowed, &preferred);
    if (mask & MODES_2345)
        mask = rat_mask_normalize (mask);
    else
        mask = MODES_2345;   /* not probed yet */
    for (i = 0; i < G_N_ELEMENTS (rat_modes); i++) {
        if (rat_modes[i].allowed != allowed || rat_modes[i].preferred != preferred ||
            (allowed & mask) != allowed)
            continue;
        *rat = rat_modes[i].rat;
        *prefer = rat_modes[i].prefer;
        return TRUE;
    }
    return FALSE;
}

gboolean
mm_mtk_ccci_rat_to_modes (guint rat, guint prefer, MMModemMode *allowed, MMModemMode *preferred)
{
    guint bits;

    if (rat > 30)
        return FALSE;
    bits = (rat + 1) & ~RAT_BIT_C2K;   /* C2K has no MM mode of its own here */
    *allowed = MM_MODEM_MODE_NONE;
    if (bits & RAT_BIT_GSM)
        *allowed |= MM_MODEM_MODE_2G;
    if (bits & RAT_BIT_UMTS)
        *allowed |= MM_MODEM_MODE_3G;
    if (bits & RAT_BIT_LTE)
        *allowed |= MM_MODEM_MODE_4G;
    if (bits & RAT_BIT_NR)
        *allowed |= MM_MODEM_MODE_5G;
    if (*allowed == MM_MODEM_MODE_NONE)
        return FALSE;

    switch (prefer) {
    case 1:  *preferred = MM_MODEM_MODE_2G; break;
    case 2:  *preferred = MM_MODEM_MODE_3G; break;
    case 3:  *preferred = MM_MODEM_MODE_4G; break;
    case MM_MTK_CCCI_RAT_PREFER_STOCK:
        *preferred = (*allowed & MM_MODEM_MODE_5G) ? MM_MODEM_MODE_5G : MM_MODEM_MODE_4G;
        break;
    default: *preferred = MM_MODEM_MODE_NONE; break;
    }
    /* a preference outside the allowed set or a lone allowed mode: none */
    if (!(*preferred & *allowed) || *preferred == *allowed)
        *preferred = MM_MODEM_MODE_NONE;
    return TRUE;
}

gboolean
mm_mtk_ccci_parse_erat (const gchar *reply, guint *curr_rat, guint *rat, guint *prefer)
{
    g_autoptr(GRegex)     regex = NULL;
    g_autoptr(GMatchInfo) match = NULL;

    if (!reply)
        return FALSE;
    regex = g_regex_new ("\\+ERAT:\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)",
                         G_REGEX_RAW, 0, NULL);
    g_assert (regex);
    if (!g_regex_match (regex, reply, 0, &match))
        return FALSE;
    return mm_get_uint_from_match_info (match, 1, curr_rat) &&
           mm_get_uint_from_match_info (match, 3, rat) &&
           mm_get_uint_from_match_info (match, 4, prefer);
}
