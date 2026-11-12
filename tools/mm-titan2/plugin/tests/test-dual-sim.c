/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#include <config.h>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <ModemManager.h>
#include <mm-errors-types.h>
#include "tests/fake-modem.h"
#include "mm-call-mtk-ccci.h"
#include "mm-sim-mtk-ccci.h"
#include "mm-modem-helpers-mtk-ccci.h"
#include "mm-call-list.h"
#include "mm-log.h"

/* Same optional-backend stubs as upstream tests/test-base-call.c: this test
 * exercises the fake modem and does not instantiate MBIM or QMI devices. */
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

typedef struct {
    GMainLoop *loop;
    gint master, slave;
    GIOChannel *channel;
    guint watch;
    guint8 slot;
    const gchar *command;
    gboolean ok;
    GError *error;
    GByteArray *rx;
    const gchar * const *commands;
    const gchar * const *responses;
    guint step;
    MMBaseSim *sim;
} Test;

static gboolean peer_read (GIOChannel *channel, GIOCondition cond, gpointer data)
{
    Test *t = data;
    guint8 bytes[4096];
    gsize n = 0, used;
    MMMipcMsg msg;
    g_autofree gchar *command = NULL;
    g_autoptr(GByteArray) tlvs = g_byte_array_new ();
    g_autoptr(GByteArray) frame = NULL;
    if (g_io_channel_read_chars (channel, (gchar *) bytes, sizeof (bytes), &n, NULL) != G_IO_STATUS_NORMAL)
        return G_SOURCE_CONTINUE;
    g_byte_array_append (t->rx, bytes, n);
    if (mm_mipc_frame_parse (t->rx->data, t->rx->len, &msg, &used) != 1)
        return G_SOURCE_CONTINUE;
    g_assert_cmpuint (msg.msg_id, ==, MM_MIPC_SYS_AT_REQ);
    g_assert_cmpuint (msg.slot, ==, t->slot);
    command = mm_mipc_tlv_find_str (msg.payload, msg.len, MM_MIPC_T_AT_STRING);
    g_assert_cmpstr (command, ==, t->commands ? t->commands[t->step] : t->command);
    mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_RESULT, 0);
    mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, t->responses ? t->responses[t->step] : "OK");
    t->step++;
    frame = mm_mipc_frame_build (MM_MIPC_SYS_AT_CNF, msg.txid, msg.slot, tlvs->data, tlvs->len);
    g_assert_cmpint (write (t->master, frame->data, frame->len), ==, (gssize) frame->len);
    g_byte_array_remove_range (t->rx, 0, used);
    return G_SOURCE_CONTINUE;
}

static void accepted (MMBaseCall *call, GAsyncResult *res, Test *t)
{
    t->ok = MM_BASE_CALL_GET_CLASS (call)->accept_finish (call, res, &t->error);
    g_main_loop_quit (t->loop);
}
static void hung_up (MMBaseCall *call, GAsyncResult *res, Test *t)
{
    t->ok = MM_BASE_CALL_GET_CLASS (call)->hangup_finish (call, res, &t->error);
    g_main_loop_quit (t->loop);
}
static void started (MMBaseCall *call, GAsyncResult *res, Test *t)
{
    t->ok = MM_BASE_CALL_GET_CLASS (call)->start_finish (call, res, &t->error);
    g_main_loop_quit (t->loop);
}

static MMPortMipc *setup (Test *t)
{
    gchar name[128];
    struct termios term;
    MMPortMipc *port;
    g_assert_cmpint (openpty (&t->master, &t->slave, name, NULL, NULL), ==, 0);
    tcgetattr (t->slave, &term); cfmakeraw (&term); tcsetattr (t->slave, TCSANOW, &term);
    t->loop = g_main_loop_new (NULL, FALSE);
    t->rx = g_byte_array_new ();
    t->channel = g_io_channel_unix_new (t->master);
    g_io_channel_set_encoding (t->channel, NULL, NULL);
    g_io_channel_set_buffered (t->channel, FALSE);
    t->watch = g_io_add_watch (t->channel, G_IO_IN, peer_read, t);
    port = mm_port_mipc_new (name + 5);
    g_assert_true (mm_port_mipc_open (port, &t->error));
    g_assert_no_error (t->error);
    return port;
}

static void cleanup (Test *t, MMPortMipc *port)
{
    mm_port_mipc_close (port);
    g_source_remove (t->watch);
    g_io_channel_unref (t->channel);
    g_byte_array_unref (t->rx);
    g_main_loop_unref (t->loop);
    close (t->master); close (t->slave);
}

static void test_dual_calls (void)
{
    Test t = { 0 };
    g_autoptr(MMPortMipc) port = NULL;
    g_autoptr(MMFakeModem) modem = NULL;
    g_autoptr(MMBaseCall) call = NULL;
    g_autoptr(MMBaseCall) sim2 = NULL;
    g_autoptr(MMCallList) list = mm_call_list_new ();
    gchar number[] = "+1234567";
    MMCallInfo info = { 1, MM_CALL_DIRECTION_INCOMING, MM_CALL_STATE_RINGING_IN, number };

    port = setup (&t);
    modem = mm_fake_modem_new (NULL);
    g_object_set (modem, MM_IFACE_MODEM_VOICE_CALL_LIST, list, NULL);

    /* Identical caller number and physical call id on two SIMs must not merge. */
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (modem), port, 1, &info);
    info.index = 101;
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (modem), port, 2, &info);
    g_assert_cmpuint (mm_call_list_get_count (list), ==, 2);
    g_assert_nonnull (mm_call_list_get_call_by_index (list, 1));
    g_assert_nonnull (mm_call_list_get_call_by_index (list, 101));
    sim2 = g_object_ref (mm_call_list_get_call_by_index (list, 101));

    call = g_object_ref (mm_call_list_get_call_by_index (list, 101));
    t.slot = 2; t.command = "ATA";
    MM_BASE_CALL_GET_CLASS (call)->accept (call, (GAsyncReadyCallback) accepted, &t);
    g_main_loop_run (t.loop);
    g_assert_no_error (t.error); g_assert_true (t.ok);
    t.command = "AT+CHLD=11";
    MM_BASE_CALL_GET_CLASS (call)->hangup (call, (GAsyncReadyCallback) hung_up, &t);
    g_main_loop_run (t.loop);
    g_assert_no_error (t.error); g_assert_true (t.ok);
    g_clear_object (&call);

    call = mm_call_mtk_ccci_new (MM_BASE_MODEM (modem), port, 2, MM_CALL_DIRECTION_OUTGOING, "+7654321", 0);
    t.slot = 2; t.command = "ATD+7654321;";
    MM_BASE_CALL_GET_CLASS (call)->start (call, NULL, (GAsyncReadyCallback) started, &t);
    g_main_loop_run (t.loop);
    g_assert_no_error (t.error); g_assert_true (t.ok);
    g_clear_object (&call);

    /* Terminating SIM2 leaves SIM1 ringing. */
    info.state = MM_CALL_STATE_TERMINATED;
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (modem), port, 2, &info);
    g_assert_cmpuint (mm_base_call_get_state (mm_call_list_get_call_by_index (list, 1)), ==, MM_CALL_STATE_RINGING_IN);
    g_assert_cmpuint (mm_base_call_get_state (sim2), ==, MM_CALL_STATE_TERMINATED);
    info.state = MM_CALL_STATE_RINGING_IN;
    mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (modem), port, 2, &info);
    g_assert_cmpuint (mm_call_list_get_count (list), ==, 3);
    g_assert_true (mm_call_list_get_call_by_index (list, 101) != sim2);
    g_clear_object (&sim2);
    g_object_set (modem, MM_IFACE_MODEM_VOICE_CALL_LIST, NULL, NULL);
    g_clear_object (&list);
    cleanup (&t, port);
}

static void sim_ready (MMBaseModem *modem, GAsyncResult *res, Test *t)
{
    t->sim = mm_sim_mtk_ccci_new_finish (res, &t->error);
    g_main_loop_quit (t->loop);
}

static void test_sim (void)
{
    Test t = { 0 };
    const gchar *commands[] = { "AT+CPIN?", "AT+CRSM=176,12258,0,0,10", "AT+CIMI", NULL };
    const gchar *responses[] = { "+CPIN: READY", "\r\n+CRSM: 144,0,\"982143658709214365F7\"\r\nOK\r\n", "\r\n123456789012345\r\nOK\r\n", NULL };
    g_autoptr(MMPortMipc) port = setup (&t);
    g_autoptr(MMFakeModem) modem = mm_fake_modem_new (NULL);
    t.slot = 2;
    t.commands = commands;
    t.responses = responses;
    mm_sim_mtk_ccci_new (MM_BASE_MODEM (modem), port, 2, (GAsyncReadyCallback) sim_ready, &t);
    g_main_loop_run (t.loop);
    g_assert_no_error (t.error);
    g_assert_nonnull (t.sim);
    g_assert_cmpuint (t.step, ==, 3);
    g_assert_cmpuint (mm_base_sim_get_slot_number (t.sim), ==, 2);
    g_assert_true (mm_gdbus_sim_get_active (MM_GDBUS_SIM (t.sim)));
    g_assert_cmpstr (mm_gdbus_sim_get_imsi (MM_GDBUS_SIM (t.sim)), ==, "123456789012345");
    g_assert_cmpstr (mm_gdbus_sim_get_sim_identifier (MM_GDBUS_SIM (t.sim)), ==, "8912345678901234567");
    /* Generic PIN/network hooks must not leak operations to the primary SIM. */
    g_assert_null (MM_BASE_SIM_GET_CLASS (t.sim)->send_pin);
    g_assert_null (MM_BASE_SIM_GET_CLASS (t.sim)->set_preferred_networks);
    g_clear_object (&t.sim);

    t.step = 0;
    responses[0] = "\r\n+CME ERROR: 10\r\n";
    mm_sim_mtk_ccci_new (MM_BASE_MODEM (modem), port, 2, (GAsyncReadyCallback) sim_ready, &t);
    g_main_loop_run (t.loop);
    g_assert_no_error (t.error);
    g_assert_null (t.sim);
    g_assert_cmpuint (t.step, ==, 1);

    t.step = 0;
    responses[0] = "\r\n+CME ERROR: 13\r\n";
    mm_sim_mtk_ccci_new (MM_BASE_MODEM (modem), port, 2, (GAsyncReadyCallback) sim_ready, &t);
    g_main_loop_run (t.loop);
    g_assert_error (t.error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_FAILURE);
    g_assert_null (t.sim);
    g_clear_error (&t.error);
    cleanup (&t, port);
}

int main (int argc, char **argv)
{
    GError *error = NULL;
    gint result;
    g_test_init (&argc, &argv, NULL);
    g_assert_true (mm_log_setup ("ERR", NULL, FALSE, FALSE, FALSE, FALSE, &error));
    g_assert_no_error (error);
    g_test_add_func ("/MM/mtk-ccci/dual-calls", test_dual_calls);
    g_test_add_func ("/MM/mtk-ccci/secondary-sim", test_sim);
    result = g_test_run ();
    mm_log_shutdown ();
    return result;
}
