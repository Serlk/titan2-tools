/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#include <config.h>
#include <string.h>
#include <mm-errors-types.h>
#include "mm-mtk-ccci-sms-ready.h"
#include "mm-base-modem-at.h"
#include "mm-port-mipc.h"
#include "mm-log-test.h"

typedef enum { READY, DELAYED, PERMANENT, NEVER_READY, CANCELLED, MALFORMED, EMPTY_STORAGES } Scenario;
typedef struct { const gchar *command, *reply; gint code; } Response;
#define STORAGES "+CPMS: (\"SM\",\"ME\"),(\"SM\",\"ME\"),(\"SM\",\"ME\")"
#define CURRENT "+CPMS: \"SM\",1,10,\"SM\",1,10,\"SM\",1,10"
static const Response delayed[] = {
    { "+CPMS=?", NULL, MM_MESSAGE_ERROR_UNKNOWN },
    { "+CPMS=?", STORAGES, 0 },
    { "+CPMS?", NULL, MM_MESSAGE_ERROR_ME_FAILURE },
    { "+CPMS=?", STORAGES, 0 },
    { "+CPMS?", CURRENT, 0 },
    { "+CMGF=0", NULL, MM_MESSAGE_ERROR_UNKNOWN },
    { "+CPMS=?", STORAGES, 0 },
    { "+CPMS?", CURRENT, 0 },
    { "+CMGF=0", "", 0 },
};
static struct {
    GMainLoop *loop;
    GCancellable *cancel;
    Scenario scenario;
    guint calls;
    gboolean ok;
    GError *error;
} t;

/* Only AT is mocked. The actual readiness state machine, parsers, retry
 * timer and cancellation path run normally; no modem/SMS is touched. */
void mm_base_modem_at_command (MMBaseModem *modem, const gchar *cmd, guint timeout,
                               gboolean cached, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (modem, NULL, callback, user_data);
    const gchar *reply = "";
    gint code = 0;
    guint call = t.calls++;

    g_assert_false (cached);
    g_assert_cmpuint (timeout, <=, 3);
    if (t.scenario == DELAYED) {
        g_assert_cmpuint (call, <, G_N_ELEMENTS (delayed));
        g_assert_cmpstr (cmd, ==, delayed[call].command);
        reply = delayed[call].reply;
        code = delayed[call].code;
    } else if (t.scenario == PERMANENT || t.scenario == NEVER_READY) {
        g_assert_cmpstr (cmd, ==, "+CPMS=?");
        code = t.scenario == PERMANENT ? MM_MESSAGE_ERROR_NOT_SUPPORTED : MM_MESSAGE_ERROR_UNKNOWN;
    } else if (t.scenario == MALFORMED) {
        g_assert_cmpstr (cmd, ==, "+CPMS=?");
        reply = "not a storage list";
    } else if (t.scenario == EMPTY_STORAGES) {
        g_assert_cmpstr (cmd, ==, "+CPMS=?");
        reply = "+CPMS: (\"SM\"),(\"SR\"),(\"SR\")";
    } else {
        static const gchar *commands[] = { "+CPMS=?", "+CPMS?", "+CMGF=0" };
        if (call >= G_N_ELEMENTS (commands)) {
            g_assert_not_reached ();
            return;
        }
        g_assert_cmpstr (cmd, ==, commands[call]);
        reply = call == 0 ? STORAGES : (call == 1 ? CURRENT : "");
    }
    if (t.scenario == CANCELLED)
        g_cancellable_cancel (t.cancel);
    if (code)
        g_task_return_new_error (task, MM_MESSAGE_ERROR, code, "Injected SMS service error");
    else
        g_task_return_pointer (task, g_strdup (reply), g_free);
    g_object_unref (task);
}

const gchar *mm_base_modem_at_command_finish (MMBaseModem *modem, GAsyncResult *res, GError **error)
{
    /* Return a borrowed response, just like MM's AT command API. */
    GTask *task = G_TASK (res);
    gchar *reply = g_task_propagate_pointer (task, error);
    g_task_set_task_data (task, reply, g_free);
    return reply;
}

static void completed (GObject *owner, GAsyncResult *res, gpointer unused)
{
    t.ok = mm_mtk_ccci_sms_wait_ready_finish (res, &t.error);
    g_main_loop_quit (t.loop);
}

static void test_ready (gconstpointer scenario)
{
    g_autoptr(MMPortMipc) owner = mm_port_mipc_new ("fake-sms-owner");
    memset (&t, 0, sizeof (t));
    t.scenario = GPOINTER_TO_UINT (scenario);
    t.loop = g_main_loop_new (NULL, FALSE);
    t.cancel = g_cancellable_new ();
    mm_mtk_ccci_sms_wait_ready ((MMBaseModem *) owner, t.cancel, completed, NULL);
    g_main_loop_run (t.loop);
    if (t.scenario == READY || t.scenario == DELAYED) {
        g_assert_true (t.ok);
        g_assert_no_error (t.error);
        g_assert_cmpuint (t.calls, ==, t.scenario == READY ? 3 : G_N_ELEMENTS (delayed));
    } else {
        g_assert_false (t.ok);
        g_assert_nonnull (t.error);
        if (t.scenario == CANCELLED)
            g_assert_error (t.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        if (t.scenario == PERMANENT)
            g_assert_error (t.error, MM_MESSAGE_ERROR, MM_MESSAGE_ERROR_NOT_SUPPORTED);
        g_assert_cmpuint (t.calls, ==, (t.scenario == NEVER_READY || t.scenario == EMPTY_STORAGES) ? 30 : 1);
    }
    g_clear_error (&t.error);
    g_object_unref (t.cancel);
    g_main_loop_unref (t.loop);
}

int main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/MM/mtk-ccci/sms/ready", GUINT_TO_POINTER (READY), test_ready);
    g_test_add_data_func ("/MM/mtk-ccci/sms/cold-delayed", GUINT_TO_POINTER (DELAYED), test_ready);
    g_test_add_data_func ("/MM/mtk-ccci/sms/not-supported", GUINT_TO_POINTER (PERMANENT), test_ready);
    g_test_add_data_func ("/MM/mtk-ccci/sms/bounded-retries", GUINT_TO_POINTER (NEVER_READY), test_ready);
    g_test_add_data_func ("/MM/mtk-ccci/sms/cancelled", GUINT_TO_POINTER (CANCELLED), test_ready);
    g_test_add_data_func ("/MM/mtk-ccci/sms/malformed", GUINT_TO_POINTER (MALFORMED), test_ready);
    g_test_add_data_func ("/MM/mtk-ccci/sms/no-common-storage", GUINT_TO_POINTER (EMPTY_STORAGES), test_ready);
    return g_test_run ();
}
