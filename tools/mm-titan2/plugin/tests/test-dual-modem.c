/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The dual-modem mode parts of the mtk-ccci plugin against a fake modem on a
 * pseudo-terminal (the real MMPortMipc transport):
 *  - the shared context: one MMPortMipc per MIPC channel, kept while any
 *    modem object holds it (reprobe of either object), session start joined
 *    (one TEST/OPEN on the wire), one incoming-call approval per command;
 *  - the DSDS approval policy: reject (cause 21) while the other slot has a
 *    call, the RSP bytes, the switch, the voice clients;
 *  - the radio coordinator: the AT+EFUN mask of both stacks, IMS before
 *    radio-on, serialised requests, SIM-busy retry, the IMS radio-off hook in
 *    the dual and the single-object mode;
 *  - the speech path reference count;
 *  - the AT bridge: ModemManager's own unix-socket AT port (MMPortSerialAt,
 *    v1 parser) against the bridge, commands as SYS_AT on slot 2, framing,
 *    local V.250 commands, CME errors, URC forwarding, the CMGS prompt.
 * Messages: the MIPC session start, the SYS_AT tunnel and the incoming-call approval.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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
#include "mm-mtk-ccci-shared.h"
#include "mm-mtk-ccci-at-bridge.h"
#include "mm-base-modem-at.h"
#include "mm-wwan-link.h"
#include "mm-port-serial-at.h"
#include "mm-serial-parsers.h"
#include <gio/gunixsocketaddress.h>

/*****************************************************************************/
/* boundaries of the IMS controller that these tests never reach */

static guint owner_port_commands;

void
mm_base_modem_at_command (MMBaseModem *self, const gchar *cmd, guint timeout,
                          gboolean cached, GAsyncReadyCallback callback, gpointer user_data)
{
    /* the shared controllers have the radio hook: never the owner's port */
    GTask *task = g_task_new (self, NULL, callback, user_data);

    owner_port_commands++;
    g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "unexpected owner AT command %s", cmd);
    g_object_unref (task);
}

const gchar *
mm_base_modem_at_command_finish (MMBaseModem *self, GAsyncResult *res, GError **error)
{
    g_task_propagate_boolean (G_TASK (res), error);
    return NULL;
}

gboolean
mm_wwan_link_add (const gchar *parent, guint id, const gchar *name, gboolean *existed, GError **error)
{
    *existed = FALSE;
    return TRUE;
}

gboolean
mm_wwan_link_del (const gchar *name, GError **error)
{
    return TRUE;
}

gboolean
mm_wwan_link_configure (const gchar *name, guint mtu, const gchar *address, GError **error)
{
    return TRUE;
}

/*****************************************************************************/
/* the fake modem */

typedef struct {
    gint        fd, slave;
    GIOChannel *channel;
    guint       watch_id;
    GByteArray *rx;
    guint       n_test, n_open, n_register;
    guint       n_approve_rsp;
    guint16     approve_txid;
    guint8      approve_slot;
    GByteArray *approve_payload; /* the last RSP's TLVs */
    guint       busy_efun;      /* answer this many EFUN with SIM busy */
    gboolean    hold_sys_at;    /* keep SYS_AT CNFs until released */
    GPtrArray  *held;           /* GByteArray frames */
    GPtrArray  *history;        /* "<slot>:<text>" of SYS_AT, "<slot>:ims-cfg" ... */
} Peer;

static Peer peer;

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
peer_send (guint16 id, guint16 txid, guint8 slot, GByteArray *tlvs)
{
    g_autoptr(GByteArray) frame = mm_mipc_frame_build (id, txid, slot, tlvs ? tlvs->data : NULL, tlvs ? tlvs->len : 0);

    g_assert_nonnull (frame);
    if (peer.hold_sys_at && id == MM_MIPC_SYS_AT_CNF) {
        g_ptr_array_add (peer.held, g_byte_array_ref (frame));
        return;
    }
    peer_write (frame->data, frame->len);
}

static void
peer_release_held (void)
{
    guint i;

    for (i = 0; i < peer.held->len; i++) {
        GByteArray *frame = g_ptr_array_index (peer.held, i);
        peer_write (frame->data, frame->len);
    }
    g_ptr_array_set_size (peer.held, 0);
}

static GByteArray *
cnf (guint32 result)
{
    GByteArray *tlvs = g_byte_array_new ();

    mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_RESULT, result);
    return tlvs;
}

/* the text the fake modem's AT engine answers on the tunnel */
static void
peer_sys_at (const MMMipcMsg *msg)
{
    g_autofree gchar     *text = NULL;
    g_autoptr(GByteArray) tlvs = NULL;
    const gchar          *reply = "OK";
    guint32               result = 0;
    const guint8         *raw;
    gsize                 vlen = 0;

    /* the raw string: an SMS submit carries CR and Ctrl-Z */
    raw = mm_mipc_tlv_find (msg->payload, msg->len, MM_MIPC_T_AT_STRING, &vlen);
    g_assert_nonnull (raw);
    text = g_strndup ((const gchar *) raw, strnlen ((const gchar *) raw, vlen));
    g_ptr_array_add (peer.history, g_strdup_printf ("%u:%s", msg->slot, text));
    if (g_str_has_prefix (text, "AT+EFUN=") && peer.busy_efun) {
        peer.busy_efun--;
        reply = "+CME ERROR: 14";
    } else if (g_str_equal (text, "AT+CIREG?"))
        reply = "+CIREG: 2,0,0\r\nOK";
    else if (g_str_equal (text, "AT+CGDCONT?"))
        reply = "+CGDCONT: 0,\"IP\",\"internet.example\",\"0.0.0.0\",0,0\r\nOK";
    else if (g_str_equal (text, "AT+CFUN?"))
        reply = "+CFUN: 4\r\nOK";
    else if (g_str_equal (text, "AT+CEREG=?"))
        /* level 3 offered on slot 1 only */
        reply = msg->slot == 1 ? "+CEREG: (0-5)\r\nOK" : "+CEREG: (0-2)\r\nOK";
    else if (g_str_equal (text, "AT+CEREG?"))
        reply = "+CEREG: 3,3,\"1262\",\"0A1B2C3D\",7,0,15\r\nOK";
    else if (g_str_equal (text, "AT+CPIN?"))
        reply = "+CPIN: READY\r\n\r\nOK";
    else if (g_str_equal (text, "AT+CPMS?"))
        reply = "+CPMS: \"SM\",1,10,\"SM\",1,10,\"SM\",1,10";      /* no final code */
    else if (g_str_equal (text, "AT+CMGR=9"))
        reply = "+CME ERROR: 10";
    else if (g_str_equal (text, "AT+EMPTY")) {
        reply = "";
        result = 5;
    } else if (g_str_has_prefix (text, "AT+CMGS=10\r"))
        reply = "+CMGS: 5\r\nOK";
    tlvs = cnf (result);
    mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, reply);
    peer_send (MM_MIPC_SYS_AT_CNF, msg->txid, msg->slot, tlvs);
}

static void
peer_handle (const MMMipcMsg *msg)
{
    g_autoptr(GByteArray) tlvs = NULL;
    guint32               v;

    switch (msg->msg_id) {
    case MM_MIPC_INTERNAL_TEST_REQ:
        peer.n_test++;
        peer_send (MM_MIPC_INTERNAL_TEST_CNF, msg->txid, msg->slot, NULL);
        return;
    case MM_MIPC_INTERNAL_OPEN_REQ:
        peer.n_open++;
        tlvs = cnf (0);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_OPEN_VERSION, 1);
        peer_send (MM_MIPC_INTERNAL_OPEN_CNF, msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_INTERNAL_REGISTER_IND_REQ:
    case MM_MIPC_INTERNAL_REGISTER_CMD_REQ:
        peer.n_register++;
        tlvs = cnf (0);
        peer_send (MM_MIPC_CNF_OF (msg->msg_id), msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_IMS_SET_CONFIG_REQ:
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_IMSCFG_FORM, &v));
        g_ptr_array_add (peer.history, g_strdup_printf ("%u:%s", msg->slot, v ? "ims-key" : "ims-flags"));
        tlvs = cnf (0);
        peer_send (MM_MIPC_IMS_SET_CONFIG_CNF, msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_APN_ADD_PROFILE_LIST_REQ:
        g_ptr_array_add (peer.history, g_strdup_printf ("%u:profiles", msg->slot));
        tlvs = cnf (0);
        peer_send (MM_MIPC_APN_ADD_PROFILE_LIST_CNF, msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_SYS_AT_REQ:
        peer_sys_at (msg);
        return;
    case MM_MIPC_DATA_SET_DATA_ALLOW_REQ:
    case MM_MIPC_CALL_DATA_PREFER_SET_REQ:
    case MM_MIPC_NW_SET_RADIO_CAPABILITY_REQ:
    case MM_MIPC_SYS_MULTI_SIM_CONFIG_REQ:
    case MM_MIPC_SYS_SET_DSBP_REQ: {
        const gchar *name = msg->msg_id == MM_MIPC_DATA_SET_DATA_ALLOW_REQ ? "allow" :
                            msg->msg_id == MM_MIPC_CALL_DATA_PREFER_SET_REQ ? "prefer" :
                            msg->msg_id == MM_MIPC_NW_SET_RADIO_CAPABILITY_REQ ? "radio-cap" :
                            msg->msg_id == MM_MIPC_SYS_MULTI_SIM_CONFIG_REQ ? "msim" : "dsbp";
        g_autofree gchar *dump = mm_mipc_tlv_dump (msg->payload, msg->len, 8);

        g_ptr_array_add (peer.history, g_strdup_printf ("%u:%s(%s)", msg->slot, name, dump));
        /* the multi-SIM config answers 1, as observed on the phone */
        tlvs = cnf (msg->msg_id == MM_MIPC_SYS_MULTI_SIM_CONFIG_REQ ? 1 : 0);
        mm_mipc_tlv_add_u8 (tlvs, 0x0101, 7);   /* an undescribed CNF TLV */
        peer_send (MM_MIPC_CNF_OF (msg->msg_id), msg->txid, msg->slot, tlvs);
        return;
    }
    case MM_MIPC_NW_SET_RADIO_STATE_REQ: {
        g_autofree gchar *dump = mm_mipc_tlv_dump (msg->payload, msg->len, 8);

        g_ptr_array_add (peer.history, g_strdup_printf ("%u:radio-state(%s)", msg->slot, dump));
        tlvs = cnf (0);
        peer_send (MM_MIPC_NW_SET_RADIO_STATE_CNF, msg->txid, msg->slot, tlvs);
        return;
    }
    case MM_MIPC_NW_GET_PS_REQ: {
        /* the registration block: searching on the first poll of a slot, then home */
        static guint polls[3];
        guint8       block[28] = { 0 };

        g_ptr_array_add (peer.history, g_strdup_printf ("%u:get-ps", msg->slot));
        block[0] = polls[msg->slot & 3]++ ? 1 : 2;
        memcpy (block + 1, "00101", 5);
        mm_mipc_put (block + 8, 4, 0x01234516);
        mm_mipc_put (block + 12, 4, 100);
        mm_mipc_put (block + 20, 4, 0x1A2B);
        tlvs = cnf (0);
        mm_mipc_tlv_add (tlvs, 0x8106, block, sizeof (block));
        peer_send (MM_MIPC_NW_GET_PS_CNF, msg->txid, msg->slot, tlvs);
        return;
    }
    case MM_MIPC_CALL_APPROVE_RSP:
        peer.n_approve_rsp++;
        peer.approve_txid = msg->txid;
        peer.approve_slot = msg->slot;
        g_byte_array_set_size (peer.approve_payload, 0);
        g_byte_array_append (peer.approve_payload, msg->payload, msg->len);
        return;
    default:
        g_assert_not_reached ();
    }
}

static gboolean
peer_input (GIOChannel *channel, GIOCondition cond, gpointer user_data)
{
    guint8 buf[4096];
    gssize n = read (peer.fd, buf, sizeof (buf));

    if (n <= 0)
        return G_SOURCE_CONTINUE;
    g_byte_array_append (peer.rx, buf, n);
    for (;;) {
        MMMipcMsg msg;
        gsize     used = 0;
        gint      r = mm_mipc_frame_parse (peer.rx->data, peer.rx->len, &msg, &used);
        g_autoptr(GByteArray) copy = NULL;

        if (r == 0)
            break;
        g_assert_cmpint (r, >, 0);
        copy = g_byte_array_new ();
        g_byte_array_append (copy, msg.payload, msg.len);
        msg.payload = copy->data;
        g_byte_array_remove_range (peer.rx, 0, used);
        peer_handle (&msg);
    }
    return G_SOURCE_CONTINUE;
}

static gchar *
peer_setup (void)
{
    gchar          path[128];
    struct termios tio;

    memset (&peer, 0, sizeof (peer));
    g_assert_cmpint (openpty (&peer.fd, &peer.slave, path, NULL, NULL), ==, 0);
    g_assert_cmpint (tcgetattr (peer.slave, &tio), ==, 0);
    cfmakeraw (&tio);
    g_assert_cmpint (tcsetattr (peer.slave, TCSANOW, &tio), ==, 0);
    g_assert_cmpint (tcgetattr (peer.fd, &tio), ==, 0);
    cfmakeraw (&tio);
    g_assert_cmpint (tcsetattr (peer.fd, TCSANOW, &tio), ==, 0);
    peer.rx = g_byte_array_new ();
    peer.held = g_ptr_array_new_with_free_func ((GDestroyNotify) g_byte_array_unref);
    peer.history = g_ptr_array_new_with_free_func (g_free);
    peer.approve_payload = g_byte_array_new ();
    peer.channel = g_io_channel_unix_new (peer.fd);
    g_io_channel_set_encoding (peer.channel, NULL, NULL);
    g_io_channel_set_buffered (peer.channel, FALSE);
    peer.watch_id = g_io_add_watch (peer.channel, G_IO_IN, peer_input, NULL);
    g_assert_true (g_str_has_prefix (path, "/dev/"));
    return g_strdup (path + 5);
}

static void
peer_teardown (void)
{
    g_source_remove (peer.watch_id);
    g_io_channel_unref (peer.channel);
    g_byte_array_unref (peer.rx);
    g_ptr_array_unref (peer.held);
    g_ptr_array_unref (peer.history);
    g_byte_array_unref (peer.approve_payload);
    close (peer.fd);
    close (peer.slave);
}

/* the SYS_AT/IMS history from index @from as one string "a|b|c" */
static gchar *
history_from (guint from)
{
    GString *s = g_string_new (NULL);
    guint    i;

    for (i = from; i < peer.history->len; i++)
        g_string_append_printf (s, "%s%s", s->len ? "|" : "", (const gchar *) g_ptr_array_index (peer.history, i));
    return g_string_free (s, FALSE);
}

/*****************************************************************************/
/* async helpers */

typedef struct {
    guint    done;
    gboolean ok;
    GError  *error;
    gchar   *reply;
} Wait;

static void
wait_for (Wait *w, guint n)
{
    while (w->done < n)
        g_main_context_iteration (NULL, TRUE);
}

static void
session_ready (MMPortMipc *port, GAsyncResult *res, Wait *w)
{
    w->ok = mm_port_mipc_open_session_finish (port, res, &w->error);
    w->done++;
}

static void
open_session (MMPortMipc *port, Wait *w)
{
    static const guint16 inds[] = { MM_MIPC_SYS_AT_IND, MM_MIPC_INTERNAL_EIPSPI_CMD };

    mm_port_mipc_open_session (port, MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2, inds, G_N_ELEMENTS (inds),
                               "test", NULL, (GAsyncReadyCallback) session_ready, w);
}

static void
radio_ready (MMMtkCcciShared *shared, GAsyncResult *res, Wait *w)
{
    w->ok = mm_mtk_ccci_shared_radio_set_finish (shared, res, &w->error);
    w->done++;
}

static void
prepare_ready (MMMtkCcciShared *shared, GAsyncResult *res, Wait *w)
{
    w->ok = mm_mtk_ccci_shared_radio_prepare_finish (shared, res, &w->error);
    w->done++;
}

static void
ims_ready (GObject *source, GAsyncResult *res, Wait *w)
{
    w->ok = mm_mtk_ccci_ims_setup_finish (res, &w->error);
    w->done++;
}

static void
at_ready (MMPortSerialAt *port, GAsyncResult *res, Wait *w)
{
    g_clear_pointer (&w->reply, g_free);
    g_clear_error (&w->error);
    w->reply = mm_port_serial_at_command_finish (port, res, &w->error);
    w->done++;
}

/* a shared context with an open MIPC session on a fresh fake modem */
static MMMtkCcciShared *
shared_setup (gchar **name)
{
    MMMtkCcciShared  *shared;
    g_autoptr(GError) error = NULL;
    Wait              w = { 0 };

    *name = peer_setup ();
    shared = mm_mtk_ccci_shared_acquire (*name);
    g_assert_true (mm_port_mipc_open (mm_mtk_ccci_shared_peek_mipc (shared), &error));
    g_assert_no_error (error);
    open_session (mm_mtk_ccci_shared_peek_mipc (shared), &w);
    wait_for (&w, 1);
    g_assert_no_error (w.error);
    g_assert_true (w.ok);
    return shared;
}

static void
shared_teardown (MMMtkCcciShared *shared, const gchar *name)
{
    mm_port_mipc_close (mm_mtk_ccci_shared_peek_mipc (shared));
    g_object_unref (shared);
    g_assert_null (mm_mtk_ccci_shared_peek (name));
    peer_teardown ();
}

/*****************************************************************************/
/* pure helpers */

static void
test_bridge_helpers (void)
{
    g_autofree gchar *a = NULL, *b = NULL, *c = NULL, *d = NULL, *e = NULL;

    g_assert_true (mm_mtk_ccci_at_bridge_urc_wanted ("+CEREG: 1,\"1A2B\",\"01A2B3C\",7"));
    g_assert_true (mm_mtk_ccci_at_bridge_urc_wanted ("  +CMTI: \"SM\",3"));
    g_assert_true (mm_mtk_ccci_at_bridge_urc_wanted ("+CUSD: 0,\"x\",15"));
    /* handled over MIPC directly (calls, IMS): never as port text */
    g_assert_false (mm_mtk_ccci_at_bridge_urc_wanted ("+ECPI: 1,130,0,0,0,20,\"1\",129,\"\""));
    g_assert_false (mm_mtk_ccci_at_bridge_urc_wanted ("+CIREGU: 1,1"));
    g_assert_false (mm_mtk_ccci_at_bridge_urc_wanted ("RING"));
    /* no handler on the port before messaging setup: would pollute replies */
    g_assert_false (mm_mtk_ccci_at_bridge_urc_wanted ("+CDSI: \"SM\",4"));
    g_assert_false (mm_mtk_ccci_at_bridge_urc_wanted ("+CGEV: ME PDN ACT 1"));
    g_assert_false (mm_mtk_ccci_at_bridge_urc_wanted (NULL));

    a = mm_mtk_ccci_at_bridge_frame_reply ("+CPIN: READY\r\n\r\nOK", 0);
    g_assert_cmpstr (a, ==, "\r\n+CPIN: READY\r\n\r\nOK\r\n");
    b = mm_mtk_ccci_at_bridge_frame_reply ("+CSQ: 20,99", 0);
    g_assert_cmpstr (b, ==, "\r\n+CSQ: 20,99\r\n\r\nOK\r\n");
    c = mm_mtk_ccci_at_bridge_frame_reply ("", 7);
    g_assert_cmpstr (c, ==, "\r\nERROR\r\n");
    d = mm_mtk_ccci_at_bridge_frame_reply ("\r\n+CME ERROR: 10\r\n", 0);
    g_assert_cmpstr (d, ==, "\r\n+CME ERROR: 10\r\n");
    e = mm_mtk_ccci_at_bridge_frame_reply ("+CMGL: 1,1,,23\r\n0791AB\r\nOK", 0);
    g_assert_cmpstr (e, ==, "\r\n+CMGL: 1,1,,23\r\n\r\n0791AB\r\n\r\nOK\r\n");
}

static void
test_speech_refcount (void)
{
    g_autofree gchar *name = peer_setup ();
    MMMtkCcciShared  *shared = mm_mtk_ccci_shared_acquire (name);

    /* SIM1 call starts the path, a SIM2 call meanwhile keeps it, the path
     * goes off with the last call of either SIM */
    g_assert_cmpint (mm_mtk_ccci_shared_speech_update (shared, 1, TRUE), ==, 1);
    g_assert_cmpint (mm_mtk_ccci_shared_speech_update (shared, 2, TRUE), ==, 0);
    g_assert_cmpint (mm_mtk_ccci_shared_speech_update (shared, 1, FALSE), ==, 0);
    g_assert_cmpuint (mm_mtk_ccci_shared_speech_get (shared), ==, 2);
    g_assert_cmpint (mm_mtk_ccci_shared_speech_update (shared, 2, FALSE), ==, -1);
    g_assert_cmpint (mm_mtk_ccci_shared_speech_update (shared, 2, FALSE), ==, 0);
    /* the single-object mode: both bits at once */
    g_assert_cmpint (mm_mtk_ccci_shared_speech_update (shared, 3, TRUE), ==, 1);
    g_assert_cmpint (mm_mtk_ccci_shared_speech_update (shared, 3, FALSE), ==, -1);
    g_object_unref (shared);
    peer_teardown ();
}

/* the last CALL_APPROVE_INCOMING_RSP, byte for byte: 0x0100 u8 approve,
 * 0x0102 u32 call id, 0x0103 u32 seq, on rejection 0x0101 u32 cause 21
 * */
static void
approve_rsp_check (guint8 approve, guint32 call_id, guint32 seq)
{
    g_autoptr(GByteArray) expected = g_byte_array_new ();

    mm_mipc_tlv_add_u8 (expected, 0x0100, approve);
    mm_mipc_tlv_add_u32 (expected, 0x0102, call_id);
    mm_mipc_tlv_add_u32 (expected, 0x0103, seq);
    if (!approve)
        mm_mipc_tlv_add_u32 (expected, 0x0101, 21);
    g_assert_cmpmem (peer.approve_payload->data, peer.approve_payload->len, expected->data, expected->len);
}

/*****************************************************************************/
/* two-modem lifecycle on one shared context */

static void
test_shared_lifecycle (void)
{
    g_autofree gchar *name = peer_setup ();
    g_autoptr(GError) error = NULL;
    MMMtkCcciShared  *sim1, *sim2, *again;
    MMPortMipc       *port;
    Wait              a = { 0 }, b = { 0 }, c = { 0 };

    /* SIM2's object may be created first; SIM1's create_wwan_port then
     * finds the same port */
    sim2 = mm_mtk_ccci_shared_acquire (name);
    sim1 = mm_mtk_ccci_shared_acquire (name);
    g_assert_true (sim1 == sim2);
    g_assert_true (mm_mtk_ccci_shared_peek (name) == sim1);
    port = mm_mtk_ccci_shared_peek_mipc (sim1);
    g_assert_cmpstr (mm_mtk_ccci_shared_get_wwan_device (sim1), ==, "wwan0");
    g_assert_nonnull (mm_mtk_ccci_shared_peek_ims (sim1, 1));
    g_assert_nonnull (mm_mtk_ccci_shared_peek_ims (sim1, 2));
    g_assert_null (mm_mtk_ccci_shared_peek_ims (sim1, 3));

    /* both objects open the port and start the session at the same time:
     * one TEST/OPEN on the wire, both get the result */
    g_assert_true (mm_port_mipc_open (port, &error));
    g_assert_true (mm_port_mipc_open (port, &error));
    open_session (port, &a);
    g_assert_true (mm_port_mipc_session_starting (port));
    open_session (port, &b);
    wait_for (&a, 1);
    wait_for (&b, 1);
    g_assert_true (a.ok && b.ok);
    g_assert_cmpuint (peer.n_test, ==, 1);
    g_assert_cmpuint (peer.n_open, ==, 1);
    g_assert_cmpuint (peer.n_register, ==, 3); /* SYS_AT_IND x 2 slots, EIPSPI CMD once */
    g_assert_false (mm_port_mipc_session_starting (port));

    /* SIM1's object is reprobed: it closes and drops its reference; SIM2's
     * keeps the session; the new SIM1 object gets the same port, still ready */
    mm_port_mipc_close (port);
    g_object_unref (sim1);
    g_assert_true (mm_port_mipc_get_session_ready (port));
    again = mm_mtk_ccci_shared_acquire (name);
    g_assert_true (again == sim2);
    g_assert_true (mm_mtk_ccci_shared_peek_mipc (again) == port);
    g_assert_true (mm_port_mipc_get_session_ready (port));

    /* an incoming call on SIM2: answered once, whatever objects listen */
    {
        g_autoptr(GByteArray) cmd = g_byte_array_new ();

        mm_mipc_tlv_add_u32 (cmd, 0x0100, 3);
        mm_mipc_tlv_add_str (cmd, 0x8101, "+70000000000");
        mm_mipc_tlv_add_u32 (cmd, 0x0103, 77);
        peer_send (MM_MIPC_CALL_APPROVE_CMD, 0x2222, MM_MIPC_SLOT_SIM2, cmd);
        while (!peer.n_approve_rsp)
            g_main_context_iteration (NULL, TRUE);
        /* let a duplicate show up if there were one */
        g_main_context_iteration (NULL, FALSE);
        g_assert_cmpuint (peer.n_approve_rsp, ==, 1);
        g_assert_cmpuint (peer.approve_txid, ==, 0x2222);
        g_assert_cmpuint (peer.approve_slot, ==, MM_MIPC_SLOT_SIM2);
        approve_rsp_check (1, 3, 77);
    }

    /* last user gone: port closed, context gone */
    mm_port_mipc_close (port);
    g_assert_false (mm_port_mipc_is_open (port));
    g_object_unref (again);
    g_object_unref (sim2);
    g_assert_null (mm_mtk_ccci_shared_peek (name));

    /* a new context starts a new session (port removed and re-added) */
    sim1 = mm_mtk_ccci_shared_acquire (name);
    port = mm_mtk_ccci_shared_peek_mipc (sim1);
    g_assert_true (mm_port_mipc_open (port, &error));
    open_session (port, &c);
    wait_for (&c, 1);
    g_assert_true (c.ok);
    g_assert_cmpuint (peer.n_open, ==, 2);
    mm_port_mipc_close (port);
    g_object_unref (sim1);
    peer_teardown ();
}

/*****************************************************************************/
/* DSDS incoming-call policy (the voice clients are stand-ins for the modem
 * objects: their call counts and the rejected-call notices) */

static guint fake_calls[3];      /* per slot */
static guint n_rejected;
static guint8 rejected_slot;
static guint rejected_id;
static gchar *rejected_number;

static guint
fake_calls_present (GObject *client, guint8 slot)
{
    return fake_calls[slot];
}

static void
fake_rejected (GObject *client, guint8 slot, guint call_id, const gchar *number)
{
    n_rejected++;
    rejected_slot = slot;
    rejected_id = call_id;
    g_free (rejected_number);
    rejected_number = g_strdup (number);
}

static void
approve_cmd (guint16 txid, guint8 slot, guint32 call_id, guint32 seq, const gchar *number)
{
    g_autoptr(GByteArray) cmd = g_byte_array_new ();
    guint                 before = peer.n_approve_rsp;

    mm_mipc_tlv_add_u32 (cmd, 0x0100, call_id);
    if (number)
        mm_mipc_tlv_add_str (cmd, 0x8101, number);
    mm_mipc_tlv_add_u32 (cmd, 0x0102, 145);
    mm_mipc_tlv_add_u32 (cmd, 0x0103, seq);
    mm_mipc_tlv_add_u32 (cmd, 0x0104, 20);
    peer_send (MM_MIPC_CALL_APPROVE_CMD, txid, slot, cmd);
    while (peer.n_approve_rsp == before)
        g_main_context_iteration (NULL, TRUE);
    while (g_main_context_iteration (NULL, FALSE));
    g_assert_cmpuint (peer.n_approve_rsp, ==, before + 1);
    g_assert_cmpuint (peer.approve_txid, ==, txid);
    g_assert_cmpuint (peer.approve_slot, ==, slot);
}

static void
test_dsds_approval (void)
{
    g_autofree gchar *name = NULL;
    MMMtkCcciShared  *shared = shared_setup (&name);
    GObject          *sim1 = g_object_new (G_TYPE_OBJECT, NULL);
    GObject          *sim2 = g_object_new (G_TYPE_OBJECT, NULL);

    memset (fake_calls, 0, sizeof (fake_calls));
    n_rejected = 0;
    g_assert_true (mm_mtk_ccci_shared_get_dsds_busy (shared));   /* the default */
    mm_mtk_ccci_shared_set_voice_client (shared, MM_MIPC_SLOT_SIM1, sim1, fake_calls_present, fake_rejected);
    mm_mtk_ccci_shared_set_voice_client (shared, MM_MIPC_SLOT_SIM2, sim2, fake_calls_present, fake_rejected);

    /* both SIMs idle: approved, as before */
    approve_cmd (0x3001, MM_MIPC_SLOT_SIM2, 4, 10, "+70000000001");
    approve_rsp_check (1, 4, 10);
    g_assert_cmpuint (n_rejected, ==, 0);

    /* SIM1 has a call: SIM2's incoming call rejected with cause 21 and
     * shown on SIM2's object with the number from 0x8101 */
    fake_calls[1] = 1;
    approve_cmd (0x3002, MM_MIPC_SLOT_SIM2, 5, 11, "+70000000002");
    approve_rsp_check (0, 5, 11);
    g_assert_cmpuint (n_rejected, ==, 1);
    g_assert_cmpuint (rejected_slot, ==, 2);
    g_assert_cmpuint (rejected_id, ==, 5);
    g_assert_cmpstr (rejected_number, ==, "+70000000002");

    /* the same request again: same answer, shown once */
    approve_cmd (0x3003, MM_MIPC_SLOT_SIM2, 5, 11, "+70000000002");
    approve_rsp_check (0, 5, 11);
    g_assert_cmpuint (n_rejected, ==, 1);
    /* ...and the AT +EAIC path asks the same question */
    g_assert_false (mm_mtk_ccci_shared_incoming_decide (shared, 2, 5, 11, "+70000000002"));
    g_assert_cmpuint (n_rejected, ==, 1);

    /* SIM1's own incoming call (call waiting is the modem's): SIM2 idle, approved */
    approve_cmd (0x3004, MM_MIPC_SLOT_SIM1, 2, 12, "+70000000003");
    approve_rsp_check (1, 2, 12);

    /* the other direction, no number TLV: empty number to the client */
    fake_calls[1] = 0;
    fake_calls[2] = 1;
    approve_cmd (0x3005, MM_MIPC_SLOT_SIM1, 3, 13, NULL);
    approve_rsp_check (0, 3, 13);
    g_assert_cmpuint (n_rejected, ==, 2);
    g_assert_cmpuint (rejected_slot, ==, 1);
    g_assert_null (rejected_number);

    /* the rejected SIM has no modem object: rejected, nothing shown */
    mm_mtk_ccci_shared_unset_voice_client (shared, sim1);
    approve_cmd (0x3006, MM_MIPC_SLOT_SIM1, 4, 14, "+70000000004");
    approve_rsp_check (0, 4, 14);
    g_assert_cmpuint (n_rejected, ==, 2);

    /* ID_MM_MTK_CCCI_DSDS_BUSY=0: the old behaviour, always approve */
    mm_mtk_ccci_shared_set_dsds_busy (shared, FALSE);
    approve_cmd (0x3007, MM_MIPC_SLOT_SIM1, 5, 15, "+70000000005");
    approve_rsp_check (1, 5, 15);
    mm_mtk_ccci_shared_set_dsds_busy (shared, TRUE);

    /* the other SIM's object gone (weak reference): approved */
    mm_mtk_ccci_shared_set_voice_client (shared, MM_MIPC_SLOT_SIM1, sim1, fake_calls_present, fake_rejected);
    g_object_unref (sim2);
    approve_cmd (0x3008, MM_MIPC_SLOT_SIM1, 6, 16, "+70000000006");
    approve_rsp_check (1, 6, 16);

    /* the single-object mode: one client for both slots */
    sim2 = g_object_new (G_TYPE_OBJECT, NULL);
    mm_mtk_ccci_shared_set_voice_client (shared, MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2, sim2, fake_calls_present, fake_rejected);
    fake_calls[1] = 1;
    fake_calls[2] = 0;
    approve_cmd (0x3009, MM_MIPC_SLOT_SIM2, 7, 17, "+70000000007");
    approve_rsp_check (0, 7, 17);
    g_assert_cmpuint (n_rejected, ==, 3);
    g_assert_cmpuint (rejected_slot, ==, 2);

    g_object_unref (sim1);
    g_object_unref (sim2);
    g_clear_pointer (&rejected_number, g_free);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* radio coordinator */

static void
radio_set_sync (MMMtkCcciShared *shared, guint8 slots, gboolean on)
{
    Wait w = { 0 };

    mm_mtk_ccci_shared_radio_set (shared, slots, on, (GAsyncReadyCallback) radio_ready, &w);
    wait_for (&w, 1);
    g_assert_no_error (w.error);
    g_assert_true (w.ok);
}

static void
radio_prepare_sync (MMMtkCcciShared *shared, guint8 requester)
{
    Wait w = { 0 };

    mm_mtk_ccci_shared_radio_prepare (shared, requester, TRUE, (GAsyncReadyCallback) prepare_ready, &w);
    wait_for (&w, 1);
    g_assert_no_error (w.error);
    g_assert_true (w.ok);
}

#define IMS_STEPS(s) s":AT+CIREG?|" s":ims-key|" s":ims-flags|" s":AT+CGDCONT?|" s":profiles|" \
                     s":AT+CIREG=2|" s":AT+EIMSPDN= \"onoff\",1|" s":AT+EIMSCFG=1,0,0,0,1,1|" s":AT+EIMSRCS=0,0"

static void
test_radio_coordinator (void)
{
    g_autofree gchar *name = NULL;
    MMMtkCcciShared  *shared = shared_setup (&name);
    guint             mark;

    mm_mtk_ccci_shared_set_dual (shared, TRUE);

    /* cold start asked by SIM1 (its AT+CFUN? said off): SIM2's radio state
     * read on its own channel, IMS of both stacks before radio-on (both off:
     * no radio-off step), then both radios (the old single-object order);
     * no SIM1 AT port: the slot-1 tunnel */
    mm_mtk_ccci_shared_radio_note (shared, 1, FALSE);
    mark = peer.history->len;
    radio_prepare_sync (shared, 1);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "2:AT+CFUN?|" IMS_STEPS ("1") "|" IMS_STEPS ("2"));
    }
    g_assert_cmpuint (mm_mtk_ccci_shared_radio_get_known (shared), ==, 3);
    mark = peer.history->len;
    radio_set_sync (shared, 1, TRUE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=3");
    }
    g_assert_cmpuint (mm_mtk_ccci_shared_radio_get_on (shared), ==, 3);

    /* SIM2 initialises later and finds its stack on: nothing to prepare */
    mm_mtk_ccci_shared_radio_note (shared, 2, TRUE);
    mark = peer.history->len;
    radio_prepare_sync (shared, 2);
    g_assert_cmpuint (peer.history->len, ==, mark + 0);

    /* low power of SIM2 keeps SIM1; of SIM1 too: flight mode */
    mark = peer.history->len;
    radio_set_sync (shared, 2, FALSE);
    radio_set_sync (shared, 2, FALSE);          /* already: no command */
    radio_set_sync (shared, 1, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=1|1:AT+CFUN=4");
    }
    g_assert_cmpuint (mm_mtk_ccci_shared_radio_get_low_power (shared), ==, 3);

    /* SIM2 back first (EFUN=2 alone, unverified on the modem), then SIM1;
     * two queued requests: strictly one after the other; one SIM busy */
    {
        Wait w1 = { 0 }, w2 = { 0 };
        g_autofree gchar *h = NULL;

        peer.busy_efun = 1;
        mark = peer.history->len;
        mm_mtk_ccci_shared_radio_set (shared, 2, TRUE, (GAsyncReadyCallback) radio_ready, &w1);
        mm_mtk_ccci_shared_radio_set (shared, 1, TRUE, (GAsyncReadyCallback) radio_ready, &w2);
        wait_for (&w1, 1);
        g_assert_true (w1.ok);
        g_assert_cmpuint (w2.done, ==, 0);
        wait_for (&w2, 1);
        g_assert_true (w2.ok);
        h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=2|1:AT+EFUN=2|1:AT+EFUN=3");
    }

    /* dual mode, SIM1 re-enabled while SIM2's radio is on and SIM1 not IMS
     * registered: its IMS setup must not switch SIM2's radio off */
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 1));
    {
        Wait w = { 0 };
        g_autofree gchar *h = NULL;

        mark = peer.history->len;
        mm_mtk_ccci_ims_setup (mm_mtk_ccci_shared_peek_ims (shared, 1), (GAsyncReadyCallback) ims_ready, &w);
        while (!w.done)
            g_main_context_iteration (NULL, TRUE);
        h = history_from (mark);
        g_assert_null (strstr (h, "CFUN"));
        g_assert_cmpuint (mm_mtk_ccci_shared_radio_get_on (shared), ==, 3);
        g_clear_error (&w.error);
    }

    /* dual mode, SIM1 reconfigured while SIM2 is in low power: the old
     * radio-off step (nothing else is on) */
    radio_set_sync (shared, 2, FALSE);
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 1));
    {
        Wait w = { 0 };
        g_autofree gchar *h = NULL;

        mark = peer.history->len;
        mm_mtk_ccci_ims_setup (mm_mtk_ccci_shared_peek_ims (shared, 1), (GAsyncReadyCallback) ims_ready, &w);
        while (!w.done)
            g_main_context_iteration (NULL, TRUE);
        h = history_from (mark);
        g_assert_true (g_str_has_prefix (h, "1:AT+CIREG?|1:AT+CFUN=4|1:ims-key"));
        g_assert_cmpuint (mm_mtk_ccci_shared_radio_get_on (shared), ==, 0);
        g_clear_error (&w.error);
    }
    radio_set_sync (shared, 2, TRUE);

    /* single-object mode: the same setup keeps the old radio-off step */
    mm_mtk_ccci_shared_set_dual (shared, FALSE);
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 1));
    {
        Wait w = { 0 };
        g_autofree gchar *h = NULL;

        mark = peer.history->len;
        mm_mtk_ccci_ims_setup (mm_mtk_ccci_shared_peek_ims (shared, 1), (GAsyncReadyCallback) ims_ready, &w);
        while (!w.done)
            g_main_context_iteration (NULL, TRUE);
        h = history_from (mark);
        g_assert_nonnull (strstr (h, "1:AT+CFUN=4"));
        g_assert_cmpuint (mm_mtk_ccci_shared_radio_get_on (shared), ==, 0);
        g_clear_error (&w.error);
    }
    /* and its power up/down: both bits at once, EFUN=3 / CFUN=4 */
    mark = peer.history->len;
    radio_set_sync (shared, 3, TRUE);
    radio_set_sync (shared, 3, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=3|1:AT+CFUN=4");
    }

    /* radio mask 1 (SIM1 only configured): EFUN=1 */
    mm_mtk_ccci_shared_set_radio_mask (shared, 1);
    mark = peer.history->len;
    radio_set_sync (shared, 3, TRUE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=1");
    }

    /* concurrent IMS setups of one stack: one sequence on the wire */
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 2));
    mm_mtk_ccci_shared_set_dual (shared, TRUE);
    mm_mtk_ccci_shared_set_radio_mask (shared, 3);
    mm_mtk_ccci_shared_radio_note (shared, 3, FALSE);
    {
        Wait w1 = { 0 }, w2 = { 0 };
        g_autofree gchar *h = NULL;

        mark = peer.history->len;
        mm_mtk_ccci_ims_setup (mm_mtk_ccci_shared_peek_ims (shared, 2), (GAsyncReadyCallback) ims_ready, &w1);
        mm_mtk_ccci_ims_setup (mm_mtk_ccci_shared_peek_ims (shared, 2), (GAsyncReadyCallback) ims_ready, &w2);
        while (!w1.done || !w2.done)
            g_main_context_iteration (NULL, TRUE);
        h = history_from (mark);
        /* radios known off: the radio-off step is skipped */
        g_assert_cmpstr (h, ==, IMS_STEPS ("2"));
        g_assert_true (w1.ok && w2.ok);
        g_clear_error (&w1.error);
        g_clear_error (&w2.error);
    }

    g_assert_cmpuint (owner_port_commands, ==, 0);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* the AT bridge with ModemManager's unix-socket AT port */

static gboolean urc_seen;
static gchar   *urc_text;

static void
cereg_urc (MMPortSerialAt *port, GMatchInfo *info, gpointer user_data)
{
    urc_seen = TRUE;
    g_free (urc_text);
    urc_text = g_match_info_fetch (info, 1);
}

static void
at_sync (MMPortSerialAt *port, const gchar *cmd, gboolean raw, Wait *w)
{
    guint n = w->done;

    mm_port_serial_at_command (port, cmd, 5, raw, FALSE, NULL, (GAsyncReadyCallback) at_ready, w);
    wait_for (w, n + 1);
}

static void
test_at_bridge (void)
{
    g_autofree gchar           *name = NULL;
    MMMtkCcciShared            *shared = shared_setup (&name);
    g_autoptr(GError)           error = NULL;
    g_autoptr(MMMtkCcciAtBridge) bridge = NULL;
    g_autoptr(MMPortSerialAt)   port = NULL;
    g_autoptr(GRegex)           cereg = NULL;
    g_autoptr(GRegex)           cmti = NULL;
    Wait                        w = { 0 };
    guint                       mark;

    bridge = mm_mtk_ccci_at_bridge_new (mm_mtk_ccci_shared_peek_mipc (shared), MM_MIPC_SLOT_SIM2, &error);
    g_assert_no_error (error);
    g_assert_true (g_str_has_prefix (mm_mtk_ccci_at_bridge_get_name (bridge), "abstract:mm-mtk-ccci-sim2-at-"));
    g_assert_cmpuint (mm_mtk_ccci_at_bridge_get_slot (bridge), ==, MM_MIPC_SLOT_SIM2);

    /* what mm-base-modem.c builds for a "virtual" port */
    port = mm_port_serial_at_new (mm_mtk_ccci_at_bridge_get_name (bridge), MM_PORT_SUBSYS_UNIX);
    g_object_set (port, MM_PORT_SERIAL_AT_INIT_SEQUENCE_ENABLED, FALSE, NULL);
    mm_port_serial_at_set_response_parser (port, mm_serial_parser_v1_parse, mm_serial_parser_v1_remove_echo,
                                           mm_serial_parser_v1_new (), mm_serial_parser_v1_destroy);
    cereg = g_regex_new ("\\r\\n\\+CEREG:\\s*([^\\r\\n]*)\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    mm_port_serial_at_add_unsolicited_msg_handler (port, cereg, (MMPortSerialAtUnsolicitedMsgFn) cereg_urc, NULL, NULL);
    /* as MMBroadbandModem's setup_ports: +CMTI consumed from the start */
    cmti = g_regex_new ("\\r\\n\\+CMTI:[^\\r\\n]*\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    mm_port_serial_at_add_unsolicited_msg_handler (port, cmti, NULL, NULL, NULL);
    g_assert_true (mm_port_serial_open (MM_PORT_SERIAL (port), &error));
    g_assert_no_error (error);
    /* let the bridge accept the connection */
    while (!mm_mtk_ccci_at_bridge_push_urc (bridge, "+CEREG: 0") || !urc_seen)
        g_main_context_iteration (NULL, TRUE);
    urc_seen = FALSE;

    /* a plain command: SYS_AT on slot 2, the reply reframed */
    mark = peer.history->len;
    at_sync (port, "+CPIN?", FALSE, &w);
    g_assert_no_error (w.error);
    g_assert_cmpstr (w.reply, ==, "+CPIN: READY");
    /* no final code in the tunnel text: the CNF result gives OK */
    at_sync (port, "+CPMS?", FALSE, &w);
    g_assert_no_error (w.error);
    g_assert_nonnull (strstr (w.reply, "+CPMS: \"SM\",1,10"));
    /* serial-line settings are answered locally, ATZ never reaches the stack */
    at_sync (port, "Z", FALSE, &w);
    g_assert_no_error (w.error);
    at_sync (port, "E0 V1", FALSE, &w);
    g_assert_no_error (w.error);
    at_sync (port, "&C1", FALSE, &w);
    g_assert_no_error (w.error);
    /* +CMEE=1 does reach it */
    at_sync (port, "+CMEE=1", FALSE, &w);
    g_assert_no_error (w.error);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "2:AT+CPIN?|2:AT+CPMS?|2:AT+CMEE=1");
    }
    /* errors: a CME code; a refused request without text */
    at_sync (port, "+CMGR=9", FALSE, &w);
    g_assert_error (w.error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_NOT_INSERTED);
    at_sync (port, "+EMPTY", FALSE, &w);
    g_assert_nonnull (w.error);

    /* URCs: wanted ones reach the port's handlers, call/IMS text does not */
    g_assert_true (mm_mtk_ccci_at_bridge_push_urc (bridge, "+CEREG: 1,\"1A2B\",\"01A2B3C\",7"));
    while (!urc_seen)
        g_main_context_iteration (NULL, TRUE);
    g_assert_false (mm_mtk_ccci_at_bridge_push_urc (bridge, "+ECPI: 1,130,0,0,0,20,\"1\",129,\"\""));
    /* level 3: the port's generic parsers get the <n> = 2 form */
    urc_seen = FALSE;
    g_assert_true (mm_mtk_ccci_at_bridge_push_urc (bridge, "+CEREG: 3,\"1262\",\"0A1B2C3D\",7,0,15"));
    while (!urc_seen)
        g_main_context_iteration (NULL, TRUE);
    g_assert_cmpstr (urc_text, ==, "3,\"1262\",\"0A1B2C3D\",7");
    /* the read response too (the port's unsolicited handler takes it, as
     * on the phone: the command's own text is then empty) */
    urc_seen = FALSE;
    at_sync (port, "+CEREG?", FALSE, &w);
    g_assert_no_error (w.error);
    g_assert_true (urc_seen);
    g_assert_cmpstr (urc_text, ==, "3,3,\"1262\",\"0A1B2C3D\",7");
    g_clear_pointer (&urc_text, g_free);
    /* a URC between commands does not disturb the next reply */
    g_assert_true (mm_mtk_ccci_at_bridge_push_urc (bridge, "+CMTI: \"SM\",3"));
    at_sync (port, "+CPIN?", FALSE, &w);
    g_assert_no_error (w.error);
    g_assert_cmpstr (w.reply, ==, "+CPIN: READY");

    /* SMS submit the way mm-sms-at.c does it: the command, the "> " prompt
     * (from the bridge), then the raw PDU with Ctrl-Z; one tunnel string */
    mark = peer.history->len;
    at_sync (port, "+CMGS=10", FALSE, &w);
    g_assert_no_error (w.error);
    at_sync (port, "0011000B917000000000F00000AA03C8329B0D\x1a", TRUE, &w);
    g_assert_no_error (w.error);
    g_assert_nonnull (strstr (w.reply, "+CMGS: 5"));
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "2:AT+CMGS=10\r0011000B917000000000F00000AA03C8329B0D\x1a");
    }

    /* the client goes away while an answer is in flight (ModemManager
     * closed the port): the answer is dropped, the next client is served.
     * A raw socket here: MM's serial port itself must not be closed with a
     * command in flight (its round-robin scheduler then stays "in command"
     * after a reopen, an upstream behaviour unrelated to the bridge). */
    mm_port_serial_close (MM_PORT_SERIAL (port));
    {
        g_autoptr(GSocket)        raw = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &error);
        g_autoptr(GSocketAddress) address = g_unix_socket_address_new_with_type (mm_mtk_ccci_at_bridge_get_name (bridge), -1,
                                                                                 G_UNIX_SOCKET_ADDRESS_ABSTRACT);

        g_assert_no_error (error);
        g_assert_true (g_socket_connect (raw, address, NULL, &error));
        g_assert_no_error (error);
        peer.hold_sys_at = TRUE;
        g_assert_cmpint (g_socket_send (raw, "AT+CSQ\r", 7, NULL, &error), ==, 7);
        while (peer.held->len == 0)
            g_main_context_iteration (NULL, TRUE);
        g_socket_close (raw, NULL);
        /* let the bridge see the hang-up, then the late answer */
        while (g_main_context_iteration (NULL, FALSE));
        peer.hold_sys_at = FALSE;
        peer_release_held ();
        while (g_main_context_iteration (NULL, FALSE));
    }
    g_assert_true (mm_port_serial_open (MM_PORT_SERIAL (port), &error));
    g_assert_no_error (error);
    urc_seen = FALSE;
    while (!mm_mtk_ccci_at_bridge_push_urc (bridge, "+CEREG: 5") || !urc_seen)
        g_main_context_iteration (NULL, TRUE);
    at_sync (port, "+CPIN?", FALSE, &w);
    g_assert_no_error (w.error);
    g_assert_cmpstr (w.reply, ==, "+CPIN: READY");

    mm_port_serial_close (MM_PORT_SERIAL (port));
    g_clear_pointer (&w.reply, g_free);
    g_clear_pointer (&bridge, mm_mtk_ccci_at_bridge_free);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* the LTE start-up contract */

static void
run_ms (guint ms)
{
    gint64 until = g_get_monotonic_time () + ms * 1000;

    while (g_get_monotonic_time () < until)
        g_main_context_iteration (NULL, FALSE);
}

#define LTE_PRE_ALL \
    "1:AT+CEREG=?|2:AT+CEREG=?|" \
    "1:prefer(0100/1=01 0101/1=01)|2:prefer(0100/1=01 0101/1=01)|" \
    "1:radio-cap(0100/4=1f000000 0101/1=01)|2:radio-cap(0100/4=1f000000 0101/1=01)|" \
    "1:AT+ESBP=5,\"SBP_TERMINAL_CAPABILITY_FLEX\",1|1:AT+EIAREG=1|1:AT+ECCAUSE=1|1:AT+ESBP=7,\"SBP_L5_AUTO_DEACT_RSP\"|" \
    "2:AT+ESBP=5,\"SBP_TERMINAL_CAPABILITY_FLEX\",1|2:AT+EIAREG=1|2:AT+ECCAUSE=1|2:AT+ESBP=7,\"SBP_L5_AUTO_DEACT_RSP\"|" \
    "1:msim(0101/1=02)|1:dsbp(0100/1=03)|" \
    "1:AT+CREG=3|1:AT+CGREG=3|1:AT+CEREG=3"
#define ALLOW(s) s":allow()"

static void
test_lte_contract (void)
{
    g_autofree gchar   *name = NULL;
    MMMtkCcciShared    *shared = shared_setup (&name);
    MMMtkCcciLteConfig  all = { TRUE, MM_MIPC_SLOT_SIM1, TRUE, TRUE, TRUE };
    guint               mark;

    mm_mtk_ccci_shared_set_dual (shared, TRUE);
    mm_mtk_ccci_shared_set_lte_config (shared, &all);

    /* cold start: IMS of both stacks, then every group before radio-on in
     * the documented order; step 6 only where AT+CEREG=? offers 3 (slot 1) */
    mm_mtk_ccci_shared_radio_note (shared, 1, FALSE);
    mark = peer.history->len;
    radio_prepare_sync (shared, 1);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "2:AT+CFUN?|" IMS_STEPS ("1") "|" IMS_STEPS ("2") "|" LTE_PRE_ALL);
    }

    /* radio-on: data allow on the data SIM's slot, three times */
    mark = peer.history->len;
    radio_set_sync (shared, 3, TRUE);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=3|" ALLOW ("1") "|" ALLOW ("1") "|" ALLOW ("1"));
    }

    /* the same MIPC session: no step again; the warm session start sends
     * nothing either (data allow already sent) */
    mark = peer.history->len;
    mm_mtk_ccci_shared_lte_session (shared, 3);
    run_ms (300);
    g_assert_cmpuint (peer.history->len, ==, mark);

    /* SIM2 low power and back: not the data SIM, no data allow */
    mark = peer.history->len;
    radio_set_sync (shared, 2, FALSE);
    radio_set_sync (shared, 2, TRUE);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=1|1:AT+EFUN=3");
    }

    /* every radio-on of the data SIM's stack repeats it; the steps before
     * radio-on are not repeated in the session */
    /* (the modem reports both stacks off, as after a modem-side radio-off) */
    mark = peer.history->len;
    mm_mtk_ccci_shared_radio_note (shared, 3, FALSE);
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 1));
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 2));
    radio_prepare_sync (shared, 1);
    radio_set_sync (shared, 3, TRUE);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IMS_STEPS ("1") "|" IMS_STEPS ("2") "|"
                                "1:AT+EFUN=3|" ALLOW ("1") "|" ALLOW ("1") "|" ALLOW ("1"));
    }

    /* the data SIM changes to SIM2 (radio on): data allow there */
    all.data_slot = MM_MIPC_SLOT_SIM2;
    mark = peer.history->len;
    mm_mtk_ccci_shared_set_lte_config (shared, &all);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, ALLOW ("2") "|" ALLOW ("2") "|" ALLOW ("2"));
    }

    /* step 6 again on SIM2's channel (after the generic <n> = 2): not
     * offered there, nothing sent; on slot 1 it is sent again */
    mark = peer.history->len;
    mm_mtk_ccci_shared_lte_reg_cause (shared, 2, "test");
    mm_mtk_ccci_shared_lte_reg_cause (shared, 1, "test");
    run_ms (300);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+CREG=3|1:AT+CGREG=3|1:AT+CEREG=3");
    }
    shared_teardown (shared, name);
}

/* this round's default: data allow only, the other groups off */
static void
test_lte_contract_default (void)
{
    g_autofree gchar   *name = NULL;
    MMMtkCcciShared    *shared = shared_setup (&name);
    MMMtkCcciLteConfig  config = { TRUE, MM_MIPC_SLOT_SIM1, FALSE, FALSE, FALSE };
    guint               mark;

    mm_mtk_ccci_shared_set_dual (shared, TRUE);

    /* not configured: nothing of the contract */
    mm_mtk_ccci_shared_radio_note (shared, 1, FALSE);
    mark = peer.history->len;
    radio_prepare_sync (shared, 1);
    radio_set_sync (shared, 3, TRUE);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "2:AT+CFUN?|" IMS_STEPS ("1") "|" IMS_STEPS ("2") "|1:AT+EFUN=3");
    }

    /* configured with the radio on (warm start): the checks, one data allow
     * sequence for the session, no pre-radio step */
    mm_mtk_ccci_shared_set_lte_config (shared, &config);
    mark = peer.history->len;
    mm_mtk_ccci_shared_lte_session (shared, 3);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+CEREG=?|2:AT+CEREG=?|" ALLOW ("1") "|" ALLOW ("1") "|" ALLOW ("1"));
    }
    mark = peer.history->len;
    mm_mtk_ccci_shared_lte_session (shared, 3);
    run_ms (300);
    g_assert_cmpuint (peer.history->len, ==, mark);

    /* cold again: only the data allow after radio-on */
    mark = peer.history->len;
    mm_mtk_ccci_shared_radio_note (shared, 3, FALSE);
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 1));
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 2));
    radio_prepare_sync (shared, 1);
    radio_set_sync (shared, 3, TRUE);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IMS_STEPS ("1") "|" IMS_STEPS ("2") "|"
                                "1:AT+EFUN=3|" ALLOW ("1") "|" ALLOW ("1") "|" ALLOW ("1"));
    }
    /* the port lost (modem restart): the contract state is reset, so the
     * next session runs its checks again (radio state unknown: no data allow) */
    g_signal_emit_by_name (mm_mtk_ccci_shared_peek_mipc (shared), "forced-close");
    mark = peer.history->len;
    mm_mtk_ccci_shared_lte_session (shared, 3);
    run_ms (300);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+CEREG=?|2:AT+CEREG=?");
    }
    shared_teardown (shared, name);
}

/* radio off by MIPC, the diagnostics, one data allow
 * sequence per radio-on */
static void
test_lte_round5 (void)
{
    g_autofree gchar   *name = NULL;
    MMMtkCcciShared    *shared = shared_setup (&name);
    MMMtkCcciLteConfig  config = { TRUE, MM_MIPC_SLOT_SIM1, FALSE, FALSE, FALSE, TRUE, TRUE };
    guint               mark, registered;

    mm_mtk_ccci_shared_set_dual (shared, TRUE);
    mm_mtk_ccci_shared_set_lte_config (shared, &config);
    mm_mtk_ccci_shared_radio_note (shared, 3, FALSE);

    /* the diagnostics indications, registered per slot at the session start */
    registered = peer.n_register;
    mark = peer.history->len;
    mm_mtk_ccci_shared_lte_session (shared, 3);
    run_ms (500);
    g_assert_cmpuint (peer.n_register - registered, ==, 14);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+CEREG=?|2:AT+CEREG=?");
    }
    /* indications are traced (decoded or raw), nothing is answered */
    {
        g_autoptr(GByteArray) cscon = g_byte_array_new ();
        g_autoptr(GByteArray) ia = g_byte_array_new ();
        static const guint8   connected[] = { 0x01, 0x07, 0x04, 0x00 };

        mm_mipc_tlv_add (cscon, 0x0100, connected, sizeof (connected));
        peer_send (MM_MIPC_NW_CSCON_IND, 0xFFFF, MM_MIPC_SLOT_SIM1, cscon);
        mm_mipc_tlv_add_str (ia, 0x8101, "internet");
        peer_send (MM_MIPC_NW_IA_IND, 0xFFFF, MM_MIPC_SLOT_SIM2, ia);
        run_ms (200);
    }

    /* radio-on of both: data allow x3 on SIM1, the PS poll of both slots
     * every second until the first registration result (2nd poll: home) */
    mark = peer.history->len;
    radio_set_sync (shared, 3, TRUE);
    run_ms (3500);
    {
        g_autofree gchar *h = history_from (mark);
        guint n1 = 0, n2 = 0, na = 0, i;

        for (i = mark; i < peer.history->len; i++) {
            const gchar *e = g_ptr_array_index (peer.history, i);
            n1 += !strcmp (e, "1:get-ps");
            n2 += !strcmp (e, "2:get-ps");
            na += !strcmp (e, "1:allow()");
        }
        g_assert_true (g_str_has_prefix (h, "1:AT+EFUN=3|1:allow()"));
        g_assert_cmpuint (n1, ==, 2);
        g_assert_cmpuint (n2, ==, 2);
        g_assert_cmpuint (na, ==, 3);
    }

    /* the repeated power up of a stack that is on (enabling after the cold
     * start): the command again, no second data allow sequence, no poll */
    mark = peer.history->len;
    radio_set_sync (shared, 1, TRUE);
    run_ms (1500);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+EFUN=3");
    }

    /* low power of both: NW_SET_RADIO_STATE_REQ 0 per slot, not AT+CFUN=4 */
    mark = peer.history->len;
    radio_set_sync (shared, 3, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:radio-state(0100/1=00)|2:radio-state(0100/1=00)");
    }
    g_assert_cmpuint (mm_mtk_ccci_shared_radio_get_on (shared), ==, 0);

    /* AT+CFUN=4 again with the switch off */
    radio_set_sync (shared, 3, TRUE);
    run_ms (3500);
    config.radio_off_mipc = FALSE;
    config.nw_diag = FALSE;
    mm_mtk_ccci_shared_set_lte_config (shared, &config);
    mark = peer.history->len;
    radio_set_sync (shared, 3, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "1:AT+CFUN=4");
    }
    shared_teardown (shared, name);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/bridge-helpers", test_bridge_helpers);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/speech-refcount", test_speech_refcount);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/shared-lifecycle", test_shared_lifecycle);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/dsds-approval", test_dsds_approval);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/radio-coordinator", test_radio_coordinator);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/at-bridge", test_at_bridge);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/lte-contract", test_lte_contract);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/lte-contract-default", test_lte_contract_default);
    g_test_add_func ("/MM/mtk-ccci/dual-modem/lte-round5", test_lte_round5);
    return g_test_run ();
}
