/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMPortMipc against a fake modem on a pseudo-terminal: the session start
 * (lost first INTERNAL_TEST, OPEN confirm, indication registration), a data
 * call confirm split over two writes with an indication in between, a stale
 * confirm, a malformed frame. The messages follow the
 * MIPC session start and data call sequence.
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

#include <config.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pty.h>

#include <glib.h>
#include <gio/gio.h>

#include <ModemManager.h>
#include <mm-errors-types.h>
#include "mm-port-mipc.h"
#include "mm-mipc-proto.h"
#include "mm-log-test.h"
#include "mm-mtk-ccci-ims.h"
#include "mm-base-modem-at.h"
#include "mm-wwan-link.h"

/*****************************************************************************/
/* the fake modem on the master side of the pty */

typedef struct {
    gint        fd;
    GIOChannel *channel;
    guint       watch_id;
    GByteArray *rx;
    guint       n_test;          /* INTERNAL_TEST requests seen */
    guint       n_register;      /* registrations seen */
    guint16     registered[16];
    guint8      registered_slots[16];
    guint8      expected_slot;
    guint8      register_slot;
    guint8      act_slot;
    gchar      *act_apn;
    guint16     act_txid;
    gboolean    deact_seen;
    guint8      deact_aid;
    guint16     ims_txid, timed_out_txid;
    guint       n_commands;
    gboolean    spi_reply_seen;
    guint       n_spi_replies;
    gint        slave;
    gboolean    reject_open, ims_boot, fail_config, ims_registered, ims_only, fail_profiles;
    const gchar *expected_internet;
    guint       concurrent;
    guint16     first_concurrent;
} Peer;

static Peer peer;
static GPtrArray *history;
static guint created_links, deleted_links, configured_links;
static gchar *configured_address;

/* AT and rtnetlink are boundaries of the IMS controller. Mock those boundaries;
 * the MIPC transport remains real and communicates through the pty. MMPort is
 * also a GObject/MMLogObject, sufficient as this controller's test owner. */
void
mm_base_modem_at_command (MMBaseModem *self, const gchar *cmd, guint timeout,
                          gboolean cached, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    const gchar *reply = "";
    g_ptr_array_add (history, g_strdup (cmd));
    if (g_str_equal (cmd, "+CIREG?"))
        reply = "+CIREG: 2,0,0";
    else if (g_str_equal (cmd, "+CGDCONT?"))
        reply = "+CGDCONT: 0,\"IP\",\"internet.example\",\"0.0.0.0\",0,0";
    g_task_set_task_data (task, g_strdup (reply), g_free);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

const gchar *
mm_base_modem_at_command_finish (MMBaseModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error) ? g_task_get_task_data (G_TASK (res)) : NULL;
}

gboolean
mm_wwan_link_add (const gchar *parent, guint id, const gchar *name, gboolean *existed, GError **error)
{
    g_assert_cmpuint (id, ==, 2);
    g_assert_cmpstr (name, ==, "wwan0-2");
    *existed = FALSE;
    created_links++;
    return TRUE;
}

gboolean
mm_wwan_link_del (const gchar *name, GError **error)
{
    g_assert_cmpstr (name, ==, "wwan0-2");
    deleted_links++;
    return TRUE;
}

gboolean
mm_wwan_link_configure (const gchar *name, guint mtu, const gchar *address, GError **error)
{
    g_assert_cmpstr (name, ==, "wwan0-2");
    g_assert_cmpuint (mtu, ==, 1500);
    configured_links++;
    g_free (configured_address);
    configured_address = g_strdup (address);
    return TRUE;
}


static void
peer_write (const guint8 *data, gsize len)
{
    gsize off = 0;

    while (off < len) {
        gssize n = write (peer.fd, data + off, len - off);

        g_assert_cmpint (n, >, 0);
        off += n;
    }
}

static void
peer_send (guint16 msg_id, guint16 txid, guint8 slot, GByteArray *tlvs)
{
    g_autoptr(GByteArray) frame = NULL;

    frame = mm_mipc_frame_build (msg_id, txid, slot, tlvs ? tlvs->data : NULL, tlvs ? tlvs->len : 0);
    g_assert_nonnull (frame);
    peer_write (frame->data, frame->len);
}

static GByteArray *
cnf_ok (void)
{
    GByteArray *tlvs = g_byte_array_new ();

    mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_RESULT, 0);
    return tlvs;
}

/* second half of the data call confirm, sent from an idle */
static GByteArray *act_cnf_frame;
static gsize       act_cnf_split;

static gboolean
send_second_half (gpointer user_data)
{
    peer_write (act_cnf_frame->data + act_cnf_split, act_cnf_frame->len - act_cnf_split);
    g_clear_pointer (&act_cnf_frame, g_byte_array_unref);
    return G_SOURCE_REMOVE;
}

static void
peer_handle (const MMMipcMsg *msg)
{
    g_autoptr(GByteArray) tlvs = NULL;
    guint32               v;

    switch (msg->msg_id) {
    case MM_MIPC_INTERNAL_TEST_REQ:
        g_assert_cmpuint (msg->slot, ==, MM_MIPC_SLOT_ANY);
        peer.n_test++;
        /* the first message after modem boot may be lost */
        if (peer.n_test == 1)
            return;
        peer_send (MM_MIPC_INTERNAL_TEST_CNF, msg->txid, msg->slot, NULL);
        return;

    case MM_MIPC_INTERNAL_OPEN_REQ: {
        g_autofree gchar *name = NULL;

        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_OPEN_VERSION, &v));
        g_assert_cmpuint (v, ==, MM_MIPC_OPEN_VERSION);
        name = mm_mipc_tlv_find_str (msg->payload, msg->len, MM_MIPC_T_OPEN_CLIENT_NAME);
        g_assert_cmpstr (name, ==, "test-client");
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_OPEN_FLAG, &v));
        g_assert_cmpuint (v, ==, 1);
        tlvs = cnf_ok ();
        if (peer.reject_open)
            mm_mipc_put (tlvs->data + 4, 4, 1);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_OPEN_VERSION, 1);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_OPEN_CNF_TIMEOUT, 300000);
        peer_send (MM_MIPC_INTERNAL_OPEN_CNF, msg->txid, msg->slot, tlvs);
        return;
    }

    case MM_MIPC_INTERNAL_REGISTER_IND_REQ:
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_REGISTER_IND_ID, &v));
        if (peer.n_register < G_N_ELEMENTS (peer.registered)) {
            peer.registered[peer.n_register] = (guint16) v;
            peer.registered_slots[peer.n_register] = msg->slot;
        }
        peer.n_register++;
        peer.register_slot = msg->slot;
        tlvs = cnf_ok ();
        peer_send (MM_MIPC_INTERNAL_REGISTER_IND_CNF, msg->txid, msg->slot, tlvs);
        return;

    case MM_MIPC_INTERNAL_REGISTER_CMD_REQ:
        g_assert_cmpuint (msg->slot, ==, MM_MIPC_SLOT_ANY);
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_REGISTER_IND_ID, &v));
        g_assert_cmpuint (v, ==, MM_MIPC_INTERNAL_EIPSPI_CMD);
        peer.n_commands++;
        tlvs = cnf_ok ();
        peer_send (MM_MIPC_INTERNAL_REGISTER_CMD_CNF, msg->txid, msg->slot, tlvs);
        return;

    case MM_MIPC_APN_ADD_PROFILE_LIST_REQ:
        g_assert_true (peer.ims_boot);
        g_assert_cmpuint (msg->slot, ==, peer.expected_slot);
        g_ptr_array_add (history, g_strdup ("profiles"));
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_APNLIST_COUNT, &v));
        g_assert_cmpuint (v, ==, peer.ims_only && !peer.expected_internet ? 2 : 3);
        {
            gsize length;
            const guint8 *record = mm_mipc_tlv_find (msg->payload, msg->len, 0x10000u - v, &length);
            g_assert_nonnull (record);
            g_assert_cmpuint (length, ==, MM_MIPC_APN_REC_SIZE);
            g_assert_cmpstr ((const gchar *) record + MM_MIPC_APN_REC_APN_OFF, ==,
                             peer.expected_internet ? peer.expected_internet : (peer.ims_only ? "ims" : "internet.example"));
            g_assert_cmpuint (mm_mipc_get (record + MM_MIPC_APN_REC_TYPE_OFF, 4), ==,
                             peer.ims_only && !peer.expected_internet ? MM_MIPC_APN_REC_TYPE_IMS : MM_MIPC_APN_REC_TYPE_DEFAULT);
        }
        tlvs = cnf_ok ();
        if (peer.fail_profiles)
            mm_mipc_put (tlvs->data + 4, 4, 1316568);
        peer_send (MM_MIPC_APN_ADD_PROFILE_LIST_CNF, msg->txid, msg->slot, tlvs);
        return;

    case MM_MIPC_IMS_SET_CONFIG_REQ:
        if (peer.ims_boot) {
            g_assert_cmpuint (msg->slot, ==, peer.expected_slot);
            g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_IMSCFG_FORM, &v));
            g_ptr_array_add (history, g_strdup (v ? "ims-key" : "ims-flags"));
            tlvs = cnf_ok ();
            if (peer.fail_config)
                mm_mipc_put (tlvs->data + 4, 4, 1);
            peer_send (MM_MIPC_IMS_SET_CONFIG_CNF, msg->txid, msg->slot, tlvs);
            return;
        }
        peer.ims_txid = msg->txid;
        tlvs = cnf_ok ();
        /* Neither a stale txid, a wrong confirm id nor another SIM may
         * complete this request. The real confirm waits for the SPI reply. */
        peer_send (MM_MIPC_IMS_SET_CONFIG_CNF, msg->txid + 50, msg->slot, tlvs);
        peer_send (MM_MIPC_DATA_ACT_CALL_CNF, msg->txid, msg->slot, tlvs);
        peer_send (MM_MIPC_IMS_SET_CONFIG_CNF, msg->txid, MM_MIPC_SLOT_SIM2, tlvs);
        g_byte_array_set_size (tlvs, 0);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_SPI_INDEX, 7);
        peer_send (MM_MIPC_INTERNAL_EIPSPI_CMD, 0x1234, 0x11, tlvs);
        return;

    case MM_MIPC_INTERNAL_EIPSPI_RSP:
        g_assert_cmpuint (msg->txid, ==, 0x1234);
        g_assert_cmpuint (msg->slot, ==, peer.ims_boot ? 0x12 : 0x11);
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_SPI_RSP_INDEX, &v));
        g_assert_cmpuint (v, ==, 7);
        peer.spi_reply_seen = TRUE;
        peer.n_spi_replies++;
        if (peer.ims_boot)
            return;
        tlvs = cnf_ok ();
        peer_send (MM_MIPC_IMS_SET_CONFIG_CNF, peer.ims_txid, MM_MIPC_SLOT_SIM1, tlvs);
        return;

    case MM_MIPC_SYS_AT_REQ:
        if (peer.concurrent) {
            if (peer.concurrent++ == 1)
                peer.first_concurrent = msg->txid;
            else {
                tlvs = cnf_ok ();
                mm_mipc_tlv_add_u32 (tlvs, 0x0100, 2);
                peer_send (MM_MIPC_SYS_AT_CNF, msg->txid, msg->slot, tlvs);
                g_byte_array_set_size (tlvs, 0);
                mm_mipc_tlv_add_u32 (tlvs, 0, 0);
                mm_mipc_tlv_add_u32 (tlvs, 0x0100, 1);
                peer_send (MM_MIPC_SYS_AT_CNF, peer.first_concurrent, msg->slot, tlvs);
            }
            return;
        }
        if (peer.ims_boot) {
            g_autofree gchar *cmd = mm_mipc_tlv_find_str (msg->payload, msg->len, MM_MIPC_T_AT_STRING);
            const gchar *reply = "OK";
            g_assert_cmpuint (msg->slot, ==, peer.expected_slot);
            g_assert_true (g_str_has_prefix (cmd, "AT"));
            if (g_str_equal (cmd, "AT+EIMSRCS=0,0"))
                g_ptr_array_add (history, g_strdup ("rcs-tunnel"));
            else {
                g_ptr_array_add (history, g_strdup (cmd + 2));
                if (g_str_equal (cmd, "AT+CIREG?"))
                    reply = peer.ims_registered ? "\r\n+CIREG: 2,1,1\r\nOK\r\n" : "\r\n+CIREG: 2,0,0\r\nOK\r\n";
                else if (g_str_equal (cmd, "AT+CGDCONT?"))
                    reply = peer.ims_only ? "\r\n+CGDCONT: 1,\"IPV4V6\",\"ims\",\"\",0,0\r\nOK\r\n" :
                        "\r\n+CGDCONT: 0,\"IP\",\"internet.example\",\"0.0.0.0\",0,0\r\nOK\r\n";
            }
            tlvs = cnf_ok ();
            mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, reply);
            peer_send (MM_MIPC_SYS_AT_CNF, msg->txid, msg->slot, tlvs);
            return;
        }
        if (!peer.timed_out_txid)
            peer.timed_out_txid = msg->txid; /* deliberately unanswered */
        else {
            tlvs = cnf_ok ();
            peer_send (MM_MIPC_SYS_AT_CNF, peer.timed_out_txid, msg->slot, tlvs);
            peer_send (MM_MIPC_SYS_AT_CNF, msg->txid, msg->slot, tlvs);
        }
        return;

    case MM_MIPC_DATA_ACT_CALL_REQ: {
        g_autoptr(GByteArray) ind = g_byte_array_new ();
        g_autoptr(GByteArray) ind_frame = NULL;
        g_autoptr(GByteArray) first = g_byte_array_new ();
        static const guint8   addr[4] = { 10, 20, 31, 221 };
        static const guint8   dns[8]  = { 192, 0, 2, 53, 192, 0, 2, 54 };
        guint8                rec[12];
        guint8                descr[12];

        peer.act_slot = msg->slot;
        peer.act_txid = msg->txid;
        g_free (peer.act_apn);
        peer.act_apn = mm_mipc_tlv_find_str (msg->payload, msg->len, MM_MIPC_T_ACT_APN);

        /* the interface indication comes before the confirm */
        mm_mipc_tlv_add_u32 (ind, MM_MIPC_T_EIF_TRANS_INTF_ID, 202);
        mm_mipc_tlv_add_u8  (ind, MM_MIPC_T_EIF_CMD, MM_MIPC_EIF_UP);
        mm_mipc_tlv_add_u8  (ind, MM_MIPC_T_EIF_V4_COUNT, 1);
        memset (rec, 0, sizeof (rec));
        memcpy (rec, addr, 4);
        mm_mipc_tlv_add (ind, MM_MIPC_T_EIF_V4_RECORDS, rec, sizeof (rec));
        ind_frame = mm_mipc_frame_build (MM_MIPC_INTERNAL_EIF_IND, MM_MIPC_TXID_NONE, msg->slot, ind->data, ind->len);

        tlvs = cnf_ok ();
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_CNF_AID, 0);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_CNF_TRANS_INTF_ID, 202);
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_CNF_V4_COUNT, 1);
        mm_mipc_tlv_add     (tlvs, MM_MIPC_T_CNF_V4_ADDRS, addr, 4);
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_CNF_V4_DNS_COUNT, 2);
        mm_mipc_tlv_add     (tlvs, MM_MIPC_T_CNF_V4_DNS, dns, 8);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_CNF_MTU_V4, 1500);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_CNF_EPS_BEARER, 5);
        /* an array descriptor (len 0, 12-byte body) with three u32 elements */
        {
            guint8 hdr[4];

            mm_mipc_put (hdr, 2, 0x812F);
            mm_mipc_put (hdr + 2, 2, 0);
            memset (descr, 0, sizeof (descr));
            mm_mipc_put (descr + 8, 2, 0xFFFD);
            mm_mipc_put (descr + 10, 2, 3);
            g_byte_array_append (tlvs, hdr, 4);
            g_byte_array_append (tlvs, descr, 12);
            mm_mipc_tlv_add_u32 (tlvs, 0xFFFD, 30);
            mm_mipc_tlv_add_u32 (tlvs, 0xFFFE, 0);
            mm_mipc_tlv_add_u32 (tlvs, 0xFFFF, 7);
        }
        act_cnf_frame = mm_mipc_frame_build (MM_MIPC_DATA_ACT_CALL_CNF, msg->txid, msg->slot, tlvs->data, tlvs->len);
        g_assert_nonnull (act_cnf_frame);

        /* one write: the whole indication plus the first 30 bytes of the
         * confirm; the rest follows from an idle */
        act_cnf_split = 30;
        g_byte_array_append (first, ind_frame->data, ind_frame->len);
        g_byte_array_append (first, act_cnf_frame->data, act_cnf_split);
        peer_write (first->data, first->len);
        g_idle_add (send_second_half, NULL);
        return;
    }

    case MM_MIPC_DATA_DEACT_CALL_REQ:
        peer.deact_seen = TRUE;
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_DEACT_AID, &v));
        peer.deact_aid = (guint8) v;
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_DEACT_CAUSE, &v));
        g_assert_cmpuint (v, ==, MM_MIPC_DEACT_CAUSE_NORMAL);
        tlvs = cnf_ok ();
        mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_CNF_AID, peer.deact_aid);
        peer_send (MM_MIPC_DATA_DEACT_CALL_CNF, msg->txid, msg->slot, tlvs);
        return;

    default:
        g_error ("fake modem: unexpected message 0x%04x", msg->msg_id);
    }
}

static gboolean
peer_input (GIOChannel *channel, GIOCondition cond, gpointer user_data)
{
    guint8 buf[4096];
    gsize  n = 0;

    if (g_io_channel_read_chars (channel, (gchar *) buf, sizeof (buf), &n, NULL) != G_IO_STATUS_NORMAL)
        return G_SOURCE_CONTINUE;
    g_byte_array_append (peer.rx, buf, n);
    for (;;) {
        MMMipcMsg msg;
        gsize     used = 0;
        gint      r;

        r = mm_mipc_frame_parse (peer.rx->data, peer.rx->len, &msg, &used);
        if (r == 0)
            break;
        g_assert_cmpint (r, ==, 1);
        peer_handle (&msg);
        g_byte_array_remove_range (peer.rx, 0, used);
    }
    return G_SOURCE_CONTINUE;
}

static gchar *
peer_setup (void)
{
    gint           slave;
    gchar          path[128];
    struct termios tio;

    g_assert_cmpint (openpty (&peer.fd, &slave, path, NULL, NULL), ==, 0);
    /* raw on both ends: a binary protocol */
    g_assert_cmpint (tcgetattr (slave, &tio), ==, 0);
    cfmakeraw (&tio);
    g_assert_cmpint (tcsetattr (slave, TCSANOW, &tio), ==, 0);
    g_assert_cmpint (tcgetattr (peer.fd, &tio), ==, 0);
    cfmakeraw (&tio);
    g_assert_cmpint (tcsetattr (peer.fd, TCSANOW, &tio), ==, 0);
    /* MMPortSerial opens the slave itself by name; keeping ours open keeps
     * the pty alive */
    peer.slave = slave;
    peer.rx = g_byte_array_new ();
    peer.channel = g_io_channel_unix_new (peer.fd);
    g_io_channel_set_encoding (peer.channel, NULL, NULL);
    g_io_channel_set_buffered (peer.channel, FALSE);
    peer.watch_id = g_io_add_watch (peer.channel, G_IO_IN, peer_input, NULL);

    g_assert_true (g_str_has_prefix (path, "/dev/"));
    return g_strdup (path + 5);
}

/*****************************************************************************/
/* the test */

typedef struct {
    GMainLoop  *loop;
    gboolean    ok;
    GError     *error;
    MMMipcCnf  *cnf;
    guint       n_indications;
    guint       last_ind_id;
    GByteArray *last_ind_payload;
    guint       completions;
    gboolean    close_on_reply;
} TestContext;

static void
indication_cb (MMPortMipc *port, guint msg_id, guint slot, guint txid, GByteArray *payload, TestContext *ctx)
{
    if (msg_id == MM_MIPC_INTERNAL_EIPSPI_CMD) {
        g_autoptr(GByteArray) reply = g_byte_array_new ();
        g_autoptr(GError) error = NULL;
        guint32 index;
        g_assert_cmpuint (txid, ==, 0x1234);
        g_assert_cmpuint (slot, ==, 0x11);
        g_assert_true (mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_SPI_INDEX, &index));
        mm_mipc_tlv_add_u32 (reply, MM_MIPC_T_SPI_RSP_INDEX, index);
        mm_mipc_tlv_add_u8 (reply, MM_MIPC_T_SPI_RSP_RESULT, 0);
        mm_mipc_tlv_add_u32 (reply, MM_MIPC_T_SPI_RSP_SPI, 0x123456);
        g_assert_true (mm_port_mipc_send_response (port, MM_MIPC_INTERNAL_EIPSPI_RSP, txid, slot, reply, &error));
        g_assert_no_error (error);
    }
    ctx->n_indications++;
    ctx->last_ind_id = msg_id;
    g_clear_pointer (&ctx->last_ind_payload, g_byte_array_unref);
    ctx->last_ind_payload = g_byte_array_ref (payload);
}

static void
session_ready (MMPortMipc *port, GAsyncResult *res, TestContext *ctx)
{
    ctx->ok = mm_port_mipc_open_session_finish (port, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static void
request_ready (MMPortMipc *port, GAsyncResult *res, TestContext *ctx)
{
    g_clear_pointer (&ctx->cnf, mm_mipc_cnf_free);
    g_clear_error (&ctx->error);
    ctx->cnf = mm_port_mipc_request_finish (port, res, &ctx->error);
    ctx->completions++;
    if (ctx->close_on_reply)
        mm_port_mipc_close (port);
    g_main_loop_quit (ctx->loop);
}

static void
test_close_in_callback (gconstpointer data)
{
    g_autofree gchar *name = NULL;
    g_autoptr(MMPortMipc) port = NULL;
    g_autoptr(GError) error = NULL;
    TestContext one = { 0 }, two = { 0 };
    gboolean timeout = GPOINTER_TO_INT (data);

    memset (&peer, 0, sizeof (peer));
    peer.n_test = timeout ? 0 : 1;
    name = peer_setup ();
    port = mm_port_mipc_new (name);
    g_assert_true (mm_port_mipc_open (port, &error));
    g_assert_no_error (error);
    one.loop = two.loop = g_main_loop_new (NULL, FALSE);
    one.close_on_reply = TRUE;
    mm_port_mipc_request (port, timeout ? MM_MIPC_SYS_AT_REQ : MM_MIPC_INTERNAL_TEST_REQ,
                          MM_MIPC_SLOT_ANY, NULL, 1, NULL,
                          (GAsyncReadyCallback) request_ready, &one);
    mm_port_mipc_request (port, MM_MIPC_INTERNAL_TEST_REQ, MM_MIPC_SLOT_ANY, NULL, 3, NULL,
                          (GAsyncReadyCallback) request_ready, &two);
    while (!one.completions || !two.completions)
        g_main_loop_run (one.loop);
    g_assert_cmpuint (one.completions, ==, 1);
    g_assert_cmpuint (two.completions, ==, 1);
    if (timeout) {
        g_assert_error (one.error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT);
        g_assert_null (one.cnf);
    } else {
        g_assert_no_error (one.error);
        g_assert_nonnull (one.cnf);
    }
    g_assert_error (two.error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED);
    g_assert_false (mm_port_mipc_is_open (port));
    g_clear_pointer (&one.cnf, mm_mipc_cnf_free);
    g_clear_error (&one.error);
    g_clear_error (&two.error);
    g_main_loop_unref (one.loop);
    g_source_remove (peer.watch_id);
    g_io_channel_unref (peer.channel);
    g_byte_array_unref (peer.rx);
    close (peer.fd);
    close (peer.slave);
}

static void
test_port_mipc (void)
{
    g_autofree gchar     *name = NULL;
    g_autoptr(MMPortMipc) port = NULL;
    g_autoptr(GError)     error = NULL;
    TestContext           ctx = { 0 };
    static const guint16  inds[] = { 0x4302, 0x4201, 0x4202, 0x4001, MM_MIPC_INTERNAL_EIPSPI_CMD };
    g_autoptr(GByteArray) tlvs = NULL;
    gsize                 vlen;
    const guint8         *val;
    guint32               v;

    ctx.loop = g_main_loop_new (NULL, FALSE);
    name = peer_setup ();

    port = mm_port_mipc_new (name);
    g_assert_nonnull (port);
    g_assert_cmpuint (mm_port_get_port_type (MM_PORT (port)), ==, MM_PORT_TYPE_UNKNOWN);
    g_assert_cmpuint (mm_port_get_subsys (MM_PORT (port)), ==, MM_PORT_SUBSYS_WWAN);
    g_assert_false (mm_port_mipc_get_session_ready (port));
    g_signal_connect (port, MM_PORT_MIPC_SIGNAL_INDICATION, G_CALLBACK (indication_cb), &ctx);

    g_assert_true (mm_port_mipc_open (port, &error));
    g_assert_no_error (error);

    /* session start: TEST (first one lost), OPEN, four registrations */
    mm_port_mipc_open_session (port, MM_MIPC_SLOT_SIM1, inds, G_N_ELEMENTS (inds), "test-client", NULL,
                               (GAsyncReadyCallback) session_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.ok);
    g_assert_true (mm_port_mipc_get_session_ready (port));
    g_assert_cmpuint (mm_port_mipc_get_modem_timeout_ms (port), ==, 300000);
    g_assert_cmpuint (peer.n_test, ==, 2);
    g_assert_cmpuint (peer.n_register, ==, 4);
    g_assert_cmpuint (peer.n_commands, ==, 1);
    g_assert_cmpuint (peer.register_slot, ==, MM_MIPC_SLOT_SIM1);
    g_assert_cmpuint (peer.registered[0], ==, 0x4302);
    g_assert_cmpuint (peer.registered[3], ==, 0x4001);

    /* data call: indication plus a confirm in two fragments */
    tlvs = g_byte_array_new ();
    mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_ACT_APN, "internet.carrier1");
    mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_ACT_APN_TYPE, MM_MIPC_ACT_APN_TYPE_DEFAULT);
    mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_ACT_PDP_TYPE, MM_MIPC_PDP_IP);
    mm_port_mipc_request (port, MM_MIPC_DATA_ACT_CALL_REQ, MM_MIPC_SLOT_SIM1, tlvs, 5, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_nonnull (ctx.cnf);
    g_assert_cmpuint (ctx.cnf->msg_id, ==, MM_MIPC_DATA_ACT_CALL_CNF);
    g_assert_cmpuint (ctx.cnf->txid, ==, peer.act_txid);
    g_assert_cmpuint (ctx.cnf->result, ==, 0);
    g_assert_cmpuint (peer.act_slot, ==, MM_MIPC_SLOT_SIM1);
    g_assert_cmpstr (peer.act_apn, ==, "internet.carrier1");
    g_assert_true (mm_mipc_tlv_find_uint (ctx.cnf->payload->data, ctx.cnf->payload->len, MM_MIPC_T_CNF_TRANS_INTF_ID, &v));
    g_assert_cmpuint (v, ==, 202);
    val = mm_mipc_tlv_find (ctx.cnf->payload->data, ctx.cnf->payload->len, MM_MIPC_T_CNF_V4_ADDRS, &vlen);
    g_assert_nonnull (val);
    g_assert_cmpuint (vlen, ==, 4);
    g_assert_cmpuint (val[3], ==, 221);
    val = mm_mipc_tlv_find (ctx.cnf->payload->data, ctx.cnf->payload->len, MM_MIPC_T_CNF_V4_DNS, &vlen);
    g_assert_cmpuint (vlen, ==, 8);
    g_assert_true (mm_mipc_tlv_find_uint (ctx.cnf->payload->data, ctx.cnf->payload->len, MM_MIPC_T_CNF_MTU_V4, &v));
    g_assert_cmpuint (v, ==, 1500);
    /* array element inside the confirm */
    g_assert_true (mm_mipc_tlv_find_uint (ctx.cnf->payload->data, ctx.cnf->payload->len, 0xFFFF, &v));
    g_assert_cmpuint (v, ==, 7);
    /* the indication was delivered separately, before the confirm */
    g_assert_cmpuint (ctx.n_indications, ==, 1);
    g_assert_cmpuint (ctx.last_ind_id, ==, MM_MIPC_INTERNAL_EIF_IND);
    g_assert_true (mm_mipc_tlv_find_uint (ctx.last_ind_payload->data, ctx.last_ind_payload->len, MM_MIPC_T_EIF_TRANS_INTF_ID, &v));
    g_assert_cmpuint (v, ==, 202);
    val = mm_mipc_tlv_find (ctx.last_ind_payload->data, ctx.last_ind_payload->len, MM_MIPC_T_EIF_V4_RECORDS, &vlen);
    g_assert_cmpuint (vlen, ==, 12);
    g_assert_cmpuint (val[0], ==, 10);

    /* a stale confirm without a request, then an indication: the confirm is
     * ignored, the indication delivered */
    {
        g_autoptr(GByteArray) stale = cnf_ok ();
        g_autoptr(GByteArray) ind = g_byte_array_new ();

        peer_send (MM_MIPC_DATA_ACT_CALL_CNF, 0x0777, MM_MIPC_SLOT_SIM1, stale);
        mm_mipc_tlv_add_u8  (ind, MM_MIPC_T_IND_AID, 0);
        mm_mipc_tlv_add_u32 (ind, MM_MIPC_T_IND_CAUSE, 36);
        peer_send (MM_MIPC_DATA_DEACT_CALL_IND, MM_MIPC_TXID_NONE, MM_MIPC_SLOT_SIM1, ind);
    }
    while (ctx.n_indications < 2)
        g_main_context_iteration (NULL, TRUE);
    g_assert_cmpuint (ctx.last_ind_id, ==, MM_MIPC_DATA_DEACT_CALL_IND);

    /* deactivation request still answered after the stale confirm */
    g_byte_array_set_size (tlvs, 0);
    mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_DEACT_AID, 0);
    mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_DEACT_CAUSE, MM_MIPC_DEACT_CAUSE_NORMAL);
    mm_port_mipc_request (port, MM_MIPC_DATA_DEACT_CALL_REQ, MM_MIPC_SLOT_SIM1, tlvs, 5, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_nonnull (ctx.cnf);
    g_assert_cmpuint (ctx.cnf->result, ==, 0);
    g_assert_true (peer.deact_seen);
    g_assert_cmpuint (peer.deact_aid, ==, 0);

    /* a malformed frame (bad magic) is dropped; the port keeps working */
    {
        static const guint8 garbage[16] = { 0x85, 0x19, 0x54, 0x24, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6 };

        peer_write (garbage, sizeof (garbage));
    }
    g_main_context_iteration (NULL, FALSE);
    mm_port_mipc_request (port, MM_MIPC_INTERNAL_TEST_REQ, MM_MIPC_SLOT_ANY, NULL, 5, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_nonnull (ctx.cnf);
    g_assert_cmpuint (ctx.cnf->msg_id, ==, MM_MIPC_INTERNAL_TEST_CNF);

    /* a request id that is not a request */
    mm_port_mipc_request (port, MM_MIPC_DATA_ACT_CALL_CNF, MM_MIPC_SLOT_SIM1, NULL, 5, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_null (ctx.cnf);
    g_assert_error (ctx.error, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS);

    {
        guint start = peer.n_register;
        guint commands = peer.n_commands;
        g_clear_error (&ctx.error);
        mm_port_mipc_open_session (port, 3, inds, G_N_ELEMENTS (inds), "test-client", NULL,
                                   (GAsyncReadyCallback) session_ready, &ctx);
        g_main_loop_run (ctx.loop);
        g_assert_no_error (ctx.error);
        g_assert_cmpuint (peer.n_register - start, ==, 8);
        g_assert_cmpuint (peer.n_commands - commands, ==, 1);
        g_assert_cmpuint (peer.registered_slots[start], ==, 1);
        g_assert_cmpuint (peer.registered_slots[start + 1], ==, 2);
    }

    /* The even-numbered IMS request waits while a modem command is answered. */
    mm_port_mipc_request (port, MM_MIPC_IMS_SET_CONFIG_REQ, MM_MIPC_SLOT_SIM1, NULL, 3, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_nonnull (ctx.cnf);
    g_assert_cmpuint (ctx.cnf->msg_id, ==, MM_MIPC_IMS_SET_CONFIG_CNF);
    g_assert_true (peer.spi_reply_seen);

    /* A late response after a real timeout must not poison the next request. */
    mm_port_mipc_request (port, MM_MIPC_SYS_AT_REQ, MM_MIPC_SLOT_SIM1, NULL, 1, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_error (ctx.error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT);
    mm_port_mipc_request (port, MM_MIPC_SYS_AT_REQ, MM_MIPC_SLOT_SIM1, NULL, 3, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_nonnull (ctx.cnf);
    g_assert_cmpuint (ctx.cnf->msg_id, ==, MM_MIPC_SYS_AT_CNF);

    /* Concurrent requests may complete in reverse order without mixing results. */
    {
        TestContext one = { 0 }, two = { 0 };
        one.loop = two.loop = ctx.loop;
        peer.concurrent = 1;
        mm_port_mipc_request (port, MM_MIPC_SYS_AT_REQ, MM_MIPC_SLOT_SIM1, NULL, 3, NULL,
                              (GAsyncReadyCallback) request_ready, &one);
        mm_port_mipc_request (port, MM_MIPC_SYS_AT_REQ, MM_MIPC_SLOT_SIM1, NULL, 3, NULL,
                              (GAsyncReadyCallback) request_ready, &two);
        while (!one.cnf || !two.cnf) {
            g_main_loop_run (ctx.loop);
            g_assert_no_error (one.error);
            g_assert_no_error (two.error);
        }
        g_assert_true (mm_mipc_tlv_find_uint (one.cnf->payload->data, one.cnf->payload->len, 0x0100, &v));
        g_assert_cmpuint (v, ==, 1);
        g_assert_true (mm_mipc_tlv_find_uint (two.cnf->payload->data, two.cnf->payload->len, 0x0100, &v));
        g_assert_cmpuint (v, ==, 2);
        mm_mipc_cnf_free (one.cnf);
        mm_mipc_cnf_free (two.cnf);
        peer.concurrent = 0;
    }

    /* An already cancelled operation must not send bytes. */
    {
        g_autoptr(GCancellable) cancelled = g_cancellable_new ();
        g_cancellable_cancel (cancelled);
        mm_port_mipc_request (port, MM_MIPC_INTERNAL_TEST_REQ, MM_MIPC_SLOT_ANY, NULL, 3, cancelled,
                              (GAsyncReadyCallback) request_ready, &ctx);
        g_main_loop_run (ctx.loop);
        g_assert_error (ctx.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        g_clear_error (&ctx.error);
    }

    /* A refused OPEN must not be reported as a usable session. */
    peer.reject_open = TRUE;
    mm_port_mipc_open_session (port, MM_MIPC_SLOT_SIM1, inds, G_N_ELEMENTS (inds), "test-client", NULL,
                               (GAsyncReadyCallback) session_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_error (ctx.error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED);
    g_assert_false (mm_port_mipc_get_session_ready (port));
    g_clear_error (&ctx.error);

    /* Closing with a request queued completes it rather than leaking a task. */
    mm_port_mipc_request (port, MM_MIPC_SYS_AT_REQ, MM_MIPC_SLOT_SIM1, NULL, 10, NULL,
                          (GAsyncReadyCallback) request_ready, &ctx);
    mm_port_mipc_close (port);
    g_main_loop_run (ctx.loop);
    g_assert_error (ctx.error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED);
    g_assert_false (mm_port_mipc_get_session_ready (port));

    g_clear_error (&ctx.error);
    g_clear_pointer (&ctx.last_ind_payload, g_byte_array_unref);
    g_clear_pointer (&ctx.cnf, mm_mipc_cnf_free);
    g_main_loop_unref (ctx.loop);
    g_source_remove (peer.watch_id);
    g_io_channel_unref (peer.channel);
    g_byte_array_unref (peer.rx);
    g_free (peer.act_apn);
    close (peer.fd);
    close (peer.slave);
}

static void
ims_ready (GObject *owner, GAsyncResult *res, TestContext *ctx)
{
    ctx->ok = mm_mtk_ccci_ims_setup_finish (res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static void
data_prepared (GObject *owner, GAsyncResult *res, TestContext *ctx)
{
    ctx->ok = mm_mtk_ccci_ims_prepare_data_finish (res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static void
test_data_profiles (gconstpointer data)
{
    g_autofree gchar *name = NULL;
    g_autoptr(MMPortMipc) port = NULL;
    g_autoptr(GError) error = NULL;
    MMMtkCcciIms *ims;
    TestContext ctx = { 0 };
    guint before;

    memset (&peer, 0, sizeof (peer));
    history = g_ptr_array_new_with_free_func (g_free);
    name = peer_setup ();
    peer.ims_boot = peer.ims_only = peer.ims_registered = TRUE;
    peer.expected_slot = GPOINTER_TO_UINT (data);
    port = mm_port_mipc_new (name);
    g_assert_true (mm_port_mipc_open (port, &error));
    g_assert_no_error (error);
    ctx.loop = g_main_loop_new (NULL, FALSE);
    ims = mm_mtk_ccci_ims_new ((MMBaseModem *) port, port, peer.expected_slot, "wwan0");
    mm_mtk_ccci_ims_set_radio_owner (ims, FALSE);
    /* A warm modem with only CID 1=ims must get two service profiles, never
     * a third default profile incorrectly pointing at the IMS APN. */
    mm_mtk_ccci_ims_setup (ims, (GAsyncReadyCallback) ims_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.ok);
    before = history->len;

    /* The next connection supplies its own APN, including after a SIM switch.
     * No CFUN/IMS restart or AT context rewrite is needed for this update. */
    peer.expected_internet = "requested.example";
    mm_mtk_ccci_ims_prepare_data (ims, peer.expected_internet, (GAsyncReadyCallback) data_prepared, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.ok);
    g_assert_cmpuint (history->len, ==, before + 1);
    g_assert_cmpstr (g_ptr_array_index (history, before), ==, "profiles");

    peer.expected_internet = "changed.example";
    peer.fail_profiles = TRUE;
    mm_mtk_ccci_ims_prepare_data (ims, peer.expected_internet, (GAsyncReadyCallback) data_prepared, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_error (ctx.error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED);
    g_assert_false (ctx.ok);
    g_clear_error (&ctx.error);

    peer.fail_profiles = FALSE;
    mm_mtk_ccci_ims_prepare_data (ims, peer.expected_internet, (GAsyncReadyCallback) data_prepared, &ctx);
    mm_mtk_ccci_ims_stop (ims);
    g_main_loop_run (ctx.loop);
    g_assert_error (ctx.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&ctx.error);

    mm_mtk_ccci_ims_free (ims);
    mm_port_mipc_close (port);
    g_main_loop_unref (ctx.loop);
    g_source_remove (peer.watch_id);
    g_io_channel_unref (peer.channel);
    g_byte_array_unref (peer.rx);
    close (peer.fd);
    close (peer.slave);
    g_ptr_array_unref (history);
}

static void
test_ims_controller (void)
{
    g_autofree gchar *name = NULL;
    g_autoptr(MMPortMipc) port = NULL;
    g_autoptr(GError) error = NULL;
    MMMtkCcciIms *ims;
    TestContext ctx = { 0 };
    g_autoptr(GByteArray) eif = g_byte_array_new ();
    g_autoptr(GByteArray) label = g_byte_array_new ();
    static const gchar *expected[] = {
        "+CIREG?", "+CFUN=4", "ims-key", "ims-flags", "+CGDCONT?", "profiles",
        "+CIREG=2", "+EIMSPDN= \"onoff\",1", "+EIMSCFG=1,0,0,0,1,1", "rcs-tunnel"
    };
    guint i;

    memset (&peer, 0, sizeof (peer));
    history = g_ptr_array_new_with_free_func (g_free);
    name = peer_setup ();
    peer.ims_boot = TRUE;
    peer.expected_slot = MM_MIPC_SLOT_SIM1;
    port = mm_port_mipc_new (name);
    g_assert_true (mm_port_mipc_open (port, &error));
    g_assert_no_error (error);
    ctx.loop = g_main_loop_new (NULL, FALSE);
    ims = mm_mtk_ccci_ims_new ((MMBaseModem *) port, port, MM_MIPC_SLOT_SIM1, "wwan0");
    mm_mtk_ccci_ims_setup (ims, (GAsyncReadyCallback) ims_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.ok);
    g_assert_cmpuint (history->len, ==, G_N_ELEMENTS (expected));
    for (i = 0; i < history->len; i++)
        g_assert_cmpstr (g_ptr_array_index (history, i), ==, expected[i]);

    /* Discovery may configure IMS before SIM power. Enabling afterwards must
     * keep that setup and must not send another CFUN=4 or profile list. */
    mm_mtk_ccci_ims_setup (ims, (GAsyncReadyCallback) ims_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.ok);
    g_assert_cmpuint (history->len, ==, G_N_ELEMENTS (expected));

    /* An unrelated internet EIF never becomes an IMS link. A positively
     * labelled IMS PDN is joined with its EIF regardless of arrival order. */
    mm_mipc_tlv_add_u32 (eif, MM_MIPC_T_EIF_TRANS_INTF_ID, 301);
    mm_mipc_tlv_add_u8 (eif, MM_MIPC_T_EIF_CMD, MM_MIPC_EIF_UP);
    g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                           MM_MIPC_INTERNAL_EIF_IND, 1u, 65535u, eif);
    g_assert_cmpuint (created_links, ==, 0);
    g_byte_array_set_size (eif, 0);
    mm_mipc_tlv_add_u32 (eif, MM_MIPC_T_EIF_TRANS_INTF_ID, 202);
    mm_mipc_tlv_add_u8 (eif, MM_MIPC_T_EIF_CMD, MM_MIPC_EIF_UP);
    g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                           MM_MIPC_INTERNAL_EIF_IND, 1u, 65535u, eif);
    g_assert_cmpuint (created_links, ==, 0);
    mm_mipc_tlv_add_u32 (label, MM_MIPC_T_IMSPDN_IFACE, 202);
    mm_mipc_tlv_add_u32 (label, MM_MIPC_T_IMSPDN_TYPE, 2);
    mm_mipc_tlv_add_u8 (label, MM_MIPC_T_IMSPDN_ACTION, 1);
    g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                           MM_MIPC_IMS_PDN_IND, 1u, 65535u, label);
    g_assert_cmpuint (created_links, ==, 1);
    g_assert_true (mm_mtk_ccci_ims_owns_link (ims, 2));
    g_assert_false (mm_mtk_ccci_ims_owns_link (ims, 1));
    {
        g_autoptr(GByteArray) urcs = g_byte_array_new ();
        mm_mipc_tlv_add_str (urcs, MM_MIPC_T_AT_STRING,
                              "\r\n+CIREGU: 1,1\r\n+EIF: 202, ifup, 1, 10.41.22.115, 0\r\n");
        g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                               MM_MIPC_SYS_AT_IND, 1u, 65535u, urcs);
    }
    g_assert_cmpstr (configured_address, ==, "10.41.22.115");
    mm_mtk_ccci_ims_stop (ims);
    g_assert_cmpuint (deleted_links, ==, 1);
    g_assert_false (mm_mtk_ccci_ims_owns_link (ims, 2));

    /* A registered primary stack must retain radio power on re-enable. */
    peer.ims_registered = TRUE;
    g_ptr_array_set_size (history, 0);
    mm_mtk_ccci_ims_setup (ims, (GAsyncReadyCallback) ims_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.ok);
    g_assert_cmpuint (history->len, ==, G_N_ELEMENTS (expected) - 1);
    for (i = 0; i < history->len; i++)
        g_assert_cmpstr (g_ptr_array_index (history, i), ==, expected[i ? i + 1 : 0]);
    mm_mtk_ccci_ims_stop (ims);
    peer.ims_registered = FALSE;

    /* SIM2 gets the same per-stack setup through its own tunnel, without a
     * second physical CFUN=4. Slot-1 indications cannot create slot-2 links. */
    mm_mtk_ccci_ims_free (ims);
    ims = mm_mtk_ccci_ims_new ((MMBaseModem *) port, port, MM_MIPC_SLOT_SIM2, "wwan0");
    mm_mtk_ccci_ims_set_radio_owner (ims, FALSE);
    peer.expected_slot = MM_MIPC_SLOT_SIM2;
    g_ptr_array_set_size (history, 0);
    mm_mtk_ccci_ims_setup (ims, (GAsyncReadyCallback) ims_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_no_error (ctx.error);
    g_assert_cmpuint (history->len, ==, G_N_ELEMENTS (expected) - 1);
    for (i = 0; i < history->len; i++)
        g_assert_cmpstr (g_ptr_array_index (history, i), ==, expected[i ? i + 1 : 0]);
    g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                           MM_MIPC_INTERNAL_EIF_IND, 1u, 65535u, eif);
    g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                           MM_MIPC_IMS_PDN_IND, 1u, 65535u, label);
    g_assert_false (mm_mtk_ccci_ims_owns_link (ims, 2));
    g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                           MM_MIPC_INTERNAL_EIF_IND, 2u, 65535u, eif);
    g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                           MM_MIPC_IMS_PDN_IND, 2u, 65535u, label);
    g_assert_true (mm_mtk_ccci_ims_owns_link (ims, 2));
    {
        g_autoptr(GByteArray) command = g_byte_array_new ();
        mm_mipc_tlv_add_u32 (command, MM_MIPC_T_SPI_INDEX, 7);
        g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                               MM_MIPC_INTERNAL_EIPSPI_CMD, 0x11u, 0x1234u, command);
        g_signal_emit_by_name (port, MM_PORT_MIPC_SIGNAL_INDICATION,
                               MM_MIPC_INTERNAL_EIPSPI_CMD, 0x12u, 0x1234u, command);
        while (!peer.n_spi_replies)
            g_main_context_iteration (NULL, TRUE);
        g_assert_cmpuint (peer.n_spi_replies, ==, 1);
    }
    mm_mtk_ccci_ims_stop (ims);

    /* Refused key setup stops before profiles/onoff/radio could proceed. */
    g_ptr_array_set_size (history, 0);
    peer.fail_config = TRUE;
    mm_mtk_ccci_ims_setup (ims, (GAsyncReadyCallback) ims_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_error (ctx.error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED);
    g_assert_cmpuint (history->len, ==, 2);
    g_clear_error (&ctx.error);

    /* Disabling while a query is pending must stop the startup sequence. */
    g_ptr_array_set_size (history, 0);
    peer.fail_config = FALSE;
    mm_mtk_ccci_ims_setup (ims, (GAsyncReadyCallback) ims_ready, &ctx);
    mm_mtk_ccci_ims_stop (ims);
    g_main_loop_run (ctx.loop);
    g_assert_error (ctx.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint (history->len, ==, 1);
    mm_mtk_ccci_ims_free (ims);
    mm_port_mipc_close (port);
    g_clear_error (&ctx.error);
    g_main_loop_unref (ctx.loop);
    g_source_remove (peer.watch_id);
    g_io_channel_unref (peer.channel);
    g_byte_array_unref (peer.rx);
    close (peer.fd);
    close (peer.slave);
    g_ptr_array_unref (history);
    g_clear_pointer (&configured_address, g_free);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/MM/mtk-ccci/port-mipc", test_port_mipc);
    g_test_add_data_func ("/MM/mtk-ccci/close-in-callback", GINT_TO_POINTER (FALSE), test_close_in_callback);
    g_test_add_data_func ("/MM/mtk-ccci/close-in-timeout", GINT_TO_POINTER (TRUE), test_close_in_callback);
    g_test_add_func ("/MM/mtk-ccci/ims-controller", test_ims_controller);
    g_test_add_data_func ("/MM/mtk-ccci/data-profiles-sim1", GUINT_TO_POINTER (MM_MIPC_SLOT_SIM1), test_data_profiles);
    g_test_add_data_func ("/MM/mtk-ccci/data-profiles-sim2", GUINT_TO_POINTER (MM_MIPC_SLOT_SIM2), test_data_profiles);
    return g_test_run ();
}
