/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#include <config.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>
#include "mm-bearer-mtk-ccci.h"
#include "mm-broadband-modem-mtk-ccci.h"
#include "mm-mtk-ccci-sms-poll.h"
#include "mm-iface-modem-messaging.h"
#include "mm-sms-list.h"
#include "mm-sms-part-3gpp.h"
#include "mm-log.h"

/* Optional backends are not instantiated, as in upstream test-base-call.c. */
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


typedef struct { MMBroadbandModem parent; } SmsTestModem;
typedef struct { MMBroadbandModemClass parent; } SmsTestModemClass;
static void messaging_init (MMIfaceModemMessagingInterface *iface);
GType sms_test_modem_get_type (void);
G_DEFINE_TYPE_EXTENDED (SmsTestModem, sms_test_modem, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_MESSAGING, messaging_init))

static struct {
    GMainLoop *loop;
    guint scans;
    gboolean fail_first, pause_first, only_sm;
    GTask *pending;
} t;

static void completed (GObject *modem, GAsyncResult *res, gpointer unused)
{
    GError *error = NULL;
    g_assert_true (mm_iface_modem_messaging_initialize_finish (MM_IFACE_MODEM_MESSAGING (modem), res, &error));
    g_assert_no_error (error);
    g_main_loop_quit (t.loop);
}

static void success (MMIfaceModemMessaging *modem, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (modem, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}
static gboolean success_finish (MMIfaceModemMessaging *modem, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}
static gboolean supported_finish (MMIfaceModemMessaging *modem, GAsyncResult *res,
                                  GArray **mem1, GArray **mem2, GArray **mem3, GError **error)
{
    MMSmsStorage values[] = { MM_SMS_STORAGE_SM, MM_SMS_STORAGE_ME };
    GArray **arrays[] = { mem1, mem2, mem3 };
    guint i;
    if (!success_finish (modem, res, error))
        return FALSE;
    for (i = 0; i < G_N_ELEMENTS (arrays); i++) {
        *arrays[i] = g_array_new (FALSE, FALSE, sizeof (MMSmsStorage));
        g_array_append_vals (*arrays[i], values, t.only_sm ? 1 : 2);
    }
    return TRUE;
}
static gboolean current_finish (MMIfaceModemMessaging *modem, GAsyncResult *res,
                                MMSmsStorage *storage, GError **error)
{
    *storage = MM_SMS_STORAGE_SM;
    return success_finish (modem, res, error);
}

/* The storage boundary substitutes a received PDU; MM's actual SMS objects,
 * list and duplicate detection remain real. Nothing is sent to a phone. */
static void scan (MMIfaceModemMessaging *modem, MMSmsStorage storage,
                  GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (modem, NULL, callback, user_data);
    t.scans++;
    if (t.pause_first && t.scans == 1) {
        t.pending = task;
        return;
    }
    if (t.fail_first && t.scans == 1) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY, "Injected busy SMS storage");
    } else {
        if (storage == MM_SMS_STORAGE_SM) {
            GError *error = NULL;
            MMSmsPart *part = mm_sms_part_3gpp_new_from_pdu (1, "0004049121430000620103000000000141", modem, &error);
            g_assert_no_error (error);
            g_assert_nonnull (part);
            if (!mm_iface_modem_messaging_take_part (modem,
                    mm_broadband_modem_create_sms (MM_BROADBAND_MODEM (modem)),
                    part, MM_SMS_STATE_RECEIVED, storage, &error)) {
                g_assert_nonnull (error);
                g_assert_nonnull (strstr (error->message, "already taken"));
                g_clear_error (&error);
            }
        }
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void messaging_init (MMIfaceModemMessagingInterface *iface)
{
    iface->check_support = success;
    iface->check_support_finish = success_finish;
    iface->load_supported_storages = success;
    iface->load_supported_storages_finish = supported_finish;
    iface->init_current_storages = success;
    iface->init_current_storages_finish = current_finish;
    iface->load_initial_sms_parts = scan;
    iface->load_initial_sms_parts_finish = success_finish;
}
static void sms_test_modem_init (SmsTestModem *self) { }
static void sms_test_modem_class_init (SmsTestModemClass *klass) { }

static void drain (void)
{
    while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);
}

static void test_poll (gconstpointer scenario)
{
    guint which = GPOINTER_TO_UINT (scenario);
    MMBaseModem *modem;
    MMSmsList *list;
    MmGdbusModem *skeleton;
    memset (&t, 0, sizeof (t));
    t.fail_first = which == 1;
    t.pause_first = which == 2;
    t.only_sm = which == 4;
    t.loop = g_main_loop_new (NULL, FALSE);
    modem = g_object_new (sms_test_modem_get_type (), MM_BASE_MODEM_DEVICE, "/sys/test",
                          MM_BASE_MODEM_PLUGIN, "sms-test", NULL);
    skeleton = mm_gdbus_modem_skeleton_new ();
    mm_gdbus_modem_set_current_capabilities (skeleton, MM_MODEM_CAPABILITY_GSM_UMTS);
    g_object_set (modem, MM_IFACE_MODEM_DBUS_SKELETON, skeleton, NULL);
    g_object_unref (skeleton);
    mm_iface_modem_messaging_initialize (MM_IFACE_MODEM_MESSAGING (modem), NULL, completed, NULL);
    g_main_loop_run (t.loop);
    list = mm_sms_list_new (G_OBJECT (modem));
    g_object_set (modem, MM_IFACE_MODEM_MESSAGING_SMS_LIST, list,
                  MM_IFACE_MODEM_MESSAGING_SMS_PDU_MODE, TRUE,
                  MM_IFACE_MODEM_STATE, MM_MODEM_STATE_REGISTERED, NULL);
    mm_mtk_ccci_sms_poll_start (modem);

    if (which == 3) {
        g_object_set (modem, MM_IFACE_MODEM_MESSAGING_SMS_DEFAULT_STORAGE, MM_SMS_STORAGE_UNKNOWN, NULL);
        mm_mtk_ccci_sms_poll_now (modem);
        drain ();
        g_assert_cmpuint (t.scans, ==, 0);
        g_object_set (modem, MM_IFACE_MODEM_MESSAGING_SMS_DEFAULT_STORAGE, MM_SMS_STORAGE_SM,
                      MM_IFACE_MODEM_STATE, MM_MODEM_STATE_DISABLED, NULL);
        mm_mtk_ccci_sms_poll_now (modem);
        drain ();
        g_assert_cmpuint (t.scans, ==, 0);
    } else {
        mm_mtk_ccci_sms_poll_now (modem);
        mm_mtk_ccci_sms_poll_now (modem); /* during a scan: exactly one more after it */
        mm_mtk_ccci_sms_poll_notify (modem, "test notice");
        if (t.pause_first) {
            g_assert_cmpuint (t.scans, ==, 1);
            mm_mtk_ccci_sms_poll_stop (modem);
            g_task_return_boolean (t.pending, TRUE);
            g_object_unref (t.pending);
            drain ();
            g_assert_cmpuint (t.scans, ==, 1); /* no ME scan and no follow-up after stopping */
            mm_mtk_ccci_sms_poll_start (modem);
        } else {
            drain ();
            g_assert_cmpuint (t.scans, ==, (t.only_sm ? 1 : 2) * 2);
        }
        mm_mtk_ccci_sms_poll_now (modem);
        drain ();
        g_assert_cmpuint (mm_sms_list_get_count (list), ==, 1);
        /* Another scan must retain the stored message without duplicating it. */
        mm_mtk_ccci_sms_poll_now (modem);
        drain ();
        g_assert_cmpuint (mm_sms_list_get_count (list), ==, 1);
        /* no suspend happened: activity does not start a scan */
        {
            guint before = t.scans;
            mm_mtk_ccci_sms_poll_check_resume (modem);
            drain ();
            g_assert_cmpuint (t.scans, ==, before);
        }
    }
    mm_mtk_ccci_sms_poll_stop (modem);
    g_object_set (modem, MM_IFACE_MODEM_MESSAGING_SMS_LIST, NULL, NULL);
    mm_iface_modem_messaging_shutdown (MM_IFACE_MODEM_MESSAGING (modem));
    g_object_unref (list);
    g_object_unref (modem);
    g_main_loop_unref (t.loop);
}

int main (int argc, char **argv)
{
    GError *error = NULL;
    gint result;
    g_test_init (&argc, &argv, NULL);
    g_assert_true (mm_log_setup ("ERR", NULL, FALSE, FALSE, FALSE, FALSE, &error));
    g_assert_no_error (error);
    g_test_add_data_func ("/MM/mtk-ccci/sms-poll/recover-and-deduplicate", GUINT_TO_POINTER (0), test_poll);
    g_test_add_data_func ("/MM/mtk-ccci/sms-poll/storage-busy", GUINT_TO_POINTER (1), test_poll);
    g_test_add_data_func ("/MM/mtk-ccci/sms-poll/stop-pending", GUINT_TO_POINTER (2), test_poll);
    g_test_add_data_func ("/MM/mtk-ccci/sms-poll/not-ready", GUINT_TO_POINTER (3), test_poll);
    g_test_add_data_func ("/MM/mtk-ccci/sms-poll/sm-only", GUINT_TO_POINTER (4), test_poll);
    result = g_test_run ();
    mm_log_shutdown ();
    return result;
}
