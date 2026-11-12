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
    0x01, 0x01, 0x12, 0x00, 'i', 'n', 't', 'e', 'r', 'n', 'e', 't', '.', 'c', 'a',
    'r', 'r', 'i', 'e', 'r', '1', 0, 0, 0,
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
    /* captured factory-firmware NW_SET_RAT_REQ payload (2026-10-27 23:55:30.519) */
    static const guint8 factory[] = { 0x00, 0x01, 0x01, 0x00, 0x16, 0, 0, 0, 0x01, 0x01, 0x01, 0x00, 0x80, 0, 0, 0 };

    all = mm_mtk_ccci_rat_build_combinations (mask5);
    g_assert_cmpuint (all->len, ==, 9);
    g_assert_cmpuint (g_array_index (all, MMModemModeCombination, 0).allowed, ==, mask5);
    g_assert_cmpuint (g_array_index (all, MMModemModeCombination, 0).preferred, ==, MM_MODEM_MODE_5G);
    no_nr = mm_mtk_ccci_rat_build_combinations (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G);
    g_assert_cmpuint (no_nr->len, ==, 7);
    for (i = 0; i < no_nr->len; i++)
        g_assert_false (g_array_index (no_nr, MMModemModeCombination, i).allowed & MM_MODEM_MODE_5G);

    /* defaults: NR first with 5G (and before probing), LTE first without */
    g_assert_true (mm_mtk_ccci_rat_from_modes (mask5, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_cmpuint (prefer, ==, 128);
    g_assert_true (mm_mtk_ccci_rat_from_modes (MM_MODEM_MODE_NONE, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 22);
    g_assert_true (mm_mtk_ccci_rat_from_modes (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G,
                                               MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, &rat, &prefer));
    g_assert_cmpuint (rat, ==, 6);
    g_assert_cmpuint (prefer, ==, 3);
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
    g_assert_cmpmem (req->data, req->len, factory, sizeof (factory));
    g_assert_true (mm_mipc_tlv_find_uint (req->data, req->len, MM_MIPC_T_SET_RAT_PREFER, &v));
    g_assert_cmpuint (v, ==, 128);
    g_assert_true (mm_mipc_request_is_valid (MM_MIPC_NW_SET_RAT_REQ));
    g_assert_cmpstr (mm_mipc_msg_id_get_string (MM_MIPC_NW_CURRENT_RAT_IND), ==, "NW_CURRENT_RAT_IND");
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

    return g_test_run ();
}
