/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMMtkCcciShared: the shared MIPC session, IMS controllers, radio power,
 * speech path and incoming-call approval of the MT6878 modem (see the
 * header). Written from tools/modemd, a clean-room behavioural description of the protocol (no code of the phone's Android software was used)
 * and the ModemManager tree.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

#include <config.h>
#include <string.h>

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-log-object.h"
#include "mm-mtk-ccci-shared.h"
#include "mm-modem-helpers-mtk-ccci.h"

#define RADIO_COMMAND_TIMEOUT 15
#define RADIO_BUSY_TRIES      30
#define RADIO_PORT_POLL_MS    500

struct _MMMtkCcciShared {
    GObject       parent;
    gchar        *name;        /* the MIPC port, the registry key */
    gchar        *wwan_device;
    MMPortMipc   *mipc;
    MMMtkCcciIms *ims[2];
    gboolean      dual;

    /* radio */
    guint           configured;
    guint8          low_power;
    guint8          on;
    guint8          known;
    MMPortSerialAt *radio_port;   /* weak */
    guint           radio_port_wait;
    GQueue          radio_queue;  /* GTask, head = running */
    gboolean        warned_tunnel;

    /* speech */
    guint8          in_call;

    /* LTE start-up contract */
    gboolean           lte_configured;
    MMMtkCcciLteConfig lte;
    guint8             lte_pre_done;     /* slots whose pre-radio steps ran (this session) */
    gboolean           lte_global_done;  /* multi-SIM config and DSBP sent (this session) */
    guint8             lte_checked;      /* slots whose read-only checks ran */
    guint8             lte_cause_ok;     /* slots offering <n> = 3 (AT+CEREG=?) */
    guint8             lte_cause_done;   /* slots with step 6 done */
    guint8             lte_allow_sent;   /* data-allow sent (this session) */
    guint              lte_allow_timer;
    guint              lte_allow_repeat;
    guint8             lte_diag_registered; /* slots with the diagnostics indications registered */
    guint              lte_poll_timer[2];
    guint              lte_poll_count[2];

    /* incoming-call approval */
    gboolean        dsds_busy;
    struct {
        GObject                       *client;  /* weak */
        MMMtkCcciSharedCallsPresentFn  calls_present;
        MMMtkCcciSharedCallRejectedFn  rejected;
    } voice[2];
    struct {
        gboolean valid;
        guint    call_id, seq;
        gboolean approve;
        gint64   when;
    } last[2];
};

static void log_object_iface_init (MMLogObjectInterface *iface);

G_DEFINE_TYPE_EXTENDED (MMMtkCcciShared, mm_mtk_ccci_shared, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_LOG_OBJECT, log_object_iface_init))

static GHashTable *registry; /* name -> MMMtkCcciShared, not referenced */

static gchar *
log_object_build_id (MMLogObject *_self)
{
    return g_strdup_printf ("mtk-ccci-shared/%s", MM_MTK_CCCI_SHARED (_self)->name);
}

static void
log_object_iface_init (MMLogObjectInterface *iface)
{
    iface->build_id = log_object_build_id;
}

/*****************************************************************************/
/* Incoming-call approval (CALL_APPROVE_INCOMING_CMD 0x8901 -> RSP 0x8902,
 * = +EAIC / AT+EAIC=<mode>,<id>,<seq>[,<cause>] of the AT port): answered here once per command, whichever modem objects exist, so
 * that a call to a SIM without a modem object (yet) is not left waiting.
 * Policy (mm_mtk_ccci_shared_incoming_decide): allow ringing, except the
 * DSDS case (the other SIM has a call): reject with cause 21. */

#define APPROVE_REPEAT_US      (30 * G_USEC_PER_SEC)
#define APPROVE_REJECT_CAUSE   21  /* 3GPP 24.008 "call rejected", the RIL's value */

static void
voice_client_clear (MMMtkCcciShared *self, guint i)
{
    if (self->voice[i].client)
        g_object_remove_weak_pointer (self->voice[i].client, (gpointer *) &self->voice[i].client);
    self->voice[i].client = NULL;
    self->voice[i].calls_present = NULL;
    self->voice[i].rejected = NULL;
}

void
mm_mtk_ccci_shared_set_voice_client (MMMtkCcciShared               *self,
                                     guint8                         slots,
                                     GObject                       *client,
                                     MMMtkCcciSharedCallsPresentFn  calls_present,
                                     MMMtkCcciSharedCallRejectedFn  rejected)
{
    guint i;

    for (i = 0; i < 2; i++) {
        if (!(slots & (1u << i))) {
            /* a slot this client no longer serves */
            if (self->voice[i].client == client)
                voice_client_clear (self, i);
            continue;
        }
        if (self->voice[i].client != client) {
            voice_client_clear (self, i);
            self->voice[i].client = client;
            g_object_add_weak_pointer (client, (gpointer *) &self->voice[i].client);
        }
        self->voice[i].calls_present = calls_present;
        self->voice[i].rejected = rejected;
    }
}

void
mm_mtk_ccci_shared_unset_voice_client (MMMtkCcciShared *self, GObject *client)
{
    mm_mtk_ccci_shared_set_voice_client (self, 0, client, NULL, NULL);
}

void
mm_mtk_ccci_shared_set_dsds_busy (MMMtkCcciShared *self, gboolean enabled)
{
    self->dsds_busy = enabled;
}

gboolean
mm_mtk_ccci_shared_get_dsds_busy (MMMtkCcciShared *self)
{
    return self->dsds_busy;
}

gboolean
mm_mtk_ccci_shared_incoming_decide (MMMtkCcciShared *self,
                                    guint8           slot,
                                    guint            call_id,
                                    guint            seq,
                                    const gchar     *number)
{
    guint8   other;
    guint    n = 0;
    gboolean approve = TRUE;
    gint64   now = g_get_monotonic_time ();

    g_return_val_if_fail (slot == 1 || slot == 2, TRUE);
    other = slot == 1 ? 2 : 1;

    /* the same request again (the other channel, a repeat): same answer */
    if (self->last[slot - 1].valid &&
        self->last[slot - 1].call_id == call_id &&
        self->last[slot - 1].seq == seq &&
        now - self->last[slot - 1].when < APPROVE_REPEAT_US) {
        mm_obj_dbg (self, "SIM%u incoming call %u (seq %u) already answered: %s",
                    slot, call_id, seq, self->last[slot - 1].approve ? "approved" : "rejected");
        return self->last[slot - 1].approve;
    }

    if (self->dsds_busy && self->voice[other - 1].client && self->voice[other - 1].calls_present)
        n = self->voice[other - 1].calls_present (self->voice[other - 1].client, other);
    approve = (n == 0);

    self->last[slot - 1].valid = TRUE;
    self->last[slot - 1].call_id = call_id;
    self->last[slot - 1].seq = seq;
    self->last[slot - 1].approve = approve;
    self->last[slot - 1].when = now;

    if (approve) {
        mm_obj_msg (self, "SIM%u incoming call %u: approving", slot, call_id);
        return TRUE;
    }
    mm_obj_msg (self, "SIM%u incoming call %u rejected: SIM%u has a call (DSDS)", slot, call_id, other);
    if (self->voice[slot - 1].client && self->voice[slot - 1].rejected)
        self->voice[slot - 1].rejected (self->voice[slot - 1].client, slot, call_id, number);
    else
        mm_obj_msg (self, "SIM%u has no modem object: the rejected call is not shown", slot);
    return FALSE;
}

/* the network indications of the diagnostics */
static const guint16 lte_diag_indications[] = {
    MM_MIPC_NW_PS_IND, MM_MIPC_NW_CS_IND, MM_MIPC_NW_CSCON_IND, MM_MIPC_NW_BARRING_INFO_IND,
    MM_MIPC_NW_IA_IND, MM_MIPC_NW_RADIO_IND, MM_MIPC_NW_PHYSICAL_CHANNEL_CONFIGS_IND,
};

static void lte_diag_indication (MMMtkCcciShared *self, guint id, guint8 slot, GByteArray *payload);

static void
mipc_indication (MMPortMipc *port, guint id, guint routing, guint txid,
                 GByteArray *payload, MMMtkCcciShared *self)
{
    g_autoptr(GByteArray) reply = NULL;
    g_autoptr(GError)     error = NULL;
    g_autofree gchar     *number = NULL;
    guint8                slot = routing & 3;
    guint32               call_id = 0, seq = 0;
    gboolean              approve;

    if ((slot == 1 || slot == 2) && self->lte_configured && self->lte.nw_diag) {
        guint i;

        for (i = 0; i < G_N_ELEMENTS (lte_diag_indications); i++)
            if (id == lte_diag_indications[i]) {
                lte_diag_indication (self, id, slot, payload);
                return;
            }
    }
    if (id != MM_MIPC_CALL_APPROVE_CMD || (slot != 1 && slot != 2))
        return;
    if (!mm_mipc_tlv_find_uint (payload->data, payload->len, 0x0100, &call_id) ||
        !mm_mipc_tlv_find_uint (payload->data, payload->len, 0x0103, &seq) ||
        !mm_mtk_ccci_call_index (slot, call_id))
        return;
    number = mm_mipc_tlv_find_str (payload->data, payload->len, 0x8101);
    approve = mm_mtk_ccci_shared_incoming_decide (self, slot, call_id, seq, number);
    reply = g_byte_array_new ();
    /* 1: allow ringing (not auto-answer); 0: reject, the modem releases it */
    mm_mipc_tlv_add_u8 (reply, 0x0100, approve ? 1 : 0);
    mm_mipc_tlv_add_u32 (reply, 0x0102, call_id);
    mm_mipc_tlv_add_u32 (reply, 0x0103, seq);
    if (!approve)
        mm_mipc_tlv_add_u32 (reply, 0x0101, APPROVE_REJECT_CAUSE);
    if (!mm_port_mipc_send_response (port, MM_MIPC_CALL_APPROVE_RSP, txid, routing, reply, &error))
        mm_obj_warn (self, "SIM%u incoming-call %s: %s", slot, approve ? "approval" : "rejection", error->message);
}

static void
mipc_lost (MMPortMipc *port, MMMtkCcciShared *self)
{
    /* the modem restarted or the port went away: radio state unknown, and
     * a new session repeats the start-up contract */
    self->known = 0;
    self->on = 0;
    self->lte_pre_done = self->lte_checked = self->lte_cause_ok = self->lte_cause_done = 0;
    self->lte_allow_sent = 0;
    self->lte_diag_registered = 0;
    self->lte_global_done = FALSE;
}

/*****************************************************************************/
/* Radio commands: the SIM1 AT port, else the SYS_AT tunnel of slot 1 */

typedef struct {
    gchar *command;
    guint  waited;
} RadioCommand;

static void
radio_command_free (RadioCommand *ctx)
{
    g_free (ctx->command);
    g_free (ctx);
}

static void radio_command_try (GTask *task);

static void
radio_serial_ready (MMPortSerialAt *port, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    gchar  *reply = mm_port_serial_at_command_finish (port, res, &error);

    if (!reply)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, reply, g_free);
    g_object_unref (task);
}

static void
radio_tunnel_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    gchar  *reply = mm_port_mipc_at_command_finish (port, res, &error);

    if (!reply)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, reply, g_free);
    g_object_unref (task);
}

static gboolean
radio_command_retry (gpointer user_data)
{
    radio_command_try (user_data);
    return G_SOURCE_REMOVE;
}

static void
radio_command_try (GTask *task)
{
    MMMtkCcciShared  *self = g_task_get_source_object (task);
    RadioCommand     *ctx = g_task_get_task_data (task);
    g_autofree gchar *full = NULL;

    if (self->radio_port && mm_port_serial_is_open (MM_PORT_SERIAL (self->radio_port))) {
        mm_obj_dbg (self, "radio: AT%s on %s", ctx->command,
                    mm_port_get_device (MM_PORT (self->radio_port)));
        mm_port_serial_at_command (self->radio_port, ctx->command, RADIO_COMMAND_TIMEOUT,
                                   FALSE, FALSE, NULL,
                                   (GAsyncReadyCallback) radio_serial_ready, task);
        return;
    }
    if (ctx->waited < self->radio_port_wait * 1000) {
        /* the SIM1 modem object (wwan0at0) is being created: wait for it */
        ctx->waited += RADIO_PORT_POLL_MS;
        g_timeout_add (RADIO_PORT_POLL_MS, radio_command_retry, task);
        return;
    }
    if (!self->mipc || !mm_port_mipc_get_session_ready (self->mipc)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "no port for the radio command AT%s", ctx->command);
        g_object_unref (task);
        return;
    }
    if (!self->warned_tunnel) {
        mm_obj_warn (self, "radio: SIM1 AT port not available, using the SYS_AT tunnel of slot 1 (unverified for radio power)");
        self->warned_tunnel = TRUE;
    }
    full = g_strdup_printf ("AT%s", ctx->command);
    mm_port_mipc_at_command (self->mipc, MM_MIPC_SLOT_SIM1, full, RADIO_COMMAND_TIMEOUT,
                             (GAsyncReadyCallback) radio_tunnel_ready, task);
}

static void
radio_command (MMMtkCcciShared     *self,
               const gchar         *command,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
    GTask        *task = g_task_new (self, NULL, callback, user_data);
    RadioCommand *ctx = g_new0 (RadioCommand, 1);

    ctx->command = g_strdup (command);
    g_task_set_task_data (task, ctx, (GDestroyNotify) radio_command_free);
    radio_command_try (task);
}

static gchar *
radio_command_finish (MMMtkCcciShared *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

/*****************************************************************************/
/* Radio off: AT+CFUN=4, or NW_SET_RADIO_STATE_REQ 0 on each configured slot
 * (ID_MM_MTK_CCCI_RADIO_OFF=mipc). Both
 * complete like radio_command (a reply text). */

static void lte_trace (MMMtkCcciShared *self, guint8 slot, const gchar *fmt, ...) G_GNUC_PRINTF (3, 4);
static void radio_off_mipc_step (GTask *task);

static void
radio_off_mipc_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMMtkCcciShared     *self = g_task_get_source_object (task);
    guint8              *left = g_task_get_task_data (task);
    guint8               slot = *left & 1 ? 1 : 2;
    g_autoptr(GError)    error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    if (!cnf)
        lte_trace (self, slot, "radio off: NW_SET_RADIO_STATE_REQ: no confirm: %s", error->message);
    else {
        g_autofree gchar *tlvs = mm_mipc_tlv_dump (cnf->payload->data, cnf->payload->len, 32);

        lte_trace (self, slot, "radio off: confirm 0x%04x slot 0x%02x txid %u result %u; TLVs %s",
                   cnf->msg_id, cnf->slot, cnf->txid, cnf->result, tlvs);
    }
    *left &= ~slot;
    radio_off_mipc_step (task);
}

static void
radio_off_mipc_step (GTask *task)
{
    MMMtkCcciShared      *self = g_task_get_source_object (task);
    guint8               *left = g_task_get_task_data (task);
    guint8                slot = *left & 1 ? 1 : 2;
    g_autoptr(GByteArray) tlvs = NULL;

    if (!*left || !self->mipc || !mm_port_mipc_get_session_ready (self->mipc)) {
        g_task_return_pointer (task, g_strdup (""), g_free);
        g_object_unref (task);
        return;
    }
    tlvs = mm_mipc_build_radio_state (FALSE);
    lte_trace (self, slot, "radio off: request NW_SET_RADIO_STATE_REQ 0x0403 slot 0x%02x; TLVs 0100/1=00 "
               "(instead of AT+CFUN=4)", slot);
    mm_port_mipc_request (self->mipc, MM_MIPC_NW_SET_RADIO_STATE_REQ, slot, tlvs, RADIO_COMMAND_TIMEOUT, NULL,
                          (GAsyncReadyCallback) radio_off_mipc_ready, task);
}

static void
radio_off_command (MMMtkCcciShared *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask  *task;
    guint8 *left;

    if (!self->lte_configured || !self->lte.radio_off_mipc || !self->mipc ||
        !mm_port_mipc_get_session_ready (self->mipc)) {
        radio_command (self, "+CFUN=4", callback, user_data);
        return;
    }
    task = g_task_new (self, NULL, callback, user_data);
    left = g_new (guint8, 1);
    *left = self->configured & 3;
    g_task_set_task_data (task, left, g_free);
    radio_off_mipc_step (task);
}

/*****************************************************************************/
/* The IMS radio-off step (CFUN=4 before a cold IMS configuration) */

static void
ims_radio_off_ready (MMMtkCcciShared *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    gchar  *reply = radio_command_finish (self, res, &error);

    if (!reply) {
        g_task_return_error (task, error);
    } else {
        self->on = 0;
        self->known = 3;
        g_task_return_pointer (task, reply, g_free);
    }
    g_object_unref (task);
}

static void
ims_radio_hook (MMMtkCcciIms *ims, guint8 slot, GTask *task, gpointer data)
{
    MMMtkCcciShared *self = data;

    if (self->dual) {
        guint8 others = self->configured & ~slot & 3;

        if (others & ~(self->known & ~self->on)) {
            /* CFUN=4 is the radio-off state of the whole modem (observed
             * after start-up; whether it is per stack is unverified): with
             * the other SIM's radio on, or not known to be off, configure
             * this stack with the radio on instead. Whether IMS then comes
             * up without a radio cycle is unverified. */
            mm_obj_msg (self, "SIM%u IMS setup: SIM%u radio %s, radio-off step skipped", slot,
                        others == 1 ? 1 : 2, (self->known & others) ? "on" : "state unknown");
            g_task_return_pointer (task, g_strdup (""), g_free);
            g_object_unref (task);
            return;
        }
        if (!(self->known & self->on & slot)) {
            /* this stack is off (cold start) or unknown: nothing to switch */
            mm_obj_dbg (self, "SIM%u IMS setup: radio not on, radio-off step not needed", slot);
            g_task_return_pointer (task, g_strdup (""), g_free);
            g_object_unref (task);
            return;
        }
    }
    radio_off_command (self, (GAsyncReadyCallback) ims_radio_off_ready, task);
}

/*****************************************************************************/
/* The LTE start-up contract (mm-mtk-ccci-shared.h).
 * Every request, its payload and its confirm (result and all TLVs) is a
 * rat-trace line of the slot. A refusal never stops the start-up; the
 * multi-SIM config's result 1 counts as accepted (documented as success). */

#define LTE_TIMEOUT 10

static void
lte_trace (MMMtkCcciShared *self, guint8 slot, const gchar *fmt, ...)
{
    g_autofree gchar *stamp = mm_mtk_ccci_trace_stamp ();
    g_autofree gchar *what = NULL;
    va_list           ap;

    va_start (ap, fmt);
    what = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    mm_obj_msg (self, "rat-trace SIM%u %s: %s", slot == MM_MIPC_SLOT_SIM2 ? 2 : 1, stamp, what);
}

typedef enum {
    LTE_STEP_CHECK,     /* AT+CEREG=? (read-only) */
    LTE_STEP_MIPC,
    LTE_STEP_AT,
    LTE_STEP_CAUSE,     /* AT+C(G|E)REG=3, only where offered */
    LTE_STEP_REGISTER,  /* INTERNAL_REGISTER_IND_REQ of a diagnostics indication */
} LteStepKind;

typedef struct {
    LteStepKind  kind;
    guint8       slot;
    guint16      id;
    GByteArray *(*build) (void);
    const gchar *at;
    const gchar *what;
    gboolean     accept_one;
    guint16      arg;      /* LTE_STEP_REGISTER: the indication id */
} LteStep;

typedef struct {
    GArray  *steps;
    guint    i;
    gboolean announced;
} LteRun;

static void
lte_run_free (LteRun *run)
{
    g_array_unref (run->steps);
    g_free (run);
}

static void lte_run_step (GTask *task);

static void
lte_add (GArray *steps, LteStepKind kind, guint8 slot, guint16 id, GByteArray *(*build) (void),
         const gchar *at, const gchar *what, gboolean accept_one)
{
    LteStep s = { kind, slot, id, build, at, what, accept_one, 0 };

    g_array_append_val (steps, s);
}

static void
lte_mipc_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMMtkCcciShared  *self = g_task_get_source_object (task);
    LteRun           *run = g_task_get_task_data (task);
    LteStep          *s = &g_array_index (run->steps, LteStep, run->i);
    g_autoptr(GError) error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    if (!cnf)
        lte_trace (self, s->slot, "start-up %s: no confirm: %s", s->what, error->message);
    else {
        g_autofree gchar *tlvs = mm_mipc_tlv_dump (cnf->payload->data, cnf->payload->len, 32);
        gboolean          ok = cnf->msg_id == MM_MIPC_CNF_OF (s->id) && cnf->slot == s->slot &&
                               (!cnf->result || (s->accept_one && cnf->result == 1));

        lte_trace (self, s->slot, "start-up %s: confirm 0x%04x slot 0x%02x txid %u result %u (%s); TLVs %s",
                   s->what, cnf->msg_id, cnf->slot, cnf->txid, cnf->result,
                   ok ? (cnf->result ? "accepted: result 1 as expected" : "accepted") : "refused, start-up goes on",
                   tlvs);
    }
    run->i++;
    lte_run_step (task);
}

static void
lte_at_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMMtkCcciShared  *self = g_task_get_source_object (task);
    LteRun           *run = g_task_get_task_data (task);
    LteStep          *s = &g_array_index (run->steps, LteStep, run->i);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    g_autofree gchar *line = mm_mtk_ccci_at_one_line (reply);

    lte_trace (self, s->slot, "start-up %s: %s %s", s->what, s->at, reply ? line : error->message);
    if (s->kind == LTE_STEP_CHECK) {
        self->lte_checked |= s->slot;
        if (mm_mtk_ccci_test_range_has (reply, "+CEREG:", 3))
            self->lte_cause_ok |= s->slot;
        lte_trace (self, s->slot, "start-up check: <n> = 3 (reject causes) %s",
                   (self->lte_cause_ok & s->slot) ? "offered" : "not offered: reporting stays as it is");
    } else if (s->kind == LTE_STEP_CAUSE && reply && g_str_has_prefix (s->at, "AT+CEREG"))
        self->lte_cause_done |= s->slot;
    run->i++;
    lte_run_step (task);
}

static void
lte_run_step (GTask *task)
{
    MMMtkCcciShared *self = g_task_get_source_object (task);
    LteRun          *run = g_task_get_task_data (task);

    for (; run->i < run->steps->len; run->i++) {
        LteStep              *s = &g_array_index (run->steps, LteStep, run->i);
        g_autoptr(GByteArray) tlvs = NULL;
        g_autofree gchar     *dump = NULL;

        if (!self->mipc || !mm_port_mipc_get_session_ready (self->mipc)) {
            lte_trace (self, s->slot, "start-up %s: not sent, no MIPC session", s->what);
            continue;
        }
        if (s->kind == LTE_STEP_CAUSE && !(self->lte_cause_ok & s->slot))
            continue;   /* not offered: reported once by the check */
        if (s->kind != LTE_STEP_MIPC && s->kind != LTE_STEP_REGISTER) {
            mm_port_mipc_at_command (self->mipc, s->slot, s->at, LTE_TIMEOUT,
                                     (GAsyncReadyCallback) lte_at_ready, task);
            return;
        }
        if (s->kind == LTE_STEP_REGISTER) {
            tlvs = g_byte_array_new ();
            mm_mipc_tlv_add_u16 (tlvs, MM_MIPC_T_REGISTER_IND_ID, s->arg);
        } else
            tlvs = s->build ? s->build () : NULL;
        dump = tlvs ? mm_mipc_tlv_dump (tlvs->data, tlvs->len, 32) : g_strdup ("none");
        lte_trace (self, s->slot, "start-up %s: request %s 0x%04x slot 0x%02x; TLVs %s", s->what,
                   mm_mipc_msg_id_get_string (s->id), s->id, s->slot, dump);
        mm_port_mipc_request (self->mipc, s->id, s->slot, tlvs, LTE_TIMEOUT, NULL,
                              (GAsyncReadyCallback) lte_mipc_ready, task);
        return;
    }
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* the steps for @slots: the read-only checks (not yet done), the pre-radio
 * groups when @radio_off (each once per session and slot), step 6 */
static GArray *
lte_build (MMMtkCcciShared *self, guint8 slots, gboolean radio_off)
{
    static const gchar *sbp[] = {
        "AT+ESBP=5,\"SBP_TERMINAL_CAPABILITY_FLEX\",1", "AT+EIAREG=1", "AT+ECCAUSE=1",
        "AT+ESBP=7,\"SBP_L5_AUTO_DEACT_RSP\"",
    };
    static const gchar *cause[] = { "AT+CREG=3", "AT+CGREG=3", "AT+CEREG=3" };
    GArray *steps = g_array_new (FALSE, FALSE, sizeof (LteStep));
    guint8  pre = radio_off ? (slots & ~self->lte_pre_done & 3) : 0;
    guint   s, i;

    for (s = 1; s <= 2; s++)
        if (slots & ~self->lte_checked & s)
            lte_add (steps, LTE_STEP_CHECK, s, 0, NULL, "AT+CEREG=?", "check", FALSE);
    /* the network indications, read-only */
    for (s = 1; self->lte.nw_diag && s <= 2; s++) {
        if (!(slots & ~self->lte_diag_registered & s))
            continue;
        for (i = 0; i < G_N_ELEMENTS (lte_diag_indications); i++) {
            LteStep st = { LTE_STEP_REGISTER, s, MM_MIPC_INTERNAL_REGISTER_IND_REQ, NULL, NULL,
                           "diag registration", FALSE, lte_diag_indications[i] };
            g_array_append_val (steps, st);
        }
        self->lte_diag_registered |= s;
    }
    /* step 1 */
    for (s = 1; self->lte.prefer_sbp && s <= 2; s++)
        if (pre & s)
            lte_add (steps, LTE_STEP_MIPC, s, MM_MIPC_CALL_DATA_PREFER_SET_REQ, mm_mipc_build_data_prefer,
                     NULL, "step 1 (call/data prefer)", FALSE);
    /* step 2 */
    for (s = 1; self->lte.radio_config && s <= 2; s++)
        if (pre & s)
            lte_add (steps, LTE_STEP_MIPC, s, MM_MIPC_NW_SET_RADIO_CAPABILITY_REQ, mm_mipc_build_radio_capability,
                     NULL, "step 2 (radio capability)", FALSE);
    /* step 3 */
    for (s = 1; self->lte.prefer_sbp && s <= 2; s++)
        for (i = 0; (pre & s) && i < G_N_ELEMENTS (sbp); i++)
            lte_add (steps, LTE_STEP_AT, s, 0, NULL, sbp[i], "step 3", FALSE);
    /* steps 4, 5: header slot 1, once per session */
    if (self->lte.radio_config && pre && !self->lte_global_done) {
        lte_add (steps, LTE_STEP_MIPC, MM_MIPC_SLOT_SIM1, MM_MIPC_SYS_MULTI_SIM_CONFIG_REQ,
                 mm_mipc_build_multi_sim_config, NULL, "step 4 (multi-SIM config)", TRUE);
        lte_add (steps, LTE_STEP_MIPC, MM_MIPC_SLOT_SIM1, MM_MIPC_SYS_SET_DSBP_REQ,
                 mm_mipc_build_dsbp, NULL, "step 5 (DSBP)", FALSE);
        self->lte_global_done = TRUE;
    }
    /* step 6 */
    for (s = 1; self->lte.reg_cause && s <= 2; s++)
        for (i = 0; (slots & ~self->lte_cause_done & s) && i < G_N_ELEMENTS (cause); i++)
            lte_add (steps, LTE_STEP_CAUSE, s, 0, NULL, cause[i], "step 6 (reject causes)", FALSE);
    self->lte_pre_done |= pre;
    return steps;
}

static void
lte_run (MMMtkCcciShared *self, guint8 slots, gboolean radio_off, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask  *task = g_task_new (self, NULL, callback, user_data);
    LteRun *run = g_new0 (LteRun, 1);

    run->steps = self->lte_configured ? lte_build (self, slots, radio_off) : g_array_new (FALSE, FALSE, sizeof (LteStep));
    g_task_set_task_data (task, run, (GDestroyNotify) lte_run_free);
    if (self->lte_configured && radio_off && !self->lte.radio_config && !self->lte.prefer_sbp)
        lte_trace (self, slots & 1 ? 1 : 2, "start-up before radio-on: groups 2 and 3 off (ID_MM_MTK_CCCI_RADIO_CONFIG, "
                   "ID_MM_MTK_CCCI_PREFER_SBP)");
    lte_run_step (task);
}

static void lte_data_allow (MMMtkCcciShared *self, const gchar *why);

/* @user_data: the data SIM's slot when its session data allow follows */
static void
lte_session_ready (MMMtkCcciShared *self, GAsyncResult *res, gpointer user_data)
{
    guint8 slot = GPOINTER_TO_UINT (user_data);

    g_task_propagate_boolean (G_TASK (res), NULL);
    if (slot && slot == self->lte.data_slot && (self->known & self->on & slot) && !(self->lte_allow_sent & slot))
        lte_data_allow (self, "session start, radio already on");
}

/* data-allow: now, then the repeats (+0.46 s, +1.10 s after the
 * first, as observed after radio-on) */
static const guint lte_allow_delays_ms[] = { 460, 640 };

static void lte_allow_send (MMMtkCcciShared *self, const gchar *why);

static gboolean
lte_allow_repeat_cb (MMMtkCcciShared *self)
{
    self->lte_allow_timer = 0;
    lte_allow_send (self, "repeat");
    return G_SOURCE_REMOVE;
}

static void
lte_allow_ready (MMPortMipc *port, GAsyncResult *res, MMMtkCcciShared *self)
{
    g_autoptr(GError)    error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    if (!cnf)
        lte_trace (self, self->lte.data_slot, "data allow: no confirm: %s", error->message);
    else {
        g_autofree gchar *tlvs = mm_mipc_tlv_dump (cnf->payload->data, cnf->payload->len, 32);

        lte_trace (self, self->lte.data_slot, "data allow: confirm 0x%04x slot 0x%02x txid %u result %u; TLVs %s",
                   cnf->msg_id, cnf->slot, cnf->txid, cnf->result, tlvs);
    }
    g_object_unref (self);
}

static void
lte_allow_send (MMMtkCcciShared *self, const gchar *why)
{
    guint8 slot = self->lte.data_slot;

    if (!self->mipc || !mm_port_mipc_get_session_ready (self->mipc)) {
        lte_trace (self, slot, "data allow (%s): not sent, no MIPC session", why);
        return;
    }
    lte_trace (self, slot, "data allow (%s, send %u of 3): request DATA_SET_DATA_ALLOW_REQ 0x%04x slot 0x%02x; TLVs none",
               why, self->lte_allow_repeat + 1, MM_MIPC_DATA_SET_DATA_ALLOW_REQ, slot);
    self->lte_allow_sent |= slot;
    mm_port_mipc_request (self->mipc, MM_MIPC_DATA_SET_DATA_ALLOW_REQ, slot, NULL, LTE_TIMEOUT, NULL,
                          (GAsyncReadyCallback) lte_allow_ready, g_object_ref (self));
    if (self->lte_allow_repeat < G_N_ELEMENTS (lte_allow_delays_ms))
        self->lte_allow_timer = g_timeout_add (lte_allow_delays_ms[self->lte_allow_repeat++],
                                               (GSourceFunc) lte_allow_repeat_cb, self);
}

/* a new data-allow sequence (radio-on, data SIM change, session start) */
static void
lte_data_allow (MMMtkCcciShared *self, const gchar *why)
{
    if (!self->lte_configured || !self->lte.data_allow)
        return;
    if (self->lte_allow_timer) {
        g_source_remove (self->lte_allow_timer);
        self->lte_allow_timer = 0;
    }
    self->lte_allow_repeat = 0;
    lte_allow_send (self, why);
}

/* the network indications, decoded where the layout is documented,
 * every TLV raw (private ones too: the diagnostics are asked for) */
static void
lte_diag_indication (MMMtkCcciShared *self, guint id, guint8 slot, GByteArray *payload)
{
    g_autofree gchar *tlvs = mm_mipc_tlv_dump_full (payload->data, payload->len, 64);
    g_autofree gchar *summary = NULL;
    const guint8     *v;
    gsize             vlen = 0;
    guint32           stat;
    MMMtkCcciNwBlock  b;

    if (id == MM_MIPC_NW_CSCON_IND) {
        v = mm_mipc_tlv_find (payload->data, payload->len, 0x0100, &vlen);
        summary = mm_mtk_ccci_cscon_describe (v, v ? vlen : 0);
    } else if (id == MM_MIPC_NW_PS_IND || id == MM_MIPC_NW_CS_IND) {
        v = mm_mipc_tlv_find (payload->data, payload->len,
                              id == MM_MIPC_NW_PS_IND ? MM_MIPC_T_PS_IND_BLOCK : MM_MIPC_T_CS_IND_BLOCK, &vlen);
        if (v && mm_mtk_ccci_nw_block_parse (v, vlen, &b))
            summary = mm_mtk_ccci_nw_block_describe (&b);
        else
            summary = g_strdup ("no registration block");
        if (id == MM_MIPC_NW_PS_IND && mm_mipc_tlv_find_uint (payload->data, payload->len, 0x0100, &stat)) {
            gchar *with = g_strdup_printf ("0x0100 stat %u; block: %s", stat, summary);
            g_free (summary);
            summary = with;
        }
    }
    lte_trace (self, slot, "diag %s 0x%04x%s%s; TLVs %s", mm_mipc_msg_id_get_string (id), id,
               summary ? ": " : "", summary ? summary : "", tlvs);
}

static gboolean lte_poll_cb (gpointer user_data);

typedef struct {
    MMMtkCcciShared *self;
    guint8           slot;
} LtePoll;

static void
lte_poll_stop (MMMtkCcciShared *self, guint8 slot, const gchar *why)
{
    guint i = slot == MM_MIPC_SLOT_SIM2 ? 1 : 0;

    if (self->lte_poll_timer[i]) {
        g_source_remove (self->lte_poll_timer[i]);
        self->lte_poll_timer[i] = 0;
    }
    if (why)
        lte_trace (self, slot, "diag poll stopped after %u polls: %s", self->lte_poll_count[i], why);
}

static void
lte_poll_ready (MMPortMipc *port, GAsyncResult *res, LtePoll *poll)
{
    MMMtkCcciShared     *self = poll->self;
    guint8               slot = poll->slot;
    g_autoptr(GError)    error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    if (!cnf)
        lte_trace (self, slot, "diag poll NW_GET_PS_REQ: no confirm: %s", error->message);
    else {
        g_autofree gchar *tlvs = mm_mipc_tlv_dump_full (cnf->payload->data, cnf->payload->len, 64);
        g_autofree gchar *summary = NULL;
        const guint8     *v;
        gsize             vlen = 0;
        MMMtkCcciNwBlock  b;

        v = mm_mipc_tlv_find (cnf->payload->data, cnf->payload->len, MM_MIPC_T_GET_PS_BLOCK, &vlen);
        if (v && mm_mtk_ccci_nw_block_parse (v, vlen, &b))
            summary = mm_mtk_ccci_nw_block_describe (&b);
        lte_trace (self, slot, "diag poll NW_GET_PS_CNF 0x%04x txid %u result %u: %s; TLVs %s", cnf->msg_id,
                   cnf->txid, cnf->result, summary ? summary : "no registration block", tlvs);
        /* the first registration result: registered (1, 5) or denied (3) */
        if (summary && (b.stat == 1 || b.stat == 3 || b.stat == 5)) {
            g_autofree gchar *why = g_strdup_printf ("first registration result, stat %u", b.stat);
            lte_poll_stop (self, slot, why);
        }
    }
    g_object_unref (self);
    g_free (poll);
}

#define LTE_POLL_MAX 120

static gboolean
lte_poll_cb (gpointer user_data)
{
    LtePoll         *tick = user_data;
    MMMtkCcciShared *self = tick->self;
    guint8           slot = tick->slot;
    guint            i = slot == MM_MIPC_SLOT_SIM2 ? 1 : 0;
    LtePoll         *poll;

    if (++self->lte_poll_count[i] > LTE_POLL_MAX) {
        self->lte_poll_timer[i] = 0;
        lte_trace (self, slot, "diag poll stopped after %u polls: no registration result", LTE_POLL_MAX);
        return G_SOURCE_REMOVE;
    }
    if (!self->mipc || !mm_port_mipc_get_session_ready (self->mipc))
        return G_SOURCE_CONTINUE;
    poll = g_new0 (LtePoll, 1);
    poll->self = g_object_ref (self);
    poll->slot = slot;
    mm_port_mipc_request (self->mipc, MM_MIPC_NW_GET_PS_REQ, slot, NULL, 5, NULL,
                          (GAsyncReadyCallback) lte_poll_ready, poll);
    return G_SOURCE_CONTINUE;
}

/* from the stack's radio-on, every second */
static void
lte_poll_start (MMMtkCcciShared *self, guint8 slot)
{
    guint    i = slot == MM_MIPC_SLOT_SIM2 ? 1 : 0;
    LtePoll *tick;

    if (!self->lte_configured || !self->lte.nw_diag)
        return;
    lte_poll_stop (self, slot, NULL);
    self->lte_poll_count[i] = 0;
    tick = g_new0 (LtePoll, 1);
    tick->self = self;   /* the timer is removed in dispose */
    tick->slot = slot;
    lte_trace (self, slot, "diag poll: NW_GET_PS_REQ 0x040d (no TLVs) every second until the first registration result");
    self->lte_poll_timer[i] = g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, lte_poll_cb, tick, g_free);
}

void
mm_mtk_ccci_shared_set_lte_config (MMMtkCcciShared *self, const MMMtkCcciLteConfig *config)
{
    gboolean slot_changed = self->lte_configured && config->data_slot != self->lte.data_slot;

    self->lte = *config;
    if (self->lte.data_slot != MM_MIPC_SLOT_SIM2)
        self->lte.data_slot = MM_MIPC_SLOT_SIM1;
    if (!self->lte_configured || slot_changed)
        mm_obj_msg (self, "LTE start-up: data allow %s (data SIM: SIM%u), radio config %s, prefer/SBP %s, reject causes %s, "
                    "radio off by %s, network diagnostics %s",
                    self->lte.data_allow ? "on" : "off", self->lte.data_slot == MM_MIPC_SLOT_SIM2 ? 2 : 1,
                    self->lte.radio_config ? "on" : "off", self->lte.prefer_sbp ? "on" : "off",
                    self->lte.reg_cause ? "on" : "off", self->lte.radio_off_mipc ? "NW_SET_RADIO_STATE" : "AT+CFUN=4",
                    self->lte.nw_diag ? "on" : "off");
    self->lte_configured = TRUE;
    if (slot_changed && (self->known & self->on & self->lte.data_slot))
        lte_data_allow (self, "data SIM changed");
}

void
mm_mtk_ccci_shared_lte_session (MMMtkCcciShared *self, guint8 slots)
{
    if (!self->lte_configured)
        return;
    /* the radio was on before this session: data allow once, after the
     * read-only checks */
    lte_run (self, slots & 3, FALSE, (GAsyncReadyCallback) lte_session_ready,
             GUINT_TO_POINTER ((guint) (slots & self->lte.data_slot)));
}

void
mm_mtk_ccci_shared_lte_reg_cause (MMMtkCcciShared *self, guint8 slots, const gchar *why)
{
    if (!self->lte_configured || !self->lte.reg_cause)
        return;
    lte_trace (self, slots & 1 ? 1 : 2, "step 6 again: %s", why);
    self->lte_cause_done &= ~slots;
    lte_run (self, slots & 3, FALSE, (GAsyncReadyCallback) lte_session_ready, NULL);
}

/*****************************************************************************/
/* Radio prepare: IMS before radio-on */

typedef struct {
    guint8   requester;
    gboolean ims_enabled;
    guint8   query;    /* slots whose radio state is still to be read */
    guint8   pending;  /* slots still to set up */
    guint8   lte;      /* slots whose LTE start-up steps run after IMS */
    gboolean lte_done;
} PrepareContext;

static void prepare_step (GTask *task);
static void prepare_start (GTask *task);

static void
prepare_lte_ready (MMMtkCcciShared *self, GAsyncResult *res, GTask *task)
{
    g_task_propagate_boolean (G_TASK (res), NULL);
    prepare_step (task);
}

static void
prepare_ims_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    MMMtkCcciShared *self = g_task_get_source_object (task);
    PrepareContext  *ctx = g_task_get_task_data (task);
    GError          *error = NULL;
    guint8           slot = ctx->pending & 1 ? 1 : 2;

    ctx->pending &= ~slot;
    if (!mm_mtk_ccci_ims_setup_finish (res, &error)) {
        if (slot == ctx->requester) {
            g_prefix_error (&error, "SIM%u IMS setup before radio-on: ", slot);
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }
        mm_obj_warn (self, "SIM%u IMS setup before radio-on: %s; its modem retries after SIM discovery",
                     slot, error->message);
        g_clear_error (&error);
    }
    prepare_step (task);
}

/* AT+CFUN? on the slot's own tunnel channel: read-only, so that neither
 * object guesses the other stack's radio state */
static void
prepare_query_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMMtkCcciShared  *self = g_task_get_source_object (task);
    PrepareContext   *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    guint8            slot = ctx->query & 1 ? 1 : 2;
    const gchar      *p = reply ? strstr (reply, "+CFUN:") : NULL;

    ctx->query &= ~slot;
    if (p) {
        guint64 v = g_ascii_strtoull (p + 6 + strspn (p + 6, " "), NULL, 10);

        mm_mtk_ccci_shared_radio_note (self, slot, v == 1);
        mm_obj_dbg (self, "SIM%u radio %s (+CFUN: %u)", slot, v == 1 ? "on" : "off", (guint) v);
    } else
        mm_obj_dbg (self, "SIM%u radio state not read: %s", slot,
                    error ? error->message : (reply ? reply : "no reply"));
    prepare_start (task);
}

static void
prepare_start (GTask *task)
{
    MMMtkCcciShared *self = g_task_get_source_object (task);
    PrepareContext  *ctx = g_task_get_task_data (task);
    guint8           target;

    if (ctx->query && self->mipc && mm_port_mipc_get_session_ready (self->mipc)) {
        mm_port_mipc_at_command (self->mipc, ctx->query & 1 ? 1 : 2, "AT+CFUN?", 10,
                                 (GAsyncReadyCallback) prepare_query_ready, task);
        return;
    }
    target = self->configured & ~self->low_power & 3;
    /* the requester's own stack always (the cold start that asks is the
     * proof that it is off); the other wanted stacks unless known on */
    ctx->pending = ctx->ims_enabled ? ((target & ~(self->on & self->known)) | (ctx->requester & target)) & 3 : 0;
    ctx->lte = ((target & ~(self->on & self->known)) | (ctx->requester & target)) & 3;
    if (ctx->pending)
        mm_obj_dbg (self, "radio prepare for SIM%u: IMS setup of slot mask 0x%02x", ctx->requester, ctx->pending);
    prepare_step (task);
}

static void
prepare_step (GTask *task)
{
    MMMtkCcciShared *self = g_task_get_source_object (task);
    PrepareContext  *ctx = g_task_get_task_data (task);
    guint8           slot;

    if (!ctx->pending) {
        /* after IMS, before radio-on: the LTE start-up steps */
        if (ctx->lte && !ctx->lte_done && self->lte_configured) {
            ctx->lte_done = TRUE;
            lte_run (self, ctx->lte, TRUE, (GAsyncReadyCallback) prepare_lte_ready, task);
            return;
        }
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    slot = ctx->pending & 1 ? 1 : 2;
    mm_mtk_ccci_ims_setup (self->ims[slot - 1], (GAsyncReadyCallback) prepare_ims_ready, task);
}

void
mm_mtk_ccci_shared_radio_prepare (MMMtkCcciShared     *self,
                                  guint8               requester,
                                  gboolean             ims_enabled,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    GTask          *task = g_task_new (self, NULL, callback, user_data);
    PrepareContext *ctx = g_new0 (PrepareContext, 1);

    ctx->requester = requester;
    ctx->ims_enabled = ims_enabled;
    /* dual-modem mode: learn the other stack's radio state first */
    ctx->query = self->dual ? (self->configured & ~self->known & 3) : 0;
    g_task_set_task_data (task, ctx, g_free);
    prepare_start (task);
}

gboolean
mm_mtk_ccci_shared_radio_prepare_finish (MMMtkCcciShared *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*****************************************************************************/
/* Radio set */

typedef struct {
    guint8  slots;
    gboolean on;
    guint8  target;
    guint   busy_tries;
    gboolean cfun_fallback;
} SetContext;

static void radio_queue_next (MMMtkCcciShared *self);

static void
radio_set_complete (GTask *task, GError *error)
{
    MMMtkCcciShared *self = g_task_get_source_object (task);

    g_object_ref (self);
    g_warn_if_fail (g_queue_peek_head (&self->radio_queue) == task);
    g_queue_remove (&self->radio_queue, task);
    if (error)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
    radio_queue_next (self);
    g_object_unref (self);
}

static void radio_set_send (GTask *task);

static gboolean
radio_set_retry (gpointer user_data)
{
    radio_set_send (user_data);
    return G_SOURCE_REMOVE;
}

static void
radio_set_ready (MMMtkCcciShared *self, GAsyncResult *res, GTask *task)
{
    SetContext        *ctx = g_task_get_task_data (task);
    g_autoptr(GError)  error = NULL;
    g_autofree gchar  *reply = radio_command_finish (self, res, &error);

    if (!reply) {
        if (g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_BUSY) &&
            ctx->busy_tries < RADIO_BUSY_TRIES) {
            ctx->busy_tries++;
            g_timeout_add_seconds (1, radio_set_retry, task);
            return;
        }
        if (ctx->target == 1 && !ctx->cfun_fallback) {
            /* SIM1 alone: the 3GPP command expresses it */
            mm_obj_dbg (self, "radio: AT+EFUN=1 failed (%s), trying AT+CFUN=1", error->message);
            ctx->cfun_fallback = TRUE;
            radio_command (self, "+CFUN=1", (GAsyncReadyCallback) radio_set_ready, task);
            return;
        }
        g_prefix_error (&error, "radio mask 0x%02x: ", ctx->target);
        radio_set_complete (task, g_steal_pointer (&error));
        return;
    }
    {
        /* the last confirmed state, not "known": a power up clears "known"
         * only to force the command, and a repeated power up of a stack that
         * is on (enabling after the cold start) is no radio-on (it gave a
         * second data allow sequence on the phone, 2026-11-25) */
        guint8 was_on = self->on;
        guint8 now_on;

        self->on = ctx->target;
        self->known = 3;
        mm_obj_msg (self, "radio: stacks on 0x%02x (SIM1 %s, SIM2 %s)", ctx->target,
                    ctx->target & 1 ? "on" : "off", ctx->target & 2 ? "on" : "off");
        now_on = ctx->on ? (ctx->target & ~was_on & 3) : 0;
        /* a radio-on of the data SIM's stack: data allow (step 8) */
        if (self->lte_configured && (now_on & self->lte.data_slot))
            lte_data_allow (self, "radio on");
        if (now_on & MM_MIPC_SLOT_SIM1)
            lte_poll_start (self, MM_MIPC_SLOT_SIM1);
        if (now_on & MM_MIPC_SLOT_SIM2)
            lte_poll_start (self, MM_MIPC_SLOT_SIM2);
    }
    radio_set_complete (task, NULL);
}

static void
radio_set_send (GTask *task)
{
    MMMtkCcciShared  *self = g_task_get_source_object (task);
    SetContext       *ctx = g_task_get_task_data (task);
    g_autofree gchar *command = NULL;

    command = ctx->target ? g_strdup_printf ("+EFUN=%u", ctx->target) : g_strdup ("+CFUN=4");
    mm_obj_msg (self, "radio: SIM%s %s -> AT%s", ctx->slots == 3 ? "1+2" : (ctx->slots & 1 ? "1" : "2"),
                ctx->on ? "on" : "low power", command);
    if (!ctx->target)
        radio_off_command (self, (GAsyncReadyCallback) radio_set_ready, task);
    else
        radio_command (self, command, (GAsyncReadyCallback) radio_set_ready, task);
}

static void
radio_set_start (GTask *task)
{
    MMMtkCcciShared *self = g_task_get_source_object (task);
    SetContext      *ctx = g_task_get_task_data (task);

    if (ctx->on) {
        self->low_power &= ~ctx->slots;
        /* power up is asked when the modem reported the stack off: send it */
        self->known &= ~ctx->slots;
    } else
        self->low_power |= ctx->slots;
    ctx->target = self->configured & ~self->low_power & 3;
    if ((self->known & 3) == 3 && self->on == ctx->target) {
        mm_obj_dbg (self, "radio: already 0x%02x", ctx->target);
        radio_set_complete (task, NULL);
        return;
    }
    radio_set_send (task);
}

static void
radio_queue_next (MMMtkCcciShared *self)
{
    GTask *next = g_queue_peek_head (&self->radio_queue);

    if (next)
        radio_set_start (next);
}

void
mm_mtk_ccci_shared_radio_set (MMMtkCcciShared     *self,
                              guint8               slots,
                              gboolean             on,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
    GTask      *task = g_task_new (self, NULL, callback, user_data);
    SetContext *ctx = g_new0 (SetContext, 1);

    ctx->slots = slots & 3;
    ctx->on = on;
    g_task_set_task_data (task, ctx, g_free);
    g_queue_push_tail (&self->radio_queue, task);
    /* one request at a time; the next one starts when the head completes */
    if (g_queue_get_length (&self->radio_queue) == 1)
        radio_queue_next (self);
}

gboolean
mm_mtk_ccci_shared_radio_set_finish (MMMtkCcciShared *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_mtk_ccci_shared_radio_note (MMMtkCcciShared *self, guint8 slots, gboolean on)
{
    slots &= 3;
    self->known |= slots;
    if (on)
        self->on |= slots;
    else
        self->on &= ~slots;
}

guint8 mm_mtk_ccci_shared_radio_get_on (MMMtkCcciShared *self) { return self->on; }
guint8 mm_mtk_ccci_shared_radio_get_known (MMMtkCcciShared *self) { return self->known; }
guint8 mm_mtk_ccci_shared_radio_get_low_power (MMMtkCcciShared *self) { return self->low_power; }

void
mm_mtk_ccci_shared_set_radio_mask (MMMtkCcciShared *self, guint configured)
{
    if (configured >= 1 && configured <= 3)
        self->configured = configured;
}

guint
mm_mtk_ccci_shared_get_radio_mask (MMMtkCcciShared *self)
{
    return self->configured;
}

void
mm_mtk_ccci_shared_set_radio_port (MMMtkCcciShared *self, MMPortSerialAt *port)
{
    if (self->radio_port == port)
        return;
    if (self->radio_port)
        g_object_remove_weak_pointer (G_OBJECT (self->radio_port), (gpointer *) &self->radio_port);
    self->radio_port = port;
    if (port)
        g_object_add_weak_pointer (G_OBJECT (port), (gpointer *) &self->radio_port);
}

void
mm_mtk_ccci_shared_set_radio_port_wait (MMMtkCcciShared *self, guint seconds)
{
    self->radio_port_wait = seconds;
}

/*****************************************************************************/

gint
mm_mtk_ccci_shared_speech_update (MMMtkCcciShared *self, guint8 slot, gboolean in_call)
{
    guint8 before = self->in_call;

    if (in_call)
        self->in_call |= slot & 3;
    else
        self->in_call &= ~(slot & 3);
    if (!before && self->in_call)
        return 1;
    if (before && !self->in_call)
        return -1;
    return 0;
}

guint8
mm_mtk_ccci_shared_speech_get (MMMtkCcciShared *self)
{
    return self->in_call;
}

/*****************************************************************************/

MMPortMipc *
mm_mtk_ccci_shared_peek_mipc (MMMtkCcciShared *self)
{
    return self->mipc;
}

const gchar *
mm_mtk_ccci_shared_get_wwan_device (MMMtkCcciShared *self)
{
    return self->wwan_device;
}

MMMtkCcciIms *
mm_mtk_ccci_shared_peek_ims (MMMtkCcciShared *self, guint8 slot)
{
    if (slot != 1 && slot != 2)
        return NULL;
    return self->ims[slot - 1];
}

gboolean
mm_mtk_ccci_shared_ims_owns_link (MMMtkCcciShared *self, guint link_id)
{
    return mm_mtk_ccci_ims_owns_link (self->ims[0], link_id) ||
           mm_mtk_ccci_ims_owns_link (self->ims[1], link_id);
}

void
mm_mtk_ccci_shared_set_dual (MMMtkCcciShared *self, gboolean dual)
{
    self->dual = dual;
}

gboolean
mm_mtk_ccci_shared_get_dual (MMMtkCcciShared *self)
{
    return self->dual;
}

MMMtkCcciShared *
mm_mtk_ccci_shared_peek (const gchar *mipc_port)
{
    return registry && mipc_port ? g_hash_table_lookup (registry, mipc_port) : NULL;
}

MMMtkCcciShared *
mm_mtk_ccci_shared_acquire (const gchar *mipc_port)
{
    MMMtkCcciShared *self;
    const gchar     *p;
    guint            i;

    g_return_val_if_fail (mipc_port && mipc_port[0], NULL);

    self = mm_mtk_ccci_shared_peek (mipc_port);
    if (self)
        return g_object_ref (self);

    self = g_object_new (MM_TYPE_MTK_CCCI_SHARED, NULL);
    self->name = g_strdup (mipc_port);
    /* "wwan0mipc1" -> "wwan0", the parent of the data/IMS links */
    p = strstr (mipc_port, "mipc");
    self->wwan_device = (p && p > mipc_port) ? g_strndup (mipc_port, p - mipc_port) : g_strdup ("wwan0");
    self->mipc = mm_port_mipc_new (mipc_port);
    g_signal_connect (self->mipc, MM_PORT_MIPC_SIGNAL_INDICATION, G_CALLBACK (mipc_indication), self);
    g_signal_connect (self->mipc, "forced-close", G_CALLBACK (mipc_lost), self);
    for (i = 0; i < 2; i++) {
        /* the controllers' owner is this object: their tasks keep it alive */
        self->ims[i] = mm_mtk_ccci_ims_new (self, self->mipc, (guint8) (1u << i),
                                            self->wwan_device);
        mm_mtk_ccci_ims_set_radio_hook (self->ims[i], ims_radio_hook, self);
    }
    if (!registry)
        registry = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_insert (registry, g_strdup (mipc_port), self);
    mm_obj_dbg (self, "shared modem context created");
    return self;
}

static void
mm_mtk_ccci_shared_init (MMMtkCcciShared *self)
{
    self->configured = 3;
    self->dsds_busy = TRUE;
    g_queue_init (&self->radio_queue);
}

static void
dispose (GObject *object)
{
    MMMtkCcciShared *self = MM_MTK_CCCI_SHARED (object);
    guint            i;

    if (registry && self->name && g_hash_table_lookup (registry, self->name) == self)
        g_hash_table_remove (registry, self->name);
    if (self->lte_allow_timer) {
        g_source_remove (self->lte_allow_timer);
        self->lte_allow_timer = 0;
    }
    lte_poll_stop (self, MM_MIPC_SLOT_SIM1, NULL);
    lte_poll_stop (self, MM_MIPC_SLOT_SIM2, NULL);
    for (i = 0; i < 2; i++)
        g_clear_pointer (&self->ims[i], mm_mtk_ccci_ims_free);
    if (self->mipc) {
        g_signal_handlers_disconnect_by_data (self->mipc, self);
        g_clear_object (&self->mipc);
    }
    mm_mtk_ccci_shared_set_radio_port (self, NULL);
    for (i = 0; i < 2; i++)
        voice_client_clear (self, i);
    G_OBJECT_CLASS (mm_mtk_ccci_shared_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMMtkCcciShared *self = MM_MTK_CCCI_SHARED (object);

    g_free (self->name);
    g_free (self->wwan_device);
    G_OBJECT_CLASS (mm_mtk_ccci_shared_parent_class)->finalize (object);
}

static void
mm_mtk_ccci_shared_class_init (MMMtkCcciSharedClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = dispose;
    object_class->finalize = finalize;
}
