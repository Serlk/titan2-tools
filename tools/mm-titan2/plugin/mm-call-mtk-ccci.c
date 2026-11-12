/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 * Per-SIM noninteractive AT call control via SYS_AT.
 * Written from a clean-room behavioural description of the protocol; no code of the factory firmware was used.
 */
#include <config.h>
#include <string.h>
#include <mm-errors-types.h>
#include "mm-call-mtk-ccci.h"
#include "mm-modem-helpers-mtk-ccci.h"
#include "mm-bind.h"
#include "mm-call-list.h"

typedef struct { MMBaseCall parent; MMPortMipc *port; guint8 default_slot; } MMCallMtkCcci;
typedef struct { MMBaseCallClass parent; } MMCallMtkCcciClass;
GType mm_call_mtk_ccci_get_type (void);
G_DEFINE_TYPE (MMCallMtkCcci, mm_call_mtk_ccci, MM_TYPE_BASE_CALL)

static guint8 call_slot (MMCallMtkCcci *self)
{
    guint index = mm_base_call_get_index (MM_BASE_CALL (self));
    return index ? (index > 100 ? 2 : 1) : self->default_slot;
}

static gboolean command_finish (MMBaseCall *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void command_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    if (!reply)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void command (MMCallMtkCcci *self, const gchar *text, GCancellable *cancellable,
                      GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    mm_port_mipc_at_command (self->port, call_slot (self), text, 30,
                              (GAsyncReadyCallback) command_ready, task);
}

static void start (MMBaseCall *base, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer data)
{
    const gchar *number = mm_base_call_get_number (base);
    g_autofree gchar *text = NULL;
    if (!number || !number[0] || strlen (number) > 128 || strspn (number, "+0123456789*#ABCDabcd") != strlen (number)) {
        g_task_report_new_error (base, cb, data, start, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS, "Invalid dial number");
        return;
    }
    text = g_strdup_printf ("ATD%s;", number);
    command ((MMCallMtkCcci *) base, text, cancellable, cb, data);
}

static void accept_call (MMBaseCall *base, GAsyncReadyCallback cb, gpointer data)
{
    command ((MMCallMtkCcci *) base, "ATA", NULL, cb, data);
}

static void hangup (MMBaseCall *base, GAsyncReadyCallback cb, gpointer data)
{
    guint index = mm_base_call_get_index (base);
    g_autofree gchar *text = index ? g_strdup_printf ("AT+CHLD=1%u", index % 100) : g_strdup ("AT+CHUP");
    command ((MMCallMtkCcci *) base, text, NULL, cb, data);
}

static void dtmf_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    if (!reply)
        g_task_return_error (task, error);
    else
        g_task_return_int (task, GPOINTER_TO_UINT (g_task_get_task_data (task)));
    g_object_unref (task);
}

static void send_dtmf (MMBaseCall *base, const gchar *digits, GAsyncReadyCallback cb, gpointer data)
{
    MMCallMtkCcci *self = (MMCallMtkCcci *) base;
    GTask *task = g_task_new (self, NULL, cb, data);
    g_autofree gchar *text = NULL;
    if (!digits || !digits[0] || strlen (digits) > 32 || strspn (digits, "0123456789*#ABCDabcd") != strlen (digits)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS, "Invalid DTMF string");
        g_object_unref (task);
        return;
    }
    text = g_strdup_printf ("AT+VTS=\"%s\"", digits);
    g_task_set_task_data (task, GUINT_TO_POINTER (strlen (digits)), NULL);
    mm_port_mipc_at_command (self->port, call_slot (self), text, 10, (GAsyncReadyCallback) dtmf_ready, task);
}

static gssize dtmf_finish (MMBaseCall *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_int (G_TASK (res), error);
}

static void dispose (GObject *object)
{
    g_clear_object (&((MMCallMtkCcci *) object)->port);
    G_OBJECT_CLASS (mm_call_mtk_ccci_parent_class)->dispose (object);
}

static void mm_call_mtk_ccci_init (MMCallMtkCcci *self) { }
static void mm_call_mtk_ccci_class_init (MMCallMtkCcciClass *klass)
{
    MMBaseCallClass *base = MM_BASE_CALL_CLASS (klass);
    G_OBJECT_CLASS (klass)->dispose = dispose;
    base->start = start; base->start_finish = command_finish;
    base->accept = accept_call; base->accept_finish = command_finish;
    base->hangup = hangup; base->hangup_finish = command_finish;
    base->send_dtmf = send_dtmf; base->send_dtmf_finish = dtmf_finish;
}

MMBaseCall *mm_call_mtk_ccci_new (MMBaseModem *modem, MMPortMipc *port, guint8 slot,
                                 MMCallDirection direction, const gchar *number, guint dtmf_duration)
{
    MMCallMtkCcci *self = g_object_new (mm_call_mtk_ccci_get_type (),
        MM_BASE_CALL_IFACE_MODEM_VOICE, modem, MM_BIND_TO, modem,
        MM_CALL_DIRECTION, direction, MM_CALL_NUMBER, number,
        MM_CALL_DTMF_TONE_DURATION, dtmf_duration,
        MM_BASE_CALL_SKIP_INCOMING_TIMEOUT, TRUE,
        MM_BASE_CALL_SUPPORTS_DIALING_TO_RINGING, TRUE,
        MM_BASE_CALL_SUPPORTS_RINGING_TO_ACTIVE, TRUE, NULL);
    self->port = g_object_ref (port);
    self->default_slot = slot;
    return MM_BASE_CALL (self);
}

/* Generic voice matching may match equal phone numbers despite distinct call
 * indexes. DSDS requires strict (slot,id) matching, including when both SIMs
 * are called by the same number. */
typedef struct { guint8 slot; MMBaseCall *call; } FindCall;
static void find_outgoing (MMBaseCall *call, gpointer user_data)
{
    FindCall *ctx = user_data;
    if (ctx->call || mm_base_call_get_index (call) ||
        mm_base_call_get_state (call) == MM_CALL_STATE_TERMINATED ||
        mm_base_call_get_direction (call) != MM_CALL_DIRECTION_OUTGOING ||
        !G_TYPE_CHECK_INSTANCE_TYPE (call, mm_call_mtk_ccci_get_type ()))
        return;
    if (((MMCallMtkCcci *) call)->default_slot == ctx->slot)
        ctx->call = call;
}

void mm_call_mtk_ccci_report (MMIfaceModemVoice *modem, MMPortMipc *port, guint8 slot, const MMCallInfo *info)
{
    g_autoptr(MMCallList) list = NULL;
    MMBaseCall *call;
    gboolean created = FALSE;
    g_object_get (modem, MM_IFACE_MODEM_VOICE_CALL_LIST, &list, NULL);
    if (!list || !info->index)
        return;
    call = mm_call_list_get_call_by_index (list, info->index);
    if (call && mm_base_call_get_state (call) == MM_CALL_STATE_TERMINATED) {
        /* Modem call ids are reusable; retain the old D-Bus object as ended. */
        mm_base_call_set_index (call, 0);
        call = NULL;
    }
    if (!call && info->direction == MM_CALL_DIRECTION_OUTGOING) {
        FindCall ctx = { slot, NULL };
        mm_call_list_foreach (list, find_outgoing, &ctx);
        call = ctx.call;
    }
    if (!call) {
        if (info->state == MM_CALL_STATE_TERMINATED || info->direction == MM_CALL_DIRECTION_UNKNOWN)
            return;
        call = mm_call_mtk_ccci_new (MM_BASE_MODEM (modem), port, slot, info->direction, info->number, 0);
        created = TRUE;
    }
    mm_base_call_set_index (call, info->index);
    if (info->number)
        mm_base_call_set_number (call, info->number);
    mm_base_call_change_state (call, info->state,
        created && info->direction == MM_CALL_DIRECTION_INCOMING ? MM_CALL_STATE_REASON_INCOMING_NEW : MM_CALL_STATE_REASON_UNKNOWN);
    if (created) {
        if (info->direction == MM_CALL_DIRECTION_INCOMING)
            mm_base_call_incoming_refresh (call);
        mm_base_call_export (call);
        mm_call_list_add_call (list, call);
        g_object_unref (call);
    }
}
