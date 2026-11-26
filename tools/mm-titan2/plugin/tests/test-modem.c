/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The modem object of the mtk-ccci plugin (MMBroadbandModemMtkCcci) on a
 * fake modem behind a pseudo-terminal (the real MMPortMipc transport). The
 * modem source is compiled into this test so that the enabling steps can be
 * driven one by one without ports, udev or D-Bus:
 *  - the IMS step of enabling through the shared IMS controllers in the
 *    dual-modem mode (SIM1 and SIM2 objects), the single-object mode (the
 *    secondary stack) and the cold start (radio prepare), and the data
 *    profile step of the bearer. The callbacks of these steps get the shared
 *    context as their source object; the build installed on the phone as v1
 *    (2026-11-16) took it for the modem and crashed (SIGSEGV in
 *    ims_setup_ready);
 *  - the DSDS incoming-call policy end to end: CALL_APPROVE_INCOMING_CMD on
 *    one SIM while the other SIM's object has a call: the RSP and the
 *    terminated Call object on the rejected SIM's object;
 *  - the RAT mode: the start-up pair and its
 *    one retry, a refused NW_SET_RAT_CNF, a late confirm of the other slot,
 *    and +ERAT? read back with and without the cache of what was set;
 *  - the LTE-loss diagnostics: a burst of registration URCs of one slot
 *    gives one serving-state snapshot (read responses) on that slot only;
 *  - the initial-attach experiment: A sends the profile list only, B the
 *    slot's APN_SET_IA_REQ right after the list's confirm (a refusal does
 *    not stop the start-up); with no internet APN at start-up B sends it
 *    with the data bearer's profile list, with a preset APN at start-up;
 *  - registration state from the MIPC AT channel: URCs and snapshot read
 *    responses applied to the 3GPP state of the object's own stack (the
 *    update calls recorded), +COPS? AcT refreshing AccessTechnologies, and
 *    AT+CEER in the snapshot after a denial;
 *  - the start-up step switches: each step absent
 *    when off, the others in their order; the profile list held until the
 *    first registration, or off; the start-up RAT setting off.
 * Messages: the MIPC session start, the SYS_AT tunnel, the RAT mode and the
 * incoming-call approval.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

/* the modem object with its static functions */
#include "mm-broadband-modem-mtk-ccci.c"

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pty.h>

#include "mm-log.h"
#include "mm-call-list.h"

/* Same optional-backend stubs as upstream tests/test-base-call.c: no MBIM or
 * QMI device is instantiated here. */
#if defined WITH_MBIM
typedef struct MMBroadbandModemMbim MMBroadbandModemMbim;
GType mm_broadband_modem_mbim_get_type (void);
MMPortMbim *mm_broadband_modem_mbim_peek_port_mbim (MMBroadbandModemMbim *self);
GType mm_broadband_modem_mbim_get_type (void) { return G_TYPE_INVALID; }
MMPortMbim *mm_broadband_modem_mbim_peek_port_mbim (MMBroadbandModemMbim *self) { return NULL; }
#endif
#if defined WITH_QMI
typedef struct MMBroadbandModemQmi MMBroadbandModemQmi;
GType mm_broadband_modem_qmi_get_type (void);
MMPortQmi *mm_broadband_modem_qmi_peek_port_qmi (MMBroadbandModemQmi *self);
GType mm_broadband_modem_qmi_get_type (void) { return G_TYPE_INVALID; }
MMPortQmi *mm_broadband_modem_qmi_peek_port_qmi (MMBroadbandModemQmi *self) { return NULL; }
#endif

/*****************************************************************************/
/* the fake modem */

typedef struct {
    gint        fd, slave;
    GIOChannel *channel;
    guint       watch_id;
    GByteArray *rx;
    guint       n_approve_rsp;
    guint8      approve;
    guint32     approve_id, approve_seq, approve_cause;
    gboolean    approve_has_cause;
    GPtrArray  *history;     /* "<slot>:<text>" */
    GArray     *rat_results; /* guint32 results of the next NW_SET_RAT_CNFs (then 0) */
    gboolean    rat_spoof;   /* a confirm for the other slot first, same txid, result 0 */
    const gchar *erat_reply; /* the AT+ERAT? answer */
    guint32     ia_result;   /* of APN_SET_IA_CNF */
    const gchar *cgdcont_reply; /* the AT+CGDCONT? answer (NULL: CID 0 internet.example) */
    const gchar *cereg_reply;   /* the AT+CEREG? answer (NULL: home LTE) */
    const gchar *cops_reply;    /* the AT+COPS? answer (NULL: LTE) */
} Peer;

static Peer peer;

static void
peer_send (guint16 id, guint16 txid, guint8 slot, GByteArray *tlvs)
{
    g_autoptr(GByteArray) frame = mm_mipc_frame_build (id, txid, slot, tlvs ? tlvs->data : NULL, tlvs ? tlvs->len : 0);
    gsize off = 0;

    g_assert_nonnull (frame);
    while (off < frame->len) {
        gssize n = write (peer.fd, frame->data + off, frame->len - off);
        g_assert_cmpint (n, >, 0);
        off += n;
    }
}

static GByteArray *
cnf (guint32 result)
{
    GByteArray *tlvs = g_byte_array_new ();

    mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_RESULT, result);
    return tlvs;
}

static void
peer_handle (const MMMipcMsg *msg)
{
    g_autoptr(GByteArray) tlvs = NULL;
    g_autofree gchar     *text = NULL;
    const gchar          *reply = "OK";
    guint32               v = 0;

    switch (msg->msg_id) {
    case MM_MIPC_INTERNAL_TEST_REQ:
        peer_send (MM_MIPC_INTERNAL_TEST_CNF, msg->txid, msg->slot, NULL);
        return;
    case MM_MIPC_INTERNAL_OPEN_REQ:
        tlvs = cnf (0);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_OPEN_VERSION, 1);
        peer_send (MM_MIPC_INTERNAL_OPEN_CNF, msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_INTERNAL_REGISTER_IND_REQ:
    case MM_MIPC_INTERNAL_REGISTER_CMD_REQ:
        tlvs = cnf (0);
        peer_send (MM_MIPC_CNF_OF (msg->msg_id), msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_IMS_SET_CONFIG_REQ:
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_IMSCFG_FORM, &v));
        g_ptr_array_add (peer.history, g_strdup_printf ("%u:%s", msg->slot, v ? "ims-key" : "ims-flags"));
        tlvs = cnf (0);
        peer_send (MM_MIPC_IMS_SET_CONFIG_CNF, msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_NW_SET_RAT_REQ: {
        guint32 rat = 0, prefer = 0, result = 0;

        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_SET_RAT_RAT, &rat));
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_SET_RAT_PREFER, &prefer));
        g_ptr_array_add (peer.history, g_strdup_printf ("%u:rat=%u/%u", msg->slot, rat, prefer));
        if (peer.rat_results->len) {
            result = g_array_index (peer.rat_results, guint32, 0);
            g_array_remove_index (peer.rat_results, 0);
        }
        if (peer.rat_spoof) {
            g_autoptr(GByteArray) other = cnf (0);
            peer_send (MM_MIPC_NW_SET_RAT_CNF, msg->txid, msg->slot ^ 3, other);
        }
        tlvs = cnf (result);
        peer_send (MM_MIPC_NW_SET_RAT_CNF, msg->txid, msg->slot, tlvs);
        return;
    }
    case MM_MIPC_APN_SET_IA_REQ: {
        g_autofree gchar *apn = mm_mipc_tlv_find_str (msg->payload, msg->len, MM_MIPC_T_IA_APN);
        guint32 pdp = 0, roam = 0, auth = 0;

        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_IA_PDP, &pdp));
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_IA_ROAM_PDP, &roam));
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, MM_MIPC_T_IA_AUTH, &auth));
        g_ptr_array_add (peer.history, g_strdup_printf ("%u:ia=%s/%u/%u/%u", msg->slot, apn, pdp, roam, auth));
        tlvs = cnf (peer.ia_result);
        peer_send (MM_MIPC_APN_SET_IA_CNF, msg->txid, msg->slot, tlvs);
        return;
    }
    case MM_MIPC_APN_ADD_PROFILE_LIST_REQ:
        g_ptr_array_add (peer.history, g_strdup_printf ("%u:profiles", msg->slot));
        tlvs = cnf (0);
        peer_send (MM_MIPC_APN_ADD_PROFILE_LIST_CNF, msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_SYS_AT_REQ:
        text = mm_mipc_tlv_find_str (msg->payload, msg->len, MM_MIPC_T_AT_STRING);
        g_assert_nonnull (text);
        g_ptr_array_add (peer.history, g_strdup_printf ("%u:%s", msg->slot, text));
        if (g_str_equal (text, "AT+CIREG?"))
            reply = "+CIREG: 2,0,0\r\nOK";
        else if (g_str_equal (text, "AT+CGDCONT?"))
            reply = peer.cgdcont_reply ? peer.cgdcont_reply :
                    "+CGDCONT: 0,\"IP\",\"internet.example\",\"0.0.0.0\",0,0\r\nOK";
        else if (g_str_equal (text, "AT+CFUN?"))
            reply = "+CFUN: 4\r\nOK";
        else if (g_str_equal (text, "AT+CPIN?"))
            reply = "+CPIN: READY\r\n\r\nOK";
        else if (g_str_equal (text, "AT+ERAT?") && peer.erat_reply)
            reply = peer.erat_reply;
        else if (g_str_equal (text, "AT+COPS?"))
            reply = peer.cops_reply ? peer.cops_reply : "+COPS: 0,2,\"00102\",7\r\nOK";
        else if (g_str_equal (text, "AT+CEREG?"))
            reply = peer.cereg_reply ? peer.cereg_reply : "+CEREG: 2,1,\"1A2B\",\"01234567\",7\r\nOK";
        tlvs = cnf (0);
        mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, reply);
        peer_send (MM_MIPC_SYS_AT_CNF, msg->txid, msg->slot, tlvs);
        return;
    case MM_MIPC_CALL_APPROVE_RSP:
        peer.n_approve_rsp++;
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, 0x0100, &v));
        peer.approve = (guint8) v;
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, 0x0102, &peer.approve_id));
        g_assert_true (mm_mipc_tlv_find_uint (msg->payload, msg->len, 0x0103, &peer.approve_seq));
        peer.approve_has_cause = mm_mipc_tlv_find_uint (msg->payload, msg->len, 0x0101, &peer.approve_cause);
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
    peer.history = g_ptr_array_new_with_free_func (g_free);
    peer.rat_results = g_array_new (FALSE, FALSE, sizeof (guint32));
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
    g_ptr_array_unref (peer.history);
    g_array_unref (peer.rat_results);
    close (peer.fd);
    close (peer.slave);
}

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
/* the context and the modem objects */

typedef struct {
    guint    done;
    gboolean ok;
    GError  *error;
    GObject *source;
} Wait;

static void
wait_for (Wait *w)
{
    while (!w->done)
        g_main_context_iteration (NULL, TRUE);
}

static void
session_ready (MMPortMipc *port, GAsyncResult *res, Wait *w)
{
    w->ok = mm_port_mipc_open_session_finish (port, res, &w->error);
    w->done++;
}

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
    mm_port_mipc_open_session (mm_mtk_ccci_shared_peek_mipc (shared), MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2,
                               mipc_indications, G_N_ELEMENTS (mipc_indications), "test", NULL,
                               (GAsyncReadyCallback) session_ready, &w);
    wait_for (&w);
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

/* a modem object as the plugin builds it, minus ports and udev: the role
 * fields load_port_configuration and create_wwan_port would set */
static MMBroadbandModemMtkCcci *
modem_new (MMMtkCcciShared *shared, gboolean dual, guint sim_slot, MMCallList *calls)
{
    static const gchar *drivers[] = { "mtk-ccci-md", NULL };
    MMBroadbandModemMtkCcci *self;

    self = mm_broadband_modem_mtk_ccci_new (sim_slot == 2 ? "test-sim2" : "test-sim1", "test-physdev",
                                            drivers, "mtk-ccci", 0, 0);
    self->priv->shared = g_object_ref (shared);
    self->priv->mipc = g_object_ref (mm_mtk_ccci_shared_peek_mipc (shared));
    g_signal_connect_object (self->priv->mipc, MM_PORT_MIPC_SIGNAL_INDICATION, G_CALLBACK (voice_mipc_indication), self, 0);
    self->priv->dual = dual;
    self->priv->sim2_role = dual && sim_slot == 2;
    self->priv->sim_slot = sim_slot;
    self->priv->served = dual ? (guint8) (1u << (sim_slot - 1)) : (MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2);
    self->priv->ims_enabled = TRUE;
    self->priv->sim_ready[0] = self->priv->sim_ready[1] = TRUE;
    if (!self->priv->sim2_role)
        mm_mtk_ccci_shared_set_dual (shared, dual);
    if (calls)
        g_object_set (self, MM_IFACE_MODEM_VOICE_CALL_LIST, calls, NULL);
    mm_mtk_ccci_shared_set_voice_client (shared, self->priv->served, G_OBJECT (self),
                                         shared_calls_present, shared_call_rejected);
    return self;
}

static void
modem_free (MMBroadbandModemMtkCcci *self)
{
    g_object_set (self, MM_IFACE_MODEM_VOICE_CALL_LIST, NULL, NULL);
    g_object_unref (self);
}

/* an enabling step's own task: its source must be the modem */
static void
step_ready (GObject *source, GAsyncResult *res, Wait *w)
{
    w->source = source;
    w->ok = g_task_propagate_boolean (G_TASK (res), &w->error);
    w->done++;
}

static void
prepare_data_ready (GObject *source, GAsyncResult *res, Wait *w)
{
    w->source = source;
    w->ok = mm_broadband_modem_mtk_ccci_prepare_data_finish (MM_BROADBAND_MODEM_MTK_CCCI (source), res, &w->error);
    w->done++;
}

#define IMS_STEPS(s) s":AT+CIREG?|" s":ims-key|" s":ims-flags|" s":AT+CGDCONT?|" s":profiles|" \
                     s":AT+CIREG=2|" s":AT+EIMSPDN= \"onoff\",1|" s":AT+EIMSCFG=1,0,0,0,1,1|" s":AT+EIMSRCS=0,0"

static void
run_step (MMBroadbandModemMtkCcci *self, void (*step) (MMBroadbandModemMtkCcci *, GTask *), gboolean cold)
{
    Wait   w = { 0 };
    GTask *task = g_task_new (self, NULL, (GAsyncReadyCallback) step_ready, &w);

    if (cold)
        g_object_set_data (G_OBJECT (task), "cold-start", GINT_TO_POINTER (TRUE));
    step (self, task);
    wait_for (&w);
    g_assert_no_error (w.error);
    g_assert_true (w.ok);
    g_assert_true (w.source == G_OBJECT (self));
}

/*****************************************************************************/
/* enabling through the shared IMS controllers */

static void
test_ims_enabling_dual (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1, *sim2;
    guint                    mark;

    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim2 = modem_new (shared, TRUE, 2, NULL);

    /* warm enabling of SIM1 (the radio was on): its stack's IMS setup through
     * the shared controller, then the voice init on its AT port (none here:
     * logged, not fatal) - the path of the v1 crash */
    mark = peer.history->len;
    run_step (sim1, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IMS_STEPS ("1"));
    }
    /* the same for the SIM2 object */
    mark = peer.history->len;
    run_step (sim2, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IMS_STEPS ("2"));
    }

    /* the bearer's data profile step (mm-bearer-mtk-ccci.c data_profiles_ready
     * takes the source object as the modem) */
    {
        Wait w = { 0 };

        mark = peer.history->len;
        mm_broadband_modem_mtk_ccci_prepare_data (sim1, "internet.example", (GAsyncReadyCallback) prepare_data_ready, &w);
        wait_for (&w);
        g_assert_no_error (w.error);
        g_assert_true (w.ok);
        g_assert_true (w.source == G_OBJECT (sim1));
        {
            g_autofree gchar *h = history_from (mark);
            g_assert_cmpstr (h, ==, "1:profiles");
        }
    }

    /* cold start of SIM1 (its AT+CFUN? said off): the shared radio prepare
     * (SIM2's radio state read, IMS of both stacks), then the voice init */
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 1));
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 2));
    mm_mtk_ccci_shared_radio_note (shared, 1, FALSE);
    mark = peer.history->len;
    run_step (sim1, cold_prepare, TRUE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, "2:AT+CFUN?|" IMS_STEPS ("1") "|" IMS_STEPS ("2"));
    }

    modem_free (sim2);
    modem_free (sim1);
    shared_teardown (shared, name);
}

static void
test_ims_enabling_single (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *modem;
    guint                    mark;

    /* the single-object mode: the selected stack, then the other one
     * (secondary_pin_ready, secondary_ims_ready) with the old CFUN=4 step */
    modem = modem_new (shared, FALSE, 1, NULL);
    mark = peer.history->len;
    run_step (modem, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==,
                         "1:AT+CIREG?|1:AT+CFUN=4|1:ims-key|1:ims-flags|1:AT+CGDCONT?|1:profiles|"
                         "1:AT+CIREG=2|1:AT+EIMSPDN= \"onoff\",1|1:AT+EIMSCFG=1,0,0,0,1,1|1:AT+EIMSRCS=0,0|"
                         "2:AT+CPIN?|" IMS_STEPS ("2"));
    }
    modem_free (modem);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* DSDS end to end */

static void
approve_cmd (guint16 txid, guint8 slot, guint32 call_id, guint32 seq, const gchar *number)
{
    g_autoptr(GByteArray) cmd = g_byte_array_new ();
    guint                 before = peer.n_approve_rsp;

    mm_mipc_tlv_add_u32 (cmd, 0x0100, call_id);
    if (number)
        mm_mipc_tlv_add_str (cmd, 0x8101, number);
    mm_mipc_tlv_add_u32 (cmd, 0x0103, seq);
    peer_send (MM_MIPC_CALL_APPROVE_CMD, txid, slot, cmd);
    while (peer.n_approve_rsp == before)
        g_main_context_iteration (NULL, TRUE);
    while (g_main_context_iteration (NULL, FALSE));
    g_assert_cmpuint (peer.n_approve_rsp, ==, before + 1);
    g_assert_cmpuint (peer.approve_id, ==, call_id);
    g_assert_cmpuint (peer.approve_seq, ==, seq);
}

static void
count_added (MMCallList *list, const gchar *path, guint *n)
{
    (*n)++;
}

static void
first_call (MMBaseCall *call, MMBaseCall **out)
{
    if (!*out)
        *out = call;
}

static void
test_dsds_end_to_end (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    g_autoptr(MMCallList)    calls1 = mm_call_list_new ();
    g_autoptr(MMCallList)    calls2 = mm_call_list_new ();
    MMBroadbandModemMtkCcci *sim1, *sim2;
    MMBaseCall              *rejected = NULL;
    gchar                    number[] = "+70000000001";
    MMCallInfo               info = { 1, MM_CALL_DIRECTION_INCOMING, MM_CALL_STATE_RINGING_IN, number };
    guint                    added2 = 0;

    sim1 = modem_new (shared, TRUE, 1, calls1);
    sim2 = modem_new (shared, TRUE, 2, calls2);
    g_signal_connect (calls2, MM_CALL_ADDED, G_CALLBACK (count_added), &added2);

    /* a call on SIM1, answered */
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (sim1), sim1->priv->mipc, 1, &info);
    info.state = MM_CALL_STATE_ACTIVE;
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (sim1), sim1->priv->mipc, 1, &info);
    g_assert_cmpuint (mm_call_list_get_count (calls1), ==, 1);

    /* a call to SIM2 meanwhile: rejected (approve 0, cause 21), and SIM2's
     * object shows it as an incoming call terminated with REFUSED_OR_BUSY */
    approve_cmd (0x4001, MM_MIPC_SLOT_SIM2, 2, 31, "+70000000002");
    g_assert_cmpuint (peer.approve, ==, 0);
    g_assert_true (peer.approve_has_cause);
    g_assert_cmpuint (peer.approve_cause, ==, 21);
    g_assert_cmpuint (added2, ==, 1);
    g_assert_cmpuint (mm_call_list_get_count (calls2), ==, 1);
    mm_call_list_foreach (calls2, (MMCallListForeachFunc) first_call, &rejected);
    g_assert_cmpuint (mm_base_call_get_state (rejected), ==, MM_CALL_STATE_TERMINATED);
    g_assert_cmpuint (mm_gdbus_call_get_state_reason (MM_GDBUS_CALL (rejected)), ==, MM_CALL_STATE_REASON_REFUSED_OR_BUSY);
    g_assert_cmpuint (mm_base_call_get_direction (rejected), ==, MM_CALL_DIRECTION_INCOMING);
    g_assert_cmpstr (mm_base_call_get_number (rejected), ==, "+70000000002");
    /* SIM1's call untouched, no speech path change from the rejected call */
    g_assert_cmpuint (mm_base_call_get_state (mm_call_list_get_call_by_index (calls1, 1)), ==, MM_CALL_STATE_ACTIVE);
    g_assert_cmpuint (mm_mtk_ccci_shared_speech_get (shared), ==, 0);
    /* the modem's report of the released call does not revive it */
    {
        MMCallInfo gone = { 102, MM_CALL_DIRECTION_INCOMING, MM_CALL_STATE_TERMINATED, NULL };

        mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (sim2), sim2->priv->mipc, 2, &gone);
        g_assert_cmpuint (mm_call_list_get_count (calls2), ==, 1);
        g_assert_cmpuint (mm_base_call_get_state (rejected), ==, MM_CALL_STATE_TERMINATED);
    }

    /* the AT path of SIM1's port asks the same policy: SIM2 idle, approved */
    g_assert_true (mm_mtk_ccci_shared_incoming_decide (shared, 1, 3, 32, NULL));

    /* SIM1's call ends: SIM2's next call is approved, nothing added */
    info.state = MM_CALL_STATE_TERMINATED;
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (sim1), sim1->priv->mipc, 1, &info);
    approve_cmd (0x4002, MM_MIPC_SLOT_SIM2, 3, 33, "+70000000003");
    g_assert_cmpuint (peer.approve, ==, 1);
    g_assert_false (peer.approve_has_cause);
    g_assert_cmpuint (added2, ==, 1);

    /* ID_MM_MTK_CCCI_DSDS_BUSY=0: approved although SIM2 now has a call */
    info.index = 102; info.state = MM_CALL_STATE_RINGING_IN;
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (sim2), sim2->priv->mipc, 2, &info);
    info.state = MM_CALL_STATE_ACTIVE;
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (sim2), sim2->priv->mipc, 2, &info);
    mm_mtk_ccci_shared_set_dsds_busy (shared, FALSE);
    approve_cmd (0x4003, MM_MIPC_SLOT_SIM1, 4, 34, "+70000000004");
    g_assert_cmpuint (peer.approve, ==, 1);
    mm_mtk_ccci_shared_set_dsds_busy (shared, TRUE);
    approve_cmd (0x4004, MM_MIPC_SLOT_SIM1, 5, 35, "+70000000005");
    g_assert_cmpuint (peer.approve, ==, 0);
    g_assert_cmpuint (mm_call_list_get_count (calls1), ==, 2);

    /* the calls hold their modem objects */
    modem_free (sim2);
    modem_free (sim1);
    g_clear_object (&calls1);
    g_clear_object (&calls2);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* RAT mode */

#define MASK4 (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G)
#define MASK5 (MASK4 | MM_MODEM_MODE_5G)

static void
rat_results (guint n, ...)
{
    va_list ap;
    guint   i;

    g_array_set_size (peer.rat_results, 0);
    va_start (ap, n);
    for (i = 0; i < n; i++) {
        guint32 v = va_arg (ap, guint);
        g_array_append_val (peer.rat_results, v);
    }
    va_end (ap);
}

/* the NW_SET_RAT_REQs since @from */
static gchar *
rat_history_from (guint from)
{
    GString *s = g_string_new (NULL);
    guint    i;

    for (i = from; i < peer.history->len; i++) {
        const gchar *h = g_ptr_array_index (peer.history, i);
        if (strstr (h, ":rat="))
            g_string_append_printf (s, "%s%s", s->len ? "|" : "", h);
    }
    return g_string_free (s, FALSE);
}

/* the start-up step (enabling, warm): the RAT mode, then IMS and voice */
static void
start_up_rat (MMBroadbandModemMtkCcci *self, const gchar *expected)
{
    guint mark = peer.history->len;

    run_step (self, apply_initial_rat, FALSE);
    {
        g_autofree gchar *h = rat_history_from (mark);
        g_assert_cmpstr (h, ==, expected);
    }
    g_assert_cmpuint (peer.rat_results->len, ==, 0);
}

static void
set_modes_ready (MMIfaceModem *self, GAsyncResult *res, Wait *w)
{
    w->ok = modem_set_current_modes_finish (self, res, &w->error);
    w->done++;
}

static gboolean
set_modes (MMBroadbandModemMtkCcci *self, MMModemMode allowed, MMModemMode preferred, const gchar *expected)
{
    Wait  w = { 0 };
    guint mark = peer.history->len;

    modem_set_current_modes (MM_IFACE_MODEM (self), allowed, preferred, (GAsyncReadyCallback) set_modes_ready, &w);
    wait_for (&w);
    g_assert_true (w.ok == !w.error);
    g_clear_error (&w.error);
    {
        g_autofree gchar *h = rat_history_from (mark);
        g_assert_cmpstr (h, ==, expected);
    }
    return w.ok;
}

typedef struct {
    Wait        w;
    MMModemMode allowed, preferred;
} LoadWait;

static void
load_modes_ready (MMIfaceModem *self, GAsyncResult *res, LoadWait *l)
{
    l->w.ok = modem_load_current_modes_finish (self, res, &l->allowed, &l->preferred, &l->w.error);
    l->w.done++;
}

static void
load_modes (MMBroadbandModemMtkCcci *self, const gchar *erat, MMModemMode allowed, MMModemMode preferred)
{
    LoadWait l = { { 0 } };

    peer.erat_reply = erat;
    modem_load_current_modes (MM_IFACE_MODEM (self), (GAsyncReadyCallback) load_modes_ready, &l);
    wait_for (&l.w);
    g_assert_no_error (l.w.error);
    g_assert_true (l.w.ok);
    g_assert_cmpuint (l.allowed, ==, allowed);
    g_assert_cmpuint (l.preferred, ==, preferred);
    peer.erat_reply = NULL;
}

static void
assert_confirmed (MMBroadbandModemMtkCcci *self, guint rat, guint prefer, MMModemMode allowed, MMModemMode preferred)
{
    g_assert_true (self->priv->rat_known);
    g_assert_cmpuint (self->priv->rat_value, ==, rat);
    g_assert_cmpuint (self->priv->rat_prefer, ==, prefer);
    g_assert_cmpuint (self->priv->rat_allowed, ==, allowed);
    g_assert_cmpuint (self->priv->rat_preferred, ==, preferred);
}

static void
test_rat_start_up (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1, *sim2;

    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim2 = modem_new (shared, TRUE, 2, NULL);

    /* the automatic mode, accepted on both slots */
    sim1->priv->rat_mask = sim2->priv->rat_mask = MASK5;
    rat_results (0);
    start_up_rat (sim1, "1:rat=22/128");
    start_up_rat (sim2, "2:rat=22/128");
    assert_confirmed (sim1, 22, 128, MASK5, MM_MODEM_MODE_5G);
    assert_confirmed (sim2, 22, 128, MASK5, MM_MODEM_MODE_5G);
    modem_free (sim2);
    modem_free (sim1);

    /* NR first refused: one retry, LTE first as 6/4 */
    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim1->priv->rat_mask = MASK5;
    rat_results (1, 2);
    start_up_rat (sim1, "1:rat=22/128|1:rat=6/4");
    assert_confirmed (sim1, 6, 4, MASK4, MM_MODEM_MODE_4G);
    modem_free (sim1);

    /* the same before the supported modes are known (cold start) */
    sim2 = modem_new (shared, TRUE, 2, NULL);
    rat_results (1, 2);
    start_up_rat (sim2, "2:rat=22/128|2:rat=6/4");
    assert_confirmed (sim2, 6, 4, MASK4, MM_MODEM_MODE_4G);
    modem_free (sim2);

    /* both refused: no third attempt, nothing confirmed */
    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim1->priv->rat_mask = MASK5;
    rat_results (2, 2, 2);
    start_up_rat (sim1, "1:rat=22/128|1:rat=6/4");
    g_assert_cmpuint (peer.rat_results->len, ==, 0);
    g_assert_false (sim1->priv->rat_known);
    rat_results (0);
    modem_free (sim1);

    /* the generic probing missed NR (2026-11-24: both slots ended at 6/4):
     * the start-up pair is still 22/128, sent once; once accepted, 5G is
     * offered as well */
    sim2 = modem_new (shared, TRUE, 2, NULL);
    sim2->priv->rat_mask = MASK4;
    rat_results (0);
    start_up_rat (sim2, "2:rat=22/128");
    assert_confirmed (sim2, 22, 128, MASK5, MM_MODEM_MODE_5G);
    g_assert_cmpuint (sim2->priv->rat_mask, ==, MASK5);
    /* the next enable in the same MIPC session: not sent again */
    start_up_rat (sim2, "");
    assert_confirmed (sim2, 22, 128, MASK5, MM_MODEM_MODE_5G);
    /* after a disable (session closed): sent again, once */
    sim2->priv->rat_session_confirmed = FALSE;
    start_up_rat (sim2, "2:rat=22/128");
    modem_free (sim2);

    /* the probing without NR and 22/128 refused: LTE first once, then
     * nothing; LTE first is never sent twice */
    sim2 = modem_new (shared, TRUE, 2, NULL);
    sim2->priv->rat_mask = MASK4;
    rat_results (2, 2, 2);
    start_up_rat (sim2, "2:rat=22/128|2:rat=6/4");
    g_assert_false (sim2->priv->rat_known);
    rat_results (0);
    modem_free (sim2);

    /* a client's choice is re-applied as it is, not replaced on refusal */
    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim1->priv->rat_mask = MASK5;
    g_assert_true (set_modes (sim1, MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE, "1:rat=3/0"));
    g_assert_true (sim1->priv->rat_user_set);
    /* the same session: not resent; after a disable: resent, a refusal is
     * not followed by another pair */
    start_up_rat (sim1, "");
    sim1->priv->rat_session_confirmed = FALSE;
    rat_results (1, 2);
    start_up_rat (sim1, "1:rat=3/0");
    assert_confirmed (sim1, 3, 0, MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE);
    modem_free (sim1);

    shared_teardown (shared, name);
}

static void
test_rat_client_request (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1, *sim2;

    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim2 = modem_new (shared, TRUE, 2, NULL);
    sim1->priv->rat_mask = sim2->priv->rat_mask = MASK5;

    /* the automatic mode, then LTE first refused: an error for the client,
     * the confirmed pair unchanged */
    g_assert_true (set_modes (sim1, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, "1:rat=22/128"));
    g_assert_false (sim1->priv->rat_user_set);
    rat_results (1, 2);
    g_assert_false (set_modes (sim1, MASK4, MM_MODEM_MODE_4G, "1:rat=6/4"));
    assert_confirmed (sim1, 22, 128, MASK5, MM_MODEM_MODE_5G);
    g_assert_false (sim1->priv->rat_user_set);
    /* accepted: now the client's choice */
    g_assert_true (set_modes (sim1, MASK4, MM_MODEM_MODE_4G, "1:rat=6/4"));
    assert_confirmed (sim1, 6, 4, MASK4, MM_MODEM_MODE_4G);
    g_assert_true (sim1->priv->rat_user_set);

    /* a late positive confirm for the other slot with this request's txid
     * does not complete it: the slot's own refusal decides */
    peer.rat_spoof = TRUE;
    rat_results (1, 2);
    g_assert_false (set_modes (sim1, MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE, "1:rat=3/0"));
    assert_confirmed (sim1, 6, 4, MASK4, MM_MODEM_MODE_4G);
    /* SIM2's request: SIM1's object is untouched by it */
    g_assert_true (set_modes (sim2, MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE, "2:rat=3/0"));
    assert_confirmed (sim2, 3, 0, MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE);
    assert_confirmed (sim1, 6, 4, MASK4, MM_MODEM_MODE_4G);
    peer.rat_spoof = FALSE;

    /* not one of the offered combinations: nothing sent */
    g_assert_false (set_modes (sim1, MM_MODEM_MODE_2G | MM_MODEM_MODE_3G, MM_MODEM_MODE_2G, ""));

    modem_free (sim2);
    modem_free (sim1);
    shared_teardown (shared, name);
}

static void
test_rat_read_back (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1;

    /* a new object (MM restarted): nothing cached, the pair alone decides */
    sim1 = modem_new (shared, TRUE, 1, NULL);
    g_assert_false (sim1->priv->rat_known);
    load_modes (sim1, "+ERAT: 2,0,6,4\r\nOK", MASK4, MM_MODEM_MODE_4G);
    load_modes (sim1, "+ERAT: 2,0,22,128\r\nOK", MASK5, MM_MODEM_MODE_5G);
    load_modes (sim1, "+ERAT: 3,1,3,0\r\nOK", MM_MODEM_MODE_4G, MM_MODEM_MODE_NONE);

    /* confirmed 22/128, but the modem reports 6/4: the modem's pair */
    sim1->priv->rat_mask = MASK5;
    g_assert_true (set_modes (sim1, MM_MODEM_MODE_ANY, MM_MODEM_MODE_NONE, "1:rat=22/128"));
    load_modes (sim1, "+ERAT: 2,0,6,4\r\nOK", MASK4, MM_MODEM_MODE_4G);
    load_modes (sim1, "+ERAT: 2,0,22,128\r\nOK", MASK5, MM_MODEM_MODE_5G);
    /* +ERAT? unusable: the confirmed pair */
    load_modes (sim1, "ERROR", MASK5, MM_MODEM_MODE_5G);

    modem_free (sim1);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* LTE-loss diagnostics */

static void
sys_at_ind (guint8 slot, const gchar *text)
{
    g_autoptr(GByteArray) tlvs = g_byte_array_new ();

    mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, text);
    peer_send (MM_MIPC_SYS_AT_IND, 0, slot, tlvs);
}

static gboolean
flag_set (gboolean *flag)
{
    *flag = TRUE;
    return G_SOURCE_REMOVE;
}

static void
run_for (guint ms)
{
    gboolean done = FALSE;

    g_timeout_add (ms, (GSourceFunc) flag_set, &done);
    while (!done)
        g_main_context_iteration (NULL, TRUE);
}

#define SNAPSHOT(s) s":AT+COPS?|" s":AT+CREG?|" s":AT+CGREG?|" s":AT+CEREG?|" s":AT+CGATT?|" s":AT+CESQ|" s":AT+CIREG?"

static void
test_rat_trace (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1, *sim2;
    guint                    mark;

    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim2 = modem_new (shared, TRUE, 2, NULL);

    /* EPS LTE, then CS/PS on HSPA within the delay: one snapshot of SIM2 */
    mark = peer.history->len;
    sys_at_ind (MM_MIPC_SLOT_SIM2, "\r\n+CEREG: 1,\"1A2B\",\"01234567\",7\r\n");
    sys_at_ind (MM_MIPC_SLOT_SIM2, "\r\n+CGREG: 1,\"1A2B\",\"0123\",6,\"01\"\r\n");
    sys_at_ind (MM_MIPC_SLOT_SIM2, "\r\n+CREG: 1,\"1A2B\",\"0123\",6\r\n");
    run_for (2000);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, SNAPSHOT ("2"));
    }
    g_assert_false (sim2->priv->trace_busy);
    g_assert_cmpuint (sim2->priv->trace_pending, ==, 0);
    g_assert_cmpuint (sim1->priv->trace_pending, ==, 0);

    /* a SIM1 call end: SIM1's snapshot */
    {
        g_autoptr(GByteArray) tlvs = g_byte_array_new ();

        mm_mipc_tlv_add_u32 (tlvs, 0x0100, 1);
        mm_mipc_tlv_add_u32 (tlvs, 0x0109, 133);
        mark = peer.history->len;
        peer_send (MM_MIPC_CALL_STATUS_IND, 0, MM_MIPC_SLOT_SIM1, tlvs);
        run_for (2000);
        {
            g_autofree gchar *h = history_from (mark);
            g_assert_cmpstr (h, ==, SNAPSHOT ("1"));
        }
    }

    /* an unrelated URC: nothing */
    mark = peer.history->len;
    sys_at_ind (MM_MIPC_SLOT_SIM1, "\r\n+CMTI: \"SM\",3\r\n");
    run_for (1000);
    g_assert_cmpuint (peer.history->len, ==, mark);

    modem_free (sim2);
    modem_free (sim1);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* initial-attach experiment */

#define IA_STEPS(s, ia) s":AT+CIREG?|" s":ims-key|" s":ims-flags|" s":AT+CGDCONT?|" s":profiles|" ia \
                        s":AT+CIREG=2|" s":AT+EIMSPDN= \"onoff\",1|" s":AT+EIMSCFG=1,0,0,0,1,1|" s":AT+EIMSRCS=0,0"

static void
test_initial_attach (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1, *sim2;
    guint                    mark;

    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim2 = modem_new (shared, TRUE, 2, NULL);

    /* B on SIM1: the IA request with CID 0's APN after the list; A on SIM2 */
    mm_mtk_ccci_ims_set_initial_attach (mm_mtk_ccci_shared_peek_ims (shared, 1), TRUE);
    mark = peer.history->len;
    run_step (sim1, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IA_STEPS ("1", "1:ia=internet.example/1/1/1|"));
    }
    mark = peer.history->len;
    run_step (sim2, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IA_STEPS ("2", ""));
    }

    /* B refused: logged, the start-up goes on */
    mm_mtk_ccci_ims_stop (mm_mtk_ccci_shared_peek_ims (shared, 1));
    peer.ia_result = 2;
    mark = peer.history->len;
    run_step (sim1, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IA_STEPS ("1", "1:ia=internet.example/1/1/1|"));
    }
    peer.ia_result = 0;

    modem_free (sim2);
    modem_free (sim1);
    shared_teardown (shared, name);
}

static void
test_initial_attach_late_and_preset (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1, *sim2;
    guint                    mark;

    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim2 = modem_new (shared, TRUE, 2, NULL);
    peer.cgdcont_reply = "OK";   /* CID 0 empty (cold boot, 2026-11-25) */

    /* B, no APN at start-up: no IA then; with the data bearer's list */
    mm_mtk_ccci_ims_set_initial_attach (mm_mtk_ccci_shared_peek_ims (shared, 1), TRUE);
    mark = peer.history->len;
    run_step (sim1, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IA_STEPS ("1", ""));
    }
    {
        Wait w = { 0 };

        mark = peer.history->len;
        mm_broadband_modem_mtk_ccci_prepare_data (sim1, "internet.example", (GAsyncReadyCallback) prepare_data_ready, &w);
        wait_for (&w);
        g_assert_no_error (w.error);
        g_assert_true (w.ok);
        {
            g_autofree gchar *h = history_from (mark);
            g_assert_cmpstr (h, ==, "1:profiles|1:ia=internet.example/1/1/1");
        }
        /* once per setup */
        w.done = 0;
        mark = peer.history->len;
        mm_broadband_modem_mtk_ccci_prepare_data (sim1, "internet.example", (GAsyncReadyCallback) prepare_data_ready, &w);
        wait_for (&w);
        g_assert_true (w.ok);
        {
            g_autofree gchar *h = history_from (mark);
            g_assert_cmpstr (h, ==, "1:profiles");
        }
    }

    /* B with the APN preset (udev): the IA at start-up although CID 0 is empty */
    mm_mtk_ccci_ims_set_profiles (mm_mtk_ccci_shared_peek_ims (shared, 2), "preset.example",
                                  "udev ID_MM_MTK_CCCI_DATA_APN_SIM2", NULL, NULL);
    mm_mtk_ccci_ims_set_initial_attach (mm_mtk_ccci_shared_peek_ims (shared, 2), TRUE);
    mark = peer.history->len;
    run_step (sim2, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, IA_STEPS ("2", "2:ia=preset.example/1/1/1|"));
    }
    peer.cgdcont_reply = NULL;

    modem_free (sim2);
    modem_free (sim1);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* registration state from the MIPC AT channel */

static GPtrArray              *reg_calls;
static MMModemAccessTechnology reg_prop;

static void
reg_record (MMIfaceModem3gpp *self, const gchar *what, guint v)
{
    g_ptr_array_add (reg_calls, g_strdup_printf ("%u:%s=%u", MM_BROADBAND_MODEM_MTK_CCCI (self)->priv->sim_slot, what, v));
}

static void rec_cs   (MMIfaceModem3gpp *s, MMModem3gppRegistrationState st, gboolean d) { reg_record (s, "cs", st); }
static void rec_ps   (MMIfaceModem3gpp *s, MMModem3gppRegistrationState st, gboolean d) { reg_record (s, "ps", st); }
static void rec_eps  (MMIfaceModem3gpp *s, MMModem3gppRegistrationState st, gboolean d) { reg_record (s, "eps", st); }
static void rec_5gs  (MMIfaceModem3gpp *s, MMModem3gppRegistrationState st, gboolean d) { reg_record (s, "5gs", st); }
static void rec_loc  (MMIfaceModem3gpp *s, gulong lac, gulong tac, gulong ci) { reg_record (s, "tac", (guint) tac); }
static void
rec_act (MMIfaceModem3gpp *s, MMModemAccessTechnology act)
{
    /* like mm_iface_modem_update_access_technologies: unknown leaves it */
    if (act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN && act != reg_prop) {
        reg_prop = act;
        reg_record (s, "act", act);
    }
}
static MMModemAccessTechnology rec_get_act (MMBroadbandModemMtkCcci *self) { return reg_prop; }

static const RegUpdateOps reg_recorder = { rec_cs, rec_ps, rec_eps, rec_5gs, rec_act, rec_loc, rec_get_act };

static gchar *
reg_calls_take (void)
{
    GString *s = g_string_new (NULL);
    guint    i;

    for (i = 0; i < reg_calls->len; i++)
        g_string_append_printf (s, "%s%s", s->len ? "|" : "", (const gchar *) g_ptr_array_index (reg_calls, i));
    g_ptr_array_set_size (reg_calls, 0);
    return g_string_free (s, FALSE);
}

static void
reg_check (const gchar *expected)
{
    g_autofree gchar *calls = reg_calls_take ();
    g_assert_cmpstr (calls, ==, expected);
}

static void
test_registration_from_mipc (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1, *sim2;
    g_autofree gchar        *hspa = g_strdup_printf ("%u", MM_MODEM_ACCESS_TECHNOLOGY_HSPA);
    g_autofree gchar        *lte = g_strdup_printf ("%u", MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    g_autofree gchar        *e1 = NULL, *e2 = NULL, *e3 = NULL, *e4 = NULL;
    guint                    mark;

    reg_calls = g_ptr_array_new_with_free_func (g_free);
    reg_update = &reg_recorder;
    sim1 = modem_new (shared, TRUE, 1, NULL);
    sim2 = modem_new (shared, TRUE, 2, NULL);

    /* SIM1 (AT port wwan0at0, URCs only here): 3G, then LTE */
    reg_prop = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    sys_at_ind (MM_MIPC_SLOT_SIM1, "\r\n+CGREG: 1,\"1A2B\",\"0123\",6,\"01\"\r\n");
    run_for (50);
    e1 = g_strdup_printf ("1:ps=%u|1:act=%s|1:tac=0", MM_MODEM_3GPP_REGISTRATION_STATE_HOME, hspa);
    reg_check (e1);
    sys_at_ind (MM_MIPC_SLOT_SIM1, "\r\n+CEREG: 1,\"1A2B\",\"01234516\",7\r\n");
    run_for (50);
    e2 = g_strdup_printf ("1:eps=%u|1:act=%s|1:tac=%u", MM_MODEM_3GPP_REGISTRATION_STATE_HOME, lte, 0x1A2B);
    reg_check (e2);
    g_assert_cmpuint (reg_prop, ==, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    /* the snapshot after them: the read response (n 2, home LTE) applied
     * again, nothing to refresh */
    run_for (2000);
    /* (the act call with the unchanged LTE is not recorded) */
    e3 = g_strdup_printf ("1:eps=%u|1:tac=%u", MM_MODEM_3GPP_REGISTRATION_STATE_HOME, 0x1A2B);
    reg_check (e3);

    /* SIM2's object the same for slot 2; SIM1's object does not take it */
    reg_prop = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    sys_at_ind (MM_MIPC_SLOT_SIM2, "\r\n+CREG: 1,\"1A2B\",\"0123\",6\r\n");
    sys_at_ind (MM_MIPC_SLOT_SIM2, "\r\n+CREG: 1,\"1A2B\",\"01234516\",7\r\n");
    run_for (50);
    e4 = g_strdup_printf ("2:cs=%u|2:act=%s|2:tac=0|2:cs=%u|2:act=%s|2:tac=%u",
                          MM_MODEM_3GPP_REGISTRATION_STATE_HOME, hspa,
                          MM_MODEM_3GPP_REGISTRATION_STATE_HOME, lte, 0x1A2B);
    reg_check (e4);
    run_for (2000);
    reg_calls_take ();

    /* a missed URC: the property says hspa, the snapshot's +COPS? LTE
     * refreshes it; a denial adds AT+CEER to the snapshot */
    reg_prop = MM_MODEM_ACCESS_TECHNOLOGY_HSPA;
    peer.cereg_reply = "+CEREG: 0,1\r\nOK";   /* no AcT: only +COPS? has it */
    mark = peer.history->len;
    trace_event (sim1, MM_MIPC_SLOT_SIM1, "test", "test event");
    run_for (2000);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, SNAPSHOT ("1"));
    }
    g_assert_cmpuint (reg_prop, ==, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    /* the property agrees: no refresh */
    reg_calls_take ();
    trace_event (sim1, MM_MIPC_SLOT_SIM1, "test", "test event");
    run_for (2000);
    {
        g_autofree gchar *calls = reg_calls_take ();
        g_assert_null (strstr (calls, "act="));
    }
    /* a denial in a read response: AT+CEER in the same snapshot */
    peer.cereg_reply = "+CEREG: 0,3\r\nOK";
    mark = peer.history->len;
    trace_event (sim1, MM_MIPC_SLOT_SIM1, "test", "test event");
    run_for (2000);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, SNAPSHOT ("1") "|1:AT+CEER");
    }
    peer.cereg_reply = NULL;
    reg_calls_take ();

    /* a denial URC: its snapshot reads AT+CEER */
    mark = peer.history->len;
    sys_at_ind (MM_MIPC_SLOT_SIM2, "\r\n+CEREG: 3,\"4598\",\"02D45403\",7\r\n");
    run_for (2000);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, SNAPSHOT ("2") "|2:AT+CEER");
    }

    /* the registration read after enabling: an attach that happened before
     * any URC; searching first, then home LTE: the reads stop at the result */
    reg_calls_take ();
    reg_prop = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    peer.cereg_reply = "+CEREG: 2,2\r\nOK";
    peer.cops_reply = "+COPS: 0\r\nOK";
    mark = peer.history->len;
    reg_read_start (sim1);
    run_for (1800);
    g_assert_true (sim1->priv->reg_read_timer != 0);
    peer.cereg_reply = "+CEREG: 2,1,\"1A2B\",\"01234516\",7\r\nOK";
    peer.cops_reply = "+COPS: 0,0,\"t2 rus\",7\r\nOK";
    run_for (3000);
    g_assert_cmpuint (reg_prop, ==, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    g_assert_true (sim1->priv->reg_read_done);
    g_assert_cmpuint (sim1->priv->reg_read_timer, ==, 0);
    {
        guint n = 0, i;

        for (i = mark; i < peer.history->len; i++)
            n += !strcmp (g_ptr_array_index (peer.history, i), "1:AT+CEREG?");
        /* at least the searching read and the home read, then no more */
        g_assert_cmpuint (n, >=, 2);
        mark = peer.history->len;
        run_for (2500);
        g_assert_cmpuint (peer.history->len, ==, mark);
    }
    peer.cereg_reply = NULL;
    peer.cops_reply = NULL;
    reg_calls_take ();

    /* the single-object mode: only the selected stack's URCs */
    modem_free (sim2);
    modem_free (sim1);
    sim1 = modem_new (shared, FALSE, 1, NULL);
    reg_calls_take ();
    sys_at_ind (MM_MIPC_SLOT_SIM2, "\r\n+CEREG: 1,\"1A2B\",\"01234516\",7\r\n");
    run_for (50);
    reg_check ("");
    run_for (2000);
    reg_check ("");
    modem_free (sim1);

    reg_update = &reg_update_default;
    g_clear_pointer (&reg_calls, g_ptr_array_unref);
    shared_teardown (shared, name);
}

/*****************************************************************************/
/* start-up step switches */

/* the IMS start-up of @sim with the steps given, against the full sequence */
static void
steps_check (MMMtkCcciShared *shared, MMBroadbandModemMtkCcci *sim, guint steps,
             MMMtkCcciImsProfiles profiles, const gchar *expected)
{
    MMMtkCcciIms *ims = mm_mtk_ccci_shared_peek_ims (shared, 1);
    guint         mark;

    mm_mtk_ccci_ims_stop (ims);
    mm_mtk_ccci_ims_set_start_steps (ims, steps, profiles);
    mark = peer.history->len;
    run_step (sim, prepare_ims_and_voice, FALSE);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_cmpstr (h, ==, expected);
    }
}

static void
test_start_switches (void)
{
    g_autofree gchar        *name = NULL;
    MMMtkCcciShared         *shared = shared_setup (&name);
    MMBroadbandModemMtkCcci *sim1;
    guint                    all = MM_MTK_CCCI_IMS_STEPS_ALL, mark;

    sim1 = modem_new (shared, TRUE, 1, NULL);

    /* all on: the full sequence, unchanged */
    steps_check (shared, sim1, all, MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO, IMS_STEPS ("1"));
    /* each one off: absent, the rest in order */
    steps_check (shared, sim1, all & ~MM_MTK_CCCI_IMS_STEP_MIPC_CONFIG, MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO,
                 "1:AT+CIREG?|1:AT+CGDCONT?|1:profiles|1:AT+CIREG=2|1:AT+EIMSPDN= \"onoff\",1|"
                 "1:AT+EIMSCFG=1,0,0,0,1,1|1:AT+EIMSRCS=0,0");
    steps_check (shared, sim1, all & ~MM_MTK_CCCI_IMS_STEP_CIREG, MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO,
                 "1:AT+CIREG?|1:ims-key|1:ims-flags|1:AT+CGDCONT?|1:profiles|1:AT+EIMSPDN= \"onoff\",1|"
                 "1:AT+EIMSCFG=1,0,0,0,1,1|1:AT+EIMSRCS=0,0");
    steps_check (shared, sim1, all & ~MM_MTK_CCCI_IMS_STEP_EIMSPDN, MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO,
                 "1:AT+CIREG?|1:ims-key|1:ims-flags|1:AT+CGDCONT?|1:profiles|1:AT+CIREG=2|"
                 "1:AT+EIMSCFG=1,0,0,0,1,1|1:AT+EIMSRCS=0,0");
    steps_check (shared, sim1, all & ~MM_MTK_CCCI_IMS_STEP_EIMSCFG, MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO,
                 "1:AT+CIREG?|1:ims-key|1:ims-flags|1:AT+CGDCONT?|1:profiles|1:AT+CIREG=2|"
                 "1:AT+EIMSPDN= \"onoff\",1|1:AT+EIMSRCS=0,0");
    steps_check (shared, sim1, all & ~MM_MTK_CCCI_IMS_STEP_EIMSRCS, MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO,
                 "1:AT+CIREG?|1:ims-key|1:ims-flags|1:AT+CGDCONT?|1:profiles|1:AT+CIREG=2|"
                 "1:AT+EIMSPDN= \"onoff\",1|1:AT+EIMSCFG=1,0,0,0,1,1");

    /* the profile list after the first registration: not at start-up, not
     * by the periodic poll (5 s), then right after +CEREG: 1 */
    steps_check (shared, sim1, all, MM_MTK_CCCI_IMS_PROFILES_AFTER_REGISTRATION,
                 "1:AT+CIREG?|1:ims-key|1:ims-flags|1:AT+CGDCONT?|1:AT+CIREG=2|1:AT+EIMSPDN= \"onoff\",1|"
                 "1:AT+EIMSCFG=1,0,0,0,1,1|1:AT+EIMSRCS=0,0");
    mark = peer.history->len;
    run_for (5600);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_null (strstr (h, "profiles"));
    }
    sys_at_ind (MM_MIPC_SLOT_SIM1, "\r\n+CEREG: 2\r\n");      /* searching: not yet */
    run_for (100);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_null (strstr (h, "profiles"));
    }
    sys_at_ind (MM_MIPC_SLOT_SIM1, "\r\n+CEREG: 1,\"1A2B\",\"01234516\",7\r\n");
    run_for (300);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_nonnull (strstr (h, "1:AT+CGDCONT?|1:profiles"));
    }
    /* only once */
    mark = peer.history->len;
    sys_at_ind (MM_MIPC_SLOT_SIM1, "\r\n+CREG: 1,\"1A2B\",\"01234516\",7\r\n");
    run_for (2000);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_null (strstr (h, "profiles"));
    }

    /* off: neither at start-up nor after registration (only a bearer's) */
    steps_check (shared, sim1, all, MM_MTK_CCCI_IMS_PROFILES_OFF,
                 "1:AT+CIREG?|1:ims-key|1:ims-flags|1:AT+CGDCONT?|1:AT+CIREG=2|1:AT+EIMSPDN= \"onoff\",1|"
                 "1:AT+EIMSCFG=1,0,0,0,1,1|1:AT+EIMSRCS=0,0");
    mark = peer.history->len;
    sys_at_ind (MM_MIPC_SLOT_SIM1, "\r\n+CEREG: 1,\"1A2B\",\"01234516\",7\r\n");
    run_for (2000);
    {
        g_autofree gchar *h = history_from (mark);
        g_assert_null (strstr (h, "profiles"));
    }

    /* the start-up RAT setting off: no NW_SET_RAT_REQ */
    sim1->priv->rat_mask = MASK5;
    sim1->priv->start_rat = FALSE;
    mm_mtk_ccci_ims_set_start_steps (mm_mtk_ccci_shared_peek_ims (shared, 1), all, MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO);
    start_up_rat (sim1, "");
    g_assert_false (sim1->priv->rat_known);
    sim1->priv->start_rat = TRUE;
    start_up_rat (sim1, "1:rat=22/128");

    modem_free (sim1);
    shared_teardown (shared, name);
}

int
main (int argc, char **argv)
{
    GError *error = NULL;
    gint    result;

    g_test_init (&argc, &argv, NULL);
    g_assert_true (mm_log_setup (g_getenv ("MM_TEST_LOG_LEVEL") ? g_getenv ("MM_TEST_LOG_LEVEL") : "ERR",
                                 NULL, FALSE, FALSE, FALSE, FALSE, &error));
    g_assert_no_error (error);
    g_test_add_func ("/MM/mtk-ccci/modem/ims-enabling-dual", test_ims_enabling_dual);
    g_test_add_func ("/MM/mtk-ccci/modem/ims-enabling-single", test_ims_enabling_single);
    g_test_add_func ("/MM/mtk-ccci/modem/dsds-end-to-end", test_dsds_end_to_end);
    g_test_add_func ("/MM/mtk-ccci/modem/rat-start-up", test_rat_start_up);
    g_test_add_func ("/MM/mtk-ccci/modem/rat-client-request", test_rat_client_request);
    g_test_add_func ("/MM/mtk-ccci/modem/rat-read-back", test_rat_read_back);
    g_test_add_func ("/MM/mtk-ccci/modem/rat-trace", test_rat_trace);
    g_test_add_func ("/MM/mtk-ccci/modem/initial-attach", test_initial_attach);
    g_test_add_func ("/MM/mtk-ccci/modem/initial-attach-late-preset", test_initial_attach_late_and_preset);
    g_test_add_func ("/MM/mtk-ccci/modem/registration-from-mipc", test_registration_from_mipc);
    g_test_add_func ("/MM/mtk-ccci/modem/start-switches", test_start_switches);
    result = g_test_run ();
    mm_log_shutdown ();
    return result;
}
