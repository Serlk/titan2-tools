/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIPC framing/TLV tests against a worked example of
 * the frame layout. Same checks as
 * the self-test of tools/modemd/src/mipc.c.
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
#include <glib.h>

#include "mm-mipc-proto.h"
#include "mm-modem-helpers-mtk-ccci.h"

/* example header: msgid 0x0201, txid 0x03CD, len 0x0318 */
static const guint8 hdr_ex[16] = { 0x84, 0x19, 0x54, 0x24, 0, 0, 0, 0, 0x01, 0x00,
                                   0x01, 0x02, 0xCD, 0x03, 0x18, 0x03 };

/* TLVs: 0x0108 u8 0x7F; 0x0101 APN (len 18 + 2 pad); 0x0109 u32;
 * an array descriptor 0x812F with three u32 elements; the result */
static const guint8 tlv_ex[] = {
    0x08, 0x01, 0x01, 0x00, 0x7F, 0, 0, 0,
    0x01, 0x01, 0x12, 0x00, 'i', 'n', 't', 'e', 'r', 'n', 'e', 't', '.', 't', 'e',
    'l', 'e', '2', '.', 'r', 'u', 0, 0, 0,
    0x09, 0x01, 0x04, 0x00, 0xFF, 0xFF, 0xFD, 0x7F,
    0x2F, 0x81, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0xFD, 0xFF, 0x03, 0x00,
    0xFD, 0xFF, 0x04, 0x00, 1, 0, 0, 0,
    0x00, 0x00, 0x04, 0x00, 0, 0, 0, 0,
};

static void
test_header (void)
{
    guint8                big[16 + 0x318];
    MMMipcMsg             msg;
    gsize                 used = 0;
    g_autoptr(GByteArray) frame = NULL;

    memcpy (big, hdr_ex, 16);
    /* needs 792 payload bytes */
    g_assert_cmpint (mm_mipc_frame_parse (big, 16, &msg, &used), ==, 0);
    memset (big + 16, 0, 0x318);
    g_assert_cmpint (mm_mipc_frame_parse (big, sizeof (big), &msg, &used), ==, 1);
    g_assert_cmpuint (msg.msg_id, ==, 0x0201);
    g_assert_cmpuint (msg.txid, ==, 0x03CD);
    g_assert_cmpuint (msg.len, ==, 0x318);
    g_assert_cmpuint (msg.slot, ==, 1);
    g_assert_cmpuint (used, ==, sizeof (big));

    frame = mm_mipc_frame_build (0x0201, 0x03CD, 1, big + 16, 0x318);
    g_assert_nonnull (frame);
    g_assert_cmpuint (frame->len, ==, sizeof (big));
    g_assert_cmpint (memcmp (frame->data, hdr_ex, 16), ==, 0);

    /* bad magic */
    big[0] = 0x85;
    g_assert_cmpint (mm_mipc_frame_parse (big, sizeof (big), &msg, &used), ==, -1);
}

static void
test_tlv_parse (void)
{
    guint32            v;
    g_autofree gchar  *s = NULL;
    gsize              vlen, off = 0;
    const guint8      *val;
    MMMipcTlv          t;
    gint               n = 0, r;

    g_assert_true (mm_mipc_tlv_find_uint (tlv_ex, sizeof (tlv_ex), MM_MIPC_T_ACT_CONST_7F, &v));
    g_assert_cmpuint (v, ==, 0x7F);
    s = mm_mipc_tlv_find_str (tlv_ex, sizeof (tlv_ex), MM_MIPC_T_ACT_APN);
    g_assert_cmpstr (s, ==, "internet.carrier1");
    g_assert_true (mm_mipc_tlv_find_uint (tlv_ex, sizeof (tlv_ex), MM_MIPC_T_ACT_BEARER_MASK, &v));
    g_assert_cmpuint (v, ==, 0x7FFDFFFF);
    g_assert_cmpuint (mm_mipc_tlv_result (tlv_ex, sizeof (tlv_ex)), ==, 0);

    /* privacy bit ignored in lookups: 0x812F is found as 0x012F, but it is an
     * array descriptor and so has no value */
    g_assert_null (mm_mipc_tlv_find (tlv_ex, sizeof (tlv_ex), 0x012F, &vlen));
    /* array element */
    val = mm_mipc_tlv_find (tlv_ex, sizeof (tlv_ex), 0xFFFD, &vlen);
    g_assert_nonnull (val);
    g_assert_cmpuint (vlen, ==, 4);
    g_assert_cmpuint (mm_mipc_get (val, 4), ==, 1);

    while (mm_mipc_tlv_next (tlv_ex, sizeof (tlv_ex), &off, &t) == 1)
        n++;
    g_assert_cmpint (n, ==, 6);
    g_assert_cmpuint (off, ==, sizeof (tlv_ex));

    /* truncated payload is malformed */
    off = 0;
    while ((r = mm_mipc_tlv_next (tlv_ex, sizeof (tlv_ex) - 10, &off, &t)) == 1)
        ;
    g_assert_cmpint (r, ==, -1);

    /* missing result -> 2 */
    g_assert_cmpuint (mm_mipc_tlv_result (tlv_ex, 8), ==, MM_MIPC_RESULT_MISSING);
}

static void
test_at_string (void)
{
    static const gchar text[] = "\r\n+CIMI:\t123456789012345\r\nOK\r\n\033\177";
    static const gchar unterminated[] = { 'O', 'K' };
    g_autoptr(GByteArray) tlvs = g_byte_array_new ();
    g_autofree gchar *at = NULL;
    g_autofree gchar *single_line = NULL;

    g_assert_null (mm_mipc_tlv_find_at_string (NULL, 0));
    mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, text);
    at = mm_mipc_tlv_find_at_string (tlvs->data, tlvs->len);
    g_assert_cmpstr (at, ==, "\r\n+CIMI:\t123456789012345\r\nOK\r\n??");
    single_line = mm_mipc_tlv_find_str (tlvs->data, tlvs->len, MM_MIPC_T_AT_STRING);
    g_assert_cmpstr (single_line, ==, "??+CIMI:?123456789012345??OK????");
    g_clear_pointer (&at, g_free);
    g_byte_array_set_size (tlvs, 0);
    mm_mipc_tlv_add (tlvs, MM_MIPC_T_AT_STRING, unterminated, sizeof (unterminated));
    at = mm_mipc_tlv_find_at_string (tlvs->data, tlvs->len);
    g_assert_cmpstr (at, ==, "OK");
}

static void
test_tlv_build (void)
{
    g_autoptr(GByteArray) b = g_byte_array_new ();

    /* same bytes as the example for the three plain TLVs */
    g_assert_true (mm_mipc_tlv_add_u8  (b, MM_MIPC_T_ACT_CONST_7F, 0x7F));
    g_assert_true (mm_mipc_tlv_add_str (b, MM_MIPC_T_ACT_APN, "internet.carrier1"));
    g_assert_true (mm_mipc_tlv_add_u32 (b, MM_MIPC_T_ACT_BEARER_MASK, 0x7FFDFFFF));
    g_assert_cmpuint (b->len, ==, 8 + 24 + 8);
    g_assert_cmpint (memcmp (b->data, tlv_ex, b->len), ==, 0);

    /* a u16 occupies 8 bytes too */
    g_assert_true (mm_mipc_tlv_add_u16 (b, MM_MIPC_T_REGISTER_IND_ID, 0x4302));
    g_assert_cmpuint (b->len, ==, 48);
    g_assert_cmpuint (b->data[40], ==, 0x00);
    g_assert_cmpuint (b->data[41], ==, 0x01);
    g_assert_cmpuint (b->data[42], ==, 0x02);
    g_assert_cmpuint (b->data[44], ==, 0x02);
    g_assert_cmpuint (b->data[45], ==, 0x43);

    /* refused: an empty value (would be an array descriptor) */
    g_assert_false (mm_mipc_tlv_add (b, 1, "", 0));
}

static void
test_frames_back_to_back (void)
{
    g_autoptr(GByteArray) frame = NULL;
    guint8                big[64];
    MMMipcMsg             msg;
    gsize                 used = 0;

    frame = mm_mipc_frame_build (MM_MIPC_INTERNAL_TEST_REQ, 5, MM_MIPC_SLOT_ANY, NULL, 0);
    g_assert_nonnull (frame);
    g_assert_cmpuint (frame->len, ==, 16);
    memcpy (big, frame->data, 16);
    memcpy (big + 16, frame->data, 16);
    g_assert_cmpint (mm_mipc_frame_parse (big, 32, &msg, &used), ==, 1);
    g_assert_cmpuint (used, ==, 16);
    g_assert_cmpuint (msg.msg_id, ==, MM_MIPC_INTERNAL_TEST_REQ);
    g_assert_cmpuint (msg.slot, ==, 0xFF);
    g_assert_cmpint (mm_mipc_frame_parse (big + 16, 16, &msg, &used), ==, 1);

    /* too long */
    g_assert_null (mm_mipc_frame_build (1, 1, 1, big, MM_MIPC_PAYLOAD_MAX));
}

static void
test_classes (void)
{
    g_assert_true (MM_MIPC_IS_IND (MM_MIPC_INTERNAL_EIF_IND));
    g_assert_false (MM_MIPC_IS_IND (MM_MIPC_DATA_ACT_CALL_CNF));
    g_assert_true (MM_MIPC_IS_CMD (0x8303));
    g_assert_false (MM_MIPC_IS_CMD (0x8304));
    g_assert_cmpuint (MM_MIPC_CNF_OF (MM_MIPC_DATA_ACT_CALL_REQ), ==, MM_MIPC_DATA_ACT_CALL_CNF);
    g_assert_cmpstr (mm_mipc_msg_id_get_string (MM_MIPC_DATA_ACT_CALL_REQ), ==, "DATA_ACT_CALL_REQ");
    g_assert_null (mm_mipc_msg_id_get_string (0x0065));
    g_assert_cmpstr (mm_mipc_msg_id_get_string (MM_MIPC_SMS_NEW_SMS_IND), ==, "SMS_NEW_SMS_IND");
    g_assert_true (MM_MIPC_IS_SMS_IND (MM_MIPC_SMS_NEW_SMS_IND));
    g_assert_true (MM_MIPC_IS_IND (MM_MIPC_SMS_NEW_STATUS_REPORT_IND));
    g_assert_false (MM_MIPC_IS_SMS_IND (MM_MIPC_SYS_AT_IND));
}

static void
test_clcc_normalize (void)
{
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;
    g_autofree gchar *c = NULL;

    /* the line observed on the phone: a dialing VoLTE call */
    a = mm_mtk_ccci_clcc_normalize ("+CLCC: 1,0,2,20,0,\"+79001234567\",145");
    g_assert_cmpstr (a, ==, "+CLCC: 1,0,2,0,0,\"+79001234567\",145");
    /* several lines, CS and IMS mixed, the +CLCC prefix and other fields untouched */
    b = mm_mtk_ccci_clcc_normalize ("+CLCC: 1,1,4,0,0,\"123\",129\r\n+CLCC: 2,0,0,25,1,\"+4912\",145,,,0\r\n");
    g_assert_cmpstr (b, ==, "+CLCC: 1,1,4,0,0,\"123\",129\r\n+CLCC: 2,0,0,0,1,\"+4912\",145,,,0\r\n");
    /* 2 (fax/data) and 200 are not IMS voice modes and stay as they are */
    c = mm_mtk_ccci_clcc_normalize ("+CLCC: 1,0,0,2,0,\"1\",129\r\n+CLCC: 2,0,0,200,0,\"2\",129");
    g_assert_cmpstr (c, ==, "+CLCC: 1,0,0,2,0,\"1\",129\r\n+CLCC: 2,0,0,200,0,\"2\",129");
    g_assert_null (mm_mtk_ccci_clcc_normalize (NULL));
}

static void
test_ims_builders (void)
{
    static const guint8 flags[6] = { 1, 0, 0, 0, 1, 1 };
    g_autoptr(GByteArray) key = mm_mipc_build_ims_config (TRUE, flags);
    g_autoptr(GByteArray) features = mm_mipc_build_ims_config (FALSE, flags);
    g_autoptr(GByteArray) profiles = mm_mipc_build_apn_profiles ("internet.example", "ims", "sos");
    const guint8 *record;
    gsize len;
    guint32 value;
    g_autofree gchar *name = NULL;

    g_assert_true (mm_mipc_request_is_valid (0x0a00));
    g_assert_false (mm_mipc_request_is_valid (0x0a01));
    g_assert_true (mm_mipc_tlv_find_uint (key->data, key->len, 0x0100, &value));
    g_assert_cmpuint (value, ==, 1);
    name = mm_mipc_tlv_find_str (key->data, key->len, 0x0101);
    g_assert_cmpstr (name, ==, "mtk_ct_volte_support");
    g_assert_true (mm_mipc_tlv_find_uint (key->data, key->len, 0x0102, &value));
    g_assert_cmpuint (value, ==, 0x32);
    record = mm_mipc_tlv_find (features->data, features->len, 0x0102, &len);
    g_assert_cmpmem (record, len, flags, sizeof (flags));
    g_assert_cmpuint (profiles->len, ==, 888);
    record = mm_mipc_tlv_find_record (profiles->data, profiles->len, 0x8101, &len);
    g_assert_cmpuint (len, ==, 284);
    g_assert_cmpuint (mm_mipc_get (record, 4), ==, 0xffffffff);
    g_assert_cmpstr ((const gchar *) record + 12, ==, "internet.example");
    g_assert_cmpuint (mm_mipc_get (record + 248, 4), ==, 0x29);
    record = mm_mipc_tlv_find (profiles->data, profiles->len, 0xfffe, &len);
    g_assert_cmpstr ((const gchar *) record + 12, ==, "ims");
    g_assert_cmpuint (mm_mipc_get (record + 248, 4), ==, 2);
    g_assert_cmpuint (record[252], ==, 3);
    g_assert_cmpuint (mm_mipc_get (record + 256, 4), ==, 1);
    record = mm_mipc_tlv_find (profiles->data, profiles->len, 0xffff, &len);
    g_assert_cmpuint (mm_mipc_get (record + 248, 4), ==, 0x100);
}

static void
test_speech_reply (void)
{
    g_autofree gchar *reply = NULL;
    g_autoptr(GError) error = NULL;
    guint8 oversized[4097] = { 1 };
    g_assert_cmpint (mm_mtk_ccci_speech_reply_feed (&reply, (const guint8 *) "speech on\no", 11, &error), ==, 0);
    g_assert_cmpint (mm_mtk_ccci_speech_reply_feed (&reply, (const guint8 *) "k\n", 2, &error), ==, 1);
    g_assert_no_error (error);
    g_clear_pointer (&reply, g_free);
    g_assert_cmpint (mm_mtk_ccci_speech_reply_feed (&reply, NULL, 0, &error), ==, -1);
    g_clear_error (&error);
    g_clear_pointer (&reply, g_free);
    g_assert_cmpint (mm_mtk_ccci_speech_reply_feed (&reply, oversized, sizeof (oversized), &error), ==, -1);
    g_clear_error (&error);
    g_assert_cmpint (mm_mtk_ccci_speech_reply_feed (&reply, (const guint8 *) "error: failed\n", 14, &error), ==, -1);
}

static void test_dual_call_parse (void)
{
    g_autoptr(GByteArray) tlvs = g_byte_array_new ();
    MMCallInfo first, second;
    mm_mipc_tlv_add_u32 (tlvs, 0x0100, 1);
    mm_mipc_tlv_add_u32 (tlvs, 0x0109, 0);
    mm_mipc_tlv_add_str (tlvs, 0x8104, "+1234567");
    g_assert_true (mm_mtk_ccci_call_parse (1, tlvs->data, tlvs->len, &first));
    g_assert_true (mm_mtk_ccci_call_parse (2, tlvs->data, tlvs->len, &second));
    g_assert_cmpuint (first.index, ==, 1);
    g_assert_cmpuint (second.index, ==, 101);
    g_assert_cmpuint (first.state, ==, MM_CALL_STATE_RINGING_IN);
    g_assert_cmpstr (first.number, ==, second.number);
    g_free (first.number);
    g_free (second.number);
    g_assert_false (mm_mtk_ccci_call_parse (3, tlvs->data, tlvs->len, &first));
    g_assert_cmpuint (mm_mtk_ccci_call_index (2, 8), ==, 0);
}

static void
test_rat_mode (void)
{
    g_autoptr(GArray)     all = NULL;
    g_autoptr(GArray)     no_nr = NULL;
    g_autoptr(GByteArray) req = NULL;
    MMModemMode           a, p, mask5 = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G;
    guint8                rat, prefer;
    guint                 curr, r, pr, i;
    guint32               v;
    /* NW_SET_RAT_REQ payload captured under Android (2026-10-27 23:55:30.519) */
    static const guint8 captured[] = { 0x00, 0x01, 0x01, 0x00, 0x16, 0, 0, 0, 0x01, 0x01, 0x01, 0x00, 0x80, 0, 0, 0 };

    all = mm_mtk_ccci_rat_build_combinations (mask5);
    g_assert_cmpuint (all->len, ==, 9);
    g_assert_cmpuint (g_array_index (all, MMModemModeCombination, 0).allowed, ==, mask5);
    g_assert_cmpuint (g_array_index (all, MMModemModeCombination, 0).preferred, ==, MM_MODEM_MODE_5G);
    no_nr = mm_mtk_ccci_rat_build_combinations (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G);
    g_assert_cmpuint (no_nr->len, ==, 7);
    for (i = 0; i < no_nr->len; i++)
        g_assert_false (g_array_index (no_nr, MMModemModeCombination, i).allowed & MM_MODEM_MODE_5G);

    /* the default is NR first with 5G, before probing and without 5G in the
     * probing result (the probing can miss NR) */
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_cmpuint (prefer, ==, 128);
    g_assert_true (mm_mtk_ccci_rat_from_modes (MM_MODEM_MODE_NONE, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_true (mm_mtk_ccci_rat_from_modes (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G,
                                               MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_cmpuint (prefer, ==, 128);
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, MM_MODEM_MODE_3G, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 2);
    g_assert_cmpuint (prefer, ==, 2);
    g_assert_false (mm_mtk_ccci_rat_from_modes (mask5, MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, MM_MODEM_MODE_2G, &rat, &prefer));
    g_assert_false (mm_mtk_ccci_rat_from_modes (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G,
                                                MM_MODEM_MODE_4G | MM_MODEM_MODE_5G, MM_MODEM_MODE_NONE, &rat, &prefer));

    /* every offered combination survives the round trip through rat/prefer */
    for (i = 0; i < all->len; i++) {
        MMModemModeCombination *c = &g_array_index (all, MMModemModeCombination, i);
        g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, c->allowed, c->preferred, &rat, &prefer));
        g_assert_true (mm_mtk_ccci_rat_to_modes (rat, prefer, &a, &p));
        g_assert_cmpuint (a, ==, c->allowed);
        g_assert_cmpuint (p, ==, c->preferred);
    }
    /* C2K variants (14 = GSM|UMTS|LTE|C2K) map to the 3GPP part */
    g_assert_true (mm_mtk_ccci_rat_to_modes (14, 0, &a, &p));
    g_assert_cmpuint (a, ==, MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G);
    g_assert_false (mm_mtk_ccci_rat_to_modes (7, 0, &a, &p));   /* C2K only */
    g_assert_false (mm_mtk_ccci_rat_to_modes (99, 0, &a, &p));

    g_assert_true (mm_mtk_ccci_parse_erat ("\r\n+ERAT: 2,0,22,128\r\n\r\nOK\r\n", &curr, &r, &pr));
    g_assert_cmpuint (curr, ==, 2);
    g_assert_cmpuint (r, ==, 22);
    g_assert_cmpuint (pr, ==, 128);
    g_assert_true (mm_mtk_ccci_parse_erat ("+ERAT: 3, 1, 6, 3", &curr, &r, &pr));
    g_assert_cmpuint (r, ==, 6);
    g_assert_false (mm_mtk_ccci_parse_erat ("+ERAT: 3", &curr, &r, &pr));
    g_assert_false (mm_mtk_ccci_parse_erat (NULL, &curr, &r, &pr));

    req = mm_mipc_build_set_rat (22, 128);
    g_assert_cmpmem (req->data, req->len, captured, sizeof (captured));
    g_assert_true (mm_mipc_tlv_find_uint (req->data, req->len, MM_MIPC_T_SET_RAT_PREFER, &v));
    g_assert_cmpuint (v, ==, 128);
    g_assert_true (mm_mipc_request_is_valid (MM_MIPC_NW_SET_RAT_REQ));
    g_assert_cmpstr (mm_mipc_msg_id_get_string (MM_MIPC_NW_CURRENT_RAT_IND), ==, "NW_CURRENT_RAT_IND");
}

/* the documented pairs, both directions, the latter
 * from the pair alone (a new modem object has no cache of what it set) */
static void
test_rat_mode_pairs (void)
{
    g_autoptr(GByteArray) req = NULL;
    MMModemMode           a, p;
    MMModemMode           mask5 = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G;
    MMModemMode           mask4 = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G;
    guint8                rat, prefer;
    static const guint8   lte_first[] = { 0x00, 0x01, 0x01, 0x00, 0x06, 0, 0, 0, 0x01, 0x01, 0x01, 0x00, 0x04, 0, 0, 0 };

    /* LTE first: 2G/3G/4G with 4G preferred <-> 6/4 */
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, mask4, MM_MODEM_MODE_4G, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 6);
    g_assert_cmpuint (prefer, ==, 4);
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask4, mask4, MM_MODEM_MODE_4G, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 6);
    g_assert_cmpuint (prefer, ==, 4);
    g_assert_true (mm_mtk_ccci_rat_to_modes (6, 4, &a, &p));
    g_assert_cmpuint (a, ==, mask4);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_4G);
    req = mm_mipc_build_set_rat (rat, prefer);
    g_assert_cmpmem (req->data, req->len, lte_first, sizeof (lte_first));

    /* 3 is the <rat> value of LTE only, not an LTE preference */
    g_assert_true (mm_mtk_ccci_rat_to_modes (6, 3, &a, &p));
    g_assert_cmpuint (a, ==, mask4);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_NONE);

    /* the automatic mode stays 22/128 */
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_cmpuint (prefer, ==, 128);
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, mask5, MM_MODEM_MODE_5G, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_cmpuint (prefer, ==, 128);
    g_assert_true (mm_mtk_ccci_rat_to_modes (22, 128, &a, &p));
    g_assert_cmpuint (a, ==, mask5);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_5G);
    /* 2G and 3G stay allowed in it */
    g_assert_cmpuint (a & (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G), ==, MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);

    /* LTE only stays 3/0 */
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 3);
    g_assert_cmpuint (prefer, ==, 0);
    g_assert_true (mm_mtk_ccci_rat_to_modes (3, 0, &a, &p));
    g_assert_cmpuint (a, ==, MM_MODEM_MODE_4G);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_NONE);
}

/* the one retry after a rejected start-up pair */
static void
test_rat_fallback (void)
{
    MMModemMode mask5 = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G;
    MMModemMode mask4 = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G;
    MMModemMode a = MM_MODEM_MODE_NONE, p = MM_MODEM_MODE_NONE;
    guint8      rat, prefer;

    /* NR first rejected: LTE first, i.e. 6/4 */
    g_assert_true (mm_mtk_ccci_rat_fallback (mask5, 22, 128, &a, &p));
    g_assert_cmpuint (a, ==, mask4);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_4G);
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, a, p, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 6);
    g_assert_cmpuint (prefer, ==, 4);
    /* the same before the supported modes are known */
    g_assert_true (mm_mtk_ccci_rat_fallback (MM_MODEM_MODE_NONE, 22, 128, &a, &p));
    g_assert_cmpuint (a, ==, mask4);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_4G);

    /* the probing without NR: the start-up pair is still NR first, its
     * refusal gives LTE first; LTE first refused is not followed by itself */
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask4, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_cmpuint (prefer, ==, 128);
    g_assert_true (mm_mtk_ccci_rat_fallback (mask4, rat, prefer, &a, &p));
    g_assert_cmpuint (a, ==, mask4);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_4G);
    g_assert_false (mm_mtk_ccci_rat_fallback (mask4, 6, 4, &a, &p));
    /* the default itself */
    mm_mtk_ccci_rat_default (&a, &p);
    g_assert_cmpuint (a, ==, mask5);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_5G);
    /* some other pair rejected without NR: LTE first */
    g_assert_true (mm_mtk_ccci_rat_fallback (mask4, 3, 0, &a, &p));
    g_assert_cmpuint (a, ==, mask4);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_4G);
    /* LTE first rejected with NR: NR first */
    g_assert_true (mm_mtk_ccci_rat_fallback (mask5, 6, 4, &a, &p));
    g_assert_cmpuint (a, ==, mask5);
    g_assert_cmpuint (p, ==, MM_MODEM_MODE_5G);
    /* a 3G-only modem has neither pair */
    g_assert_false (mm_mtk_ccci_rat_fallback (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, 2, 2, &a, &p));
}

/* registration lines for the diagnostics: URC and read response kept apart,
 * the reject cause marked absent when the modem does not give it */
static void
test_reg_parse (void)
{
    MMMtkCcciReg      reg;
    g_autofree gchar *d1 = NULL, *d2 = NULL, *d3 = NULL, *d4 = NULL, *d5 = NULL, *line = NULL;

    /* URC, reporting level 2 */
    g_assert_true (mm_mtk_ccci_reg_parse ("+CEREG: 1,\"1A2B\",\"01234567\",7", FALSE, &reg));
    g_assert_cmpstr (reg.domain, ==, "EPS");
    g_assert_cmpint (reg.n, ==, -1);
    g_assert_cmpint (reg.stat, ==, 1);
    g_assert_cmpint (reg.act, ==, 7);
    g_assert_cmpint (reg.cause_type, ==, -1);
    g_assert_cmpint (reg.reject_cause, ==, -1);
    d1 = mm_mtk_ccci_reg_describe (&reg);
    g_assert_cmpstr (d1, ==, "EPS stat 1 (home) AcT 7, reject cause not reported");

    /* the same text as a read response: the first field is <n> */
    g_assert_true (mm_mtk_ccci_reg_parse ("+CEREG: 2,1,\"1A2B\",\"01234567\",7", TRUE, &reg));
    g_assert_cmpint (reg.n, ==, 2);
    g_assert_cmpint (reg.stat, ==, 1);
    g_assert_cmpint (reg.act, ==, 7);

    /* +CGREG with <rac>, then the reject cause (level 3) */
    g_assert_true (mm_mtk_ccci_reg_parse ("+CGREG: 3,\"1A2B\",\"0123\",6,\"01\",0,15", FALSE, &reg));
    g_assert_cmpstr (reg.domain, ==, "PS");
    g_assert_cmpint (reg.stat, ==, 3);
    g_assert_cmpint (reg.act, ==, 6);
    g_assert_cmpint (reg.cause_type, ==, 0);
    g_assert_cmpint (reg.reject_cause, ==, 15);
    d2 = mm_mtk_ccci_reg_describe (&reg);
    g_assert_cmpstr (d2, ==, "PS stat 3 (denied) AcT 6, reject cause type 0 code 15");

    /* +CREG read response, level 3 with a cause */
    g_assert_true (mm_mtk_ccci_reg_parse ("+CREG: 3,0,\"1A2B\",\"0123\",7,0,111", TRUE, &reg));
    g_assert_cmpstr (reg.domain, ==, "CS");
    g_assert_cmpint (reg.stat, ==, 0);
    g_assert_cmpint (reg.cause_type, ==, 0);
    g_assert_cmpint (reg.reject_cause, ==, 111);

    /* level 0/1: no location, no AcT */
    g_assert_true (mm_mtk_ccci_reg_parse ("+CREG: 0,2", TRUE, &reg));
    g_assert_cmpint (reg.stat, ==, 2);
    g_assert_cmpint (reg.act, ==, -1);
    d3 = mm_mtk_ccci_reg_describe (&reg);
    g_assert_cmpstr (d3, ==, "CS stat 2 (searching) AcT not reported, reject cause not reported (n 0)");

    g_assert_false (mm_mtk_ccci_reg_parse ("+COPS: 0,2,\"00102\",7", TRUE, &reg));
    g_assert_false (mm_mtk_ccci_reg_parse ("+CEREG: ", FALSE, &reg));
    g_assert_false (mm_mtk_ccci_reg_parse (NULL, FALSE, &reg));

    /* a whole reply */
    d4 = mm_mtk_ccci_reg_describe_text ("\r\n+CGREG: 2,1,\"1A2B\",\"0123\",6,\"01\"\r\n\r\nOK\r\n", TRUE);
    g_assert_cmpstr (d4, ==, "PS stat 1 (home) AcT 6, reject cause not reported (n 2)");
    d5 = mm_mtk_ccci_reg_describe_text ("+COPS: 0,2,\"00102\",7\r\nOK", TRUE);
    g_assert_null (d5);

    line = mm_mtk_ccci_at_one_line ("\r\n+COPS: 0,2,\"00102\",7\r\n\r\nOK\r\n");
    g_assert_cmpstr (line, ==, "+COPS: 0,2,\"00102\",7 | OK");
}

/* APN_SET_IA_REQ 0x0101 */
static void
test_set_ia (void)
{
    g_autoptr(GByteArray) req = mm_mipc_build_set_ia ("internet.example", 1, 1, 1, NULL, NULL);
    g_autofree gchar     *apn = NULL, *user = NULL, *password = NULL;
    guint32               v;

    g_assert_nonnull (req);
    apn = mm_mipc_tlv_find_str (req->data, req->len, MM_MIPC_T_IA_APN);
    g_assert_cmpstr (apn, ==, "internet.example");
    g_assert_true (mm_mipc_tlv_find_uint (req->data, req->len, MM_MIPC_T_IA_PDP, &v));
    g_assert_cmpuint (v, ==, 1);
    g_assert_true (mm_mipc_tlv_find_uint (req->data, req->len, MM_MIPC_T_IA_ROAM_PDP, &v));
    g_assert_cmpuint (v, ==, 1);
    g_assert_true (mm_mipc_tlv_find_uint (req->data, req->len, MM_MIPC_T_IA_AUTH, &v));
    g_assert_cmpuint (v, ==, 1);
    /* user and password as (empty) strings: 0x8105, 0x8106 */
    user = mm_mipc_tlv_find_str (req->data, req->len, MM_MIPC_T_IA_USER);
    password = mm_mipc_tlv_find_str (req->data, req->len, MM_MIPC_T_IA_PASSWORD);
    g_assert_cmpstr (user, ==, "");
    g_assert_cmpstr (password, ==, "");
    /* APN 17+1, three u8, two empty strings: 24 + 3 * 8 + 2 * 8 bytes */
    g_assert_cmpuint (req->len, ==, 64);
    g_assert_true (mm_mipc_request_is_valid (MM_MIPC_APN_SET_IA_REQ));
    g_assert_cmpstr (mm_mipc_msg_id_get_string (MM_MIPC_APN_SET_IA_CNF), ==, "APN_SET_IA_CNF");
    g_assert_null (mm_mipc_build_set_ia (NULL, 1, 1, 1, NULL, NULL));
    g_assert_null (mm_mipc_build_set_ia ("", 1, 1, 1, NULL, NULL));
}

/* registration lines as the generic code applies them */
static void
test_reg_update_parse (void)
{
    MMMtkCcciRegUpdate      u;
    MMModemAccessTechnology act;

    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CEREG: 1,\"1A2B\",\"01234516\",7", FALSE, &u));
    g_assert_true (u.cereg);
    g_assert_cmpuint (u.state, ==, MM_MODEM_3GPP_REGISTRATION_STATE_HOME);
    g_assert_cmpuint (u.act, ==, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    g_assert_cmpuint (u.tac, ==, 0x1A2B);
    g_assert_cmpuint (u.lac, ==, 0);
    g_assert_cmpuint (u.ci, ==, 0x01234516);

    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CGREG: 1,\"1A2B\",\"0123\",6", FALSE, &u));
    g_assert_true (u.cgreg);
    g_assert_cmpuint (u.act, ==, MM_MODEM_ACCESS_TECHNOLOGY_HSPA);
    g_assert_cmpuint (u.lac, ==, 0x1A2B);

    /* +CREG with LTE AcT carries the TAC */
    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CREG: 1,\"1A2B\",\"01234516\",7", FALSE, &u));
    g_assert_false (u.cgreg || u.cereg || u.c5greg);
    g_assert_cmpuint (u.tac, ==, 0x1A2B);

    /* denied, searching */
    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CEREG: 3,\"4598\",\"02D45403\",7", FALSE, &u));
    g_assert_cmpuint (u.state, ==, MM_MODEM_3GPP_REGISTRATION_STATE_DENIED);
    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CEREG: 2", FALSE, &u));
    g_assert_cmpuint (u.state, ==, MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING);

    /* read responses (<n> first), with and without location */
    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CEREG: 2,1,\"1A2B\",\"01234567\",7", TRUE, &u));
    g_assert_true (u.cereg);
    g_assert_cmpuint (u.state, ==, MM_MODEM_3GPP_REGISTRATION_STATE_HOME);
    g_assert_cmpuint (u.act, ==, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CREG: 0,1", TRUE, &u));
    g_assert_cmpuint (u.state, ==, MM_MODEM_3GPP_REGISTRATION_STATE_HOME);
    g_assert_cmpuint (u.act, ==, MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN);

    g_assert_false (mm_mtk_ccci_reg_update_parse ("+COPS: 0,0,\"t2 rus\",7", FALSE, &u));
    g_assert_false (mm_mtk_ccci_reg_update_parse (NULL, FALSE, &u));

    g_assert_true (mm_mtk_ccci_cops_act ("+COPS: 0,0,\"t2 rus\",7\r\nOK", &act));
    g_assert_cmpuint (act, ==, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    g_assert_true (mm_mtk_ccci_cops_act ("\r\n+COPS: 0,2,\"00101\",6\r\n\r\nOK\r\n", &act));
    g_assert_cmpuint (act, ==, MM_MODEM_ACCESS_TECHNOLOGY_HSPA);
    g_assert_false (mm_mtk_ccci_cops_act ("+COPS: 0\r\nOK", &act));
    g_assert_false (mm_mtk_ccci_cops_act ("OK", &act));
}

/* the LTE start-up payloads, the raw dump, n = 3 reporting */
static void
test_lte_contract (void)
{
    g_autoptr(GByteArray) prefer = mm_mipc_build_data_prefer ();
    g_autoptr(GByteArray) caps = mm_mipc_build_radio_capability ();
    g_autoptr(GByteArray) msim = mm_mipc_build_multi_sim_config ();
    g_autoptr(GByteArray) dsbp = mm_mipc_build_dsbp ();
    g_autoptr(GByteArray) cnf = g_byte_array_new ();
    g_autofree gchar     *dump = NULL, *s1 = NULL, *s2 = NULL, *s3 = NULL, *s4 = NULL, *s5 = NULL;
    MMMtkCcciRegUpdate    u;
    MMMtkCcciReg          reg;
    guint32               v;
    static const guint8   caps_bytes[] = { 0x00, 0x01, 0x04, 0x00, 0x1f, 0, 0, 0, 0x01, 0x01, 0x01, 0x00, 0x01, 0, 0, 0 };

    g_assert_true (mm_mipc_tlv_find_uint (prefer->data, prefer->len, 0x0100, &v) && v == 1);
    g_assert_true (mm_mipc_tlv_find_uint (prefer->data, prefer->len, 0x0101, &v) && v == 1);
    g_assert_cmpmem (caps->data, caps->len, caps_bytes, sizeof (caps_bytes));
    g_assert_true (mm_mipc_tlv_find_uint (msim->data, msim->len, 0x0101, &v) && v == 2);
    g_assert_cmpuint (msim->len, ==, 8);
    g_assert_true (mm_mipc_tlv_find_uint (dsbp->data, dsbp->len, 0x0100, &v) && v == 3);
    g_assert_cmpuint (dsbp->len, ==, 8);
    g_assert_true (mm_mipc_request_is_valid (MM_MIPC_DATA_SET_DATA_ALLOW_REQ));
    g_assert_true (mm_mipc_request_is_valid (MM_MIPC_SYS_SET_DSBP_REQ));
    g_assert_cmpuint (MM_MIPC_CNF_OF (MM_MIPC_DATA_SET_DATA_ALLOW_REQ), ==, MM_MIPC_DATA_SET_DATA_ALLOW_CNF);
    g_assert_cmpstr (mm_mipc_msg_id_get_string (MM_MIPC_SYS_MULTI_SIM_CONFIG_CNF), ==, "SYS_MULTI_SIM_CONFIG_CNF");

    /* the raw dump: values of public ids, private ones masked */
    mm_mipc_tlv_add_u32 (cnf, MM_MIPC_T_RESULT, 1);
    mm_mipc_tlv_add_u8 (cnf, 0x0101, 2);
    mm_mipc_tlv_add_str (cnf, 0x8102, "secret");
    dump = mm_mipc_tlv_dump (cnf->data, cnf->len, 16);
    g_assert_cmpstr (dump, ==, "0000/4=01000000 0101/1=02 8102/7 (masked)");

    /* test responses */
    g_assert_true (mm_mtk_ccci_test_range_has ("+CEREG: (0-5)\r\nOK", "+CEREG:", 3));
    g_assert_true (mm_mtk_ccci_test_range_has ("+CEREG: (0,1,2,3,4,5)", "+CEREG:", 3));
    g_assert_false (mm_mtk_ccci_test_range_has ("+CEREG: (0-2)", "+CEREG:", 3));
    g_assert_true (mm_mtk_ccci_test_range_has ("+CEREG: (0-2,3)", "+CEREG:", 3));
    g_assert_false (mm_mtk_ccci_test_range_has ("ERROR", "+CEREG:", 3));

    /* n = 3 lines: the n = 2 form for the generic parsers, the cause decoded */
    s1 = mm_mtk_ccci_reg_strip_cause ("+CEREG: 3,\"1262\",\"0A1B2C3D\",7,0,15", FALSE);
    g_assert_cmpstr (s1, ==, "+CEREG: 3,\"1262\",\"0A1B2C3D\",7");
    s2 = mm_mtk_ccci_reg_strip_cause ("+CGREG: 3,3,\"1A2B\",\"0123\",6,\"01\",0,111", TRUE);
    g_assert_cmpstr (s2, ==, "+CGREG: 3,3,\"1A2B\",\"0123\",6,\"01\"");
    s3 = mm_mtk_ccci_reg_strip_cause ("+CEREG: 1,\"1A2B\",\"01234516\",7", FALSE);
    g_assert_cmpstr (s3, ==, "+CEREG: 1,\"1A2B\",\"01234516\",7");
    s4 = mm_mtk_ccci_reg_strip_cause ("+CEREG: 3,0,,,,0,15", TRUE);
    g_assert_cmpstr (s4, ==, "+CEREG: 3,0");
    s5 = mm_mtk_ccci_reg_strip_cause ("+COPS: 0", FALSE);
    g_assert_null (s5);
    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CEREG: 3,\"1262\",\"0A1B2C3D\",7,0,15", FALSE, &u));
    g_assert_cmpuint (u.state, ==, MM_MODEM_3GPP_REGISTRATION_STATE_DENIED);
    g_assert_cmpuint (u.act, ==, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    g_assert_cmpuint (u.tac, ==, 0x1262);
    g_assert_true (mm_mtk_ccci_reg_update_parse ("+CEREG: 3,1,\"1A2B\",\"01234516\",7,,", TRUE, &u));
    g_assert_cmpuint (u.state, ==, MM_MODEM_3GPP_REGISTRATION_STATE_HOME);
    g_assert_true (mm_mtk_ccci_reg_parse ("+CEREG: 3,\"1262\",\"0A1B2C3D\",7,0,15", FALSE, &reg));
    g_assert_cmpint (reg.cause_type, ==, 0);
    g_assert_cmpint (reg.reject_cause, ==, 15);
}

/* the registration block, CSCON, the radio state */
static void
test_nw_diag_decoders (void)
{
    guint8                block[32] = { 0 };
    MMMtkCcciNwBlock      b;
    g_autofree gchar     *d = NULL, *c1 = NULL, *c2 = NULL, *c3 = NULL, *full = NULL, *masked = NULL;
    g_autoptr(GByteArray) off = mm_mipc_build_radio_state (FALSE);
    g_autoptr(GByteArray) tlvs = g_byte_array_new ();
    static const guint8   connected[] = { 0x01, 0x07, 0x04, 0x00 };
    static const guint8   idle[] = { 0x00, 0xff, 0x04, 0x00 };
    guint32               v;

    block[0] = 1;
    memcpy (block + 1, "00101", 5);
    mm_mipc_put (block + 8, 4, 0x01234534);
    mm_mipc_put (block + 12, 4, 100);
    mm_mipc_put (block + 16, 4, 123);
    mm_mipc_put (block + 20, 4, 0x1A2B);
    g_assert_true (mm_mtk_ccci_nw_block_parse (block, sizeof (block), &b));
    g_assert_cmpuint (b.stat, ==, 1);
    g_assert_cmpstr (b.plmn, ==, "00101");
    g_assert_cmpuint (b.cell, ==, 0x01234534);
    g_assert_cmpuint (b.earfcn, ==, 100);
    g_assert_cmpuint (b.pci, ==, 123);
    g_assert_cmpuint (b.tac, ==, 0x1A2B);
    d = mm_mtk_ccci_nw_block_describe (&b);
    g_assert_cmpstr (d, ==, "stat 1, PLMN '00101', cell 0x01234534, TAC 0x1A2B, EARFCN 100 (inferred offset), PCI 123 (inferred offset)");
    g_assert_false (mm_mtk_ccci_nw_block_parse (block, 23, &b));

    c1 = mm_mtk_ccci_cscon_describe (connected, 4);
    g_assert_true (g_str_has_prefix (c1, "01 07 04 00 (byte 0 = 1: connected"));
    c2 = mm_mtk_ccci_cscon_describe (idle, 4);
    g_assert_true (g_str_has_prefix (c2, "00 ff 04 00 (byte 0 = 0: idle"));
    c3 = mm_mtk_ccci_cscon_describe (NULL, 0);
    g_assert_cmpstr (c3, ==, "(absent)");

    g_assert_true (mm_mipc_tlv_find_uint (off->data, off->len, 0x0100, &v) && v == 0);
    g_assert_cmpuint (off->len, ==, 8);
    g_assert_true (mm_mipc_request_is_valid (MM_MIPC_NW_SET_RADIO_STATE_REQ));
    g_assert_true (mm_mipc_request_is_valid (MM_MIPC_NW_GET_PS_REQ));
    g_assert_cmpstr (mm_mipc_msg_id_get_string (MM_MIPC_NW_CSCON_IND), ==, "NW_CSCON_IND");

    /* the diagnostics dump shows private values, the normal one masks them */
    mm_mipc_tlv_add (tlvs, 0x8106, block, 4);
    full = mm_mipc_tlv_dump_full (tlvs->data, tlvs->len, 16);
    g_assert_cmpstr (full, ==, "8106/4=01323530");
    masked = mm_mipc_tlv_dump (tlvs->data, tlvs->len, 16);
    g_assert_cmpstr (masked, ==, "8106/4 (masked)");
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/MM/mtk-ccci/mipc/header",      test_header);
    g_test_add_func ("/MM/mtk-ccci/mipc/tlv-parse",   test_tlv_parse);
    g_test_add_func ("/MM/mtk-ccci/mipc/tlv-build",   test_tlv_build);
    g_test_add_func ("/MM/mtk-ccci/mipc/at-string",   test_at_string);
    g_test_add_func ("/MM/mtk-ccci/mipc/frames",      test_frames_back_to_back);
    g_test_add_func ("/MM/mtk-ccci/mipc/classes",     test_classes);
    g_test_add_func ("/MM/mtk-ccci/dual-call-parse", test_dual_call_parse);
    g_test_add_func ("/MM/mtk-ccci/speech-reply", test_speech_reply);
    g_test_add_func ("/MM/mtk-ccci/ims-builders", test_ims_builders);
    g_test_add_func ("/MM/mtk-ccci/clcc-normalize",   test_clcc_normalize);
    g_test_add_func ("/MM/mtk-ccci/rat-mode",         test_rat_mode);
    g_test_add_func ("/MM/mtk-ccci/rat-mode-pairs",   test_rat_mode_pairs);
    g_test_add_func ("/MM/mtk-ccci/rat-fallback",     test_rat_fallback);
    g_test_add_func ("/MM/mtk-ccci/reg-parse",        test_reg_parse);
    g_test_add_func ("/MM/mtk-ccci/set-ia",           test_set_ia);
    g_test_add_func ("/MM/mtk-ccci/reg-update-parse", test_reg_update_parse);
    g_test_add_func ("/MM/mtk-ccci/lte-contract",     test_lte_contract);
    g_test_add_func ("/MM/mtk-ccci/nw-diag-decoders", test_nw_diag_decoders);

    return g_test_run ();
}
