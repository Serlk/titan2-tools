/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 * Standard 3GPP SMS readiness probes, based on our phone observations and
 * ModemManager's existing CPMS parser. No vendor implementation is used.
 */
#include <config.h>
#include <mm-errors-types.h>
#include "mm-mtk-ccci-sms-ready.h"
#include "mm-base-modem-at.h"
#include "mm-modem-helpers.h"
#include "mm-log-object.h"

#ifndef SMS_READY_RETRY_MS
#define SMS_READY_RETRY_MS 1000
#endif
#define SMS_READY_MAX_ATTEMPTS 30

typedef struct { guint step, attempts; } Ready;
static const gchar *commands[] = { "+CPMS=?", "+CPMS?", "+CMGF=0" };
static gboolean ready_step (gpointer user_data);

static gboolean cancelled (GTask *task)
{
    if (!g_task_return_error_if_cancelled (task))
        return FALSE;
    g_object_unref (task);
    return TRUE;
}

static void ready_response (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    Ready *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    const gchar *response = mm_base_modem_at_command_finish (modem, res, &error);

    if (cancelled (task))
        return;
    if (response && !error && ctx->step == 0) {
        g_autoptr(GArray) mem1 = NULL;
        g_autoptr(GArray) mem2 = NULL;
        g_autoptr(GArray) mem3 = NULL;
        gboolean common = FALSE;
        guint i, j;

        if (mm_3gpp_parse_cpms_test_response (response, &mem1, &mem2, &mem3, &error)) {
            for (i = 0; mem2 && i < mem2->len; i++) {
                MMSmsStorage storage = g_array_index (mem2, MMSmsStorage, i);
                if (storage != MM_SMS_STORAGE_SM && storage != MM_SMS_STORAGE_ME && storage != MM_SMS_STORAGE_MT)
                    continue;
                for (j = 0; mem3 && j < mem3->len; j++)
                    common |= storage == g_array_index (mem3, MMSmsStorage, j);
            }
            if (!mem1 || !mem1->len || !common)
                g_set_error_literal (&error, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                      "SMS storages are not ready");
        }
    } else if (response && !error && ctx->step == 1) {
        MMSmsStorage mem1, mem2;
        mm_3gpp_parse_cpms_query_response (response, &mem1, &mem2, &error);
    }
    if (error || !response) {
        gboolean transient = error &&
            ((error->domain == MM_MESSAGE_ERROR && error->code != MM_MESSAGE_ERROR_NOT_SUPPORTED) ||
             g_error_matches (error, MM_CORE_ERROR, MM_CORE_ERROR_RETRY) ||
             g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_BUSY));
        if (transient && ctx->attempts < SMS_READY_MAX_ATTEMPTS) {
            mm_obj_dbg (modem, "SMS service not ready at %s (attempt %u): %s",
                        commands[ctx->step], ctx->attempts, error->message);
            ctx->step = 0;
            g_timeout_add (SMS_READY_RETRY_MS, ready_step, task);
            return;
        }
        if (!error)
            error = g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "SMS readiness probe returned no response");
        g_prefix_error (&error, "SMS service readiness at %s: ", commands[ctx->step]);
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    ctx->step++;
    ready_step (task);
}

static gboolean ready_step (gpointer user_data)
{
    GTask *task = user_data;
    Ready *ctx = g_task_get_task_data (task);
    MMBaseModem *modem = g_task_get_source_object (task);

    if (cancelled (task))
        return G_SOURCE_REMOVE;
    if (ctx->step == G_N_ELEMENTS (commands)) {
        mm_obj_dbg (modem, "SMS service ready for storage discovery and PDU mode");
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }
    if (!ctx->step)
        ctx->attempts++;
    mm_base_modem_at_command (modem, commands[ctx->step], 3, FALSE,
                              (GAsyncReadyCallback) ready_response, task);
    return G_SOURCE_REMOVE;
}

void mm_mtk_ccci_sms_wait_ready (MMBaseModem *modem, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (modem, cancellable, callback, user_data);
    g_task_set_task_data (task, g_new0 (Ready, 1), g_free);
    ready_step (task);
}

gboolean mm_mtk_ccci_sms_wait_ready_finish (GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}
