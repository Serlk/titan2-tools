/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIPC framing and TLV coding. Written from a clean-room behavioural description of the protocol; no code of the phone's Android software was used.
 * Ported to GLib from tools/modemd/src/mipc.c.
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

#include "mm-mipc-proto.h"

/*****************************************************************************/
/* bytes (little-endian) */

void
mm_mipc_put (guint8 *p, gsize size, guint32 v)
{
    gsize i;

    for (i = 0; i < size; i++)
        p[i] = (guint8) (i < 4 ? v >> (8 * i) : 0);
}

guint32
mm_mipc_get (const guint8 *p, gsize size)
{
    guint32 v = 0;
    gsize   i;

    for (i = 0; i < size && i < 4; i++)
        v |= (guint32) p[i] << (8 * i);
    return v;
}

/*****************************************************************************/
/* TLV builder */

gboolean
mm_mipc_tlv_add (GByteArray *b, guint16 id, gconstpointer val, gsize len)
{
    gsize  need;
    guint  old_len;
    guint8 hdr[MM_MIPC_TLV_HDR];

    g_return_val_if_fail (b != NULL, FALSE);
    g_return_val_if_fail (val != NULL, FALSE);

    /* len 0 is an array descriptor: not built here */
    if (len == 0 || len > 0xFFFF)
        return FALSE;
    need = MM_MIPC_TLV_SIZE (len);
    if (b->len + need > MM_MIPC_PAYLOAD_MAX)
        return FALSE;

    old_len = b->len;
    mm_mipc_put (hdr, 2, id);
    mm_mipc_put (hdr + 2, 2, (guint32) len);
    g_byte_array_append (b, hdr, MM_MIPC_TLV_HDR);
    g_byte_array_append (b, val, len);
    /* zero padding to the 8-byte boundary */
    g_byte_array_set_size (b, old_len + need);
    memset (b->data + old_len + MM_MIPC_TLV_HDR + len, 0, need - MM_MIPC_TLV_HDR - len);
    return TRUE;
}

gboolean
mm_mipc_tlv_add_u8 (GByteArray *b, guint16 id, guint8 v)
{
    return mm_mipc_tlv_add (b, id, &v, 1);
}

gboolean
mm_mipc_tlv_add_u16 (GByteArray *b, guint16 id, guint16 v)
{
    guint8 t[2];

    mm_mipc_put (t, 2, v);
    return mm_mipc_tlv_add (b, id, t, 2);
}

gboolean
mm_mipc_tlv_add_u32 (GByteArray *b, guint16 id, guint32 v)
{
    guint8 t[4];

    mm_mipc_put (t, 4, v);
    return mm_mipc_tlv_add (b, id, t, 4);
}

gboolean
mm_mipc_tlv_add_str (GByteArray *b, guint16 id, const gchar *s)
{
    g_return_val_if_fail (s != NULL, FALSE);
    return mm_mipc_tlv_add (b, id, s, strlen (s) + 1);
}

/*****************************************************************************/
/* TLV parser */

gint
mm_mipc_tlv_next (const guint8 *p, gsize len, gsize *off, MMMipcTlv *t)
{
    gsize o = *off, vlen, size;

    if (o == len)
        return 0;
    if (o > len || len - o < MM_MIPC_TLV_HDR)
        return -1;
    t->raw_id = (guint16) mm_mipc_get (p + o, 2);
    t->id = t->raw_id & MM_MIPC_TLV_ID_MASK;
    vlen = mm_mipc_get (p + o + 2, 2);
    size = MM_MIPC_TLV_SIZE (vlen);
    if (size > len - o) {
        /* the last element may be unpadded */
        if (vlen && MM_MIPC_TLV_HDR + vlen <= len - o)
            size = len - o;
        else
            return -1;
    }
    t->val = p + o + MM_MIPC_TLV_HDR;
    t->len = vlen;
    *off = o + size;
    return 1;
}

const guint8 *
mm_mipc_tlv_find (const guint8 *p, gsize len, guint16 id, gsize *vlen)
{
    MMMipcTlv t;
    gsize     off = 0;

    id &= MM_MIPC_TLV_ID_MASK;
    while (mm_mipc_tlv_next (p, len, &off, &t) == 1) {
        if (t.id == id && t.len) {
            if (vlen)
                *vlen = t.len;
            return t.val;
        }
    }
    return NULL;
}

gboolean
mm_mipc_tlv_find_uint (const guint8 *p, gsize len, guint16 id, guint32 *v)
{
    gsize         vlen;
    const guint8 *val;

    val = mm_mipc_tlv_find (p, len, id, &vlen);
    if (!val || vlen > 4)
        return FALSE;
    *v = mm_mipc_get (val, vlen);
    return TRUE;
}

static gchar *
tlv_find_string (const guint8 *p, gsize len, guint16 id, gboolean at_text)
{
    gsize         vlen, l, i;
    const guint8 *val;
    gchar        *out;

    val = mm_mipc_tlv_find (p, len, id, &vlen);
    if (!val)
        return NULL;
    l = strnlen ((const gchar *) val, vlen);
    out = g_strndup ((const gchar *) val, l);
    /* AT replies need line boundaries and horizontal whitespace for parsing.
     * Other strings retain the single-line representation used in logs. */
    for (i = 0; i < l; i++)
        if (((guchar) out[i] < 0x20 || out[i] == 0x7F) &&
            !(at_text && (out[i] == '\r' || out[i] == '\n' || out[i] == '\t')))
            out[i] = '?';
    return out;
}

gchar *
mm_mipc_tlv_find_str (const guint8 *p, gsize len, guint16 id)
{
    return tlv_find_string (p, len, id, FALSE);
}

gchar *
mm_mipc_tlv_find_at_string (const guint8 *p, gsize len)
{
    return tlv_find_string (p, len, MM_MIPC_T_AT_STRING, TRUE);
}

guint32
mm_mipc_tlv_result (const guint8 *p, gsize len)
{
    guint32 result;

    if (!mm_mipc_tlv_find_uint (p, len, MM_MIPC_T_RESULT, &result))
        return MM_MIPC_RESULT_MISSING;
    return result;
}

/*****************************************************************************/
/* framing */

GByteArray *
mm_mipc_frame_build (guint16       msg_id,
                     guint16       txid,
                     guint8        slot,
                     const guint8 *payload,
                     gsize         len)
{
    GByteArray *out;
    guint8      hdr[MM_MIPC_HDR_SIZE];

    if (len >= MM_MIPC_PAYLOAD_MAX || (len && !payload))
        return NULL;
    memset (hdr, 0, sizeof (hdr));
    mm_mipc_put (hdr + MM_MIPC_HDR_MAGIC_OFF, 4, MM_MIPC_MAGIC);
    hdr[MM_MIPC_HDR_SLOT_OFF] = slot;
    mm_mipc_put (hdr + MM_MIPC_HDR_MSGID_OFF, 2, msg_id);
    mm_mipc_put (hdr + MM_MIPC_HDR_TXID_OFF, 2, txid);
    mm_mipc_put (hdr + MM_MIPC_HDR_LEN_OFF, 2, (guint32) len);
    out = g_byte_array_sized_new (MM_MIPC_HDR_SIZE + len);
    g_byte_array_append (out, hdr, MM_MIPC_HDR_SIZE);
    if (len)
        g_byte_array_append (out, payload, len);
    return out;
}

gint
mm_mipc_frame_parse (const guint8 *buf,
                     gsize         len,
                     MMMipcMsg    *msg,
                     gsize        *consumed)
{
    gsize plen;

    if (len < MM_MIPC_HDR_SIZE)
        return 0;
    if (mm_mipc_get (buf + MM_MIPC_HDR_MAGIC_OFF, 4) != MM_MIPC_MAGIC)
        return -1;
    plen = mm_mipc_get (buf + MM_MIPC_HDR_LEN_OFF, 2);
    if (plen > MM_MIPC_MSG_MAX - MM_MIPC_HDR_SIZE)
        return -1;
    if (len < MM_MIPC_HDR_SIZE + plen)
        return 0;
    msg->slot    = buf[MM_MIPC_HDR_SLOT_OFF];
    msg->msg_id  = (guint16) mm_mipc_get (buf + MM_MIPC_HDR_MSGID_OFF, 2);
    msg->txid    = (guint16) mm_mipc_get (buf + MM_MIPC_HDR_TXID_OFF, 2);
    msg->payload = buf + MM_MIPC_HDR_SIZE;
    msg->len     = plen;
    *consumed    = MM_MIPC_HDR_SIZE + plen;
    return 1;
}

/*****************************************************************************/

const gchar *
mm_mipc_msg_id_get_string (guint16 msg_id)
{
    switch (msg_id) {
    case MM_MIPC_INTERNAL_OPEN_REQ:           return "INTERNAL_OPEN_REQ";
    case MM_MIPC_INTERNAL_OPEN_CNF:           return "INTERNAL_OPEN_CNF";
    case MM_MIPC_INTERNAL_CLOSE_REQ:          return "INTERNAL_CLOSE_REQ";
    case MM_MIPC_INTERNAL_CLOSE_CNF:          return "INTERNAL_CLOSE_CNF";
    case MM_MIPC_INTERNAL_TEST_REQ:           return "INTERNAL_TEST_REQ";
    case MM_MIPC_INTERNAL_TEST_CNF:           return "INTERNAL_TEST_CNF";
    case MM_MIPC_INTERNAL_REGISTER_IND_REQ:   return "INTERNAL_REGISTER_IND_REQ";
    case MM_MIPC_INTERNAL_REGISTER_IND_CNF:   return "INTERNAL_REGISTER_IND_CNF";
    case MM_MIPC_INTERNAL_UNREGISTER_IND_REQ: return "INTERNAL_UNREGISTER_IND_REQ";
    case MM_MIPC_INTERNAL_EIF_IND:            return "INTERNAL_EIF_IND";
    case MM_MIPC_SYS_AT_REQ:                  return "SYS_AT_REQ";
    case MM_MIPC_SYS_AT_CNF:                  return "SYS_AT_CNF";
    case MM_MIPC_SYS_AT_IND:                  return "SYS_AT_IND";
    case MM_MIPC_DATA_ACT_CALL_REQ:           return "DATA_ACT_CALL_REQ";
    case MM_MIPC_DATA_ACT_CALL_CNF:           return "DATA_ACT_CALL_CNF";
    case MM_MIPC_DATA_DEACT_CALL_REQ:         return "DATA_DEACT_CALL_REQ";
    case MM_MIPC_DATA_DEACT_CALL_CNF:         return "DATA_DEACT_CALL_CNF";
    case MM_MIPC_DATA_ACT_CALL_IND:           return "DATA_ACT_CALL_IND";
    case MM_MIPC_DATA_DEACT_CALL_IND:         return "DATA_DEACT_CALL_IND";
    case MM_MIPC_SMS_CFG_IND:                 return "SMS_CFG_IND";
    case MM_MIPC_SMS_NEW_SMS_IND:             return "SMS_NEW_SMS_IND";
    case MM_MIPC_SMS_STORE_STATUS_IND:        return "SMS_STORE_STATUS_IND";
    case MM_MIPC_SMS_NEW_STATUS_REPORT_IND:   return "SMS_NEW_STATUS_REPORT_IND";
    case MM_MIPC_APN_ADD_PROFILE_LIST_REQ:    return "APN_ADD_PROFILE_LIST_REQ";
    case MM_MIPC_APN_ADD_PROFILE_LIST_CNF:    return "APN_ADD_PROFILE_LIST_CNF";
    case MM_MIPC_APN_SET_IA_REQ:              return "APN_SET_IA_REQ";
    case MM_MIPC_DATA_SET_DATA_ALLOW_REQ:     return "DATA_SET_DATA_ALLOW_REQ";
    case MM_MIPC_DATA_SET_DATA_ALLOW_CNF:     return "DATA_SET_DATA_ALLOW_CNF";
    case MM_MIPC_CALL_DATA_PREFER_SET_REQ:    return "CALL_DATA_PREFER_SET_REQ";
    case MM_MIPC_CALL_DATA_PREFER_SET_CNF:    return "CALL_DATA_PREFER_SET_CNF";
    case MM_MIPC_NW_SET_RADIO_CAPABILITY_REQ: return "NW_SET_RADIO_CAPABILITY_REQ";
    case MM_MIPC_NW_SET_RADIO_CAPABILITY_CNF: return "NW_SET_RADIO_CAPABILITY_CNF";
    case MM_MIPC_SYS_MULTI_SIM_CONFIG_REQ:    return "SYS_MULTI_SIM_CONFIG_REQ";
    case MM_MIPC_SYS_MULTI_SIM_CONFIG_CNF:    return "SYS_MULTI_SIM_CONFIG_CNF";
    case MM_MIPC_SYS_SET_DSBP_REQ:            return "SYS_SET_DSBP_REQ";
    case MM_MIPC_SYS_SET_DSBP_CNF:            return "SYS_SET_DSBP_CNF";
    case MM_MIPC_NW_SET_RADIO_STATE_REQ:      return "NW_SET_RADIO_STATE_REQ";
    case MM_MIPC_NW_SET_RADIO_STATE_CNF:      return "NW_SET_RADIO_STATE_CNF";
    case MM_MIPC_NW_GET_PS_REQ:               return "NW_GET_PS_REQ";
    case MM_MIPC_NW_GET_PS_CNF:               return "NW_GET_PS_CNF";
    case MM_MIPC_NW_PS_IND:                   return "NW_PS_IND";
    case MM_MIPC_NW_RADIO_IND:                return "NW_RADIO_IND";
    case MM_MIPC_NW_IA_IND:                   return "NW_IA_IND";
    case MM_MIPC_NW_CS_IND:                   return "NW_CS_IND";
    case MM_MIPC_NW_CSCON_IND:                return "NW_CSCON_IND";
    case MM_MIPC_NW_PHYSICAL_CHANNEL_CONFIGS_IND: return "NW_PHYSICAL_CHANNEL_CONFIGS_IND";
    case MM_MIPC_NW_BARRING_INFO_IND:         return "NW_BARRING_INFO_IND";
    case MM_MIPC_APN_SET_IA_CNF:              return "APN_SET_IA_CNF";
    case MM_MIPC_NW_SET_RAT_REQ:              return "NW_SET_RAT_REQ";
    case MM_MIPC_NW_SET_RAT_CNF:              return "NW_SET_RAT_CNF";
    case MM_MIPC_NW_CURRENT_RAT_IND:          return "NW_CURRENT_RAT_IND";
    default:                                  return NULL;
    }
}


/* Explicit pairs: IMS_SET_CONFIG starts at the even id 0x0a00. */
gboolean
mm_mipc_request_is_valid (guint16 id)
{
    switch (id) {
    case MM_MIPC_INTERNAL_TEST_REQ:
    case MM_MIPC_INTERNAL_OPEN_REQ:
    case MM_MIPC_INTERNAL_CLOSE_REQ:
    case MM_MIPC_INTERNAL_REGISTER_IND_REQ:
    case MM_MIPC_INTERNAL_UNREGISTER_IND_REQ:
    case MM_MIPC_INTERNAL_REGISTER_CMD_REQ:
    case MM_MIPC_SYS_AT_REQ:
    case MM_MIPC_DATA_ACT_CALL_REQ:
    case MM_MIPC_DATA_DEACT_CALL_REQ:
    case MM_MIPC_IMS_SET_CONFIG_REQ:
    case MM_MIPC_APN_ADD_PROFILE_LIST_REQ:
    case MM_MIPC_APN_SET_IA_REQ:
    case MM_MIPC_NW_SET_RAT_REQ:
    case MM_MIPC_DATA_SET_DATA_ALLOW_REQ:
    case MM_MIPC_CALL_DATA_PREFER_SET_REQ:
    case MM_MIPC_NW_SET_RADIO_CAPABILITY_REQ:
    case MM_MIPC_SYS_MULTI_SIM_CONFIG_REQ:
    case MM_MIPC_SYS_SET_DSBP_REQ:
    case MM_MIPC_NW_SET_RADIO_STATE_REQ:
    case MM_MIPC_NW_GET_PS_REQ:
        return TRUE;
    default:
        return FALSE;
    }
}

const guint8 *
mm_mipc_tlv_find_record (const guint8 *p, gsize len, guint16 id, gsize *vlen)
{
    const guint8 *value = mm_mipc_tlv_find (p, len, id, vlen);
    MMMipcTlv tlv;
    gsize off = 0;

    if (value)
        return value;
    while (mm_mipc_tlv_next (p, len, &off, &tlv) == 1) {
        if (tlv.id == (id & MM_MIPC_TLV_ID_MASK) && !tlv.len) {
            guint16 first = mm_mipc_get (tlv.val + 8, 2);
            if (mm_mipc_get (tlv.val + 10, 2))
                return mm_mipc_tlv_find (p, len, first, vlen);
            break;
        }
    }
    return NULL;
}

GByteArray *
mm_mipc_build_ims_config (gboolean key_form, const guint8 flags[6])
{
    GByteArray *tlvs = g_byte_array_new ();
    mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_IMSCFG_FORM, key_form ? MM_MIPC_IMSCFG_FORM_KEY : MM_MIPC_IMSCFG_FORM_FLAGS);
    if (key_form) {
        mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_IMSCFG_KEY, MM_MIPC_IMSCFG_KEY_VOLTE);
        mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_IMSCFG_VALUE, MM_MIPC_IMSCFG_KEY_VOLTE_VALUE);
    } else
        mm_mipc_tlv_add (tlvs, MM_MIPC_T_IMSCFG_VALUE, flags, MM_MIPC_IMSCFG_FLAGS);
    return tlvs;
}

GByteArray *
mm_mipc_build_apn_profiles (const gchar *internet, const gchar *ims, const gchar *sos)
{
    const gchar *names[] = { internet, ims, sos };
    const guint32 types[] = { MM_MIPC_APN_REC_TYPE_DEFAULT, MM_MIPC_APN_REC_TYPE_IMS, MM_MIPC_APN_REC_TYPE_EMERGENCY };
    guint count = 0, index = 0, i;
    guint8 descriptor[16] = { 0 };
    GByteArray *tlvs;

    for (i = 0; i < G_N_ELEMENTS (names); i++) {
        if (names[i] && names[i][0]) {
            if (strlen (names[i]) >= MM_MIPC_APN_REC_APN_MAX)
                return NULL;
            count++;
        }
    }
    if (!count)
        return NULL;
    tlvs = g_byte_array_new ();
    mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_APNLIST_COUNT, count);
    mm_mipc_put (descriptor, 2, MM_MIPC_T_APNLIST_ARRAY);
    mm_mipc_put (descriptor + 12, 2, 0x10000u - count);
    mm_mipc_put (descriptor + 14, 2, count);
    g_byte_array_append (tlvs, descriptor, sizeof (descriptor));
    for (i = 0; i < G_N_ELEMENTS (names); i++) {
        guint8 record[MM_MIPC_APN_REC_SIZE] = { 0 };
        if (!names[i] || !names[i][0])
            continue;
        mm_mipc_put (record + MM_MIPC_APN_REC_ID_OFF, 4, G_MAXUINT32);
        memcpy (record + MM_MIPC_APN_REC_APN_OFF, names[i], strlen (names[i]));
        mm_mipc_put (record + MM_MIPC_APN_REC_BEARER_OFF, 4, MM_MIPC_APN_REC_BEARER_ALL);
        mm_mipc_put (record + MM_MIPC_APN_REC_TYPE_OFF, 4, types[i]);
        record[MM_MIPC_APN_REC_PDP_OFF] = i ? MM_MIPC_PDP_IPV4V6 : MM_MIPC_APN_INTERNET_PDP;
        record[MM_MIPC_APN_REC_ROAM_PDP_OFF] = record[MM_MIPC_APN_REC_PDP_OFF];
        /* Captured values; coding of authentication and enabled is unverified. */
        record[MM_MIPC_APN_REC_AUTH_OFF] = i ? 0 : MM_MIPC_APN_INTERNET_AUTH;
        mm_mipc_put (record + MM_MIPC_APN_REC_ENABLED_OFF, 4, 1);
        mm_mipc_tlv_add (tlvs, (guint16) (0x10000u - count + index++), record, sizeof (record));
    }
    return tlvs;
}

GByteArray *
mm_mipc_build_set_ia (const gchar *apn, guint8 pdp, guint8 roaming_pdp, guint8 auth,
                      const gchar *user, const gchar *password)
{
    GByteArray *b;

    if (!apn || !apn[0] || strlen (apn) >= MM_MIPC_APN_REC_APN_MAX)
        return NULL;
    b = g_byte_array_new ();
    if (!mm_mipc_tlv_add_str (b, MM_MIPC_T_IA_APN, apn) ||
        !mm_mipc_tlv_add_u8 (b, MM_MIPC_T_IA_PDP, pdp) ||
        !mm_mipc_tlv_add_u8 (b, MM_MIPC_T_IA_ROAM_PDP, roaming_pdp) ||
        !mm_mipc_tlv_add_u8 (b, MM_MIPC_T_IA_AUTH, auth) ||
        !mm_mipc_tlv_add_str (b, MM_MIPC_T_IA_USER, user ? user : "") ||
        !mm_mipc_tlv_add_str (b, MM_MIPC_T_IA_PASSWORD, password ? password : "")) {
        g_byte_array_unref (b);
        return NULL;
    }
    return b;
}

GByteArray *
mm_mipc_build_data_prefer (void)
{
    GByteArray *b = g_byte_array_new ();

    mm_mipc_tlv_add_u8 (b, 0x0100, 1);
    mm_mipc_tlv_add_u8 (b, 0x0101, 1);
    return b;
}

GByteArray *
mm_mipc_build_radio_capability (void)
{
    GByteArray *b = g_byte_array_new ();

    mm_mipc_tlv_add_u32 (b, 0x0100, MM_MIPC_RADIO_CAPABILITY_ALL);
    mm_mipc_tlv_add_u8 (b, 0x0101, 1);
    return b;
}

GByteArray *
mm_mipc_build_multi_sim_config (void)
{
    GByteArray *b = g_byte_array_new ();

    mm_mipc_tlv_add_u8 (b, 0x0101, 2);
    return b;
}

GByteArray *
mm_mipc_build_dsbp (void)
{
    GByteArray *b = g_byte_array_new ();

    mm_mipc_tlv_add_u8 (b, 0x0100, 3);
    return b;
}

static gchar *tlv_dump (const guint8 *p, gsize len, gsize max_value, gboolean show_private);

gchar *
mm_mipc_tlv_dump (const guint8 *p, gsize len, gsize max_value)
{
    return tlv_dump (p, len, max_value, FALSE);
}

gchar *
mm_mipc_tlv_dump_full (const guint8 *p, gsize len, gsize max_value)
{
    return tlv_dump (p, len, max_value, TRUE);
}

GByteArray *
mm_mipc_build_radio_state (gboolean on)
{
    GByteArray *b = g_byte_array_new ();

    mm_mipc_tlv_add_u8 (b, 0x0100, on ? 1 : 0);
    return b;
}

static gchar *
tlv_dump (const guint8 *p, gsize len, gsize max_value, gboolean show_private)
{
    GString  *s = g_string_new (NULL);
    MMMipcTlv tlv;
    gsize     off = 0, i;
    gint      r;

    while ((r = mm_mipc_tlv_next (p, len, &off, &tlv)) == 1) {
        g_string_append_printf (s, "%s%04x/%" G_GSIZE_FORMAT, s->len ? " " : "", tlv.raw_id, tlv.len);
        if ((tlv.raw_id & 0x8000) && !show_private) {
            g_string_append (s, " (masked)");
            continue;
        }
        if (!tlv.len)
            continue;
        g_string_append_c (s, '=');
        for (i = 0; i < tlv.len && i < max_value; i++)
            g_string_append_printf (s, "%02x", tlv.val[i]);
        if (tlv.len > max_value)
            g_string_append (s, "...");
    }
    if (r < 0)
        g_string_append (s, s->len ? " (malformed)" : "(malformed)");
    return g_string_free (s, FALSE);
}

GByteArray *
mm_mipc_build_set_rat (guint8 rat, guint8 prefer)
{
    GByteArray *b = g_byte_array_new ();

    mm_mipc_tlv_add_u8 (b, MM_MIPC_T_SET_RAT_RAT, rat);
    mm_mipc_tlv_add_u8 (b, MM_MIPC_T_SET_RAT_PREFER, prefer);
    return b;
}
