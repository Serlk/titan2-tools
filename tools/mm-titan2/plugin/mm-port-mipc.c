/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMPortMipc: MIPC control port with independent request and modem-response queues.
 *
 * Written from a clean-room behavioural description of the protocol; no code of the phone's Android software was used.
 * A modem command may arrive while a request awaits its confirm; its response
 * must be written without completing or waiting for that request.
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <glib-unix.h>
#include <ModemManager.h>
#include <mm-errors-types.h>
#include "mm-port-mipc.h"
#include "mm-log-object.h"
#include "mm-error-helpers.h"

#define SESSION_TEST_TRIES 10
#define SESSION_TEST_TIMEOUT 1
#define SESSION_OPEN_TIMEOUT 5
#define SESSION_REG_TIMEOUT 5
#define MAX_PENDING 64

G_DEFINE_TYPE (MMPortMipc, mm_port_mipc, MM_TYPE_PORT)
enum { SIGNAL_INDICATION, SIGNAL_FORCED_CLOSE, SIGNAL_LAST };
static guint signals[SIGNAL_LAST];

typedef struct {
    GTask *task;
    guint16 id, txid;
    guint8 slot;
    guint timeout, timeout_seconds;
} Pending;

typedef struct {
    GByteArray *frame;
    gsize offset;
    guint16 txid;
    gboolean response;
} Output;

struct _MMPortMipcPrivate {
    gint fd;
    guint opens, read_id, write_id, send_timeout;
    guint16 next_txid;
    gboolean session_ready;
    guint modem_timeout_ms, consecutive_timeouts;
    GByteArray *rx;
    GHashTable *pending;
    GQueue output;
    /* callers waiting for the session start in flight (NULL: none in flight);
     * two modem objects (one per SIM, DESIGN.md §11) share this port and may
     * both ask for the session while it starts */
    GPtrArray *session_waiters;
};

static void pending_disarm (Pending *p)
{
    if (p->timeout) {
        g_source_remove (p->timeout);
        p->timeout = 0;
    }
}

static void pending_free (Pending *p)
{
    pending_disarm (p);
    g_object_unref (p->task);
    g_free (p);
}

static void output_free (Output *o)
{
    g_byte_array_unref (o->frame);
    g_free (o);
}

void mm_mipc_cnf_free (MMMipcCnf *cnf)
{
    if (cnf) {
        g_byte_array_unref (cnf->payload);
        g_slice_free (MMMipcCnf, cnf);
    }
}

gboolean mm_port_mipc_is_open (MMPortMipc *self)
{
    return self->priv->fd >= 0;
}

gboolean mm_port_mipc_get_session_ready (MMPortMipc *self)
{
    return self->priv->session_ready && mm_port_mipc_is_open (self);
}

guint mm_port_mipc_get_modem_timeout_ms (MMPortMipc *self)
{
    return self->priv->modem_timeout_ms;
}

static void port_close (MMPortMipc *self, gboolean forced)
{
    GHashTableIter iter;
    gpointer value;
    MMPortMipcPrivate *p = self->priv;
    g_autoptr(GPtrArray) pending_tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) pending_free);
    guint i;

    if (p->read_id) { g_source_remove (p->read_id); p->read_id = 0; }
    if (p->write_id) { g_source_remove (p->write_id); p->write_id = 0; }
    if (p->send_timeout) { g_source_remove (p->send_timeout); p->send_timeout = 0; }
    if (p->fd >= 0) { close (p->fd); p->fd = -1; }
    p->opens = 0;
    p->session_ready = FALSE;
    p->modem_timeout_ms = 0;
    g_byte_array_set_size (p->rx, 0);
    while (!g_queue_is_empty (&p->output))
        output_free (g_queue_pop_head (&p->output));
    g_hash_table_iter_init (&iter, p->pending);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        Pending *pending = value;
        pending_disarm (pending);
        g_ptr_array_add (pending_tasks, pending);
        g_hash_table_iter_steal (&iter);
    }
    /* GTask callbacks may run immediately and reopen/close the port. Detach
     * every old request before completing any of them. */
    for (i = 0; i < pending_tasks->len; i++) {
        Pending *pending = g_ptr_array_index (pending_tasks, i);
        g_task_return_new_error (pending->task, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED,
                                 "MIPC port closed");
    }
    if (forced)
        g_signal_emit (self, signals[SIGNAL_FORCED_CLOSE], 0);
}

void mm_port_mipc_close (MMPortMipc *self)
{
    if (self->priv->opens && --self->priv->opens == 0)
        port_close (self, FALSE);
}

static gboolean request_timeout (gpointer user_data)
{
    Pending *pending = user_data;
    g_autoptr(MMPortMipc) self = g_object_ref (g_task_get_source_object (pending->task));
    guint16 txid = pending->txid;

    pending->timeout = 0;
    g_hash_table_steal (self->priv->pending, GUINT_TO_POINTER ((guint) txid + 1));
    g_task_return_new_error (pending->task, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT,
                             "MIPC request 0x%04x txid %u timed out", pending->id, txid);
    pending_free (pending);
    g_signal_emit_by_name (self, MM_PORT_SIGNAL_TIMED_OUT, ++self->priv->consecutive_timeouts);
    return G_SOURCE_REMOVE;
}

static gboolean send_timeout (gpointer user_data)
{
    g_autoptr(MMPortMipc) self = g_object_ref (user_data);
    self->priv->send_timeout = 0;
    mm_obj_warn (self, "MIPC transmit queue stalled");
    port_close (self, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean write_ready (gint fd, GIOCondition cond, gpointer user_data)
{
    g_autoptr(MMPortMipc) self = g_object_ref (user_data);
    MMPortMipcPrivate *p = self->priv;

    if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        p->write_id = 0;
        port_close (self, TRUE);
        return G_SOURCE_REMOVE;
    }
    while (!g_queue_is_empty (&p->output)) {
        Output *o = g_queue_peek_head (&p->output);
        gssize n = write (fd, o->frame->data + o->offset, o->frame->len - o->offset);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN))
            return G_SOURCE_CONTINUE;
        if (n <= 0) {
            p->write_id = 0;
            port_close (self, TRUE);
            return G_SOURCE_REMOVE;
        }
        o->offset += n;
        if (o->offset != o->frame->len)
            return G_SOURCE_CONTINUE;
        if (!o->response) {
            Pending *pending = g_hash_table_lookup (p->pending, GUINT_TO_POINTER ((guint) o->txid + 1));
            if (pending)
                pending->timeout = g_timeout_add_seconds (pending->timeout_seconds,
                                                          request_timeout, pending);
        }
        output_free (g_queue_pop_head (&p->output));
    }
    p->write_id = 0;
    if (p->send_timeout) { g_source_remove (p->send_timeout); p->send_timeout = 0; }
    return G_SOURCE_REMOVE;
}

static void queue_output (MMPortMipc *self, GByteArray *frame, guint16 txid, gboolean response)
{
    Output *o = g_new0 (Output, 1);
    o->frame = g_byte_array_ref (frame);
    o->txid = txid;
    o->response = response;
    /* Never insert in the middle of a partially written frame. */
    if (response && !g_queue_is_empty (&self->priv->output))
        g_queue_insert_after (&self->priv->output, self->priv->output.head, o);
    else
        g_queue_push_tail (&self->priv->output, o);
    if (!self->priv->write_id)
        self->priv->write_id = g_unix_fd_add (self->priv->fd, G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                             write_ready, self);
    if (!self->priv->send_timeout)
        self->priv->send_timeout = g_timeout_add_seconds (5, send_timeout, self);
}

static void dispatch (MMPortMipc *self, const MMMipcMsg *msg)
{
    Pending *pending;
    MMMipcCnf *cnf;

    mm_obj_dbg (self, "<-- MIPC id 0x%04x slot 0x%02x txid %u payload %" G_GSIZE_FORMAT,
                msg->msg_id, msg->slot, msg->txid, msg->len);
    if (MM_MIPC_CLASS (msg->msg_id) != MM_MIPC_CLASS_REQCNF) {
        g_autoptr(GByteArray) payload = g_byte_array_new ();
        g_byte_array_append (payload, msg->payload, msg->len);
        g_signal_emit (self, signals[SIGNAL_INDICATION], 0,
                       (guint) msg->msg_id, (guint) msg->slot, (guint) msg->txid, payload);
        return;
    }
    pending = g_hash_table_lookup (self->priv->pending, GUINT_TO_POINTER ((guint) msg->txid + 1));
    if (!pending || msg->msg_id != MM_MIPC_CNF_OF (pending->id) ||
        (pending->slot != MM_MIPC_SLOT_ANY && pending->slot != msg->slot)) {
        mm_obj_dbg (self, "unmatched MIPC confirm ignored");
        return;
    }
    self->priv->consecutive_timeouts = 0;
    cnf = g_slice_new0 (MMMipcCnf);
    cnf->msg_id = msg->msg_id;
    cnf->txid = msg->txid;
    cnf->slot = msg->slot;
    cnf->payload = g_byte_array_new ();
    g_byte_array_append (cnf->payload, msg->payload, msg->len);
    cnf->result = mm_mipc_tlv_result (msg->payload, msg->len);
    /* Completion may synchronously close the port or queue another request. */
    g_hash_table_steal (self->priv->pending, GUINT_TO_POINTER ((guint) msg->txid + 1));
    pending_disarm (pending);
    g_task_return_pointer (pending->task, cnf, (GDestroyNotify) mm_mipc_cnf_free);
    pending_free (pending);
}

static gboolean read_ready (gint fd, GIOCondition cond, gpointer user_data)
{
    g_autoptr(MMPortMipc) self = g_object_ref (user_data);
    MMPortMipcPrivate *p = self->priv;
    guint8 buf[4096];
    gssize n;

    do { n = read (fd, buf, sizeof (buf)); } while (n < 0 && errno == EINTR);
    if (n > 0) {
        g_byte_array_append (p->rx, buf, n);
        while (p->rx->len) {
            MMMipcMsg msg;
            gsize used = 0;
            gint result = mm_mipc_frame_parse (p->rx->data, p->rx->len, &msg, &used);
            g_autoptr(GByteArray) payload = NULL;

            if (result == 0)
                break;
            if (result < 0) {
                mm_obj_warn (self, "malformed MIPC stream dropped");
                g_byte_array_set_size (p->rx, 0);
                break;
            }
            /* Handlers may close the port, so remove the frame before emitting. */
            payload = g_byte_array_new ();
            g_byte_array_append (payload, msg.payload, msg.len);
            msg.payload = payload->data;
            g_byte_array_remove_range (p->rx, 0, used);
            dispatch (self, &msg);
            if (p->fd < 0)
                return G_SOURCE_REMOVE;
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN) ||
        (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))) {
        p->read_id = 0;
        port_close (self, TRUE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean mm_port_mipc_open (MMPortMipc *self, GError **error)
{
    g_autofree gchar *path = NULL;
    struct stat st;
    gint fd;

    if (self->priv->opens) { self->priv->opens++; return TRUE; }
    path = g_build_filename ("/dev", mm_port_get_device (MM_PORT (self)), NULL);
    fd = open (path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "open %s: %s", path, g_strerror (errno));
        return FALSE;
    }
    if (fstat (fd, &st) || !S_ISCHR (st.st_mode)) {
        close (fd);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "MIPC port is not a character device");
        return FALSE;
    }
    self->priv->fd = fd;
    self->priv->opens = 1;
    self->priv->read_id = g_unix_fd_add (fd, G_IO_IN | G_IO_ERR | G_IO_HUP, read_ready, self);
    return TRUE;
}

MMMipcCnf *mm_port_mipc_request_finish (MMPortMipc *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

void mm_port_mipc_request (MMPortMipc *self, guint16 msg_id, guint8 slot, GByteArray *tlvs,
                           guint timeout_seconds, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    Pending *pending;
    guint16 txid;
    g_autoptr(GByteArray) frame = NULL;

    if (!mm_mipc_request_is_valid (msg_id)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS, "Invalid MIPC request 0x%04x", msg_id);
        g_object_unref (task);
        return;
    }
    if (!mm_port_mipc_is_open (self) || g_hash_table_size (self->priv->pending) >= MAX_PENDING) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "MIPC port closed or request queue full");
        g_object_unref (task);
        return;
    }
    if (g_task_return_error_if_cancelled (task)) { g_object_unref (task); return; }
    do {
        txid = self->priv->next_txid++;
    } while (txid == MM_MIPC_TXID_NONE || g_hash_table_contains (self->priv->pending, GUINT_TO_POINTER ((guint) txid + 1)));
    frame = mm_mipc_frame_build (msg_id, txid, slot, tlvs ? tlvs->data : NULL, tlvs ? tlvs->len : 0);
    if (!frame) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_TOO_MANY, "MIPC payload too long");
        g_object_unref (task);
        return;
    }
    pending = g_new0 (Pending, 1);
    pending->task = task;
    pending->id = msg_id;
    pending->txid = txid;
    pending->slot = slot;
    pending->timeout_seconds = MAX (timeout_seconds, 1);
    g_hash_table_insert (self->priv->pending, GUINT_TO_POINTER ((guint) txid + 1), pending);
    mm_obj_dbg (self, "--> MIPC id 0x%04x slot 0x%02x txid %u payload %u", msg_id, slot, txid, tlvs ? tlvs->len : 0);
    queue_output (self, frame, txid, FALSE);
}

gboolean mm_port_mipc_send_response (MMPortMipc *self, guint16 id, guint16 txid, guint8 slot,
                                      GByteArray *tlvs, GError **error)
{
    g_autoptr(GByteArray) frame = NULL;
    if (!mm_port_mipc_is_open (self) || (id != MM_MIPC_INTERNAL_EIPSPI_RSP && id != MM_MIPC_CALL_APPROVE_RSP) ||
        txid == MM_MIPC_TXID_NONE || g_queue_get_length (&self->priv->output) >= MAX_PENDING) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "Cannot queue MIPC response");
        return FALSE;
    }
    frame = mm_mipc_frame_build (id, txid, slot, tlvs ? tlvs->data : NULL, tlvs ? tlvs->len : 0);
    if (!frame) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_TOO_MANY, "MIPC response too long");
        return FALSE;
    }
    queue_output (self, frame, txid, TRUE);
    return TRUE;
}

typedef enum {
    SESSION_STEP_FIRST,
    SESSION_STEP_TEST,
    SESSION_STEP_OPEN,
    SESSION_STEP_REGISTER,
    SESSION_STEP_LAST,
} SessionStep;

typedef struct {
    SessionStep  step;
    guint8       slot;
    guint16     *indications;
    guint8      *registration_slots;
    guint        n_indications;
    guint        reg_index;
    guint        test_tries;
    gchar       *client_name;
} SessionContext;

static void
session_context_free (SessionContext *ctx)
{
    g_free (ctx->indications);
    g_free (ctx->registration_slots);
    g_free (ctx->client_name);
    g_slice_free (SessionContext, ctx);
}

gboolean
mm_port_mipc_open_session_finish (MMPortMipc    *self,
                                  GAsyncResult  *res,
                                  GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void session_step (GTask *task);

static void
session_test_ready (MMPortMipc   *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    SessionContext       *ctx;
    g_autoptr(MMMipcCnf)  cnf = NULL;
    GError               *error = NULL;

    ctx = g_task_get_task_data (task);
    cnf = mm_port_mipc_request_finish (self, res, &error);
    if (!cnf) {
        if (g_error_matches (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT) &&
            ctx->test_tries < SESSION_TEST_TRIES) {
            /* the first message after modem boot may be lost */
            mm_obj_dbg (self, "INTERNAL_TEST not answered (try %u), retrying", ctx->test_tries);
            g_clear_error (&error);
            session_step (task);
            return;
        }
        g_prefix_error (&error, "MIPC session: test failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_obj_dbg (self, "INTERNAL_TEST answered after %u tries", ctx->test_tries);
    ctx->step++;
    session_step (task);
}

static void
session_open_ready (MMPortMipc   *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    SessionContext       *ctx;
    g_autoptr(MMMipcCnf)  cnf = NULL;
    GError               *error = NULL;
    guint32               v;

    ctx = g_task_get_task_data (task);
    cnf = mm_port_mipc_request_finish (self, res, &error);
    if (!cnf) {
        g_prefix_error (&error, "MIPC session: open failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (mm_mipc_tlv_find_uint (cnf->payload->data, cnf->payload->len, MM_MIPC_T_OPEN_CNF_TIMEOUT, &v) && v)
        self->priv->modem_timeout_ms = v;
    if (mm_mipc_tlv_find_uint (cnf->payload->data, cnf->payload->len, MM_MIPC_T_OPEN_VERSION, &v))
        mm_obj_msg (self, "MIPC session open: result %u, target version %u, default timeout %u ms",
                    cnf->result, v, self->priv->modem_timeout_ms);
    else
        mm_obj_msg (self, "MIPC session open: result %u, default timeout %u ms",
                    cnf->result, self->priv->modem_timeout_ms);
    if (cnf->result) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "MIPC OPEN refused: result %u", cnf->result);
        g_object_unref (task);
        return;
    }
    ctx->step++;
    session_step (task);
}

static void
session_register_ready (MMPortMipc   *self,
                        GAsyncResult *res,
                        GTask        *task)
{
    SessionContext       *ctx;
    g_autoptr(MMMipcCnf)  cnf = NULL;
    g_autoptr(GError)     error = NULL;

    ctx = g_task_get_task_data (task);
    cnf = mm_port_mipc_request_finish (self, res, &error);
    if (!cnf || cnf->result) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "MIPC registration 0x%04x failed: %s (result %u)",
                                 ctx->indications[ctx->reg_index],
                                 error ? error->message : "refused", cnf ? cnf->result : 0);
        g_object_unref (task);
        return;
    }
    ctx->reg_index++;
    session_step (task);
}

static void
session_step (GTask *task)
{
    MMPortMipc     *self;
    SessionContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    switch (ctx->step) {
    case SESSION_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case SESSION_STEP_TEST:
        ctx->test_tries++;
        mm_port_mipc_request (self, MM_MIPC_INTERNAL_TEST_REQ, MM_MIPC_SLOT_ANY, NULL,
                              SESSION_TEST_TIMEOUT, g_task_get_cancellable (task),
                              (GAsyncReadyCallback) session_test_ready, task);
        return;

    case SESSION_STEP_OPEN: {
        g_autoptr(GByteArray) tlvs = g_byte_array_new ();

        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_OPEN_VERSION, MM_MIPC_OPEN_VERSION);
        mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_OPEN_CLIENT_NAME, ctx->client_name);
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_OPEN_FLAG, 1);
        mm_port_mipc_request (self, MM_MIPC_INTERNAL_OPEN_REQ, MM_MIPC_SLOT_ANY, tlvs,
                              SESSION_OPEN_TIMEOUT, g_task_get_cancellable (task),
                              (GAsyncReadyCallback) session_open_ready, task);
        return;
    }

    case SESSION_STEP_REGISTER:
        if (ctx->reg_index < ctx->n_indications) {
            g_autoptr(GByteArray) tlvs = g_byte_array_new ();

            mm_mipc_tlv_add_u16 (tlvs, MM_MIPC_T_REGISTER_IND_ID, ctx->indications[ctx->reg_index]);
            mm_port_mipc_request (self,
                                  MM_MIPC_IS_CMD (ctx->indications[ctx->reg_index]) ?
                                  MM_MIPC_INTERNAL_REGISTER_CMD_REQ : MM_MIPC_INTERNAL_REGISTER_IND_REQ,
                                  ctx->registration_slots[ctx->reg_index], tlvs,
                                  SESSION_REG_TIMEOUT, g_task_get_cancellable (task),
                                  (GAsyncReadyCallback) session_register_ready, task);
            return;
        }
        ctx->step++;
        /* fall through */

    case SESSION_STEP_LAST:
        self->priv->session_ready = TRUE;
        mm_obj_msg (self, "MIPC session ready (slot 0x%02x)", ctx->slot);
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

static void
session_internal_ready (MMPortMipc   *self,
                        GAsyncResult *res,
                        gpointer      user_data)
{
    g_autoptr(GPtrArray) waiters = NULL;
    g_autoptr(GError)    error = NULL;
    guint                i;

    g_task_propagate_boolean (G_TASK (res), &error);
    /* a completion callback may start the next session: detach first */
    waiters = g_steal_pointer (&self->priv->session_waiters);
    for (i = 0; waiters && i < waiters->len; i++) {
        GTask *task = g_ptr_array_index (waiters, i);

        if (error)
            g_task_return_error (task, g_error_copy (error));
        else
            g_task_return_boolean (task, TRUE);
        g_object_unref (task);
    }
}

void
mm_port_mipc_open_session (MMPortMipc          *self,
                           guint8               slot,
                           const guint16       *indications,
                           guint                n_indications,
                           const gchar         *client_name,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    GTask          *task;
    GTask          *internal;
    SessionContext *ctx;

    g_return_if_fail (MM_IS_PORT_MIPC (self));

    task = g_task_new (self, cancellable, callback, user_data);

    if (!mm_port_mipc_is_open (self)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "MIPC port is not open");
        g_object_unref (task);
        return;
    }

    /* One session start at a time on the shared port: a second caller
     * joins the one in flight and gets its result (the indication list and
     * slot mask are the plugin's constants, the same for every caller). */
    if (self->priv->session_waiters) {
        mm_obj_dbg (self, "MIPC session start in progress: joining it");
        g_ptr_array_add (self->priv->session_waiters, task);
        return;
    }
    self->priv->session_waiters = g_ptr_array_new ();
    g_ptr_array_add (self->priv->session_waiters, task);

    internal = g_task_new (self, cancellable, (GAsyncReadyCallback) session_internal_ready, NULL);
    ctx = g_slice_new0 (SessionContext);
    ctx->step          = SESSION_STEP_FIRST;
    ctx->slot          = slot;
    {
        guint i, bit;
        ctx->indications = g_new0 (guint16, n_indications * 8 + 1);
        ctx->registration_slots = g_new0 (guint8, n_indications * 8 + 1);
        for (i = 0; indications && i < n_indications; i++) {
            if (MM_MIPC_IS_CMD (indications[i])) {
                ctx->indications[ctx->n_indications] = indications[i];
                ctx->registration_slots[ctx->n_indications++] = MM_MIPC_SLOT_ANY;
            } else {
                for (bit = 1; bit <= 0x80; bit <<= 1) {
                    if (!(slot & bit))
                        continue;
                    ctx->indications[ctx->n_indications] = indications[i];
                    ctx->registration_slots[ctx->n_indications++] = bit;
                }
            }
        }
    }
    ctx->client_name   = g_strdup (client_name ? client_name : "ModemManager");
    g_task_set_task_data (internal, ctx, (GDestroyNotify) session_context_free);

    self->priv->session_ready = FALSE;
    session_step (internal);
}

gboolean
mm_port_mipc_session_starting (MMPortMipc *self)
{
    return self->priv->session_waiters != NULL;
}

static void removed_cb (MMPort *port, MMPortMipc *self)
{
    port_close (self, TRUE);
}

MMPortMipc *mm_port_mipc_new (const gchar *name)
{
    return g_object_new (MM_TYPE_PORT_MIPC, MM_PORT_DEVICE, name,
                         MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                         MM_PORT_GROUP, MM_PORT_GROUP_USED, MM_PORT_TYPE, MM_PORT_TYPE_UNKNOWN, NULL);
}

static void mm_port_mipc_init (MMPortMipc *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_PORT_MIPC, MMPortMipcPrivate);
    self->priv->fd = -1;
    self->priv->rx = g_byte_array_new ();
    self->priv->pending = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) pending_free);
    g_signal_connect (self, MM_PORT_SIGNAL_REMOVED, G_CALLBACK (removed_cb), self);
}

static void dispose (GObject *object)
{
    port_close (MM_PORT_MIPC (object), FALSE);
    G_OBJECT_CLASS (mm_port_mipc_parent_class)->dispose (object);
}

static void finalize (GObject *object)
{
    MMPortMipc *self = MM_PORT_MIPC (object);
    g_byte_array_unref (self->priv->rx);
    g_hash_table_unref (self->priv->pending);
    G_OBJECT_CLASS (mm_port_mipc_parent_class)->finalize (object);
}

static void mm_port_mipc_class_init (MMPortMipcClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    g_type_class_add_private (klass, sizeof (MMPortMipcPrivate));
    object_class->dispose = dispose;
    object_class->finalize = finalize;
    signals[SIGNAL_INDICATION] = g_signal_new (MM_PORT_MIPC_SIGNAL_INDICATION,
        G_OBJECT_CLASS_TYPE (klass), G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (MMPortMipcClass, indication), NULL, NULL, NULL,
        G_TYPE_NONE, 4, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BYTE_ARRAY);
    signals[SIGNAL_FORCED_CLOSE] = g_signal_new ("forced-close", G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
at_command_ready (MMPortMipc *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (self, res, &error);
    gchar *text = cnf ? mm_mipc_tlv_find_at_string (cnf->payload->data, cnf->payload->len) : NULL;
    if (text && !error) {
        g_auto(GStrv) lines = g_strsplit_set (text, "\r\n", -1);
        guint i;
        for (i = 0; lines[i]; i++) {
            gchar *line = g_strstrip (lines[i]);
            gchar *end;
            guint64 code;
            if (!g_str_has_prefix (line, "+CME ERROR:"))
                continue;
            line += strlen ("+CME ERROR:");
            while (g_ascii_isspace (*line))
                line++;
            code = g_ascii_strtoull (line, &end, 10);
            if (end != line && !*end && code <= G_MAXINT)
                error = mm_mobile_equipment_error_for_code ((MMMobileEquipmentError) code, self);
            break;
        }
    }
    if (error || !cnf || cnf->result || !text || strstr (text, "ERROR")) {
        if (!error)
            error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "SYS_AT failed: result %u%s%s",
                                 cnf ? cnf->result : 0, text ? ": " : "", text ? text : "");
        g_free (text);
        g_task_return_error (task, error);
    } else
        g_task_return_pointer (task, text, g_free);
    g_object_unref (task);
}

void
mm_port_mipc_at_command (MMPortMipc *self, guint8 slot, const gchar *command, guint timeout,
                          GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_autoptr(GByteArray) tlvs = g_byte_array_new ();
    g_task_set_source_tag (task, mm_port_mipc_at_command);
    if ((slot != MM_MIPC_SLOT_SIM1 && slot != MM_MIPC_SLOT_SIM2) ||
        !command || !g_str_has_prefix (command, "AT") || strpbrk (command, "\r\n") || strlen (command) > 2048) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS, "Invalid slot-addressed AT command");
        g_object_unref (task);
        return;
    }
    mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, command);
    mm_port_mipc_request (self, MM_MIPC_SYS_AT_REQ, slot, tlvs, timeout, NULL,
                          (GAsyncReadyCallback) at_command_ready, task);
}

gchar *
mm_port_mipc_at_command_finish (MMPortMipc *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}
