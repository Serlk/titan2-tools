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
#include <time.h>

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
 * rule above. Entries 0 (NR first, 22/128), 1 (LTE first, 6/4) and 4 (LTE
 * only, 3/0) are the documented pairs for a normal SIM; the LTE-first
 * pair used to be sent as 6/3, which the modem rejected (result 2). */
#define RAT_MODE_NR_FIRST  0
#define RAT_MODE_LTE_FIRST 1
static const RatMode rat_modes[] = {
    { MODES_2345, MM_MODEM_MODE_5G, MM_MTK_CCCI_RAT_STOCK_DEFAULT, MM_MTK_CCCI_RAT_PREFER_STOCK },
    { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_4G,
      MM_MTK_CCCI_RAT_GSM_UMTS_LTE, MM_MTK_CCCI_RAT_PREFER_LTE },
    { MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE, 6, 0 },
    { MM_MODEM_MODE_4G | MM_MODEM_MODE_5G, MM_MODEM_MODE_NONE, 19, 0 },
    { MM_MODEM_MODE_4G,                    MM_MODEM_MODE_NONE,  MM_MTK_CCCI_RAT_LTE_ONLY, 0 },
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
mm_mtk_ccci_rat_default (MMModemMode *allowed, MMModemMode *preferred)
{
    /* the modem's automatic mode (22/128), whatever the generic probing of
     * the AT port reported: that probing only finds 5G when +C5GREG=? or an
     * NR value of +WS46=? answers in time, and an LTE-first default from it
     * left both slots without NR (2026-11-24) */
    *allowed = rat_modes[RAT_MODE_NR_FIRST].allowed;
    *preferred = rat_modes[RAT_MODE_NR_FIRST].preferred;
}

gboolean
mm_mtk_ccci_rat_fallback (MMModemMode mask, guint8 rejected_rat, guint8 rejected_prefer,
                          MMModemMode *allowed, MMModemMode *preferred)
{
    static const guint candidates[] = { RAT_MODE_NR_FIRST, RAT_MODE_LTE_FIRST };
    guint i;

    /* the same mask rule as mm_mtk_ccci_rat_from_modes () */
    mask = (mask & MODES_2345) ? rat_mask_normalize (mask) : MODES_2345;
    for (i = 0; i < G_N_ELEMENTS (candidates); i++) {
        const RatMode *m = &rat_modes[candidates[i]];

        if ((m->allowed & mask) != m->allowed)
            continue;
        /* never the rejected pair again under the name of a fallback */
        if (m->rat == rejected_rat && m->prefer == rejected_prefer)
            continue;
        *allowed = m->allowed;
        *preferred = m->preferred;
        return TRUE;
    }
    return FALSE;
}

gboolean
mm_mtk_ccci_rat_from_modes (MMModemMode mask, MMModemMode allowed, MMModemMode preferred,
                            guint8 *rat, guint8 *prefer)
{
    guint i;

    if (allowed == MM_MODEM_MODE_ANY && preferred == MM_MODEM_MODE_NONE) {
        /* the automatic mode is offered independent of @mask */
        *rat = rat_modes[RAT_MODE_NR_FIRST].rat;
        *prefer = rat_modes[RAT_MODE_NR_FIRST].prefer;
        return TRUE;
    }
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

    /* decoded from the pair alone (no cache): the same values as encoded */
    switch (prefer) {
    case 1:  *preferred = MM_MODEM_MODE_2G; break;
    case 2:  *preferred = MM_MODEM_MODE_3G; break;
    case MM_MTK_CCCI_RAT_PREFER_LTE: *preferred = MM_MODEM_MODE_4G; break;
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

/*****************************************************************************/
/* LTE loss diagnostics */

gchar *
mm_mtk_ccci_trace_stamp (void)
{
    struct timespec mono = { 0 }, boot = { 0 };

    clock_gettime (CLOCK_MONOTONIC, &mono);
    clock_gettime (CLOCK_BOOTTIME, &boot);
    return g_strdup_printf ("mono %ld.%03ld boot %ld.%03ld",
                            (long) mono.tv_sec, (long) (mono.tv_nsec / 1000000),
                            (long) boot.tv_sec, (long) (boot.tv_nsec / 1000000));
}

gchar *
mm_mtk_ccci_at_one_line (const gchar *text)
{
    GString     *s = g_string_new (NULL);
    gboolean     sep = FALSE;
    const gchar *c;

    for (c = text ? text : ""; *c; c++) {
        if (*c == '\r' || *c == '\n') {
            sep = TRUE;
            continue;
        }
        if (sep && s->len)
            g_string_append (s, " | ");
        sep = FALSE;
        g_string_append_c (s, *c);
    }
    return g_string_free (s, FALSE);
}

/* 3GPP TS 27.007 §7.2 (+CREG), §10.1.20 (+CGREG), §10.1.22 (+CEREG): after
 * the <n> of the read response, <stat>, two location fields, <AcT>, then
 * (+CGREG: after <rac>) <cause_type>,<reject_cause> when reporting level 3
 * or higher gives them */
static const struct {
    const gchar *prefix;
    const gchar *domain;
    guint        cause_index;   /* of <cause_type>, counted from <stat> */
} reg_kinds[] = {
    { "+CREG:",  "CS",  4 },
    { "+CGREG:", "PS",  5 },
    { "+CEREG:", "EPS", 4 },
};

/* comma-separated fields, commas inside quotes kept; spaces trimmed */
static GPtrArray *
split_fields (const gchar *s)
{
    GPtrArray *fields = g_ptr_array_new_with_free_func (g_free);
    GString   *cur = g_string_new (NULL);
    gboolean   quoted = FALSE;

    for (; *s && *s != '\r' && *s != '\n'; s++) {
        if (*s == '"')
            quoted = !quoted;
        if (*s == ',' && !quoted) {
            g_ptr_array_add (fields, g_strstrip (g_string_free (cur, FALSE)));
            cur = g_string_new (NULL);
            continue;
        }
        g_string_append_c (cur, *s);
    }
    g_ptr_array_add (fields, g_strstrip (g_string_free (cur, FALSE)));
    return fields;
}

static gint
field_uint (GPtrArray *fields, guint i)
{
    guint64 v;

    if (i >= fields->len || !*(const gchar *) g_ptr_array_index (fields, i) ||
        !g_ascii_string_to_unsigned (g_ptr_array_index (fields, i), 10, 0, G_MAXINT, &v, NULL))
        return -1;
    return (gint) v;
}

gboolean
mm_mtk_ccci_reg_parse (const gchar *line, gboolean solicited, MMMtkCcciReg *reg)
{
    g_autoptr(GPtrArray) fields = NULL;
    guint                i, first;

    memset (reg, 0, sizeof (*reg));
    if (!line)
        return FALSE;
    while (g_ascii_isspace (*line))
        line++;
    for (i = 0; i < G_N_ELEMENTS (reg_kinds); i++)
        if (g_str_has_prefix (line, reg_kinds[i].prefix))
            break;
    if (i == G_N_ELEMENTS (reg_kinds))
        return FALSE;
    fields = split_fields (line + strlen (reg_kinds[i].prefix));
    first = solicited ? 1 : 0;
    reg->domain = reg_kinds[i].domain;
    reg->n = solicited ? field_uint (fields, 0) : -1;
    reg->stat = field_uint (fields, first);
    if (reg->stat < 0 || (solicited && reg->n < 0))
        return FALSE;
    reg->act = field_uint (fields, first + 3);
    reg->cause_type = field_uint (fields, first + reg_kinds[i].cause_index);
    reg->reject_cause = field_uint (fields, first + reg_kinds[i].cause_index + 1);
    return TRUE;
}

static const gchar *
reg_stat_name (gint stat)
{
    static const gchar *names[] = { "not registered", "home", "searching", "denied", "unknown", "roaming" };

    return stat >= 0 && stat < (gint) G_N_ELEMENTS (names) ? names[stat] : "other";
}

gchar *
mm_mtk_ccci_reg_describe (const MMMtkCcciReg *reg)
{
    GString *s = g_string_new (NULL);

    g_string_append_printf (s, "%s stat %d (%s)", reg->domain, reg->stat, reg_stat_name (reg->stat));
    if (reg->act >= 0)
        g_string_append_printf (s, " AcT %d", reg->act);
    else
        g_string_append (s, " AcT not reported");
    if (reg->cause_type >= 0 || reg->reject_cause >= 0)
        g_string_append_printf (s, ", reject cause type %d code %d", reg->cause_type, reg->reject_cause);
    else
        g_string_append (s, ", reject cause not reported");
    if (reg->n >= 0)
        g_string_append_printf (s, " (n %d)", reg->n);
    return g_string_free (s, FALSE);
}

gchar *
mm_mtk_ccci_reg_describe_text (const gchar *text, gboolean solicited)
{
    g_auto(GStrv) lines = NULL;
    GString      *s = NULL;
    guint         i;

    if (!text)
        return NULL;
    lines = g_strsplit_set (text, "\r\n", -1);
    for (i = 0; lines[i]; i++) {
        MMMtkCcciReg      reg;
        g_autofree gchar *d = NULL;

        if (!mm_mtk_ccci_reg_parse (lines[i], solicited, &reg))
            continue;
        d = mm_mtk_ccci_reg_describe (&reg);
        if (!s)
            s = g_string_new (d);
        else
            g_string_append_printf (s, "; %s", d);
    }
    return s ? g_string_free (s, FALSE) : NULL;
}

/*****************************************************************************/
/* Registration lines into ModemManager's 3GPP state: the generic parser and
 * regular expressions, as the generic code applies them to its own port */

gboolean
mm_mtk_ccci_reg_update_parse (const gchar *line, gboolean solicited, MMMtkCcciRegUpdate *u)
{
    static GPtrArray *regexes[2];
    g_autofree gchar *text = NULL;
    guint             i;

    memset (u, 0, sizeof (*u));
    if (!line)
        return FALSE;
    while (g_ascii_isspace (*line))
        line++;
    if (!g_str_has_prefix (line, "+CREG:") && !g_str_has_prefix (line, "+CGREG:") &&
        !g_str_has_prefix (line, "+CEREG:") && !g_str_has_prefix (line, "+C5GREG:"))
        return FALSE;
    /* the level 3 form (cause fields) as the generic n <= 2 expressions read it */
    text = mm_mtk_ccci_reg_strip_cause (line, solicited);
    if (!text)   /* +C5GREG: as it is */
        text = g_strndup (line, strcspn (line, "\r\n"));
    g_strchomp (text);
    if (!solicited) {
        /* the unsolicited expressions want the line in its CR LF frame */
        gchar *framed = g_strdup_printf ("\r\n%s\r\n", text);
        g_free (text);
        text = framed;
    }
    if (!regexes[solicited ? 1 : 0])
        regexes[solicited ? 1 : 0] = mm_3gpp_creg_regex_get (solicited);
    for (i = 0; i < regexes[solicited ? 1 : 0]->len; i++) {
        g_autoptr(GMatchInfo) match = NULL;
        gulong                lac = 0, ci = 0;

        if (!g_regex_match (g_ptr_array_index (regexes[solicited ? 1 : 0], i), text, 0, &match))
            continue;
        if (!mm_3gpp_parse_creg_response (match, NULL, &u->state, &lac, &ci, &u->act,
                                          &u->cgreg, &u->cereg, &u->c5greg, NULL))
            return FALSE;
        /* TS 27.007: +CEREG/+C5GREG, and +CREG with LTE AcT, carry the TAC */
        if (u->cereg || u->c5greg || (!u->cgreg && u->act == MM_MODEM_ACCESS_TECHNOLOGY_LTE))
            u->tac = lac;
        else
            u->lac = lac;
        u->ci = ci;
        return TRUE;
    }
    return FALSE;
}

gboolean
mm_mtk_ccci_cops_act (const gchar *reply, MMModemAccessTechnology *act)
{
    g_auto(GStrv) lines = NULL;
    guint         i;

    *act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    if (!reply)
        return FALSE;
    lines = g_strsplit_set (reply, "\r\n", -1);
    for (i = 0; lines[i]; i++) {
        guint mode = 0, format = 0;
        g_autofree gchar *op = NULL;

        if (!g_str_has_prefix (g_strstrip (lines[i]), "+COPS:"))
            continue;
        if (mm_3gpp_parse_cops_read_response (lines[i], &mode, &format, &op, act, NULL, NULL))
            return *act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    }
    return FALSE;
}

/*****************************************************************************/
/* Registration reporting with causes */

gboolean
mm_mtk_ccci_test_range_has (const gchar *reply, const gchar *prefix, guint n)
{
    const gchar *p, *end;
    g_auto(GStrv) items = NULL;
    g_autofree gchar *list = NULL;
    guint         i;

    if (!reply || !prefix || !(p = strstr (reply, prefix)) || !(p = strchr (p, '(')) || !(end = strchr (p, ')')))
        return FALSE;
    list = g_strndup (p + 1, end - p - 1);
    items = g_strsplit (list, ",", -1);
    for (i = 0; items[i]; i++) {
        guint64      lo, hi;
        gchar       *dash = strchr (items[i], '-');

        g_strstrip (items[i]);
        if (dash) {
            *dash = 0;
            if (!g_ascii_string_to_unsigned (g_strstrip (items[i]), 10, 0, G_MAXUINT, &lo, NULL) ||
                !g_ascii_string_to_unsigned (g_strstrip (dash + 1), 10, 0, G_MAXUINT, &hi, NULL))
                continue;
        } else if (g_ascii_string_to_unsigned (items[i], 10, 0, G_MAXUINT, &lo, NULL))
            hi = lo;
        else
            continue;
        if (n >= lo && n <= hi)
            return TRUE;
    }
    return FALSE;
}

gchar *
mm_mtk_ccci_reg_strip_cause (const gchar *line, gboolean solicited)
{
    g_autoptr(GPtrArray) fields = NULL;
    GString             *s;
    guint                i, keep;

    if (!line)
        return NULL;
    while (g_ascii_isspace (*line))
        line++;
    for (i = 0; i < G_N_ELEMENTS (reg_kinds); i++)
        if (g_str_has_prefix (line, reg_kinds[i].prefix))
            break;
    if (i == G_N_ELEMENTS (reg_kinds))
        return NULL;
    fields = split_fields (line + strlen (reg_kinds[i].prefix));
    /* <n>, <stat>, two location fields, <AcT> (+CGREG: and <rac>) */
    keep = (solicited ? 1 : 0) + reg_kinds[i].cause_index;
    if (fields->len <= keep)
        return g_strndup (line, strcspn (line, "\r\n"));
    g_ptr_array_set_size (fields, keep);
    /* level 3 with nothing registered: empty location/AcT fields */
    while (fields->len > (solicited ? 2u : 1u) && !*(const gchar *) g_ptr_array_index (fields, fields->len - 1))
        g_ptr_array_set_size (fields, fields->len - 1);
    s = g_string_new (reg_kinds[i].prefix);
    g_string_append_c (s, ' ');
    for (i = 0; i < fields->len; i++)
        g_string_append_printf (s, "%s%s", i ? "," : "", (const gchar *) g_ptr_array_index (fields, i));
    return g_string_free (s, FALSE);
}

/*****************************************************************************/
/* Network diagnostics decoders */

gboolean
mm_mtk_ccci_nw_block_parse (const guint8 *p, gsize len, MMMtkCcciNwBlock *b)
{
    memset (b, 0, sizeof (*b));
    if (!p || len < 24)
        return FALSE;
    b->stat = p[0];
    memcpy (b->plmn, p + 1, 7);
    b->plmn[7] = 0;
    b->cell = mm_mipc_get (p + 8, 4);
    b->earfcn = mm_mipc_get (p + 12, 4);
    b->pci = mm_mipc_get (p + 16, 4);
    b->tac = mm_mipc_get (p + 20, 4);
    return TRUE;
}

gchar *
mm_mtk_ccci_nw_block_describe (const MMMtkCcciNwBlock *b)
{
    g_autofree gchar *plmn = g_strndup (b->plmn, 7);
    guint             i;

    /* printable PLMN only */
    for (i = 0; plmn[i]; i++)
        if (!g_ascii_isprint (plmn[i]))
            plmn[i] = '?';
    return g_strdup_printf ("stat %u, PLMN '%s', cell 0x%08X, TAC 0x%04X, EARFCN %u (inferred offset), PCI %u (inferred offset)",
                            b->stat, plmn, b->cell, b->tac, b->earfcn, b->pci);
}

gchar *
mm_mtk_ccci_cscon_describe (const guint8 *v, gsize len)
{
    GString *s = g_string_new (NULL);
    gsize    i;

    for (i = 0; v && i < len; i++)
        g_string_append_printf (s, "%s%02x", i ? " " : "", v[i]);
    if (!v || !len)
        g_string_append (s, "(absent)");
    else if (v[0] == 1)
        g_string_append (s, " (byte 0 = 1: connected, as the documented LTE example 01 07 04 00)");
    else if (v[0] == 0)
        g_string_append (s, " (byte 0 = 0: idle, as the documented examples 00 ff ..)");
    else
        g_string_append (s, " (byte 0 not in the documented examples)");
    return g_string_free (s, FALSE);
}
