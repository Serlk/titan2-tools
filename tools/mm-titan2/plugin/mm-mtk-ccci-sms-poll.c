/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 * Reading stored SMS on demand: the modem stores every incoming SMS-DELIVER
 * in the receive storage (+CPMS mem3) and tells the AP over MIPC (a +CMTI in
 * SYS_AT_IND, or an SMS-group indication 0x46xx). On such a notice the plugin
 * re-reads SM/ME at once through MM's own storage locking, PDU reader, SMS
 * list and duplicate detection (Messaging.Added for every new message).
 * A slow fallback scan covers a lost notice: every POLL_SECONDS while awake
 * and once after the system was asleep (the first MIPC message or timer tick
 * after a resume). This is not a native MIPC SMS implementation or a second
 * AT-port owner.
 */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mm-mtk-ccci-sms-poll.h"
#include "mm-iface-modem-messaging.h"
#include "mm-sms-list.h"
#include "mm-log-object.h"

#define STATE "mtk-ccci-sms-poll-state"
/* fallback scan while awake (was 15 s before the notifications were enabled) */
#ifndef POLL_SECONDS
#define POLL_SECONDS 300
#endif
/* a scan after a resume, at most this often (boot-time seconds) */
#define RESUME_SCAN_MIN_SECONDS 30
/* the boot-time clock ran ahead of the monotonic one by this much: a suspend */
#define RESUME_MIN_US (1 * G_USEC_PER_SEC)
#ifndef SMS_WAKE_LOCK_PATH
#define SMS_WAKE_LOCK_PATH "/sys/power/wake_lock"
#endif
#define SMS_WAKE_LOCK_NAME "mm-mtk-ccci-sms"

typedef struct {
    guint    source;      /* the fallback timer; 0 = stopped */
    guint    epoch;       /* bumped by stop: old scans end quietly */
    gboolean busy;        /* a scan is running */
    gboolean rerun;       /* a notice came during the scan: scan again after it */
    gint64   offset_us;   /* CLOCK_BOOTTIME - CLOCK_MONOTONIC at the last look */
    gint64   last_scan_us;/* CLOCK_BOOTTIME of the last scan start */
} PollState;

typedef struct { guint epoch, storage; gchar *reason; } Scan;
static const MMSmsStorage storages[] = { MM_SMS_STORAGE_SM, MM_SMS_STORAGE_ME };
static void scan_step (GTask *task);
static void poll_request (MMBaseModem *modem, const gchar *reason);

static gint64 clock_us (clockid_t id)
{
    struct timespec ts;
    if (clock_gettime (id, &ts))
        return 0;
    return (gint64) ts.tv_sec * G_USEC_PER_SEC + ts.tv_nsec / 1000;
}

static PollState *state (MMBaseModem *modem)
{
    PollState *st = g_object_get_data (G_OBJECT (modem), STATE);
    if (!st) {
        st = g_new0 (PollState, 1);
        st->offset_us = clock_us (CLOCK_BOOTTIME) - clock_us (CLOCK_MONOTONIC);
        g_object_set_data_full (G_OBJECT (modem), STATE, st, g_free);
    }
    return st;
}

static void scan_free (Scan *ctx)
{
    g_free (ctx->reason);
    g_free (ctx);
}

static gboolean can_scan (MMBaseModem *modem)
{
    g_autoptr(MMSmsList) list = NULL;
    MMModemState mstate;
    MMSmsStorage storage;
    gboolean pdu;

    g_object_get (modem, MM_IFACE_MODEM_STATE, &mstate,
                  MM_IFACE_MODEM_MESSAGING_SMS_LIST, &list,
                  MM_IFACE_MODEM_MESSAGING_SMS_DEFAULT_STORAGE, &storage,
                  MM_IFACE_MODEM_MESSAGING_SMS_PDU_MODE, &pdu, NULL);
    return mstate >= MM_MODEM_STATE_ENABLED && list && pdu && storage != MM_SMS_STORAGE_UNKNOWN &&
           !g_cancellable_is_cancelled (mm_base_modem_peek_cancellable (modem));
}

static void scan_done (GTask *task)
{
    MMBaseModem *modem = g_task_get_source_object (task);
    PollState *st = state (modem);
    Scan *ctx = g_task_get_task_data (task);

    if (ctx->epoch == st->epoch) {
        st->busy = FALSE;
        if (ctx->reason) {
            g_autoptr(MMSmsList) list = NULL;
            g_object_get (modem, MM_IFACE_MODEM_MESSAGING_SMS_LIST, &list, NULL);
            mm_obj_msg (modem, "SMS storage scan (%s) done: %u message(s) known",
                        ctx->reason, list ? mm_sms_list_get_count (list) : 0);
        }
        if (st->rerun) {
            st->rerun = FALSE;
            poll_request (modem, ctx->reason ? "follow-up" : NULL);
        }
    }
    g_object_unref (task);
}

static void storage_scanned (MMIfaceModemMessaging *modem, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;
    Scan *ctx = g_task_get_task_data (task);
    if (!MM_IFACE_MODEM_MESSAGING_GET_IFACE (modem)->load_initial_sms_parts_finish (modem, res, &error))
        mm_obj_dbg (modem, "SMS storage scan %s: %s", mm_sms_storage_get_string (storages[ctx->storage]), error->message);
    ctx->storage++;
    scan_step (task);
}

static void scan_step (GTask *task)
{
    MMBaseModem *modem = g_task_get_source_object (task);
    MMIfaceModemMessaging *messaging = MM_IFACE_MODEM_MESSAGING (modem);
    Scan *ctx = g_task_get_task_data (task);

    if (ctx->epoch != state (modem)->epoch || !can_scan (modem)) {
        scan_done (task);
        return;
    }
    while (ctx->storage < G_N_ELEMENTS (storages) &&
           !mm_iface_modem_messaging_is_storage_supported_for_storing (messaging, storages[ctx->storage], NULL))
        ctx->storage++;
    if (ctx->storage == G_N_ELEMENTS (storages)) {
        scan_done (task);
        return;
    }
    MM_IFACE_MODEM_MESSAGING_GET_IFACE (messaging)->load_initial_sms_parts (
        messaging, storages[ctx->storage], (GAsyncReadyCallback) storage_scanned, task);
}

static void poll_request (MMBaseModem *modem, const gchar *reason)
{
    MMIfaceModemMessagingInterface *iface = MM_IFACE_MODEM_MESSAGING_GET_IFACE (modem);
    PollState *st = state (modem);
    GTask *task;
    Scan *ctx;

    if (!st->source || !can_scan (modem) ||
        !iface->load_initial_sms_parts || !iface->load_initial_sms_parts_finish)
        return;
    if (st->busy) {
        /* the running scan may have passed the storage already */
        st->rerun = TRUE;
        return;
    }
    if (reason)
        mm_obj_msg (modem, "SMS storage scan: %s", reason);
    st->busy = TRUE;
    st->last_scan_us = clock_us (CLOCK_BOOTTIME);
    task = g_task_new (modem, NULL, NULL, NULL);
    ctx = g_new0 (Scan, 1);
    ctx->epoch = st->epoch;
    ctx->reason = g_strdup (reason);
    g_task_set_task_data (task, ctx, (GDestroyNotify) scan_free);
    scan_step (task);
}

void mm_mtk_ccci_sms_poll_now (MMBaseModem *modem)
{
    poll_request (modem, NULL);
}

void mm_mtk_ccci_sms_poll_notify (MMBaseModem *modem, const gchar *reason)
{
    poll_request (modem, reason ? reason : "notification");
}

void mm_mtk_ccci_sms_poll_check_resume (MMBaseModem *modem)
{
    PollState *st = state (modem);
    gint64 boot = clock_us (CLOCK_BOOTTIME);
    gint64 offset = boot - clock_us (CLOCK_MONOTONIC);
    gint64 slept = offset - st->offset_us;

    if (slept < RESUME_MIN_US)
        return;
    st->offset_us = offset;
    if (boot - st->last_scan_us < RESUME_SCAN_MIN_SECONDS * G_USEC_PER_SEC)
        return;
    {
        g_autofree gchar *reason = g_strdup_printf ("after %" G_GINT64_FORMAT " s asleep", slept / G_USEC_PER_SEC);
        poll_request (modem, reason);
    }
}

void mm_mtk_ccci_sms_wake_hold (MMBaseModem *modem, guint ms)
{
    g_autofree gchar *line = NULL;
    int fd;

    /* a timed kernel wakelock (CONFIG_PM_WAKELOCKS): the system does not
     * suspend before the message is read and Messaging.Added went out;
     * t2-powerd's wakeup_count check sees it. It expires by itself. */
    fd = open (SMS_WAKE_LOCK_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        mm_obj_dbg (modem, "SMS wakelock: %s: %s", SMS_WAKE_LOCK_PATH, g_strerror (errno));
        return;
    }
    line = g_strdup_printf ("%s %" G_GUINT64_FORMAT, SMS_WAKE_LOCK_NAME, (guint64) ms * 1000000u);
    if (write (fd, line, strlen (line)) < 0)
        mm_obj_dbg (modem, "SMS wakelock: %s", g_strerror (errno));
    close (fd);
}

static gboolean poll_tick (gpointer modem)
{
    mm_mtk_ccci_sms_poll_check_resume (modem);
    mm_mtk_ccci_sms_poll_now (modem);
    return G_SOURCE_CONTINUE;
}

void mm_mtk_ccci_sms_poll_start (MMBaseModem *modem)
{
    PollState *st = state (modem);
    if (!st->source)
        st->source = g_timeout_add_seconds (POLL_SECONDS, poll_tick, modem);
}

void mm_mtk_ccci_sms_poll_stop (MMBaseModem *modem)
{
    PollState *st = state (modem);
    if (st->source)
        g_source_remove (st->source);
    st->source = 0;
    st->busy = FALSE;
    st->rerun = FALSE;
    st->epoch++;
}
