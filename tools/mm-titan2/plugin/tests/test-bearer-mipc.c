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
#include "mm-wwan-link.h"
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

typedef enum { CANCEL, LINK_ERROR, LINK_EXISTS, LINK_TIMEOUT, IMS_CONFLICT, MISSING_INTERFACE, STALE_LINK,
               PROFILE_ERROR, CANCEL_PROFILES } Scenario;
typedef struct {
    GMainLoop *loop;
    MMPortMipc *port;
    GCancellable *cancel;
    GByteArray *rx;
    GIOChannel *channel;
    gint master, slave;
    guint watch, profiles, activations, deactivations, adds, deletes;
    Scenario scenario;
    GError *error;
    MMBaseModem *modem;
    MMKernelDevice *kernel;
    MMPort *old_link;
    gboolean old_released, replacement_announced;
} Test;
static Test *current;

typedef struct { MMKernelDevice parent; } TestLinkDevice;
typedef struct { MMKernelDeviceClass parent; } TestLinkDeviceClass;
GType test_link_device_get_type (void);
G_DEFINE_TYPE (TestLinkDevice, test_link_device, MM_TYPE_KERNEL_DEVICE)
static const gchar *test_link_subsystem (MMKernelDevice *self) { return "net"; }
static const gchar *test_link_name (MMKernelDevice *self) { return "wwan-test-2"; }
static void test_link_device_init (TestLinkDevice *self) { }
static void test_link_device_class_init (TestLinkDeviceClass *klass)
{
    MM_KERNEL_DEVICE_CLASS (klass)->get_subsystem = test_link_subsystem;
    MM_KERNEL_DEVICE_CLASS (klass)->get_name = test_link_name;
}

static void link_grabbed (MMBaseModem *modem, MMPort *port, Test *t)
{
    if (!t->old_link)
        t->old_link = g_object_ref (port);
    else {
        g_assert_true (port != t->old_link);
        t->replacement_announced = TRUE;
    }
}
static void link_released (MMBaseModem *modem, MMPort *port, Test *t)
{
    if (port == t->old_link)
        t->old_released = TRUE;
}
static gboolean announce_replacement (gpointer data)
{
    Test *t = data;
    g_assert_true (t->old_released);
    g_assert_true (mm_base_modem_grab_link_port (t->modem, t->kernel, &t->error));
    g_assert_no_error (t->error);
    return G_SOURCE_REMOVE;
}

/* Keep the real broadband/base modem object, but stub the plugin's modem
 * accessors and rtnetlink boundary. Bearer and MIPC transport are real. */
G_DEFINE_TYPE (MMBroadbandModemMtkCcci, mm_broadband_modem_mtk_ccci, MM_TYPE_BROADBAND_MODEM)
static void mm_broadband_modem_mtk_ccci_init (MMBroadbandModemMtkCcci *self) { }
static void mm_broadband_modem_mtk_ccci_class_init (MMBroadbandModemMtkCcciClass *klass) { }
MMPortMipc *mm_broadband_modem_mtk_ccci_peek_port_mipc (MMBroadbandModemMtkCcci *self) { return current->port; }
guint8 mm_broadband_modem_mtk_ccci_get_mipc_slot (MMBroadbandModemMtkCcci *self) { return MM_MIPC_SLOT_SIM2; }
const gchar *mm_broadband_modem_mtk_ccci_get_wwan_device (MMBroadbandModemMtkCcci *self) { return "wwan-test"; }
void mm_broadband_modem_mtk_ccci_prepare_data (MMBroadbandModemMtkCcci *self, const gchar *apn,
                                             GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_assert_cmpstr (apn, ==, "internet.example");
    g_assert_cmpuint (current->activations, ==, 0);
    current->profiles++;
    if (current->scenario == PROFILE_ERROR)
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Injected APN profile refusal");
    else {
        if (current->scenario == CANCEL_PROFILES)
            g_cancellable_cancel (current->cancel);
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}
gboolean mm_broadband_modem_mtk_ccci_prepare_data_finish (MMBroadbandModemMtkCcci *self,
                                                        GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}
gboolean mm_broadband_modem_mtk_ccci_ims_owns_link (MMBroadbandModemMtkCcci *self, guint id)
{
    g_assert_cmpuint (id, ==, 2);
    return current->scenario == IMS_CONFLICT;
}
gboolean mm_wwan_link_add (const gchar *parent, guint id, const gchar *name, gboolean *existed, GError **error)
{
    current->adds++;
    g_assert_cmpstr (parent, ==, "wwan-test");
    g_assert_cmpuint (id, ==, 2);
    g_assert_cmpstr (name, ==, "wwan-test-2");
    *existed = current->scenario == LINK_EXISTS;
    if (current->scenario == LINK_ERROR) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Injected link creation failure");
        return FALSE;
    }
    if (current->scenario == STALE_LINK)
        g_idle_add (announce_replacement, current);
    return TRUE;
}
gboolean mm_wwan_link_del (const gchar *name, GError **error)
{
    g_assert_cmpstr (name, ==, "wwan-test-2");
    current->deletes++;
    return TRUE;
}

static gboolean peer_read (GIOChannel *channel, GIOCondition cond, gpointer data)
{
    Test *t = data;
    guint8 bytes[4096];
    gsize n = 0, used;
    MMMipcMsg msg;
    gint parsed;
    if (g_io_channel_read_chars (channel, (gchar *) bytes, sizeof (bytes), &n, NULL) != G_IO_STATUS_NORMAL)
        return G_SOURCE_CONTINUE;
    g_byte_array_append (t->rx, bytes, n);
    while ((parsed = mm_mipc_frame_parse (t->rx->data, t->rx->len, &msg, &used)) == 1) {
        g_autoptr(GByteArray) tlvs = g_byte_array_new ();
        g_autoptr(GByteArray) frame = NULL;
        guint32 aid;
        static const guint8 address[] = { 10, 1, 2, 3 }, dns[] = { 1, 1, 1, 1 };
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_RESULT, 0);
        switch (msg.msg_id) {
        case MM_MIPC_INTERNAL_TEST_REQ:
        case MM_MIPC_INTERNAL_OPEN_REQ:
            break;
        case MM_MIPC_DATA_ACT_CALL_REQ:
            g_assert_cmpuint (t->profiles, ==, 1);
            g_assert_cmpuint (msg.slot, ==, MM_MIPC_SLOT_SIM2);
            t->activations++;
            mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_CNF_AID, 7);
            if (t->scenario != MISSING_INTERFACE)
                mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_CNF_TRANS_INTF_ID, 202);
            mm_mipc_tlv_add (tlvs, MM_MIPC_T_CNF_V4_ADDRS, address, sizeof (address));
            mm_mipc_tlv_add (tlvs, MM_MIPC_T_CNF_V4_DNS, dns, sizeof (dns));
            if (t->scenario == CANCEL)
                g_cancellable_cancel (t->cancel);
            break;
        case MM_MIPC_DATA_DEACT_CALL_REQ:
            g_assert_cmpuint (msg.slot, ==, MM_MIPC_SLOT_SIM2);
            g_assert_true (mm_mipc_tlv_find_uint (msg.payload, msg.len, MM_MIPC_T_DEACT_AID, &aid));
            g_assert_cmpuint (aid, ==, 7);
            t->deactivations++;
            break;
        default:
            g_assert_not_reached ();
        }
        frame = mm_mipc_frame_build (MM_MIPC_CNF_OF (msg.msg_id), msg.txid, msg.slot, tlvs->data, tlvs->len);
        g_assert_cmpint (write (t->master, frame->data, frame->len), ==, (gssize) frame->len);
        g_byte_array_remove_range (t->rx, 0, used);
    }
    g_assert_cmpint (parsed, ==, 0);
    return G_SOURCE_CONTINUE;
}

static void session_ready (MMPortMipc *port, GAsyncResult *res, Test *t)
{
    g_assert_true (mm_port_mipc_open_session_finish (port, res, &t->error));
    g_assert_no_error (t->error);
    g_main_loop_quit (t->loop);
}
static void connected (MMBaseBearer *bearer, GAsyncResult *res, Test *t)
{
    MMBearerConnectResult *result = MM_BASE_BEARER_GET_CLASS (bearer)->connect_finish (bearer, res, &t->error);
    g_assert_null (result);
    /* The failure is delivered only after rollback has been confirmed. */
    g_assert_cmpuint (t->deactivations, ==, (t->scenario == PROFILE_ERROR || t->scenario == CANCEL_PROFILES) ? 0 : 1);
    if (t->scenario == STALE_LINK) {
        g_assert_true (t->old_released);
        g_assert_true (t->replacement_announced);
    }
    g_main_loop_quit (t->loop);
}
static void test_rollback (gconstpointer data)
{
    Test t = { 0 };
    gchar name[128];
    struct termios term;
    g_autoptr(MMBaseModem) modem = NULL;
    g_autoptr(MMBaseBearer) bearer = NULL;
    g_autoptr(MMKernelDevice) kernel = NULL;
    g_autoptr(MMBearerProperties) config = mm_bearer_properties_new ();

    current = &t;
    t.scenario = GPOINTER_TO_INT (data);
    t.loop = g_main_loop_new (NULL, FALSE);
    t.cancel = g_cancellable_new ();
    t.rx = g_byte_array_new ();
    g_assert_cmpint (openpty (&t.master, &t.slave, name, NULL, NULL), ==, 0);
    g_assert_cmpint (tcgetattr (t.slave, &term), ==, 0);
    cfmakeraw (&term);
    g_assert_cmpint (tcsetattr (t.slave, TCSANOW, &term), ==, 0);
    t.channel = g_io_channel_unix_new (t.master);
    g_io_channel_set_encoding (t.channel, NULL, NULL);
    g_io_channel_set_buffered (t.channel, FALSE);
    t.watch = g_io_add_watch (t.channel, G_IO_IN, peer_read, &t);
    t.port = mm_port_mipc_new (name + 5);
    g_assert_true (mm_port_mipc_open (t.port, &t.error));
    g_assert_no_error (t.error);
    mm_port_mipc_open_session (t.port, MM_MIPC_SLOT_SIM2, NULL, 0, "rollback-test", NULL,
                               (GAsyncReadyCallback) session_ready, &t);
    g_main_loop_run (t.loop);
    modem = g_object_new (MM_TYPE_BROADBAND_MODEM_MTK_CCCI, MM_BASE_MODEM_DEVICE, "/sys/test",
                          MM_BASE_MODEM_PLUGIN, "mtk-ccci", NULL);
    if (t.scenario == STALE_LINK) {
        kernel = g_object_new (test_link_device_get_type (), NULL);
        t.modem = modem;
        t.kernel = kernel;
        g_signal_connect (modem, MM_BASE_MODEM_SIGNAL_LINK_PORT_GRABBED, G_CALLBACK (link_grabbed), &t);
        g_signal_connect (modem, MM_BASE_MODEM_SIGNAL_LINK_PORT_RELEASED, G_CALLBACK (link_released), &t);
        g_assert_true (mm_base_modem_grab_link_port (modem, kernel, &t.error));
        g_assert_no_error (t.error);
        g_assert_nonnull (t.old_link);
    }
    mm_bearer_properties_set_apn (config, "internet.example");
    mm_bearer_properties_set_ip_type (config, MM_BEARER_IP_FAMILY_IPV4);
    bearer = mm_bearer_mtk_ccci_new (MM_BROADBAND_MODEM_MTK_CCCI (modem), config);
    MM_BASE_BEARER_GET_CLASS (bearer)->connect (bearer, t.cancel, (GAsyncReadyCallback) connected, &t);
    g_main_loop_run (t.loop);
    g_assert_nonnull (t.error);
    if (t.scenario == CANCEL || t.scenario == CANCEL_PROFILES)
        g_assert_error (t.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint (t.profiles, ==, 1);
    g_assert_cmpuint (t.activations, ==, (t.scenario == PROFILE_ERROR || t.scenario == CANCEL_PROFILES) ? 0 : 1);
    g_assert_cmpuint (t.adds, ==, (t.scenario == LINK_ERROR || t.scenario == LINK_EXISTS || t.scenario == LINK_TIMEOUT || t.scenario == STALE_LINK) ? 1 : 0);
    g_assert_cmpuint (t.deletes, ==, (t.scenario == LINK_TIMEOUT || t.scenario == STALE_LINK) ? 1 : 0);
    g_clear_error (&t.error);
    g_clear_object (&bearer);
    g_assert_cmpuint (t.deletes, ==, (t.scenario == LINK_TIMEOUT || t.scenario == STALE_LINK) ? 1 : 0);
    g_clear_object (&modem);
    g_clear_object (&t.old_link);
    mm_port_mipc_close (t.port);
    g_clear_object (&t.port);
    g_source_remove (t.watch);
    g_io_channel_unref (t.channel);
    g_byte_array_unref (t.rx);
    g_object_unref (t.cancel);
    g_main_loop_unref (t.loop);
    close (t.master);
    close (t.slave);
    current = NULL;
}
int main (int argc, char **argv)
{
    GError *error = NULL;
    gint result;
    g_test_init (&argc, &argv, NULL);
    g_assert_true (mm_log_setup ("ERR", NULL, FALSE, FALSE, FALSE, FALSE, &error));
    g_assert_no_error (error);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/cancel-after-activation", GINT_TO_POINTER (CANCEL), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/link-error", GINT_TO_POINTER (LINK_ERROR), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/link-exists", GINT_TO_POINTER (LINK_EXISTS), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/link-timeout", GINT_TO_POINTER (LINK_TIMEOUT), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/ims-conflict", GINT_TO_POINTER (IMS_CONFLICT), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/missing-interface", GINT_TO_POINTER (MISSING_INTERFACE), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/stale-link-cache", GINT_TO_POINTER (STALE_LINK), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/profile-error", GINT_TO_POINTER (PROFILE_ERROR), test_rollback);
    g_test_add_data_func ("/MM/mtk-ccci/bearer/cancel-before-activation", GINT_TO_POINTER (CANCEL_PROFILES), test_rollback);
    result = g_test_run ();
    mm_log_shutdown ();
    return result;
}
