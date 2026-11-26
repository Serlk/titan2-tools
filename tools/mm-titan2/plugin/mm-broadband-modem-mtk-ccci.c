/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMBroadbandModemMtkCcci: MT6878 integrated modem on the mtk_ccci driver.
 *
 * Everything 3GPP comes from MMBroadbandModem (registration, SIM, signal, SMS
 * in PDU mode, the generic AT voice calls). This class adds the MediaTek
 * behaviour established on the phone with our own tools (t2-modemd,
 * tools/modemd/src/vendor.c) and a clean-room behavioural description of the
 * MediaTek AT/MIPC protocol (no code of the phone's Android software was used):
 *
 *  - radio power: AT+EFUN=<mask> (1 SIM1, 2 SIM2, 3 both) instead of AT+CFUN=1,
 *    AT+CFUN=4 for off (the modem's start-up state; AT+EFUN=0 is untried);
 *  - the AT port serves one SIM protocol stack at a time: AT+ESUO=4 (SIM1) or
 *    AT+ESUO=5 (SIM2), selected before reading the SIM;
 *  - the per-stack voice initialisation, then the
 *    +ECPI call-progress reports as the primary call-state source, the +EAIC
 *    incoming-call approval (answered at once), +ESPEECH speech-path reports;
 *  - the MIPC data port (wwan0mipc1, MMPortMipc): session started when the
 *    modem is enabled, used by MMBearerMtkCcci for the PDN activation;
 *  - the voice audio path: t2-speechd is told call-start/call-stop from MM's
 *    in-call audio channel hooks (Android's timing: at dial / answer and when the
 *    last call is gone).
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gio/gunixsocketaddress.h>

#include "ModemManager.h"
#include "mm-log-object.h"
#include "mm-errors-types.h"
#include "mm-modem-helpers.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-voice.h"
#include "mm-iface-modem-messaging.h"
#include "mm-call-at.h"
#include "mm-call-mtk-ccci.h"
#include "mm-base-sim.h"
#include "mm-sim-mtk-ccci.h"
#include "mm-modem-helpers-mtk-ccci.h"
#include "mm-broadband-modem-mtk-ccci.h"
#include "mm-bearer-mtk-ccci.h"
#include "mm-port-mipc.h"
#include "mm-mtk-ccci-ims.h"
#include "mm-mtk-ccci-sms-ready.h"
#include "mm-mtk-ccci-sms-poll.h"
#include "mm-mtk-ccci-shared.h"
#include "mm-mtk-ccci-at-bridge.h"
#include "mm-kernel-device-generic.h"

static void iface_modem_init       (MMIfaceModemInterface      *iface);
static void iface_modem_3gpp_init  (MMIfaceModem3gppInterface  *iface);
static void iface_modem_voice_init (MMIfaceModemVoiceInterface *iface);
static void iface_modem_messaging_init (MMIfaceModemMessagingInterface *iface);

static MMIfaceModemInterface      *iface_modem_parent;
static MMIfaceModem3gppInterface  *iface_modem_3gpp_parent;
static MMIfaceModemVoiceInterface *iface_modem_voice_parent;
static MMIfaceModemMessagingInterface *iface_modem_messaging_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMtkCcci, mm_broadband_modem_mtk_ccci, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM,       iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP,  iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_VOICE, iface_modem_voice_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_MESSAGING, iface_modem_messaging_init))

/* udev property on the AT port: the t2-speechd control socket; "" disables the trigger */
#define ID_MM_MTK_CCCI_SPEECHD_SOCKET "ID_MM_MTK_CCCI_SPEECHD_SOCKET"
#define DEFAULT_SPEECHD_SOCKET        "/run/t2-speechd.sock"
#define DEFAULT_RADIO_MASK            3
#define DEFAULT_SIM_SLOT              1
#define DEFAULT_WWAN_DEVICE           "wwan0"
#define MIPC_CLIENT_NAME              "ModemManager"
#define N_SIM_SLOTS                   2

/* the SIM slot chosen through the SimSlots API survives the reprobe that
 * follows a switch (and daemon restarts) in this file; it overrides the udev
 * default */
#ifndef PKGSTATEDIR
# define PKGSTATEDIR "/var/lib/ModemManager"
#endif
#define SIM_SLOT_STATE_FILE PKGSTATEDIR "/mtk-ccci-sim-slot"

/* indications the data client registers for */
static const guint16 mipc_indications[] = {
    MM_MIPC_INTERNAL_EIF_IND,
    MM_MIPC_DATA_ACT_CALL_IND,
    MM_MIPC_DATA_DEACT_CALL_IND,
    MM_MIPC_SYS_AT_IND,
    MM_MIPC_IMS_PDN_IND,
    MM_MIPC_INTERNAL_EIPSPI_CMD,
    MM_MIPC_CALL_STATUS_IND,
    MM_MIPC_CALL_EVENT_IND,
    MM_MIPC_CALL_APPROVE_CMD,
};

struct _MMBroadbandModemMtkCcciPrivate {
    MMPortMipc *mipc;          /* the shared MIPC port (SIM1: also grabbed as its data port) */
    MMMtkCcciShared *shared;   /* session, IMS controllers, radio, speech (mm-mtk-ccci-shared.c) */
    gboolean    sim2_role;     /* the SIM2 modem object of the dual-modem mode */
    gboolean    dual;          /* dual-modem mode: this object serves one SIM stack */
    guint8      served;        /* MIPC slot bits this object handles (legacy 0x03) */
    MMMtkCcciAtBridge *bridge; /* SIM2: the AT port over SYS_AT slot 2 */
    MMKernelDevice *config_kdev; /* SIM2: the anchor port (wwan0mipc2), the udev configuration */
    gboolean    sim2_data;     /* SIM2: bearers allowed (ID_MM_MTK_CCCI_SIM2_DATA) */
    gboolean    ims_enabled;
    gboolean    mipc_opened;   /* we hold an open reference on it */
    gchar      *wwan_device;   /* "wwan0" */
    guint       radio_mask;
    guint       sim_slot;
    gchar      *speechd_socket;
    gboolean    sim_ready[2];
    gboolean    speech_on;     /* last +ESPEECH */

    /* RAT mode (allowed/preferred modes): the modes the generic probing
     * found, the last pair the modem accepted, and whether a client chose
     * the modes (kept across disable/enable of this modem object only) */
    MMModemMode rat_mask;
    gboolean    rat_known;
    guint8      rat_value;
    guint8      rat_prefer;
    MMModemMode rat_allowed;
    MMModemMode rat_preferred;
    gboolean    rat_user_set;
    gboolean    rat_session_confirmed; /* rat_value/prefer confirmed in this MIPC session */
    gboolean    start_rat;     /* the start-up RAT setting (ID_MM_MTK_CCCI_START_RAT) */
    gboolean    nw_diag;       /* the network diagnostics (ID_MM_MTK_CCCI_NW_DIAG) */
    gboolean    plain_cereg_busy;
    guint       reg_read_timer;  /* the registration read after enabling */
    guint       reg_read_count;
    gboolean    reg_read_done;   /* a registration result was read */

    /* LTE loss diagnostics ("rat-trace" lines) */
    guint8        trace_pending;      /* slot bits waiting for a snapshot */
    guint8        trace_ceer;         /* slot bits whose next snapshot adds AT+CEER (a denial) */
    GString      *trace_reason;       /* the events since the last snapshot */
    guint         trace_timer;
    gboolean      trace_busy;
    gboolean      trace_sleep_known;
    gint64        trace_sleep_offset; /* CLOCK_BOOTTIME - CLOCK_MONOTONIC, us */
    MmGdbusModem *trace_skeleton;
    MMModemAccessTechnology trace_act;

    /* vendor URCs */
    GRegex *ecpi_regex;
    GRegex *eaic_regex;
    GRegex *espeech_regex;
    GRegex *evocd_regex;
    GRegex *eind_regex;
    GRegex *ims_regex;
};

/*****************************************************************************/
/* Accessors used by the bearer */

MMPortMipc *
mm_broadband_modem_mtk_ccci_peek_port_mipc (MMBroadbandModemMtkCcci *self)
{
    g_return_val_if_fail (MM_IS_BROADBAND_MODEM_MTK_CCCI (self), NULL);
    return self->priv->mipc;
}

guint8
mm_broadband_modem_mtk_ccci_get_mipc_slot (MMBroadbandModemMtkCcci *self)
{
    g_return_val_if_fail (MM_IS_BROADBAND_MODEM_MTK_CCCI (self), MM_MIPC_SLOT_SIM1);
    /* slot n -> 1 << n */
    return (guint8) (1u << ((self->priv->sim_slot > 0 ? self->priv->sim_slot : 1) - 1));
}

const gchar *
mm_broadband_modem_mtk_ccci_get_wwan_device (MMBroadbandModemMtkCcci *self)
{
    g_return_val_if_fail (MM_IS_BROADBAND_MODEM_MTK_CCCI (self), DEFAULT_WWAN_DEVICE);
    return self->priv->wwan_device ? self->priv->wwan_device : DEFAULT_WWAN_DEVICE;
}

gboolean
mm_broadband_modem_mtk_ccci_is_dual (MMBroadbandModemMtkCcci *self)
{
    return self->priv->dual;
}

guint8
mm_broadband_modem_mtk_ccci_get_served_slots (MMBroadbandModemMtkCcci *self)
{
    return self->priv->served;
}

static MMMtkCcciIms *
ims_of (MMBroadbandModemMtkCcci *self, guint sim)
{
    return self->priv->shared ? mm_mtk_ccci_shared_peek_ims (self->priv->shared, (guint8) (1u << (sim - 1))) : NULL;
}

/*****************************************************************************/
/* LTE loss diagnostics (localising the remaining loss of LTE). Every line
 * reads
 *   rat-trace SIM<n> mono <s> boot <s>: <what>
 * with both clocks (mm_mtk_ccci_trace_stamp) and the SIM named by its MIPC
 * slot, so the name survives a new D-Bus index after an MM restart. The
 * slot's events are logged as they happen: registration URCs of the MIPC AT
 * channel (as URCs, with the reject cause or "not reported"), RAT mode
 * confirms/refusals and 0x441D, AccessTechnologies changes on D-Bus, radio
 * on/low power, cold start and enabling, data bearer up/down/lost, call
 * active/end, the first modem traffic after a suspend. Most of them are
 * followed TRACE_DELAY_MS later by one snapshot of the slot's serving state:
 * read responses of trace_commands on the slot-addressed MIPC AT channel,
 * logged raw on one line (registration lines also decoded). Read-only
 * queries; nothing is changed by them. */

#define TRACE_DELAY_MS     500
#define TRACE_SLEEP_MIN_US (2 * G_USEC_PER_SEC)
/* per boot (tmpfs): how many times this plugin started on this boot, so
 * that the rat-trace lines of an MM stop/start on the same boot line up */
#ifndef TRACE_START_FILE
# define TRACE_START_FILE "/run/mm-mtk-ccci-trace.starts"
#endif

/* per process and slot: the first EPS indication (URC) and read response
 * since this ModemManager started */
static guint8 trace_first_eps_urc;
static guint8 trace_first_eps_read;

/* once per process: the marker that ties this process's lines to the boot */
static void
trace_process_marker (gpointer log_object)
{
    static gboolean   done;
    g_autofree gchar *boot_id = NULL;
    g_autofree gchar *count_text = NULL;
    g_autofree gchar *stamp = NULL;
    g_autofree gchar *count_str = NULL;
    guint64           count = 0;

    if (done)
        return;
    done = TRUE;
    stamp = mm_mtk_ccci_trace_stamp ();
    if (g_file_get_contents ("/proc/sys/kernel/random/boot_id", &boot_id, NULL, NULL))
        g_strstrip (boot_id);
    if (g_file_get_contents (TRACE_START_FILE, &count_text, NULL, NULL))
        count = g_ascii_strtoull (count_text, NULL, 10);
    count++;
    count_str = g_strdup_printf ("%" G_GUINT64_FORMAT "\n", count);
    if (!g_file_set_contents (TRACE_START_FILE, count_str, -1, NULL))
        count = 0;
    if (count)
        mm_obj_msg (log_object, "rat-trace process %s: ModemManager pid %d started, plugin start %" G_GUINT64_FORMAT
                    " on boot %s", stamp, (gint) getpid (), count, boot_id ? boot_id : "unknown");
    else
        mm_obj_msg (log_object, "rat-trace process %s: ModemManager pid %d started, boot %s (start count unavailable: %s)",
                    stamp, (gint) getpid (), boot_id ? boot_id : "unknown", TRACE_START_FILE);
}

static const gchar *trace_commands[] = {
    "AT+COPS?", "AT+CREG?", "AT+CGREG?", "AT+CEREG?", "AT+CGATT?", "AT+CESQ", "AT+CIREG?",
};

static const gchar *
trace_sim_name (guint8 slots)
{
    switch (slots & (MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2)) {
    case MM_MIPC_SLOT_SIM1: return "SIM1";
    case MM_MIPC_SLOT_SIM2: return "SIM2";
    default:                return "SIM1+SIM2";
    }
}

/* the slots this object's events concern */
static guint8
trace_slots (MMBroadbandModemMtkCcci *self)
{
    return self->priv->served ? self->priv->served : mm_broadband_modem_mtk_ccci_get_mipc_slot (self);
}

/*****************************************************************************/
/* Registration state from the MIPC AT channel (keep the modem's transition
 * in the interface).
 *
 * The slot's registration URCs arrive on the slot-addressed MIPC AT channel
 * (SYS_AT_IND). The SIM2 object's AT port is that channel (the bridge
 * forwards them to the generic handlers); the SIM1 object's AT port is
 * wwan0at0, where they do not come, so AccessTechnologies and the
 * registration state stayed at the first values (hspa) while the modem was
 * on LTE (2026-11-25). URCs that are not forwarded to this object's AT port
 * are applied here exactly as the generic handler applies its own (generic
 * expressions and parser, CS/PS/EPS/5GS state, AcT, LAC/TAC/cell id), and
 * the read responses of each snapshot as well, so that a missed URC is
 * corrected; the snapshot's +COPS? AcT refreshes AccessTechnologies when the
 * property disagrees. Only for the stack this object's 3GPP interface is
 * (its MIPC slot). The updates go through reg_update (tests replace it). */

typedef struct {
    void (*cs)       (MMIfaceModem3gpp *self, MMModem3gppRegistrationState state, gboolean deferred);
    void (*ps)       (MMIfaceModem3gpp *self, MMModem3gppRegistrationState state, gboolean deferred);
    void (*eps)      (MMIfaceModem3gpp *self, MMModem3gppRegistrationState state, gboolean deferred);
    void (*nr5g)     (MMIfaceModem3gpp *self, MMModem3gppRegistrationState state, gboolean deferred);
    void (*act)      (MMIfaceModem3gpp *self, MMModemAccessTechnology act);
    void (*location) (MMIfaceModem3gpp *self, gulong lac, gulong tac, gulong ci);
    MMModemAccessTechnology (*get_act) (MMBroadbandModemMtkCcci *self);
} RegUpdateOps;

static MMModemAccessTechnology
reg_get_act (MMBroadbandModemMtkCcci *self)
{
    MmGdbusModem            *skeleton = NULL;
    MMModemAccessTechnology  act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;

    g_object_get (self, MM_IFACE_MODEM_DBUS_SKELETON, &skeleton, NULL);
    if (skeleton) {
        act = mm_gdbus_modem_get_access_technologies (skeleton);
        g_object_unref (skeleton);
    }
    return act;
}

static const RegUpdateOps reg_update_default = {
    mm_iface_modem_3gpp_update_cs_registration_state,
    mm_iface_modem_3gpp_update_ps_registration_state,
    mm_iface_modem_3gpp_update_eps_registration_state,
    mm_iface_modem_3gpp_update_5gs_registration_state,
    mm_iface_modem_3gpp_update_access_technologies,
    mm_iface_modem_3gpp_update_location,
    reg_get_act,
};
static const RegUpdateOps *reg_update = &reg_update_default;

/* every registration line of @text into this object's 3GPP state when
 * @slot is its stack; returns how many were applied */
static guint
reg_apply_text (MMBroadbandModemMtkCcci *self, guint8 slot, const gchar *text, gboolean solicited)
{
    MMIfaceModem3gpp *iface = MM_IFACE_MODEM_3GPP (self);
    g_auto(GStrv)     lines = NULL;
    guint             i, n = 0;

    if (!text || slot != mm_broadband_modem_mtk_ccci_get_mipc_slot (self))
        return 0;
    lines = g_strsplit_set (text, "\r\n", -1);
    for (i = 0; lines[i]; i++) {
        MMMtkCcciRegUpdate u;

        if (!mm_mtk_ccci_reg_update_parse (lines[i], solicited, &u))
            continue;
        if (u.cgreg)
            reg_update->ps (iface, u.state, FALSE);
        else if (u.cereg)
            reg_update->eps (iface, u.state, FALSE);
        else if (u.c5greg)
            reg_update->nr5g (iface, u.state, FALSE);
        else
            reg_update->cs (iface, u.state, FALSE);
        reg_update->act (iface, u.act);
        reg_update->location (iface, u.lac, u.tac, u.ci);
        n++;
    }
    if (n)
        mm_obj_dbg (self, "SIM%u: %u registration %s applied from the MIPC AT channel", slot == MM_MIPC_SLOT_SIM2 ? 2 : 1,
                    n, solicited ? "read response(s)" : "URC(s)");
    return n;
}

/* a denial (stat 3) in @text: the next snapshot also reads AT+CEER */
static gboolean
reg_text_denied (const gchar *text, gboolean solicited)
{
    g_auto(GStrv) lines = NULL;
    guint         i;

    if (!text)
        return FALSE;
    lines = g_strsplit_set (text, "\r\n", -1);
    for (i = 0; lines[i]; i++) {
        MMMtkCcciReg reg;

        if (mm_mtk_ccci_reg_parse (lines[i], solicited, &reg) && reg.stat == 3)
            return TRUE;
    }
    return FALSE;
}

/* a registration result (stat 1, 3 or 5) in a read response */
static gboolean
reg_text_result (const gchar *text)
{
    g_auto(GStrv) lines = g_strsplit_set (text ? text : "", "\r\n", -1);
    guint         i;

    for (i = 0; lines[i]; i++) {
        MMMtkCcciReg reg;

        if (mm_mtk_ccci_reg_parse (lines[i], TRUE, &reg) && (reg.stat == 1 || reg.stat == 3 || reg.stat == 5))
            return TRUE;
    }
    return FALSE;
}

/* the slot's first registration result for the IMS controller (a profile
 * list held until then): +CREG or +CEREG with stat 1 or 5 */
static void
reg_hint_registered (MMBroadbandModemMtkCcci *self, guint8 slot, const gchar *text, gboolean solicited)
{
    g_auto(GStrv) lines = NULL;
    guint         i;

    if (!text || !self->priv->shared || (slot != MM_MIPC_SLOT_SIM1 && slot != MM_MIPC_SLOT_SIM2))
        return;
    lines = g_strsplit_set (text, "\r\n", -1);
    for (i = 0; lines[i]; i++) {
        MMMtkCcciReg reg;

        if (mm_mtk_ccci_reg_parse (lines[i], solicited, &reg) && (reg.stat == 1 || reg.stat == 5) &&
            (!strcmp (reg.domain, "CS") || !strcmp (reg.domain, "EPS"))) {
            mm_mtk_ccci_ims_registered (mm_mtk_ccci_shared_peek_ims (self->priv->shared, slot));
            return;
        }
    }
}

typedef struct {
    MMBroadbandModemMtkCcci *self;   /* a reference */
    guint8                   slot;
    gboolean                 ceer;   /* AT+CEER after trace_commands */
    MMModemAccessTechnology  cops_act;
    guint                    i;
    gchar                   *reason;
    gchar                   *stamp;
    GString                 *out;
} TraceSnapshot;

static void trace_schedule (MMBroadbandModemMtkCcci *self);
static void trace_snapshot_step (TraceSnapshot *ctx);

/* diagnostics: SIM1's reject cause read on
 * the plain AT port (wwan0at0), where the generic code keeps <n> = 2:
 * AT+CEREG=3, AT+CEREG?, back to AT+CEREG=2 */
static const gchar *plain_cereg_commands[] = { "+CEREG=3", "+CEREG?", "+CEREG=2" };

typedef struct {
    guint    i;
    GString *out;
    gchar   *stamp;
} PlainCereg;

static void plain_cereg_step (MMBroadbandModemMtkCcci *self, PlainCereg *ctx);

static void
plain_cereg_ready (MMBaseModem *_self, GAsyncResult *res, PlainCereg *ctx)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    g_autoptr(GError)        error = NULL;
    const gchar             *reply = mm_base_modem_at_command_finish (_self, res, &error);

    g_string_append_printf (ctx->out, "%sAT%s ", ctx->out->len ? "; " : "", plain_cereg_commands[ctx->i]);
    if (!reply)
        g_string_append (ctx->out, error->message);
    else {
        g_autofree gchar *line = mm_mtk_ccci_at_one_line (reply);
        g_autofree gchar *reg = mm_mtk_ccci_reg_describe_text (reply, TRUE);

        g_string_append (ctx->out, *line ? line : "OK");
        if (reg)
            g_string_append_printf (ctx->out, " [%s]", reg);
    }
    ctx->i++;
    plain_cereg_step (self, ctx);
    g_object_unref (self);
}

static void
plain_cereg_step (MMBroadbandModemMtkCcci *self, PlainCereg *ctx)
{
    if (ctx->i < G_N_ELEMENTS (plain_cereg_commands)) {
        mm_base_modem_at_command (MM_BASE_MODEM (self), plain_cereg_commands[ctx->i], 5, FALSE,
                                  (GAsyncReadyCallback) plain_cereg_ready, ctx);
        g_object_ref (self);
        return;
    }
    {
        MMPortSerialAt *primary = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));

        mm_obj_msg (self, "rat-trace SIM1 %s: diag plain AT port (%s): %s", ctx->stamp,
                    primary ? mm_port_get_device (MM_PORT (primary)) : "gone", ctx->out->str);
    }
    self->priv->plain_cereg_busy = FALSE;
    g_string_free (ctx->out, TRUE);
    g_free (ctx->stamp);
    g_free (ctx);
}

static void
plain_cereg_read (MMBroadbandModemMtkCcci *self)
{
    PlainCereg *ctx;

    /* SIM1's object only, on its own AT port (not the SIM2 bridge) */
    if (!self->priv->nw_diag || self->priv->bridge || self->priv->plain_cereg_busy ||
        !mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)))
        return;
    self->priv->plain_cereg_busy = TRUE;
    ctx = g_new0 (PlainCereg, 1);
    ctx->out = g_string_new (NULL);
    ctx->stamp = mm_mtk_ccci_trace_stamp ();
    plain_cereg_step (self, ctx);
}

static guint
trace_n_commands (TraceSnapshot *ctx)
{
    return G_N_ELEMENTS (trace_commands) + (ctx->ceer ? 1 : 0);
}

static const gchar *
trace_command (TraceSnapshot *ctx)
{
    return ctx->i < G_N_ELEMENTS (trace_commands) ? trace_commands[ctx->i] : "AT+CEER";
}

static void
trace_snapshot_ready (MMPortMipc *port, GAsyncResult *res, TraceSnapshot *ctx)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);

    g_string_append_printf (ctx->out, "%s%s ", ctx->out->len ? "; " : "", trace_command (ctx));
    if (!reply)
        g_string_append_printf (ctx->out, "failed: %s", error ? error->message : "no reply");
    else {
        g_autofree gchar *line = mm_mtk_ccci_at_one_line (reply);
        g_autofree gchar *reg = mm_mtk_ccci_reg_describe_text (reply, TRUE);

        /* the modem's answer for this slot: into the 3GPP state; a denial
         * adds AT+CEER (the network cause, if the modem has one) */
        if (reg) {
            reg_apply_text (ctx->self, ctx->slot, reply, TRUE);
            reg_hint_registered (ctx->self, ctx->slot, reply, TRUE);
            if (reg_text_result (reply))
                ctx->self->priv->reg_read_done = TRUE;
            if (reg_text_denied (reply, TRUE))
                ctx->ceer = TRUE;
        }
        if (g_str_equal (trace_command (ctx), "AT+COPS?"))
            mm_mtk_ccci_cops_act (reply, &ctx->cops_act);
        g_string_append (ctx->out, line);
        if (reg)
            g_string_append_printf (ctx->out, " [%s]", reg);
        if (reg && strstr (reply, "+CEREG:") && !(trace_first_eps_read & ctx->slot)) {
            trace_first_eps_read |= ctx->slot;
            g_string_append (ctx->out, " (first EPS read response since this MM started)");
        }
    }
    ctx->i++;
    trace_snapshot_step (ctx);
}

static void
trace_snapshot_step (TraceSnapshot *ctx)
{
    MMBroadbandModemMtkCcci *self = ctx->self;

    if (ctx->i < trace_n_commands (ctx) && self->priv->mipc &&
        mm_port_mipc_get_session_ready (self->priv->mipc)) {
        mm_port_mipc_at_command (self->priv->mipc, ctx->slot, trace_command (ctx), 5,
                                 (GAsyncReadyCallback) trace_snapshot_ready, ctx);
        return;
    }
    if (ctx->i < trace_n_commands (ctx))
        g_string_append_printf (ctx->out, "%sstopped: no MIPC session", ctx->out->len ? "; " : "");
    /* the serving AcT the modem reports vs the D-Bus property */
    if (ctx->cops_act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN &&
        ctx->slot == mm_broadband_modem_mtk_ccci_get_mipc_slot (self)) {
        MMModemAccessTechnology prop = reg_update->get_act (self);

        if (prop != ctx->cops_act) {
            g_autofree gchar *prop_str = mm_modem_access_technology_build_string_from_mask (prop);
            g_autofree gchar *cops_str = mm_modem_access_technology_build_string_from_mask (ctx->cops_act);

            g_string_append_printf (ctx->out, "; AccessTechnologies %s disagreed with +COPS? AcT %s: refreshed",
                                    prop_str, cops_str);
            reg_update->act (MM_IFACE_MODEM_3GPP (self), ctx->cops_act);
        }
    }
    mm_obj_msg (self, "rat-trace %s %s: snapshot after %s (read responses): %s",
                trace_sim_name (ctx->slot), ctx->stamp, ctx->reason, ctx->out->str);
    if (ctx->slot == MM_MIPC_SLOT_SIM1)
        plain_cereg_read (self);
    self->priv->trace_busy = FALSE;
    trace_schedule (self);
    g_free (ctx->reason);
    g_free (ctx->stamp);
    g_string_free (ctx->out, TRUE);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gboolean
trace_timer_cb (MMBroadbandModemMtkCcci *self)
{
    TraceSnapshot *ctx;
    guint8         slot;

    self->priv->trace_timer = 0;
    if (!self->priv->trace_pending)
        return G_SOURCE_REMOVE;
    if (!self->priv->mipc || !mm_port_mipc_get_session_ready (self->priv->mipc)) {
        mm_obj_dbg (self, "rat-trace: no MIPC session, snapshot dropped");
        self->priv->trace_pending = 0;
        g_string_truncate (self->priv->trace_reason, 0);
        return G_SOURCE_REMOVE;
    }
    /* one slot at a time (both only in the single-object mode) */
    slot = (self->priv->trace_pending & MM_MIPC_SLOT_SIM1) ? MM_MIPC_SLOT_SIM1 : MM_MIPC_SLOT_SIM2;
    self->priv->trace_pending &= ~slot;
    ctx = g_new0 (TraceSnapshot, 1);
    ctx->self = g_object_ref (self);
    ctx->slot = slot;
    ctx->ceer = !!(self->priv->trace_ceer & slot);
    self->priv->trace_ceer &= ~slot;
    ctx->reason = g_strdup (self->priv->trace_reason->str);
    ctx->stamp = mm_mtk_ccci_trace_stamp ();
    ctx->out = g_string_new (NULL);
    if (!self->priv->trace_pending)
        g_string_truncate (self->priv->trace_reason, 0);
    self->priv->trace_busy = TRUE;
    trace_snapshot_step (ctx);
    return G_SOURCE_REMOVE;
}

static void
trace_schedule (MMBroadbandModemMtkCcci *self)
{
    if (self->priv->trace_pending && !self->priv->trace_busy && !self->priv->trace_timer)
        self->priv->trace_timer = g_timeout_add (TRACE_DELAY_MS, (GSourceFunc) trace_timer_cb, self);
}

/* @reason: a short label; NULL logs the event without a snapshot */
static void trace_event (MMBroadbandModemMtkCcci *self, guint8 slots, const gchar *reason,
                         const gchar *fmt, ...) G_GNUC_PRINTF (4, 5);

static void
trace_event (MMBroadbandModemMtkCcci *self, guint8 slots, const gchar *reason, const gchar *fmt, ...)
{
    g_autofree gchar *stamp = mm_mtk_ccci_trace_stamp ();
    g_autofree gchar *what = NULL;
    va_list           ap;

    va_start (ap, fmt);
    what = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    mm_obj_msg (self, "rat-trace %s %s: %s", trace_sim_name (slots), stamp, what);
    if (!reason || !(slots & (MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2)))
        return;
    self->priv->trace_pending |= slots & (MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2);
    if (!self->priv->trace_reason)
        self->priv->trace_reason = g_string_new (NULL);
    if (!strstr (self->priv->trace_reason->str, reason) && self->priv->trace_reason->len < 200)
        g_string_append_printf (self->priv->trace_reason, "%s%s", self->priv->trace_reason->len ? ", " : "", reason);
    trace_schedule (self);
}

/* After the registration reporting is enabled (the 3GPP interface takes
 * updates from then on): an attach within seconds of radio-on may have
 * happened before any URC reached this object, so the slot's state is read
 * every second (a snapshot: its read responses go into the 3GPP state and
 * +COPS? refreshes AccessTechnologies) until the first registration result
 * (stat 1, 3 or 5), at most REG_READ_MAX times. */
#define REG_READ_MAX 20

static void
reg_read_stop (MMBroadbandModemMtkCcci *self)
{
    if (self->priv->reg_read_timer) {
        g_source_remove (self->priv->reg_read_timer);
        self->priv->reg_read_timer = 0;
    }
}

static gboolean
reg_read_cb (MMBroadbandModemMtkCcci *self)
{
    if (self->priv->reg_read_done || ++self->priv->reg_read_count > REG_READ_MAX) {
        trace_event (self, mm_broadband_modem_mtk_ccci_get_mipc_slot (self), NULL,
                     "registration read after enabling stopped after %u reads: %s", self->priv->reg_read_count - 1,
                     self->priv->reg_read_done ? "registration result read" : "no result");
        self->priv->reg_read_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (!self->priv->trace_busy)
        trace_event (self, mm_broadband_modem_mtk_ccci_get_mipc_slot (self), "registration read", "registration read %u",
                     self->priv->reg_read_count);
    return G_SOURCE_CONTINUE;
}

static void
reg_read_start (MMBroadbandModemMtkCcci *self)
{
    reg_read_stop (self);
    self->priv->reg_read_done = FALSE;
    self->priv->reg_read_count = 1;
    trace_event (self, mm_broadband_modem_mtk_ccci_get_mipc_slot (self), "registration read",
                 "registration reporting enabled: reading the registration state");
    self->priv->reg_read_timer = g_timeout_add (1000, (GSourceFunc) reg_read_cb, self);
}

void
mm_broadband_modem_mtk_ccci_trace_event (MMBroadbandModemMtkCcci *self, guint8 slot, const gchar *what)
{
    g_return_if_fail (MM_IS_BROADBAND_MODEM_MTK_CCCI (self));
    trace_event (self, slot ? slot : trace_slots (self), "data bearer", "%s", what);
}

/* the first modem traffic after a suspend: the boot-time clock ran ahead */
static void
trace_check_sleep (MMBroadbandModemMtkCcci *self)
{
    struct timespec mono = { 0 }, boot = { 0 };
    gint64          offset;

    clock_gettime (CLOCK_MONOTONIC, &mono);
    clock_gettime (CLOCK_BOOTTIME, &boot);
    offset = ((gint64) boot.tv_sec - mono.tv_sec) * G_USEC_PER_SEC + (boot.tv_nsec - mono.tv_nsec) / 1000;
    if (self->priv->trace_sleep_known && offset - self->priv->trace_sleep_offset >= TRACE_SLEEP_MIN_US)
        trace_event (self, trace_slots (self), "wake-up",
                     "first modem traffic after a suspend of %.1f s",
                     (gdouble) (offset - self->priv->trace_sleep_offset) / G_USEC_PER_SEC);
    self->priv->trace_sleep_known = TRUE;
    self->priv->trace_sleep_offset = offset;
}

static void
trace_act_changed (MmGdbusModem *skeleton, GParamSpec *pspec, MMBroadbandModemMtkCcci *self)
{
    MMModemAccessTechnology act = mm_gdbus_modem_get_access_technologies (skeleton);
    g_autofree gchar       *old_str = NULL;
    g_autofree gchar       *new_str = NULL;

    if (act == self->priv->trace_act)
        return;
    old_str = mm_modem_access_technology_build_string_from_mask (self->priv->trace_act);
    new_str = mm_modem_access_technology_build_string_from_mask (act);
    self->priv->trace_act = act;
    /* what the D-Bus property (the shell's source) says; the snapshot shows
     * what the modem says for the same slot */
    trace_event (self, mm_broadband_modem_mtk_ccci_get_mipc_slot (self), "AccessTechnologies",
                 "AccessTechnologies %s -> %s (D-Bus property)", old_str, new_str);
}

/* once the Modem interface has its skeleton (enabling) */
static void
trace_watch (MMBroadbandModemMtkCcci *self)
{
    MmGdbusModem *skeleton = NULL;

    g_object_get (self, MM_IFACE_MODEM_DBUS_SKELETON, &skeleton, NULL);
    if (!skeleton)
        return;
    if (skeleton == self->priv->trace_skeleton) {
        g_object_unref (skeleton);
        return;
    }
    if (self->priv->trace_skeleton) {
        g_signal_handlers_disconnect_by_data (self->priv->trace_skeleton, self);
        g_object_unref (self->priv->trace_skeleton);
    }
    self->priv->trace_skeleton = skeleton;
    self->priv->trace_act = mm_gdbus_modem_get_access_technologies (skeleton);
    g_signal_connect (skeleton, "notify::access-technologies", G_CALLBACK (trace_act_changed), self);
}

/*****************************************************************************/
/* Port creation (MMBaseModem class): the MIPC port */

static void
mipc_port_lost (MMPortMipc *port, MMBroadbandModemMtkCcci *self)
{
    self->priv->mipc_opened = FALSE;
    self->priv->rat_session_confirmed = FALSE;
    mm_base_modem_set_reprobe (MM_BASE_MODEM (self), TRUE);
    g_cancellable_cancel (mm_base_modem_peek_cancellable (MM_BASE_MODEM (self)));
}

/* SMS notices (both arrive on the MIPC data port, CCCI channel MIPC1, whose
 * packets wake the AP from s2idle: "woken by the modem: ... (ccci-mipc) mipc
 * msg 0x4001|0x46xx" in the kernel log, 500 ms wakeup event per packet):
 *  - +CMTI/+CDSI in SYS_AT_IND, enabled by AT+CNMI on the MIPC AT channel of
 *    each slot (modem_messaging_enable_unsolicited_events);
 *  - the SMS-group indications 0x46xx, registered per slot.
 * Either one starts an immediate storage scan for the selected SIM. */
static void
sms_notice (MMBroadbandModemMtkCcci *self, guint8 slot, const gchar *what)
{
    g_autofree gchar *reason = NULL;

    if (slot != mm_broadband_modem_mtk_ccci_get_mipc_slot (self)) {
        /* the AT port serves the selected stack only (+ESUO) */
        mm_obj_msg (self, "SMS notice %s for SIM%u, which is not the selected SIM: not read now",
                    what, slot);
        return;
    }
    mm_mtk_ccci_sms_wake_hold (MM_BASE_MODEM (self), 5000);
    reason = g_strdup_printf ("SIM%u %s", slot, what);
    mm_mtk_ccci_sms_poll_notify (MM_BASE_MODEM (self), reason);
}

/* the TLV ids and sizes only: the payload may hold a number or text */
static gchar *
mipc_tlv_summary (const guint8 *p, gsize len)
{
    GString *s = g_string_new (NULL);
    MMMipcTlv tlv;
    gsize off = 0;
    gint r;

    while ((r = mm_mipc_tlv_next (p, len, &off, &tlv)) == 1)
        g_string_append_printf (s, "%s%04x/%" G_GSIZE_FORMAT, s->len ? " " : "", tlv.raw_id, tlv.len);
    if (r < 0)
        g_string_append (s, s->len ? " (malformed)" : "(malformed)");
    return g_string_free (s, FALSE);
}

static void
voice_mipc_indication (MMPortMipc *port, guint id, guint routing, guint txid,
                       GByteArray *payload, MMBroadbandModemMtkCcci *self)
{
    guint8 slot = routing & 3;
    guint32 event = 0;
    /* any modem traffic after a suspend: one fallback scan (lost notices) */
    mm_mtk_ccci_sms_poll_check_resume (MM_BASE_MODEM (self));
    trace_check_sleep (self);
    if (slot != 1 && slot != 2)
        return;
    /* dual-modem mode: the other SIM's modem object handles its slot */
    if (!(slot & self->priv->served))
        return;
    if (id == MM_MIPC_SYS_AT_IND) {
        g_autofree gchar *text = mm_mipc_tlv_find_at_string (payload->data, payload->len);
        g_autofree gchar *reg = NULL;
        if (!text)
            return;
        /* registration URCs of the slot, as URCs (the snapshot after them
         * holds the read responses) */
        reg = mm_mtk_ccci_reg_describe_text (text, FALSE);
        if (reg || strstr (text, "+CIREGU:")) {
            g_autofree gchar *line = mm_mtk_ccci_at_one_line (text);
            gboolean          first_eps = reg && strstr (text, "+CEREG:") && !(trace_first_eps_urc & slot);

            if (first_eps)
                trace_first_eps_urc |= slot;
            if (reg && reg_text_denied (text, FALSE))
                self->priv->trace_ceer |= slot;
            if (reg)
                reg_hint_registered (self, slot, text, FALSE);
            trace_event (self, slot, reg ? "registration URC" : NULL, "URC %s%s%s%s%s",
                         line, reg ? " [" : "", reg ? reg : "", reg ? "]" : "",
                         first_eps ? " (first EPS indication since this MM started)" : "");
        }
        /* SIM2 object: the slot's URCs are the unsolicited text of its AT port
         * (the generic handlers apply them); otherwise apply them here */
        if (!self->priv->bridge || !mm_mtk_ccci_at_bridge_push_urc (self->priv->bridge, text))
            reg_apply_text (self, slot, text, FALSE);
        if (strstr (text, "+CMTI:"))
            sms_notice (self, slot, "+CMTI");
        else if (strstr (text, "+CDSI:"))
            sms_notice (self, slot, "+CDSI");
        else if (strstr (text, "+CMT:") || strstr (text, "+CDS:"))
            /* not expected with <mt>=1 / <ds>=1 and +CSMS=0; the message is not stored */
            mm_obj_warn (self, "SIM%u: unstored SMS URC on the MIPC AT channel, not handled", slot);
    } else if (MM_MIPC_IS_SMS_IND (id)) {
        g_autofree gchar *tlvs = mipc_tlv_summary (payload->data, payload->len);
        const gchar *name = mm_mipc_msg_id_get_string (id);
        mm_obj_msg (self, "SIM%u MIPC SMS indication 0x%04x%s%s%s (TLVs %s)", slot, id,
                    name ? " (" : "", name ? name : "", name ? ")" : "", tlvs);
        if (id == MM_MIPC_SMS_NEW_SMS_IND || id == MM_MIPC_SMS_NEW_STATUS_REPORT_IND ||
            id == MM_MIPC_SMS_STORE_STATUS_IND || id == MM_MIPC_SMS_CFG_IND)
            sms_notice (self, slot, name ? name : "SMS indication");
    } else if (id == MM_MIPC_CALL_APPROVE_CMD) {
        /* answered once by the shared context (mm-mtk-ccci-shared.c) */
        mm_obj_dbg (self, "SIM%u incoming-call approval request", slot);
    } else if (id == MM_MIPC_CALL_STATUS_IND) {
        MMCallInfo info;
        if (mm_mtk_ccci_call_parse (slot, payload->data, payload->len, &info)) {
            mm_obj_dbg (self, "SIM%u call index %u state %u", slot, info.index, info.state);
            if (info.state == MM_CALL_STATE_ACTIVE)
                trace_event (self, slot, NULL, "call %u active", info.index);
            else if (info.state == MM_CALL_STATE_TERMINATED)
                trace_event (self, slot, "call end", "call %u ended", info.index);
            mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (self), port, slot, &info);
            g_free (info.number);
        }
    } else if (id == MM_MIPC_CALL_EVENT_IND &&
               mm_mipc_tlv_find_uint (payload->data, payload->len, 0x0100, &event))
        mm_obj_dbg (self, "SIM%u call event %u", slot, event);
    else if (id == MM_MIPC_NW_CURRENT_RAT_IND) {
        guint32 rat = 0, prefer = 0;
        if (mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_CURRENT_RAT_RAT, &rat) &&
            mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_CURRENT_RAT_PREFER, &prefer)) {
            g_autofree gchar *cmp = NULL;

            if (!self->priv->rat_known)
                cmp = g_strdup ("nothing confirmed by this object yet");
            else if (rat == self->priv->rat_value && prefer == self->priv->rat_prefer)
                cmp = g_strdup ("the confirmed pair");
            else
                cmp = g_strdup_printf ("differs from the confirmed rat %u prefer_rat %u",
                                       self->priv->rat_value, self->priv->rat_prefer);
            /* the mode setting only, not the serving RAT: the snapshot shows that */
            trace_event (self, slot, "NW_CURRENT_RAT_IND",
                         "RAT mode now rat %u prefer_rat %u (NW_CURRENT_RAT_IND 0x441D; %s)", rat, prefer, cmp);
        }
    }
}

static MMPort *
create_wwan_port (MMBaseModem *_self,
                  const gchar *name,
                  MMPortType   ptype)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);

    /* The plugin grabs the MIPC data port with type UNKNOWN (the probe was told
     * it is not AT); every other MIPC port is grabbed as ignored and never
     * reaches this hook. */
    if (ptype == MM_PORT_TYPE_UNKNOWN && strstr (name, "mipc")) {
        if (self->priv->mipc) {
            mm_obj_warn (self, "second MIPC port %s ignored: %s is already the data port",
                         name, mm_port_get_device (MM_PORT (self->priv->mipc)));
            return NULL;
        }
        /* one MMPortMipc per MIPC channel for every modem object of this
         * modem (the SIM2 object may have created it already) */
        if (!self->priv->shared)
            self->priv->shared = mm_mtk_ccci_shared_acquire (name);
        self->priv->mipc = g_object_ref (mm_mtk_ccci_shared_peek_mipc (self->priv->shared));
        g_signal_connect_object (self->priv->mipc, "forced-close", G_CALLBACK (mipc_port_lost), self, 0);
        g_signal_connect_object (self->priv->mipc, MM_PORT_MIPC_SIGNAL_INDICATION, G_CALLBACK (voice_mipc_indication), self, 0);
        mm_obj_dbg (self, "MIPC data port created: %s", name);
        return MM_PORT (g_object_ref (self->priv->mipc));
    }

    return MM_BASE_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->create_wwan_port (_self, name, ptype);
}

/*****************************************************************************/
/* The DSDS approval policy's view of this object's calls (mm-mtk-ccci-shared.h) */

static guint
shared_calls_present (GObject *client, guint8 slot)
{
    return mm_call_mtk_ccci_count_present (MM_IFACE_MODEM_VOICE (client), slot);
}

static void
shared_call_rejected (GObject *client, guint8 slot, guint call_id, const gchar *number)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (client);

    if (!self->priv->mipc ||
        !mm_call_mtk_ccci_report_rejected (MM_IFACE_MODEM_VOICE (self), self->priv->mipc, slot, number))
        mm_obj_msg (self, "SIM%u rejected incoming call %u not shown: voice not set up", slot, call_id);
    else
        mm_obj_msg (self, "SIM%u rejected incoming call %u shown as a terminated call", slot, call_id);
}

/*****************************************************************************/
/* Configuration from the AT port's udev properties */

/* the kernel device that carries this object's udev configuration: the
 * AT port (wwan0at0) for SIM1/legacy, the anchor port (wwan0mipc2) for SIM2 */
static MMKernelDevice *
config_kernel_device (MMBroadbandModemMtkCcci *self)
{
    MMPortSerialAt *primary;

    if (self->priv->config_kdev)
        return self->priv->config_kdev;
    primary = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));
    return primary ? mm_port_peek_kernel_device (MM_PORT (primary)) : NULL;
}

/* the LTE start-up contract, one property per trial
 * group, read from the SIM1/legacy object's port (wwan0at0):
 *   ID_MM_MTK_CCCI_DATA_ALLOW    group 1, data allow after radio-on (default 1)
 *   ID_MM_MTK_CCCI_DATA_SIM      the data SIM for it, 1 or 2 (default 1)
 *   ID_MM_MTK_CCCI_RADIO_CONFIG  group 2, radio capability, multi-SIM config,
 *                                DSBP before radio-on (default 0)
 *   ID_MM_MTK_CCCI_PREFER_SBP    group 3, call/data prefer and the
 *                                ESBP/EIAREG/ECCAUSE commands (default 0)
 *   ID_MM_MTK_CCCI_REG_CAUSE     CREG/CGREG/CEREG level 3 with reject
 *                                causes where offered (default 0) */
static gboolean
lte_property (MMKernelDevice *kdev, const gchar *name, gboolean fallback)
{
    return mm_kernel_device_has_property (kdev, name) ? mm_kernel_device_get_property_as_boolean (kdev, name) : fallback;
}

static void
load_lte_configuration (MMBroadbandModemMtkCcci *self, MMKernelDevice *kdev)
{
    MMMtkCcciLteConfig config = { 0 };
    gint               v = 1;

    config.data_allow = lte_property (kdev, "ID_MM_MTK_CCCI_DATA_ALLOW", TRUE);
    config.radio_config = lte_property (kdev, "ID_MM_MTK_CCCI_RADIO_CONFIG", FALSE);
    config.prefer_sbp = lte_property (kdev, "ID_MM_MTK_CCCI_PREFER_SBP", FALSE);
    config.reg_cause = lte_property (kdev, "ID_MM_MTK_CCCI_REG_CAUSE", FALSE);
    config.radio_off_mipc = !g_strcmp0 (mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_RADIO_OFF"), "mipc");
    config.nw_diag = lte_property (kdev, "ID_MM_MTK_CCCI_NW_DIAG", FALSE);
    self->priv->nw_diag = config.nw_diag;
    if (mm_kernel_device_has_property (kdev, "ID_MM_MTK_CCCI_DATA_SIM")) {
        v = mm_kernel_device_get_property_as_int (kdev, "ID_MM_MTK_CCCI_DATA_SIM");
        if (v != 1 && v != 2) {
            mm_obj_warn (self, "ignoring invalid ID_MM_MTK_CCCI_DATA_SIM %d", v);
            v = 1;
        }
    }
    config.data_slot = (guint8) (1u << (v - 1));
    mm_mtk_ccci_shared_set_lte_config (self->priv->shared, &config);
}

static void
load_port_configuration (MMBroadbandModemMtkCcci *self)
{
    MMKernelDevice *kdev;
    const gchar    *name, *p;
    gint            v;

    kdev = config_kernel_device (self);
    if (!kdev)
        return;

    self->priv->ims_enabled = !mm_kernel_device_has_property (kdev, "ID_MM_MTK_CCCI_IMS") ||
                             mm_kernel_device_get_property_as_boolean (kdev, "ID_MM_MTK_CCCI_IMS");
    self->priv->start_rat = lte_property (kdev, "ID_MM_MTK_CCCI_START_RAT", TRUE);
    if (!self->priv->sim2_role && mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_RADIO_MASK)) {
        v = mm_kernel_device_get_property_as_int (kdev, ID_MM_MTK_CCCI_RADIO_MASK);
        if (v >= 1 && v <= 3)
            self->priv->radio_mask = (guint) v;
        else
            mm_obj_warn (self, "ignoring invalid " ID_MM_MTK_CCCI_RADIO_MASK " %d", v);
    }

    if (self->priv->sim2_role) {
        /* the second SIM stack's own modem object (DESIGN.md §11) */
        self->priv->dual = TRUE;
        self->priv->sim_slot = 2;
        self->priv->sim2_data = mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_SIM2_DATA) &&
                                mm_kernel_device_get_property_as_boolean (kdev, ID_MM_MTK_CCCI_SIM2_DATA);
    } else {
        /* SIM1 / legacy: two modem objects unless switched off */
        self->priv->dual = !mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_DUAL_MODEM) ||
                           mm_kernel_device_get_property_as_boolean (kdev, ID_MM_MTK_CCCI_DUAL_MODEM);
        if (self->priv->dual) {
            self->priv->sim_slot = 1;
            if (mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_SIM_SLOT) &&
                mm_kernel_device_get_property_as_int (kdev, ID_MM_MTK_CCCI_SIM_SLOT) != 1)
                mm_obj_warn (self, "dual-modem mode: " ID_MM_MTK_CCCI_SIM_SLOT " ignored (this object is SIM1)");
            if (g_file_test (SIM_SLOT_STATE_FILE, G_FILE_TEST_EXISTS))
                mm_obj_dbg (self, "dual-modem mode: %s (single-modem SIM choice) ignored", SIM_SLOT_STATE_FILE);
        } else {
            if (mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_SIM_SLOT)) {
                v = mm_kernel_device_get_property_as_int (kdev, ID_MM_MTK_CCCI_SIM_SLOT);
                if (v == 1 || v == 2)
                    self->priv->sim_slot = (guint) v;
                else
                    mm_obj_warn (self, "ignoring invalid " ID_MM_MTK_CCCI_SIM_SLOT " %d", v);
            }
            /* a slot chosen through the SimSlots API wins over the udev default */
            {
                g_autofree gchar *contents = NULL;

                if (g_file_get_contents (SIM_SLOT_STATE_FILE, &contents, NULL, NULL)) {
                    v = (gint) g_ascii_strtoll (g_strstrip (contents), NULL, 10);
                    if (v == 1 || v == 2) {
                        self->priv->sim_slot = (guint) v;
                        mm_obj_dbg (self, "SIM slot %d from %s", v, SIM_SLOT_STATE_FILE);
                    } else
                        mm_obj_warn (self, "ignoring invalid SIM slot '%s' in %s", contents, SIM_SLOT_STATE_FILE);
                }
            }
        }
    }
    self->priv->served = self->priv->dual ? (guint8) (1u << (self->priv->sim_slot - 1))
                                          : (MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2);

    if (mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_SPEECHD_SOCKET)) {
        g_free (self->priv->speechd_socket);
        self->priv->speechd_socket = g_strdup (mm_kernel_device_get_property (kdev, ID_MM_MTK_CCCI_SPEECHD_SOCKET));
    }

    /* the WWAN device the ports hang off: the wwan class device's name when the
     * kernel device backend knows it, else derived from the MIPC port name
     * ("wwan0mipc1" -> "wwan0") or the AT port name ("wwan0at0" -> "wwan0") */
    g_clear_pointer (&self->priv->wwan_device, g_free);
    p = mm_kernel_device_get_wwandev_sysfs_path (kdev);
    if (p && *p)
        self->priv->wwan_device = g_path_get_basename (p);
    if (!self->priv->wwan_device && self->priv->shared)
        self->priv->wwan_device = g_strdup (mm_mtk_ccci_shared_get_wwan_device (self->priv->shared));
    if (!self->priv->wwan_device) {
        name = mm_kernel_device_get_name (kdev);
        p = name ? strstr (name, "at") : NULL;
        if (p && p > name)
            self->priv->wwan_device = g_strndup (name, p - name);
    }

    if (self->priv->shared) {
        if (!self->priv->sim2_role) {
            MMPortSerialAt *primary = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));

            /* the SIM1 object (or the single legacy object) owns the radio
             * configuration and wwan0at0, the proven radio command port */
            mm_mtk_ccci_shared_set_radio_mask (self->priv->shared, self->priv->radio_mask);
            load_lte_configuration (self, kdev);
            mm_mtk_ccci_shared_set_dual (self->priv->shared, self->priv->dual);
            if (primary)
                mm_mtk_ccci_shared_set_radio_port (self->priv->shared, primary);
            /* the incoming-call policy is one setting of the modem (both SIMs) */
            mm_mtk_ccci_shared_set_dsds_busy (self->priv->shared,
                                              !mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_DSDS_BUSY) ||
                                              mm_kernel_device_get_property_as_boolean (kdev, ID_MM_MTK_CCCI_DSDS_BUSY));
        } else
            /* SIM2 may start first: give the SIM1 object time to appear */
            mm_mtk_ccci_shared_set_radio_port_wait (self->priv->shared, 20);
        /* this object's calls decide on the other slot's incoming calls */
        mm_mtk_ccci_shared_set_voice_client (self->priv->shared, self->priv->served, G_OBJECT (self),
                                             shared_calls_present, shared_call_rejected);
    }

    mm_obj_dbg (self, "configuration: %s, radio mask %u, SIM slot %u, WWAN device %s, speechd socket '%s', DSDS busy %s",
                self->priv->dual ? (self->priv->sim2_role ? "dual-modem SIM2 object" : "dual-modem SIM1 object")
                                 : "single modem object",
                self->priv->radio_mask, self->priv->sim_slot,
                mm_broadband_modem_mtk_ccci_get_wwan_device (self),
                self->priv->speechd_socket ? self->priv->speechd_socket : "",
                self->priv->shared && !mm_mtk_ccci_shared_get_dsds_busy (self->priv->shared) ? "off" : "on");
}

/*****************************************************************************/
/* Power (Modem interface) */

/* Radio power goes through the shared coordinator: the stacks of both modem
 * objects are one AT+EFUN mask (mm-mtk-ccci-shared.h). Power up of this
 * object adds its slot(s), low power removes them; no slot left gives
 * AT+CFUN=4, exactly the old single-object commands (EFUN=<mask>, SIM busy
 * retried 30 x 1 s, CFUN=1 for SIM1 alone, CFUN=4 for off). */

static gboolean
modem_power_up_finish (MMIfaceModem  *self,
                       GAsyncResult  *res,
                       GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
radio_set_ready (MMMtkCcciShared *shared, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    gboolean                 on = GPOINTER_TO_INT (g_task_get_task_data (task));
    GError                  *error = NULL;

    if (!mm_mtk_ccci_shared_radio_set_finish (shared, res, &error)) {
        trace_event (self, trace_slots (self), NULL, "radio %s failed: %s", on ? "on" : "to low power", error->message);
        g_task_return_error (task, error);
    } else {
        trace_event (self, trace_slots (self), NULL, "radio %s", on ? "on" : "in low power");
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
radio_set (MMBroadbandModemMtkCcci *self, gboolean on, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    if (!self->priv->shared) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "No MIPC port");
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task, GINT_TO_POINTER (on), NULL);
    mm_obj_dbg (self, "radio of slot mask 0x%02x %s", self->priv->served, on ? "on" : "to low power");
    mm_mtk_ccci_shared_radio_set (self->priv->shared, self->priv->served, on,
                                  (GAsyncReadyCallback) radio_set_ready, task);
}

static void
modem_power_up (MMIfaceModem        *_self,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    radio_set (MM_BROADBAND_MODEM_MTK_CCCI (_self), TRUE, callback, user_data);
}

static gboolean
modem_power_down_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_power_down (MMIfaceModem        *_self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    /* low power of one SIM keeps the other stack's radio (AT+EFUN=<rest>);
     * the last one gives the 3GPP flight mode AT+CFUN=4 (start-up state;
     * AT+EFUN=0 is untried, t2-modemd QUESTIONS C17) */
    radio_set (MM_BROADBAND_MODEM_MTK_CCCI (_self), FALSE, callback, user_data);
}

/*****************************************************************************/
/* Allowed/preferred modes (Modem interface): the RAT mode
 *
 * Android's RIL sets the RAT mode with MIPC
 * NW_SET_RAT_REQ (TLV 0x0100 u8 rat, 0x0101 u8 prefer_rat) per slot at
 * start-up, rat 22 = GSM|UMTS|LTE|NR, prefer_rat 128, Android's default
 * NR/LTE/WCDMA/GSM; the plugin sends the same at every enable unless a client
 * chose other modes for this modem object, and reads the mode back with
 * AT+ERAT? on the slot-addressed MIPC AT channel (+ERAT: <curr_rat>,
 * <gprs_status>,<rat>,<prefer_rat>, the layout ModemManager's mtk-legacy
 * plugin parses). The values and their MM mapping are in
 * mm-modem-helpers-mtk-ccci.c. */

static GArray *
modem_load_supported_modes_finish (MMIfaceModem  *self,
                                   GAsyncResult  *res,
                                   GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
parent_load_supported_modes_ready (MMIfaceModem *_self,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    g_autoptr(GError)  error = NULL;
    g_autoptr(GArray)  all = NULL;
    g_autofree gchar  *mask_str = NULL;
    MMModemMode        mask = MM_MODEM_MODE_NONE;
    GArray            *combinations;
    guint              i;

    all = iface_modem_parent->load_supported_modes_finish (_self, res, &error);
    if (!all)
        mm_obj_warn (self, "generic supported-modes probing failed: %s; assuming 2G/3G/4G",
                     error ? error->message : "unknown error");
    for (i = 0; all && i < all->len; i++)
        mask |= g_array_index (all, MMModemModeCombination, i).allowed;
    mask &= (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G);
    if (!mask)
        mask = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G;
    /* a pair the modem already accepted (cold start, before this probing)
     * shows what it supports better than the probing of the AT port */
    if (self->priv->rat_known && (self->priv->rat_allowed & ~mask & MM_MODEM_MODE_5G)) {
        mm_obj_msg (self, "SIM%u generic probing did not report 5G, but the modem accepted rat %u prefer_rat %u: 5G kept",
                    self->priv->sim_slot, self->priv->rat_value, self->priv->rat_prefer);
        mask |= self->priv->rat_allowed & (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G);
    }
    self->priv->rat_mask = mask;

    combinations = mm_mtk_ccci_rat_build_combinations (mask);
    mask_str = mm_modem_mode_build_string_from_mask (mask);
    mm_obj_msg (self, "RAT modes reported: %s; %u allowed/preferred combinations offered",
                mask_str, combinations->len);
    g_task_return_pointer (task, combinations, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
modem_load_supported_modes (MMIfaceModem        *self,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
    iface_modem_parent->load_supported_modes (self,
                                              (GAsyncReadyCallback) parent_load_supported_modes_ready,
                                              g_task_new (self, NULL, callback, user_data));
}

typedef struct {
    MMModemMode allowed;
    MMModemMode preferred;
} RatModes;

static gboolean
modem_load_current_modes_finish (MMIfaceModem  *self,
                                 GAsyncResult  *res,
                                 MMModemMode   *allowed,
                                 MMModemMode   *preferred,
                                 GError       **error)
{
    RatModes *modes = g_task_propagate_pointer (G_TASK (res), error);

    if (!modes)
        return FALSE;
    *allowed = modes->allowed;
    *preferred = modes->preferred;
    g_free (modes);
    return TRUE;
}

static void
load_current_modes_reply (MMBroadbandModemMtkCcci *self,
                          const gchar             *reply,
                          GError                  *error,
                          GTask                   *task)
{
    RatModes *modes = g_new0 (RatModes, 1);
    guint     curr = 0, rat = 0, prefer = 0;

    if (reply && mm_mtk_ccci_parse_erat (reply, &curr, &rat, &prefer)) {
        if (self->priv->rat_known && rat == self->priv->rat_value && prefer == self->priv->rat_prefer) {
            /* the pair we set: report exactly the combination it came from */
            modes->allowed = self->priv->rat_allowed;
            modes->preferred = self->priv->rat_preferred;
        } else if (!mm_mtk_ccci_rat_to_modes (rat, prefer, &modes->allowed, &modes->preferred)) {
            g_free (modes);
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                     "unknown RAT mode in +ERAT: rat %u prefer_rat %u", rat, prefer);
            g_object_unref (task);
            return;
        } else if (self->priv->rat_known) {
            /* the modem's pair wins over what this object last had confirmed */
            mm_obj_msg (self, "SIM%u +ERAT? pair rat %u prefer_rat %u differs from the last confirmed "
                        "rat %u prefer_rat %u; reporting the modem's pair",
                        self->priv->sim_slot, rat, prefer, self->priv->rat_value, self->priv->rat_prefer);
        }
        mm_obj_msg (self, "SIM%u RAT mode: rat %u prefer_rat %u (current RAT %u)",
                    self->priv->sim_slot, rat, prefer, curr);
    } else if (self->priv->rat_known) {
        /* AT+ERAT? unusable: the last pair the modem accepted */
        mm_obj_dbg (self, "AT+ERAT? unusable (%s); reporting the last RAT mode set",
                    error ? error->message : (reply ? reply : "no reply"));
        modes->allowed = self->priv->rat_allowed;
        modes->preferred = self->priv->rat_preferred;
    } else {
        g_free (modes);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "couldn't read the RAT mode: %s",
                                 error ? error->message : (reply ? reply : "no reply"));
        g_object_unref (task);
        return;
    }
    g_task_return_pointer (task, modes, g_free);
    g_object_unref (task);
}

static void
erat_mipc_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError)  error = NULL;
    g_autofree gchar  *reply = mm_port_mipc_at_command_finish (port, res, &error);

    load_current_modes_reply (g_task_get_source_object (task), reply, error, task);
}

static void
erat_at_ready (MMBaseModem *self, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError)  error = NULL;
    const gchar       *reply = mm_base_modem_at_command_finish (self, res, &error);

    load_current_modes_reply (MM_BROADBAND_MODEM_MTK_CCCI (self), reply, error, task);
}

static void
modem_load_current_modes (MMIfaceModem        *_self,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    GTask                   *task = g_task_new (self, NULL, callback, user_data);

    /* slot-addressed, independent of the stack the AT port serves (+ESUO) */
    if (self->priv->mipc && mm_port_mipc_get_session_ready (self->priv->mipc))
        mm_port_mipc_at_command (self->priv->mipc, mm_broadband_modem_mtk_ccci_get_mipc_slot (self),
                                 "AT+ERAT?", 10, (GAsyncReadyCallback) erat_mipc_ready, task);
    else
        mm_base_modem_at_command (MM_BASE_MODEM (self), "+ERAT?", 10, FALSE,
                                  (GAsyncReadyCallback) erat_at_ready, task);
}

typedef struct {
    MMModemMode allowed;
    MMModemMode preferred;
    guint8      rat;
    guint8      prefer;
    gboolean    encoded;   /* rat/prefer hold the pair sent (or to be sent) */
    guint8      slot;      /* the MIPC slot bit the request is addressed to */
} SetRatContext;

/* @rat/@prefer (optional): the pair that was sent, when there was one */
/* the modem accepted a pair with modes the generic probing did not report
 * (e.g. NR when +C5GREG=? did not answer in time): offer them too */
static void
rat_mask_extend (MMBroadbandModemMtkCcci *self, MMModemMode allowed)
{
    MMModemMode       added = allowed & (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G);
    MmGdbusModem     *skeleton = NULL;
    g_autoptr(GArray) combinations = NULL;
    g_autofree gchar *added_str = NULL;

    /* not probed yet: the probing result is extended when it comes */
    if (!self->priv->rat_mask || !(added & ~self->priv->rat_mask))
        return;
    added &= ~self->priv->rat_mask;
    self->priv->rat_mask |= added;
    combinations = mm_mtk_ccci_rat_build_combinations (self->priv->rat_mask);
    added_str = mm_modem_mode_build_string_from_mask (added);
    mm_obj_msg (self, "SIM%u the modem accepted '%s', which the generic probing did not report: "
                "%u allowed/preferred combinations offered now", self->priv->sim_slot, added_str, combinations->len);
    g_object_get (self, MM_IFACE_MODEM_DBUS_SKELETON, &skeleton, NULL);
    if (skeleton) {
        mm_gdbus_modem_set_supported_modes (skeleton, mm_common_mode_combinations_garray_to_variant (combinations));
        g_object_unref (skeleton);
    }
}

static gboolean
set_rat_finish (MMBroadbandModemMtkCcci  *self,
                GAsyncResult             *res,
                gboolean                 *encoded,
                guint8                   *rat,
                guint8                   *prefer,
                GError                  **error)
{
    SetRatContext *ctx = g_task_get_task_data (G_TASK (res));

    if (encoded)
        *encoded = ctx->encoded;
    if (rat)
        *rat = ctx->rat;
    if (prefer)
        *prefer = ctx->prefer;
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_rat_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    SetRatContext           *ctx = g_task_get_task_data (task);
    g_autoptr(MMMipcCnf)     cnf = NULL;
    GError                  *error = NULL;

    gsize                    vlen = 0;

    cnf = mm_port_mipc_request_finish (port, res, &error);
    if (!cnf) {
        /* no confirm (timeout, port gone): nothing proves a change, the
         * last confirmed pair stays */
        trace_event (self, ctx->slot, NULL, "RAT mode rat %u prefer_rat %u: no confirm: %s",
                     ctx->rat, ctx->prefer, error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* Only NW_SET_RAT_CNF 0x0428 of this request's slot with result 0 is an
     * acceptance. The port completes a request with the confirm of its own
     * transaction id, confirm id and slot only (a late confirm of another
     * slot or of an earlier request is dropped there); id and slot are
     * checked here once more. */
    if (cnf->msg_id != MM_MIPC_NW_SET_RAT_CNF || cnf->slot != ctx->slot) {
        trace_event (self, ctx->slot, NULL, "RAT mode rat %u prefer_rat %u: unexpected confirm 0x%04x for slot 0x%02x",
                     ctx->rat, ctx->prefer, cnf->msg_id, cnf->slot);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "NW_SET_RAT rat %u prefer_rat %u: unexpected confirm 0x%04x for slot 0x%02x",
                                 ctx->rat, ctx->prefer, cnf->msg_id, cnf->slot);
        g_object_unref (task);
        return;
    }
    if (cnf->result) {
        /* MM_MIPC_RESULT_MISSING is 2 as well: say which one it was */
        trace_event (self, ctx->slot, NULL, "RAT mode rat %u prefer_rat %u refused: NW_SET_RAT_CNF result %u%s",
                     ctx->rat, ctx->prefer, cnf->result,
                     mm_mipc_tlv_find (cnf->payload->data, cnf->payload->len, MM_MIPC_T_RESULT, &vlen) ?
                         "" : " (no result TLV)");
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "NW_SET_RAT rat %u prefer_rat %u: result %u%s",
                                 ctx->rat, ctx->prefer, cnf->result,
                                 mm_mipc_tlv_find (cnf->payload->data, cnf->payload->len, MM_MIPC_T_RESULT, &vlen) ?
                                     "" : " (no result TLV in the confirm)");
        g_object_unref (task);
        return;
    }
    self->priv->rat_known = TRUE;
    self->priv->rat_session_confirmed = TRUE;
    self->priv->rat_value = ctx->rat;
    self->priv->rat_prefer = ctx->prefer;
    self->priv->rat_allowed = ctx->allowed;
    self->priv->rat_preferred = ctx->preferred;
    mm_obj_msg (self, "SIM%u RAT mode set: rat %u prefer_rat %u",
                mm_broadband_modem_mtk_ccci_get_mipc_slot (self), ctx->rat, ctx->prefer);
    trace_event (self, ctx->slot, NULL, "RAT mode confirmed (NW_SET_RAT_CNF result 0): rat %u prefer_rat %u",
                 ctx->rat, ctx->prefer);
    rat_mask_extend (self, ctx->allowed);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* NW_SET_RAT_REQ on the selected slot; ANY/NONE means the modem's start-up default */
static void
set_rat (MMBroadbandModemMtkCcci *self,
         MMModemMode              allowed,
         MMModemMode              preferred,
         GAsyncReadyCallback      callback,
         gpointer                 user_data)
{
    GTask                *task = g_task_new (self, NULL, callback, user_data);
    SetRatContext        *ctx = g_new0 (SetRatContext, 1);
    g_autoptr(GByteArray) tlvs = NULL;

    g_task_set_task_data (task, ctx, g_free);
    ctx->slot = mm_broadband_modem_mtk_ccci_get_mipc_slot (self);
    /* ANY/NONE is encoded as such: the automatic pair does not depend on
     * the probed modes */
    if (!mm_mtk_ccci_rat_from_modes (self->priv->rat_mask, allowed, preferred, &ctx->rat, &ctx->prefer)) {
        g_autofree gchar *allowed_str = mm_modem_mode_build_string_from_mask (allowed);
        g_autofree gchar *preferred_str = mm_modem_mode_build_string_from_mask (preferred);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "Requested mode (allowed: '%s', preferred: '%s') not supported by the modem",
                                 allowed_str, preferred_str);
        g_object_unref (task);
        return;
    }
    if (allowed == MM_MODEM_MODE_ANY && preferred == MM_MODEM_MODE_NONE)
        mm_mtk_ccci_rat_default (&allowed, &preferred);
    ctx->allowed = allowed;
    ctx->preferred = preferred;
    if (!self->priv->mipc || !mm_port_mipc_get_session_ready (self->priv->mipc)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "MIPC session not ready");
        g_object_unref (task);
        return;
    }
    ctx->encoded = TRUE;
    tlvs = mm_mipc_build_set_rat (ctx->rat, ctx->prefer);
    mm_port_mipc_request (self->priv->mipc, MM_MIPC_NW_SET_RAT_REQ, ctx->slot, tlvs, 15, NULL,
                          (GAsyncReadyCallback) set_rat_ready, task);
}

static gboolean
modem_set_current_modes_finish (MMIfaceModem  *self,
                                GAsyncResult  *res,
                                GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_current_modes_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    /* a refused explicit request: the error to the client, the last
     * confirmed pair (and CurrentModes) unchanged */
    if (!set_rat_finish (self, res, NULL, NULL, NULL, &error))
        g_task_return_error (task, error);
    else {
        /* ANY/NONE returns to the default: not a choice to keep */
        self->priv->rat_user_set = !GPOINTER_TO_INT (g_task_get_task_data (task));
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
modem_set_current_modes (MMIfaceModem        *_self,
                         MMModemMode          allowed,
                         MMModemMode          preferred,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    GTask                   *task = g_task_new (self, NULL, callback, user_data);

    g_task_set_task_data (task,
                          GINT_TO_POINTER (allowed == MM_MODEM_MODE_ANY && preferred == MM_MODEM_MODE_NONE),
                          NULL);
    set_rat (self, allowed, preferred, (GAsyncReadyCallback) set_current_modes_ready, task);
}

/* Before the radio comes up (cold start) and at every enable: the automatic
 * mode 22/128 (whatever the generic probing found), or what a client chose
 * for this modem object. Never fatal: the modem then keeps the mode it has.
 * A pair this object had confirmed in the same MIPC session (the cold start
 * just before enabling) is not sent again. */
static void
initial_rat_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task);
static void
initial_rat_continue (MMBroadbandModemMtkCcci *self, GTask *task);

static void
apply_initial_rat (MMBroadbandModemMtkCcci *self, GTask *task)
{
    MMModemMode allowed = MM_MODEM_MODE_ANY;
    MMModemMode preferred = MM_MODEM_MODE_NONE;
    guint8      rat, prefer;

    if (!self->priv->start_rat) {
        trace_event (self, mm_broadband_modem_mtk_ccci_get_mipc_slot (self), NULL,
                     "start-up step RAT setting: off (ID_MM_MTK_CCCI_START_RAT=0)");
        trace_watch (self);
        initial_rat_continue (self, task);
        return;
    }
    if (self->priv->rat_user_set) {
        allowed = self->priv->rat_allowed;
        preferred = self->priv->rat_preferred;
    }
    if (self->priv->rat_session_confirmed && self->priv->rat_known &&
        mm_mtk_ccci_rat_from_modes (self->priv->rat_mask, allowed, preferred, &rat, &prefer) &&
        rat == self->priv->rat_value && prefer == self->priv->rat_prefer) {
        trace_event (self, mm_broadband_modem_mtk_ccci_get_mipc_slot (self), NULL,
                     "RAT mode rat %u prefer_rat %u already confirmed in this MIPC session: not sent again",
                     rat, prefer);
        trace_watch (self);
        initial_rat_continue (self, task);
        return;
    }
    set_rat (self, allowed, preferred, (GAsyncReadyCallback) initial_rat_ready, task);
}

/*****************************************************************************/
/* After power up (Modem interface): the stack and the voice initialisation */

/* once per boot (per stack); none of them is fatal */
static const MMBaseModemAtCommand voice_init_sequence[] = {
    { "+CMOD=0",          5, FALSE, NULL },
    { "S0=0",             5, FALSE, NULL },
    { "+CCWA=1",          5, FALSE, NULL },
    { "+CLIP=1",          5, FALSE, NULL },
    { "+CSSN=1,1",        5, FALSE, NULL },
    { "+CMUT=0",          5, FALSE, NULL },
    { "+ECPI=4294967295", 5, FALSE, NULL },
    { "+EAIC=2",          5, FALSE, NULL },
    { "+EVOCD=1",         5, FALSE, NULL },
    { NULL }
};

static void
voice_init_ready (MMBaseModem  *self,
                  GAsyncResult *res,
                  GTask        *task)
{
    g_autoptr(GError) error = NULL;

    mm_base_modem_at_sequence_finish (self, res, NULL, &error);
    if (error)
        mm_obj_warn (self, "voice initialisation sequence: %s", error->message);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*****************************************************************************/
/* SIM slots (Modem interface)
 *
 * The modem has two SIM slots but the AT port serves one protocol stack at a
 * time (AT+ESUO=4|5). The primary SIM uses that
 * port; the other SIM is read through slot-addressed SYS_AT without changing
 * ESUO. Both inserted SIMs are active. Changing the primary still reprobes
 * the modem because the generic registration/SMS interface follows ESUO. */

typedef struct {
    GPtrArray *sim_slots;
    guint      primary;
} LoadSimSlotsContext;

static void
load_sim_slots_context_free (LoadSimSlotsContext *ctx)
{
    if (ctx->sim_slots)
        g_ptr_array_unref (ctx->sim_slots);
    g_slice_free (LoadSimSlotsContext, ctx);
}

static gboolean
modem_load_sim_slots_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GPtrArray    **sim_slots,
                             guint         *primary_sim_slot,
                             GError       **error)
{
    LoadSimSlotsContext *ctx;

    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;
    ctx = g_task_get_task_data (G_TASK (res));
    if (sim_slots)
        *sim_slots = g_ptr_array_ref (ctx->sim_slots);
    if (primary_sim_slot)
        *primary_sim_slot = ctx->primary;
    return TRUE;
}

static void
sim_slot_unref (gpointer sim)
{
    if (sim)
        g_object_unref (sim);
}

static void
other_sim_new_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    LoadSimSlotsContext *ctx = g_task_get_task_data (task);
    GError *error = NULL;
    MMBaseSim *sim = mm_sim_mtk_ccci_new_finish (res, &error);
    if (error) {
        /* A timeout or protocol failure must not be advertised as an empty
         * slot. Let MM retry initialization rather than inventing a SIM. */
        g_prefix_error (&error, "could not read SIM%u: ", ctx->primary == 1 ? 2 : 1);
        g_task_return_error (task, error);
    } else {
        g_ptr_array_index (ctx->sim_slots, ctx->primary == 1 ? 1 : 0) = sim;
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
active_sim_new_ready (GObject      *source,
                      GAsyncResult *res,
                      GTask        *task)
{
    MMBroadbandModemMtkCcci *self;
    LoadSimSlotsContext     *ctx;
    GError                  *error = NULL;
    MMBaseSim               *active;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    active = mm_base_sim_new_finish (res, &error);
    if (!active) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_object_set (active, MM_BASE_SIM_SLOT_NUMBER, ctx->primary, NULL);

    ctx->sim_slots = g_ptr_array_new_full (N_SIM_SLOTS, sim_slot_unref);
    g_ptr_array_set_size (ctx->sim_slots, N_SIM_SLOTS);
    g_ptr_array_index (ctx->sim_slots, ctx->primary - 1) = active;
    mm_sim_mtk_ccci_new (MM_BASE_MODEM (self), self->priv->mipc, ctx->primary == 1 ? 2 : 1,
                          (GAsyncReadyCallback) other_sim_new_ready, task);
}

static void
modem_load_sim_slots (MMIfaceModem        *_self,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    GTask                   *task;
    LoadSimSlotsContext     *ctx;

    task = g_task_new (self, NULL, callback, user_data);
    if (self->priv->dual) {
        /* one SIM per modem object: ModemManager's generic SIM of the
         * object's own stack, no SimSlots/PrimarySimSlot (DESIGN.md §11) */
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "dual-modem mode: SIM%u is the only SIM of this modem object",
                                 self->priv->sim_slot);
        g_object_unref (task);
        return;
    }
    ctx = g_slice_new0 (LoadSimSlotsContext);
    ctx->primary = self->priv->sim_slot;
    g_task_set_task_data (task, ctx, (GDestroyNotify) load_sim_slots_context_free);

    /* the active slot's SIM is the generic AT SIM (+CIMI, +CRSM, +CPIN ...) */
    mm_base_sim_new (MM_BASE_MODEM (self), G_OBJECT (self), NULL,
                     (GAsyncReadyCallback) active_sim_new_ready, task);
}

static gboolean
modem_set_primary_sim_slot_finish (MMIfaceModem  *self,
                                   GAsyncResult  *res,
                                   GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_set_primary_sim_slot (MMIfaceModem        *_self,
                            guint                sim_slot,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    GTask                   *task;
    GError                  *error = NULL;
    g_autofree gchar        *contents = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    if (self->priv->dual) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "dual-modem mode: each SIM has its own modem object");
        g_object_unref (task);
        return;
    }
    if (sim_slot < 1 || sim_slot > N_SIM_SLOTS) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "SIM slot %u does not exist (1..%u)", sim_slot, N_SIM_SLOTS);
        g_object_unref (task);
        return;
    }
    if (sim_slot == self->priv->sim_slot) {
        /* EXISTS: ModemManager skips the reprobe */
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_EXISTS,
                                 "SIM slot %u is already the primary one", sim_slot);
        g_object_unref (task);
        return;
    }

    contents = g_strdup_printf ("%u\n", sim_slot);
    if (g_mkdir_with_parents (PKGSTATEDIR, 0755) != 0 ||
        !g_file_set_contents (SIM_SLOT_STATE_FILE, contents, -1, &error)) {
        if (!error)
            error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "cannot create %s", PKGSTATEDIR);
        g_prefix_error (&error, "could not store the SIM slot choice: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* the data connection (MIPC slot bit) and the voice init belong to the
     * stack: ModemManager disables and reprobes the modem after this, and the
     * new stack is selected with AT+ESUO at power-up */
    mm_obj_msg (self, "SIM slot %u selected (AT+ESUO=%u after the reprobe)", sim_slot, 3 + sim_slot);
    /* Keep the old stack until teardown finishes. The reprobed instance
     * reads the persisted choice; old PDNs must be deactivated on their slot. */
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*****************************************************************************/
/* Bearers (Modem interface) */

static void
ims_prepare_data_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    /* source is the shared context (the IMS controller's owner) */
    if (!mm_mtk_ccci_ims_prepare_data_finish (res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_broadband_modem_mtk_ccci_prepare_data (MMBroadbandModemMtkCcci *self, const gchar *apn,
                                         GAsyncReadyCallback callback, gpointer user_data)
{
    /* the result's source object is this modem (the bearer's callback takes
     * it as the modem), not the shared IMS controller's owner */
    GTask *task = g_task_new (self, NULL, callback, user_data);
    MMMtkCcciIms *ims = self->priv->ims_enabled ? ims_of (self, self->priv->sim_slot) : NULL;

    if (self->priv->ims_enabled && !ims) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "No IMS controller (no MIPC port)");
        g_object_unref (task);
        return;
    }
    if (ims) {
        mm_mtk_ccci_ims_prepare_data (ims, apn, (GAsyncReadyCallback) ims_prepare_data_ready, task);
        return;
    }
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

gboolean
mm_broadband_modem_mtk_ccci_prepare_data_finish (MMBroadbandModemMtkCcci *self,
                                               GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static MMBaseBearer *
modem_create_bearer_finish (MMIfaceModem  *self,
                            GAsyncResult  *res,
                            GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
modem_create_bearer (MMIfaceModem        *self,
                     MMBearerProperties  *properties,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    GTask        *task;
    MMBaseBearer *bearer;

    task = g_task_new (self, NULL, callback, user_data);
    if (MM_BROADBAND_MODEM_MTK_CCCI (self)->priv->sim2_role && !MM_BROADBAND_MODEM_MTK_CCCI (self)->priv->sim2_data) {
        /* default: mobile data on SIM1 only; a connection manager
         * profile without a SIM binding must not end up on SIM2 */
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "mobile data is configured for SIM1 only (" ID_MM_MTK_CCCI_SIM2_DATA ")");
        g_object_unref (task);
        return;
    }
    mm_obj_dbg (self, "creating MIPC data bearer...");
    bearer = mm_bearer_mtk_ccci_new (MM_BROADBAND_MODEM_MTK_CCCI (self), properties);
    g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

/*****************************************************************************/
/* Vendor URCs on the AT port */

static void
eind_received (MMPortSerialAt          *port,
               GMatchInfo              *info,
               MMBroadbandModemMtkCcci *self)
{
    g_autofree gchar *str = NULL;

    str = g_match_info_fetch (info, 1);
    mm_obj_dbg (self, "vendor indication: %s", str);
}

static void
ims_urc_received (MMPortSerialAt          *port,
                  GMatchInfo              *info,
                  MMBroadbandModemMtkCcci *self)
{
    g_autofree gchar *str = NULL;


    str = g_match_info_fetch (info, 1);
    mm_obj_dbg (self, "IMS report: %s", str);
    mm_mtk_ccci_ims_urc (ims_of (self, self->priv->sim_slot), str);
}

/* +ESPEECH: <speech_on>,<x>,<y>: only the first field is used */
static void
espeech_received (MMPortSerialAt          *port,
                  GMatchInfo              *info,
                  MMBroadbandModemMtkCcci *self)
{
    guint on = 0;

    if (!mm_get_uint_from_match_info (info, 1, &on))
        return;
    self->priv->speech_on = (on != 0);
    /* the speech path is switched on MM's in-call state (Android's timing), this
     * is the modem's view of it */
    mm_obj_msg (self, "modem speech path %s", on ? "on" : "off");
}

/* +EVOCD: <codec> */
static void
evocd_received (MMPortSerialAt          *port,
                GMatchInfo              *info,
                MMBroadbandModemMtkCcci *self)
{
    guint codec = 0;

    if (mm_get_uint_from_match_info (info, 1, &codec))
        mm_obj_dbg (self, "speech codec %u", codec);
}

/*****************************************************************************/
/* Voice: +ECPI call progress */

typedef enum {
    ECPI_SETUP           = 0,
    ECPI_ALERTING        = 2,
    ECPI_CONNECTED       = 6,
    ECPI_ALL_RELEASED    = 7,
    ECPI_CREATED         = 130,
    ECPI_HELD_OR_ACTIVE1 = 131,
    ECPI_HELD_OR_ACTIVE2 = 132,
    ECPI_DISCONNECTED    = 133,
} EcpiMsgType;

static void
reload_all_calls_ready (MMIfaceModemVoice *self,
                        GAsyncResult      *res)
{
    g_autoptr(GError) error = NULL;

    if (!mm_iface_modem_voice_reload_all_calls_finish (self, res, &error))
        mm_obj_warn (self, "could not reload the call list: %s", error->message);
}

static void
ecpi_received (MMPortSerialAt          *port,
               GMatchInfo              *info,
               MMBroadbandModemMtkCcci *self)
{
    guint                call_id = 0, msg_type = 0;
    g_autofree gchar    *rest = NULL;
    g_auto(GStrv)        fields = NULL;
    guint                n_fields = 0;
    MMCallInfo           call_info;
    g_autofree gchar    *number = NULL;

    if (!mm_get_uint_from_match_info (info, 1, &call_id) ||
        !mm_get_uint_from_match_info (info, 2, &msg_type))
        return;

    /* <is_ibt>,<is_tch>,<dir>,<call_mode>,"<number>",<type>,"<pau>"[,<cause>] */
    rest = g_match_info_fetch (info, 3);
    if (rest && rest[0]) {
        fields = g_strsplit (rest, ",", -1);
        n_fields = g_strv_length (fields);
    }
    if (n_fields >= 5 && fields[4][0]) {
        number = g_strdup (fields[4]);
        mm_strip_quotes (number);
        if (!number[0])
            g_clear_pointer (&number, g_free);
    }
    if (n_fields >= 4) {
        guint call_mode = (guint) strtoul (fields[3], NULL, 10);

        if (call_mode >= 20 && call_mode <= 25)
            mm_obj_dbg (self, "call %u is an IMS call (mode %u)", call_id, call_mode);
    }

    mm_obj_dbg (self, "call progress: id %u type %u%s%s", call_id, msg_type,
                number ? " number " : "", number ? number : "");

    call_info.index     = call_id;
    call_info.direction = MM_CALL_DIRECTION_UNKNOWN;
    call_info.state     = MM_CALL_STATE_UNKNOWN;
    call_info.number    = number;

    switch (msg_type) {
    case ECPI_CREATED:
        /* the modem accepted our dial and assigned the id */
        call_info.direction = MM_CALL_DIRECTION_OUTGOING;
        call_info.state     = MM_CALL_STATE_DIALING;
        break;
    case ECPI_ALERTING:
        call_info.direction = MM_CALL_DIRECTION_OUTGOING;
        call_info.state     = MM_CALL_STATE_RINGING_OUT;
        break;
    case ECPI_SETUP:
        /* an incoming call appears (after the +EAIC approval, with RING/+CLIP) */
        call_info.direction = MM_CALL_DIRECTION_INCOMING;
        call_info.state     = MM_CALL_STATE_RINGING_IN;
        break;
    case ECPI_CONNECTED:
        call_info.state = MM_CALL_STATE_ACTIVE;
        break;
    case ECPI_DISCONNECTED:
        if (n_fields >= 8)
            mm_obj_msg (self, "call %u disconnected, cause %s", call_id, fields[7]);
        call_info.state = MM_CALL_STATE_TERMINATED;
        break;
    case ECPI_ALL_RELEASED:
    case ECPI_HELD_OR_ACTIVE1:
    case ECPI_HELD_OR_ACTIVE2:
        /* which of 131/132 is held and which active is unverified: ask +CLCC */
        mm_iface_modem_voice_reload_all_calls (MM_IFACE_MODEM_VOICE (self),
                                               (GAsyncReadyCallback) reload_all_calls_ready, NULL);
        return;
    default:
        /* 1, 3, 4, 5, 129, 134..137: progress/sync, not forwarded by Android's RIL either */
        return;
    }

    mm_iface_modem_voice_report_call (MM_IFACE_MODEM_VOICE (self), &call_info);
}

/*****************************************************************************/
/* Voice: +EAIC incoming call approval */

static void
eaic_answer_ready (MMBaseModem  *self,
                   GAsyncResult *res)
{
    g_autoptr(GError) error = NULL;

    if (!mm_base_modem_at_command_finish (self, res, &error))
        mm_obj_warn (self, "incoming call approval failed: %s", error->message);
}

static void
eaic_received (MMPortSerialAt          *port,
               GMatchInfo              *info,
               MMBroadbandModemMtkCcci *self)
{
    guint             call_id = 0, seq = 0;
    g_autofree gchar *number = NULL;
    g_autofree gchar *cmd = NULL;

    if (!mm_get_uint_from_match_info (info, 1, &call_id) ||
        !mm_get_uint_from_match_info (info, 5, &seq))
        return;
    number = g_match_info_fetch (info, 2);
    mm_obj_msg (self, "incoming call indication: id %u from %s", call_id, number);

    /* The call does not ring until the AP answers; mode 0 = let it proceed
     * (the modem then sends RING, +CLIP, +ECPI), 1 = reject with cause 21
     *. Policy as for CALL_APPROVE_INCOMING_CMD
     * (shared context): approve, except the DSDS case; the user rejects a
     * ringing call through the Call object as with any modem. This port
     * serves the stack of the selected SIM (+ESUO). */
    if (self->priv->shared &&
        !mm_mtk_ccci_shared_incoming_decide (self->priv->shared, (guint8) self->priv->sim_slot,
                                             call_id, seq, number))
        cmd = g_strdup_printf ("+EAIC=1,%u,%u,21", call_id, seq);
    else
        cmd = g_strdup_printf ("+EAIC=0,%u,%u", call_id, seq);
    mm_base_modem_at_command_full (MM_BASE_MODEM (self), MM_IFACE_PORT_AT (port), cmd, 5,
                                   FALSE, FALSE, NULL,
                                   (GAsyncReadyCallback) eaic_answer_ready, NULL);
}

/*****************************************************************************/
/* Unsolicited event handlers (3GPP and Voice interfaces) */

static void
set_vendor_unsolicited_events_handlers (MMBroadbandModemMtkCcci *self,
                                        gboolean                 enable)
{
    MMPortSerialAt *ports[2];
    guint           i;

    ports[0] = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));

    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (!ports[i])
            continue;
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i], self->priv->eind_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn) eind_received : NULL,
            enable ? self : NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i], self->priv->ims_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn) ims_urc_received : NULL,
            enable ? self : NULL, NULL);
    }
}

static void
set_voice_unsolicited_events_handlers (MMBroadbandModemMtkCcci *self,
                                       gboolean                 enable)
{
    MMPortSerialAt *ports[2];
    guint           i;

    ports[0] = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));

    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (!ports[i])
            continue;
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i], self->priv->ecpi_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn) ecpi_received : NULL,
            enable ? self : NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i], self->priv->eaic_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn) eaic_received : NULL,
            enable ? self : NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i], self->priv->espeech_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn) espeech_received : NULL,
            enable ? self : NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i], self->priv->evocd_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn) evocd_received : NULL,
            enable ? self : NULL, NULL);
    }
}

/* --- 3GPP --- */

static gboolean
modem_3gpp_setup_cleanup_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                                    GAsyncResult      *res,
                                                    GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_3gpp_setup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                            GAsyncResult     *res,
                                            GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->setup_unsolicited_events_finish (self, res, &error))
        g_task_return_error (task, error);
    else {
        set_vendor_unsolicited_events_handlers (MM_BROADBAND_MODEM_MTK_CCCI (self), TRUE);
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
modem_3gpp_setup_unsolicited_events (MMIfaceModem3gpp    *self,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
    iface_modem_3gpp_parent->setup_unsolicited_events (
        self,
        (GAsyncReadyCallback) parent_3gpp_setup_unsolicited_events_ready,
        g_task_new (self, NULL, callback, user_data));
}

static void
parent_3gpp_cleanup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                              GAsyncResult     *res,
                                              GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->cleanup_unsolicited_events_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_3gpp_cleanup_unsolicited_events (MMIfaceModem3gpp    *self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    set_vendor_unsolicited_events_handlers (MM_BROADBAND_MODEM_MTK_CCCI (self), FALSE);
    iface_modem_3gpp_parent->cleanup_unsolicited_events (
        self,
        (GAsyncReadyCallback) parent_3gpp_cleanup_unsolicited_events_ready,
        g_task_new (self, NULL, callback, user_data));
}

/* --- Voice --- */

static gboolean
modem_voice_setup_cleanup_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                                     GAsyncResult       *res,
                                                     GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_voice_setup_unsolicited_events (MMIfaceModemVoice *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    set_voice_unsolicited_events_handlers (MM_BROADBAND_MODEM_MTK_CCCI (self), FALSE);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_voice_cleanup_unsolicited_events (MMIfaceModemVoice *self, GAsyncReadyCallback callback, gpointer user_data)
{
    modem_voice_setup_unsolicited_events (self, callback, user_data);
}

typedef struct { guint slot_index, command_index; } VoiceInit;
static const gchar *dual_voice_commands[] = {
    "AT+CPIN?", "AT+EURCRPT=2", "AT+CMOD=0", "ATS0=0", "AT+CCWA=1", "AT+CLIP=1",
    "AT+CSSN=1,1", "AT+CMUT=0", "AT+ECPI=4294967295", "AT+EAIC=2", "AT+EVOCD=1"
};
static void dual_voice_step (GTask *task);

static void
dual_voice_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    VoiceInit *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    if (!ctx->command_index) {
        self->priv->sim_ready[ctx->slot_index] = reply && strstr (reply, "READY");
        if (!self->priv->sim_ready[ctx->slot_index]) {
            mm_obj_warn (self, "SIM%u voice unavailable: %s", ctx->slot_index + 1,
                         error ? error->message : (reply ? reply : "unknown"));
            ctx->slot_index++;
            ctx->command_index = 0;
            dual_voice_step (task);
            return;
        }
    } else if (error)
        mm_obj_warn (self, "SIM%u %s: %s", ctx->slot_index + 1,
                     dual_voice_commands[ctx->command_index], error->message);
    ctx->command_index++;
    if (ctx->command_index == G_N_ELEMENTS (dual_voice_commands)) {
        ctx->slot_index++;
        ctx->command_index = 0;
    }
    dual_voice_step (task);
}

static void
dual_voice_step (GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    VoiceInit *ctx = g_task_get_task_data (task);
    if (!mm_port_mipc_get_session_ready (self->priv->mipc)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "MIPC session lost during voice setup");
        g_object_unref (task);
        return;
    }
    /* the slots this object serves (both in the single-object mode) */
    while (ctx->slot_index < 2 && !(self->priv->served & (1u << ctx->slot_index))) {
        ctx->slot_index++;
        ctx->command_index = 0;
    }
    if (ctx->slot_index == 2) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    mm_port_mipc_at_command (self->priv->mipc, 1u << ctx->slot_index,
        dual_voice_commands[ctx->command_index], 5, (GAsyncReadyCallback) dual_voice_ready, task);
}

static gboolean
modem_voice_enable_unsolicited_events_finish (MMIfaceModemVoice *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_voice_enable_unsolicited_events (MMIfaceModemVoice *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, g_new0 (VoiceInit, 1), g_free);
    dual_voice_step (task);
}

/*****************************************************************************/
/* Messaging: new-SMS notices over MIPC
 *
 * Observed on mainline (2026-10-30): with
 * +CNMI=2,1,2,1,0 set on wwan0at0 only, an incoming SMS-DELIVER was stored in
 * SM and nothing came on wwan0at0 or MIPC. The URCs of this modem arrive on
 * the MIPC AT channel (SYS_AT_IND per slot, after AT+EURCRPT=2 there), so the
 * same 27.005 +CNMI goes to that channel of both slots as well: <mt>=1 keeps
 * "store, then indicate the index" (+CMTI). In addition the SMS-group
 * indications that Android's RIL registers (mm-mipc-proto.h) are registered
 * per slot. Both steps are best effort: the generic CNMI result decides. */

static const guint16 sms_indications[] = {
    MM_MIPC_SMS_CFG_IND,
    MM_MIPC_SMS_NEW_SMS_IND,
    MM_MIPC_SMS_STORE_STATUS_IND,
    MM_MIPC_SMS_NEW_STATUS_REPORT_IND,
};
#define SMS_TUNNEL_CNMI "AT+CNMI=2,1,2,1,0"

typedef struct {
    GError *parent_error;
    guint   step;   /* 0..1 CNMI per slot, then the registrations slot x id */
} SmsEnableContext;

static void
sms_enable_context_free (SmsEnableContext *ctx)
{
    g_clear_error (&ctx->parent_error);
    g_free (ctx);
}

static void sms_enable_step (GTask *task);

static void
sms_tunnel_cnmi_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    SmsEnableContext *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);

    if (error)
        mm_obj_warn (self, "SIM%u %s on the MIPC AT channel: %s", ctx->step + 1, SMS_TUNNEL_CNMI, error->message);
    else
        mm_obj_msg (self, "SIM%u new-SMS indications on the MIPC AT channel: %s", ctx->step + 1, SMS_TUNNEL_CNMI);
    ctx->step++;
    sms_enable_step (task);
}

static void
sms_register_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    SmsEnableContext *ctx = g_task_get_task_data (task);
    guint i = ctx->step - 2, n = G_N_ELEMENTS (sms_indications);
    g_autoptr(GError) error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    if (!cnf || cnf->result)
        mm_obj_warn (self, "SIM%u MIPC registration 0x%04x: %s (result %u)",
                     i / n + 1, sms_indications[i % n],
                     error ? error->message : "refused", cnf ? cnf->result : 0);
    else if (i % n == n - 1)
        mm_obj_msg (self, "SIM%u MIPC SMS indications registered", i / n + 1);
    ctx->step++;
    sms_enable_step (task);
}

static void
sms_enable_step (GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    SmsEnableContext *ctx = g_task_get_task_data (task);
    guint n_reg = N_SIM_SLOTS * G_N_ELEMENTS (sms_indications);

    if (!self->priv->mipc || !mm_port_mipc_get_session_ready (self->priv->mipc)) {
        if (ctx->step < 2 + n_reg)
            mm_obj_warn (self, "no MIPC session: new-SMS notices not enabled, the fallback scan only");
        ctx->step = 2 + n_reg;
    }
    /* the slots this object serves only (both in the single-object mode) */
    while ((ctx->step < 2 && !(self->priv->served & (1u << ctx->step))) ||
           (ctx->step >= 2 && ctx->step < 2 + n_reg &&
            !(self->priv->served & (1u << ((ctx->step - 2) / G_N_ELEMENTS (sms_indications))))))
        ctx->step++;
    if (ctx->step < 2) {
        mm_port_mipc_at_command (self->priv->mipc, (guint8) (1u << ctx->step), SMS_TUNNEL_CNMI, 5,
                                 (GAsyncReadyCallback) sms_tunnel_cnmi_ready, task);
        return;
    }
    if (ctx->step < 2 + n_reg) {
        guint i = ctx->step - 2;
        g_autoptr(GByteArray) tlvs = g_byte_array_new ();

        mm_mipc_tlv_add_u16 (tlvs, MM_MIPC_T_REGISTER_IND_ID, sms_indications[i % G_N_ELEMENTS (sms_indications)]);
        mm_port_mipc_request (self->priv->mipc, MM_MIPC_INTERNAL_REGISTER_IND_REQ,
                              (guint8) (1u << (i / G_N_ELEMENTS (sms_indications))), tlvs, 5, NULL,
                              (GAsyncReadyCallback) sms_register_ready, task);
        return;
    }
    if (ctx->parent_error)
        g_task_return_error (task, g_steal_pointer (&ctx->parent_error));
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_messaging_enable_ready (MMIfaceModemMessaging *self, GAsyncResult *res, GTask *task)
{
    SmsEnableContext *ctx = g_task_get_task_data (task);

    iface_modem_messaging_parent->enable_unsolicited_events_finish (self, res, &ctx->parent_error);
    sms_enable_step (task);
}

static gboolean
modem_messaging_enable_unsolicited_events_finish (MMIfaceModemMessaging *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_messaging_enable_unsolicited_events (MMIfaceModemMessaging *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    g_task_set_task_data (task, g_new0 (SmsEnableContext, 1), (GDestroyNotify) sms_enable_context_free);
    /* the generic +CNMI on the AT port first (its result is the result) */
    iface_modem_messaging_parent->enable_unsolicited_events (
        self, (GAsyncReadyCallback) parent_messaging_enable_ready, task);
}

/*****************************************************************************/
/* Voice: call list (+CLCC)
 *
 * The modem reports IMS (VoLTE) calls with <mode> 20..25 (observed on the
 * phone: "+CLCC: 1,0,2,20,0,...,145" for a dialing
 * call). ModemManager's generic parser lists voice calls (mode 0) only and
 * would terminate the call object, so the mode is rewritten before parsing. */

static gboolean
modem_voice_load_call_list_finish (MMIfaceModemVoice  *self,
                                   GAsyncResult       *res,
                                   GList             **out_call_info_list,
                                   GError            **error)
{
    GList *call_info_list;

    call_info_list = g_task_propagate_pointer (G_TASK (res), error);
    if (g_task_had_error (G_TASK (res)))
        return FALSE;
    *out_call_info_list = call_info_list;
    return TRUE;
}

typedef struct { guint slot; GList *calls; } CallsContext;
static void calls_step (GTask *task);
static void calls_context_free (CallsContext *ctx)
{
    mm_3gpp_call_info_list_free (ctx->calls);
    g_free (ctx);
}
static void
clcc_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    CallsContext *ctx = g_task_get_task_data (task);
    GError *error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    g_autofree gchar *normalized = reply ? mm_mtk_ccci_clcc_normalize (reply) : NULL;
    GList *calls = NULL, *l;
    if (!reply || !mm_3gpp_parse_clcc_response (normalized, self, &calls, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    for (l = calls; l; l = l->next) {
        MMCallInfo *info = l->data;
        info->index = mm_mtk_ccci_call_index (1u << ctx->slot, info->index);
        mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (self), port, 1u << ctx->slot, info);
        /* The generic reconciliation must use our stable index, not a
         * caller number that may be identical on the other SIM. */
        g_clear_pointer (&info->number, g_free);
    }
    ctx->calls = g_list_concat (ctx->calls, calls);
    ctx->slot++;
    calls_step (task);
}
static void calls_step (GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    CallsContext *ctx = g_task_get_task_data (task);
    while (ctx->slot < 2 && (!(self->priv->served & (1u << ctx->slot)) || !self->priv->sim_ready[ctx->slot]))
        ctx->slot++;
    if (ctx->slot == 2) {
        GList *calls = ctx->calls;
        ctx->calls = NULL;
        g_task_return_pointer (task, calls, (GDestroyNotify) mm_3gpp_call_info_list_free);
        g_object_unref (task);
        return;
    }
    mm_port_mipc_at_command (self->priv->mipc, 1u << ctx->slot, "AT+CLCC", 5,
                              (GAsyncReadyCallback) clcc_ready, task);
}
static void modem_voice_load_call_list (MMIfaceModemVoice *self, GAsyncReadyCallback cb, gpointer data)
{
    GTask *task = g_task_new (self, NULL, cb, data);
    g_task_set_task_data (task, g_new0 (CallsContext, 1), (GDestroyNotify) calls_context_free);
    calls_step (task);
}

/*****************************************************************************/
/* Voice: calls */

static MMBaseCall *
modem_voice_create_call (MMIfaceModemVoice *self,
                         MMCallDirection    direction,
                         const gchar       *number,
                         const guint        dtmf_tone_duration)
{
    MMBroadbandModemMtkCcci *modem = MM_BROADBAND_MODEM_MTK_CCCI (self);
    return mm_call_mtk_ccci_new (MM_BASE_MODEM (self), modem->priv->mipc,
        mm_broadband_modem_mtk_ccci_get_mipc_slot (modem), direction, number, dtmf_tone_duration);
}

static gboolean
modem_voice_hangup_all_finish (MMIfaceModemVoice  *self,
                               GAsyncResult       *res,
                               GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void hangup_all_step (GTask *task);
static void hangup_all_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    if (!reply) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task, GUINT_TO_POINTER (GPOINTER_TO_UINT (g_task_get_task_data (task)) + 1), NULL);
    hangup_all_step (task);
}
static void hangup_all_step (GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    guint slot = GPOINTER_TO_UINT (g_task_get_task_data (task));
    while (slot < 2 && (!(self->priv->served & (1u << slot)) || !self->priv->sim_ready[slot])) slot++;
    if (slot == 2) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task, GUINT_TO_POINTER (slot), NULL);
    mm_port_mipc_at_command (self->priv->mipc, 1u << slot, "AT+CHLD=6", 30,
                              (GAsyncReadyCallback) hangup_all_ready, task);
}
static void modem_voice_hangup_all (MMIfaceModemVoice *self, GAsyncReadyCallback cb, gpointer data)
{
    hangup_all_step (g_task_new (self, NULL, cb, data));
}

/*****************************************************************************/
/* Voice: the in-call audio channel = the t2-speechd trigger
 *
 * MM enters the "in-call" state when a call is dialing, ringing out, held or
 * active (not for a ringing incoming call) and leaves it when no call is:
 * exactly Android's timing (speech on when ATD/ATA is accepted, off when
 * the last call is gone). t2-speechd owns the audio
 * path and the modem's speech engine; its line protocol is
 * tools/speechd/README.md ("call-start", "call-stop", reply ends with "ok" or
 * "error: ..."). */

typedef struct {
    gchar *command;
    gchar *reply;
} SpeechdContext;

static void
speechd_context_free (SpeechdContext *ctx)
{
    g_free (ctx->command);
    g_free (ctx->reply);
    g_slice_free (SpeechdContext, ctx);
}

static gboolean
speechd_command_finish (MMBroadbandModemMtkCcci  *self,
                        GAsyncResult             *res,
                        GError                  **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
speechd_reply_ready (GInputStream *stream,
                     GAsyncResult *res,
                     GTask        *task)
{
    MMBroadbandModemMtkCcci *self;
    SpeechdContext          *ctx;
    GError                  *error = NULL;
    g_autoptr(GBytes)        bytes = NULL;
    const gchar             *data;
    gsize                    len;


    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    bytes = g_input_stream_read_bytes_finish (stream, res, &error);
    if (!bytes) {
        g_prefix_error (&error, "t2-speechd %s: ", ctx->command);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    data = g_bytes_get_data (bytes, &len);
    switch (mm_mtk_ccci_speech_reply_feed (&ctx->reply, (const guint8 *) data, len, &error)) {
    case 1:
        mm_obj_dbg (self, "t2-speechd %s: %s", ctx->command, ctx->reply);
        g_task_return_boolean (task, TRUE);
        break;
    case 0:
        g_input_stream_read_bytes_async (stream, 4096, G_PRIORITY_DEFAULT, g_task_get_cancellable (task),
                                         (GAsyncReadyCallback) speechd_reply_ready, task);
        return;
    default:
        g_prefix_error (&error, "t2-speechd %s: ", ctx->command);
        g_task_return_error (task, error);
        break;
    }
    g_object_unref (task);
}

static void
speechd_connect_ready (GSocketClient *client,
                       GAsyncResult  *res,
                       GTask         *task)
{
    SpeechdContext             *ctx;
    GError                     *error = NULL;
    g_autoptr(GSocketConnection) conn = NULL;
    g_autofree gchar           *line = NULL;

    ctx = g_task_get_task_data (task);

    conn = g_socket_client_connect_finish (client, res, &error);
    if (!conn) {
        g_prefix_error (&error, "t2-speechd %s: ", ctx->command);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    line = g_strdup_printf ("%s\n", ctx->command);
    if (!g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (conn)),
                                    line, strlen (line), NULL, NULL, &error)) {
        g_prefix_error (&error, "t2-speechd %s: ", ctx->command);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* keep the connection alive until the reply is read */
    g_object_set_data_full (G_OBJECT (task), "connection", g_steal_pointer (&conn), g_object_unref);
    g_input_stream_read_bytes_async (g_io_stream_get_input_stream (G_IO_STREAM (g_object_get_data (G_OBJECT (task), "connection"))),
                                     4096, G_PRIORITY_DEFAULT, g_task_get_cancellable (task),
                                     (GAsyncReadyCallback) speechd_reply_ready, task);
}

static void
speechd_command (MMBroadbandModemMtkCcci *self,
                 const gchar             *command,
                 GCancellable            *cancellable,
                 GAsyncReadyCallback      callback,
                 gpointer                 user_data)
{
    GTask                     *task;
    SpeechdContext            *ctx;
    g_autoptr(GSocketClient)   client = NULL;
    g_autoptr(GSocketAddress)  address = NULL;

    task = g_task_new (self, cancellable, callback, user_data);
    ctx = g_slice_new0 (SpeechdContext);
    ctx->command = g_strdup (command);
    g_task_set_task_data (task, ctx, (GDestroyNotify) speechd_context_free);

    if (!self->priv->speechd_socket || !self->priv->speechd_socket[0]) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "speech trigger disabled (no t2-speechd socket configured)");
        g_object_unref (task);
        return;
    }

    client  = g_socket_client_new ();
    g_socket_client_set_timeout (client, 5);
    address = g_unix_socket_address_new (self->priv->speechd_socket);
    g_socket_client_connect_async (client, G_SOCKET_CONNECTABLE (address), cancellable,
                                   (GAsyncReadyCallback) speechd_connect_ready, task);
}

static gboolean
modem_voice_setup_in_call_audio_channel_finish (MMIfaceModemVoice  *self,
                                                GAsyncResult       *res,
                                                MMPort            **audio_port,
                                                MMCallAudioFormat **audio_format,
                                                GError            **error)
{
    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;
    /* the voice samples never pass through the AP: no audio port, no format */
    if (audio_port)
        *audio_port = NULL;
    if (audio_format)
        *audio_format = NULL;
    return TRUE;
}

static void
speechd_in_call_ready (MMBroadbandModemMtkCcci *self,
                       GAsyncResult            *res,
                       GTask                   *task)
{
    g_autoptr(GError) error = NULL;

    /* a missing or failing t2-speechd must not break the call itself */
    if (!speechd_command_finish (self, res, &error))
        mm_obj_warn (self, "voice audio path: %s", error->message);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_voice_setup_in_call_audio_channel (MMIfaceModemVoice   *self,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
    MMBroadbandModemMtkCcci *modem = MM_BROADBAND_MODEM_MTK_CCCI (self);

    /* one speech path for both SIMs: the first call switches it on */
    if (modem->priv->shared && mm_mtk_ccci_shared_speech_update (modem->priv->shared, modem->priv->served, TRUE) != 1) {
        GTask *task = g_task_new (self, NULL, callback, user_data);

        mm_obj_msg (self, "call in progress: voice audio path already on (other SIM's call)");
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    mm_obj_msg (self, "call in progress: switching the voice audio path on");
    /* 48 kHz: the rate t2-speechd assumes (speech-ccci questions 5) */
    speechd_command (modem, "call-start --rate 48000", NULL,
                     (GAsyncReadyCallback) speechd_in_call_ready,
                     g_task_new (self, NULL, callback, user_data));
}

static gboolean
modem_voice_cleanup_in_call_audio_channel_finish (MMIfaceModemVoice  *self,
                                                  GAsyncResult       *res,
                                                  GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_voice_cleanup_in_call_audio_channel (MMIfaceModemVoice   *self,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
    MMBroadbandModemMtkCcci *modem = MM_BROADBAND_MODEM_MTK_CCCI (self);

    /* the other SIM's call still needs the speech path */
    if (modem->priv->shared && mm_mtk_ccci_shared_speech_update (modem->priv->shared, modem->priv->served, FALSE) != -1) {
        GTask *task = g_task_new (self, NULL, callback, user_data);

        mm_obj_msg (self, "no call left on this SIM: voice audio path kept for the other SIM's call");
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    mm_obj_msg (self, "no call left: switching the voice audio path off");
    speechd_command (modem, "call-stop", NULL,
                     (GAsyncReadyCallback) speechd_in_call_ready,
                     g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Enabling / disabling (Broadband modem class): the MIPC session */

static void
ensure_session_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!mm_port_mipc_open_session_finish (port, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
ensure_session (MMBroadbandModemMtkCcci *self, GAsyncReadyCallback cb, gpointer data)
{
    GTask *task = g_task_new (self, NULL, cb, data);
    GError *error = NULL;
    if (!self->priv->mipc) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND, "No MIPC control port");
        g_object_unref (task);
        return;
    }
    if (mm_port_mipc_get_session_ready (self->priv->mipc)) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    if (!self->priv->mipc_opened) {
        if (!mm_port_mipc_open (self->priv->mipc, &error)) {
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }
        self->priv->mipc_opened = TRUE;
    }
    mm_port_mipc_open_session (self->priv->mipc, MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2,
                               mipc_indications, G_N_ELEMENTS (mipc_indications),
                               MIPC_CLIENT_NAME, NULL, (GAsyncReadyCallback) ensure_session_ready, task);
}

static gboolean
enabling_started_finish (MMBroadbandModem  *self,
                         GAsyncResult      *res,
                         GError           **error)
{
    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;
    mm_mtk_ccci_sms_poll_start (MM_BASE_MODEM (self));
    return TRUE;
}

static void
secondary_ims_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    /* the IMS controllers are shared (mm-mtk-ccci-shared): the source object
     * of their results is the shared context, not this modem */
    MMBaseModem *modem = g_task_get_source_object (task);
    GError *error = NULL;
    if (!mm_mtk_ccci_ims_setup_finish (res, &error)) {
        g_prefix_error (&error, "secondary IMS setup: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_base_modem_at_sequence (modem, voice_init_sequence, NULL, NULL,
                               (GAsyncReadyCallback) voice_init_ready, task);
}

static void
secondary_pin_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = mm_port_mipc_at_command_finish (port, res, &error);
    guint secondary = self->priv->sim_slot == 1 ? 2 : 1;
    if (reply && strstr (reply, "READY")) {
        mm_mtk_ccci_ims_setup (ims_of (self, secondary), (GAsyncReadyCallback) secondary_ims_ready, task);
        return;
    }
    mm_obj_warn (self, "SIM%u not ready for IMS: %s", secondary, error ? error->message : (reply ? reply : "unknown"));
    mm_base_modem_at_sequence (MM_BASE_MODEM (self), voice_init_sequence, NULL, NULL,
                               (GAsyncReadyCallback) voice_init_ready, task);
}

static void
ims_setup_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    /* source is the shared context that owns the IMS controller, not this modem */
    MMBaseModem *modem = g_task_get_source_object (task);
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (modem);
    GError *error = NULL;
    guint8 secondary_slot = self->priv->sim_slot == 1 ? MM_MIPC_SLOT_SIM2 : MM_MIPC_SLOT_SIM1;
    if (!mm_mtk_ccci_ims_setup_finish (res, &error)) {
        g_prefix_error (&error, "primary IMS setup: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (self->priv->dual) {
        /* the other stack is the other modem object's */
        mm_base_modem_at_sequence (modem, voice_init_sequence, NULL, NULL,
                                   (GAsyncReadyCallback) voice_init_ready, task);
        return;
    }
    mm_port_mipc_at_command (self->priv->mipc, secondary_slot, "AT+CPIN?", 10,
                              (GAsyncReadyCallback) secondary_pin_ready, task);
}

/* the IMS profiles and radio-owner flags of the shared controllers: the
 * SIM1/legacy object configures both stacks from wwan0at0's properties (as
 * before); the SIM2 object applies its anchor's properties to SIM2 */
static void
configure_ims_controllers (MMBroadbandModemMtkCcci *self)
{
    MMKernelDevice *kdev = config_kernel_device (self);
    guint i;

    for (i = 0; i < 2; i++) {
        g_autofree gchar *property = g_strdup_printf ("ID_MM_MTK_CCCI_DATA_APN_SIM%u", i + 1);
        MMMtkCcciIms *ims = ims_of (self, i + 1);
        const gchar *internet;
        const gchar *source;

        if (!ims || (self->priv->sim2_role && i == 0))
            continue;
        /* legacy: the selected stack issues the radio-off step; dual: each
         * stack may, the shared radio hook keeps the other stack's radio */
        mm_mtk_ccci_ims_set_radio_owner (ims, self->priv->dual || i + 1 == self->priv->sim_slot);
        if (!kdev)
            continue;
        internet = mm_kernel_device_get_property (kdev, property);
        source = i ? "udev ID_MM_MTK_CCCI_DATA_APN_SIM2" : "udev ID_MM_MTK_CCCI_DATA_APN_SIM1";
        if (!internet && i + 1 == self->priv->sim_slot) {
            internet = mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_DATA_APN");
            source = "udev ID_MM_MTK_CCCI_DATA_APN";
        }
        mm_mtk_ccci_ims_set_profiles (ims, internet, source,
                                      mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_IMS_APN"),
                                      mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_SOS_APN"));
        /* the initial-attach experiment: A (unset, 0) or B (1), per SIM or for both */
        {
            g_autofree gchar *ia_property = g_strdup_printf ("ID_MM_MTK_CCCI_IA_SIM%u", i + 1);
            const gchar      *name = mm_kernel_device_has_property (kdev, ia_property) ? ia_property :
                                     "ID_MM_MTK_CCCI_IA";

            mm_mtk_ccci_ims_set_initial_attach (ims, mm_kernel_device_has_property (kdev, name) &&
                                                     mm_kernel_device_get_property_as_boolean (kdev, name));
        }
        /* the start-up steps before radio-on, each switchable */
        {
            const gchar          *profiles = mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_START_PROFILES");
            MMMtkCcciImsProfiles  mode = MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO;
            guint                 steps = 0;

            if (!g_strcmp0 (profiles, "after-registration"))
                mode = MM_MTK_CCCI_IMS_PROFILES_AFTER_REGISTRATION;
            else if (!g_strcmp0 (profiles, "off") || !g_strcmp0 (profiles, "0"))
                mode = MM_MTK_CCCI_IMS_PROFILES_OFF;
            else if (profiles && g_strcmp0 (profiles, "before-radio") && g_strcmp0 (profiles, "1"))
                mm_obj_warn (self, "ignoring invalid ID_MM_MTK_CCCI_START_PROFILES '%s'", profiles);
            if (lte_property (kdev, "ID_MM_MTK_CCCI_IMS_MIPC_CONFIG", TRUE))
                steps |= MM_MTK_CCCI_IMS_STEP_MIPC_CONFIG;
            if (lte_property (kdev, "ID_MM_MTK_CCCI_CIREG", TRUE))
                steps |= MM_MTK_CCCI_IMS_STEP_CIREG;
            if (lte_property (kdev, "ID_MM_MTK_CCCI_EIMSPDN", TRUE))
                steps |= MM_MTK_CCCI_IMS_STEP_EIMSPDN;
            if (lte_property (kdev, "ID_MM_MTK_CCCI_EIMSCFG", TRUE))
                steps |= MM_MTK_CCCI_IMS_STEP_EIMSCFG;
            if (lte_property (kdev, "ID_MM_MTK_CCCI_EIMSRCS", TRUE))
                steps |= MM_MTK_CCCI_IMS_STEP_EIMSRCS;
            mm_mtk_ccci_ims_set_start_steps (ims, steps, mode);
        }
    }
}

/* enabling: this object's IMS stack (idempotent when configured before
 * radio-on), the other one in the single-object mode, then the voice
 * initialisation on the AT port */
static void
prepare_ims_and_voice (MMBroadbandModemMtkCcci *self, GTask *task)
{
    set_vendor_unsolicited_events_handlers (self, TRUE);
    /* the LTE start-up contract's parts that do not need the radio off */
    if (self->priv->shared)
        mm_mtk_ccci_shared_lte_session (self->priv->shared, self->priv->served);
    if (self->priv->ims_enabled && ims_of (self, self->priv->sim_slot)) {
        configure_ims_controllers (self);
        mm_mtk_ccci_ims_setup (ims_of (self, self->priv->sim_slot), (GAsyncReadyCallback) ims_setup_ready, task);
    } else
        mm_base_modem_at_sequence (MM_BASE_MODEM (self), voice_init_sequence, NULL, NULL,
                                   (GAsyncReadyCallback) voice_init_ready, task);
}

/* cold start (this stack's radio off): IMS of every stack about to come on
 * (the shared coordinator), the voice initialisation, then (caller) radio */
static void
cold_prepare_ready (MMMtkCcciShared *shared, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    GError *error = NULL;

    if (!mm_mtk_ccci_shared_radio_prepare_finish (shared, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_base_modem_at_sequence (MM_BASE_MODEM (self), voice_init_sequence, NULL, NULL,
                               (GAsyncReadyCallback) voice_init_ready, task);
}

static void
cold_prepare (MMBroadbandModemMtkCcci *self, GTask *task)
{
    set_vendor_unsolicited_events_handlers (self, TRUE);
    configure_ims_controllers (self);
    mm_mtk_ccci_shared_radio_prepare (self->priv->shared, (guint8) (1u << (self->priv->sim_slot - 1)),
                                      self->priv->ims_enabled,
                                      (GAsyncReadyCallback) cold_prepare_ready, task);
}

static void
initial_rat_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;
    gboolean          encoded = FALSE;
    guint8            rat = 0, prefer = 0;

    trace_watch (self);
    if (!set_rat_finish (self, res, &encoded, &rat, &prefer, &error)) {
        MMModemMode allowed, preferred;

        /* the default rejected (e.g. NR first without NR in this
         * firmware/SIM): one retry with another pair (decided
         * 2026-11-12), never the rejected one again; a client's choice is
         * not replaced */
        if (!self->priv->rat_user_set && encoded &&
            !g_object_get_data (G_OBJECT (task), "rat-fallback") &&
            mm_mtk_ccci_rat_fallback (self->priv->rat_mask, rat, prefer, &allowed, &preferred)) {
            g_autofree gchar *allowed_str = mm_modem_mode_build_string_from_mask (allowed);
            g_autofree gchar *preferred_str = mm_modem_mode_build_string_from_mask (preferred);

            mm_obj_warn (self, "SIM%u start-up RAT mode not accepted: %s; one retry with allowed '%s', preferred '%s'",
                         self->priv->sim_slot, error->message, allowed_str, preferred_str);
            g_object_set_data (G_OBJECT (task), "rat-fallback", GINT_TO_POINTER (TRUE));
            set_rat (self, allowed, preferred, (GAsyncReadyCallback) initial_rat_ready, task);
            return;
        }
        if (self->priv->rat_known)
            mm_obj_warn (self, "SIM%u RAT mode not set: %s; no other pair to try, the modem keeps its own "
                         "(last confirmed here: rat %u prefer_rat %u)",
                         self->priv->sim_slot, error->message, self->priv->rat_value, self->priv->rat_prefer);
        else
            mm_obj_warn (self, "SIM%u RAT mode not set: %s; no other pair to try, the modem keeps its own",
                         self->priv->sim_slot, error->message);
        if (g_object_get_data (G_OBJECT (task), "cold-start"))
            cold_prepare (self, task);
        else
            prepare_ims_and_voice (self, task);
        return;
    }
    initial_rat_continue (self, task);
}

static void
initial_rat_continue (MMBroadbandModemMtkCcci *self, GTask *task)
{
    if (self->priv->rat_mask && self->priv->rat_known) {
        /* enabling: show the mode on D-Bus (the generic init read it before) */
        MmGdbusModem *skeleton = NULL;
        g_object_get (self, MM_IFACE_MODEM_DBUS_SKELETON, &skeleton, NULL);
        if (skeleton) {
            mm_gdbus_modem_set_current_modes (skeleton, g_variant_new ("(uu)", self->priv->rat_allowed,
                                                                       self->priv->rat_preferred));
            g_object_unref (skeleton);
        }
    }
    if (g_object_get_data (G_OBJECT (task), "cold-start"))
        cold_prepare (self, task);
    else
        prepare_ims_and_voice (self, task);
}

static void
mipc_session_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    trace_event (self, trace_slots (self), NULL, "enabling");
    apply_initial_rat (self, task);
}

static void
parent_enabling_started_ready (MMBroadbandModem *_self,
                               GAsyncResult     *res,
                               GTask            *task)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    GError                  *error = NULL;

    if (!MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->enabling_started_finish (_self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ensure_session (self, (GAsyncReadyCallback) mipc_session_ready, task);
}

static void
enabling_started (MMBroadbandModem    *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    load_port_configuration (MM_BROADBAND_MODEM_MTK_CCCI (self));
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->enabling_started (
        self,
        (GAsyncReadyCallback) parent_enabling_started_ready,
        g_task_new (self, NULL, callback, user_data));
}

static gboolean
disabling_stopped (MMBroadbandModem  *_self,
                   GError           **error)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);

    mm_mtk_ccci_sms_poll_stop (MM_BASE_MODEM (self));
    reg_read_stop (self);
    /* this object's stacks only: the other SIM's modem object keeps its own */
    if (self->priv->served & MM_MIPC_SLOT_SIM1)
        mm_mtk_ccci_ims_stop (ims_of (self, 1));
    if (self->priv->served & MM_MIPC_SLOT_SIM2)
        mm_mtk_ccci_ims_stop (ims_of (self, 2));
    if (self->priv->mipc && self->priv->mipc_opened) {
        /* INTERNAL_CLOSE_REQ is not sent: a new session simply starts with
         * INTERNAL_TEST/OPEN next time, as after a client restart */
        mm_port_mipc_close (self->priv->mipc);
        self->priv->mipc_opened = FALSE;
    }
    self->priv->rat_session_confirmed = FALSE;

    return MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->disabling_stopped (_self, error);
}

static gpointer
initialization_started_finish (MMBroadbandModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
initial_complete (GTask *task, GError *error)
{
    MMBroadbandModem *self = g_task_get_source_object (task);
    gpointer ports = g_object_get_data (G_OBJECT (task), "parent-ports");
    if (error) {
        MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->initialization_stopped (
            self, ports, NULL);
        g_task_return_error (task, error);
    } else
        g_task_return_pointer (task, ports, NULL);
    g_object_unref (task);
}

static gboolean initial_query_pin (gpointer user_data);

static void
initial_sms_ready (MMBaseModem *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    mm_mtk_ccci_sms_wait_ready_finish (res, &error);
    initial_complete (task, error);
}

static void
initial_pin_ready (MMBaseModem *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    const gchar *reply = mm_base_modem_at_command_finish (self, res, &error);
    guint tries = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (task), "sim-ready-tries"));
    if (g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_BUSY) && tries < 30) {
        g_clear_error (&error);
        g_object_set_data (G_OBJECT (task), "sim-ready-tries", GUINT_TO_POINTER (tries + 1));
        g_timeout_add_seconds (1, initial_query_pin, task);
        return;
    }
    if (reply && strstr (reply, "READY")) {
        g_clear_error (&error);
        mm_mtk_ccci_sms_wait_ready (self, mm_base_modem_peek_cancellable (self),
                                    (GAsyncReadyCallback) initial_sms_ready, task);
        return;
    }
    /* Missing/locked SIMs are classified by the normal MM initialization. */
    if (reply || g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_NOT_INSERTED))
        g_clear_error (&error);
    initial_complete (task, error);
}

static gboolean
initial_query_pin (gpointer user_data)
{
    GTask *task = user_data;
    MMBaseModem *self = g_task_get_source_object (task);
    if (g_cancellable_is_cancelled (mm_base_modem_peek_cancellable (self))) {
        initial_complete (task, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                    "Modem removed while powering SIMs"));
        return G_SOURCE_REMOVE;
    }
    mm_base_modem_at_command (self, "+CPIN?", 10, FALSE, (GAsyncReadyCallback) initial_pin_ready, task);
    return G_SOURCE_REMOVE;
}

static void
initial_radio_ready (MMIfaceModem *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!modem_power_up_finish (self, res, &error)) {
        initial_complete (task, error);
        return;
    }
    initial_query_pin (task);
}

static void
initial_ims_ready (MMBaseModem *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        initial_complete (task, error);
        return;
    }
    modem_power_up (MM_IFACE_MODEM (self), (GAsyncReadyCallback) initial_radio_ready, task);
}

static void
initial_power_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    const gchar *reply = mm_base_modem_at_command_finish (modem, res, &error);
    MMModemPowerState state;
    GTask *prepare;
    if (!reply || !mm_3gpp_parse_cfun_query_generic_response (reply, &state, &error)) {
        initial_complete (task, error);
        return;
    }
    /* AT+CFUN? answers for the stack of this object's AT port */
    mm_mtk_ccci_shared_radio_note (MM_BROADBAND_MODEM_MTK_CCCI (modem)->priv->shared,
                                   (guint8) (1u << (MM_BROADBAND_MODEM_MTK_CCCI (modem)->priv->sim_slot - 1)),
                                   state == MM_MODEM_POWER_STATE_ON);
    if (state == MM_MODEM_POWER_STATE_ON) {
        initial_query_pin (task);
        return;
    }
    /* On this firmware CFUN=4 leaves both SIMs inaccessible. IMS must be
     * configured before EFUN, but EFUN must precede generic SIM discovery. */
    mm_obj_msg (modem, "cold start: preparing IMS before powering SIMs for discovery");
    trace_event (MM_BROADBAND_MODEM_MTK_CCCI (modem), trace_slots (MM_BROADBAND_MODEM_MTK_CCCI (modem)), NULL,
                 "cold start (radio was off)");
    prepare = g_task_new (modem, NULL, (GAsyncReadyCallback) initial_ims_ready, task);
    g_object_set_data (G_OBJECT (prepare), "cold-start", GINT_TO_POINTER (TRUE));
    /* the RAT mode first, so that the radio camps with it from the start */
    apply_initial_rat (MM_BROADBAND_MODEM_MTK_CCCI (modem), prepare);
}

/* SIM2 object: its AT port is the slot-2 tunnel channel. Numeric +CME
 * errors (the port's open-time +CMEE=1 ran before the session existed) and
 * the slot's URCs on MIPC (+EURCRPT=2, sent by Android's RIL at start)
 * before anything else; not fatal. */
static const MMBaseModemAtCommand sim2_channel_sequence[] = {
    { "+CMEE=1",     5, FALSE, mm_base_modem_response_processor_continue_on_error },
    { "+EURCRPT=2",  5, FALSE, mm_base_modem_response_processor_continue_on_error },
    { NULL }
};

static void
sim2_channel_ready (MMBaseModem *self, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;

    mm_base_modem_at_sequence_finish (self, res, NULL, &error);
    if (error)
        mm_obj_warn (self, "SIM2 AT channel setup: %s", error->message);
    mm_base_modem_at_command (self, "+CFUN?", 5, FALSE,
                              (GAsyncReadyCallback) initial_power_ready, task);
}

static void
initial_session_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        initial_complete (task, error);
        return;
    }
    if (self->priv->sim2_role) {
        mm_base_modem_at_sequence (MM_BASE_MODEM (self), sim2_channel_sequence, NULL, NULL,
                                   (GAsyncReadyCallback) sim2_channel_ready, task);
        return;
    }
    mm_base_modem_at_command (MM_BASE_MODEM (self), "+CFUN?", 5, FALSE,
                              (GAsyncReadyCallback) initial_power_ready, task);
}

static void
initial_stack_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    gpointer ports = g_object_get_data (G_OBJECT (task), "parent-ports");
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->initialization_stopped (
            MM_BROADBAND_MODEM (modem), ports, NULL);
        g_task_return_error (task, error);
    } else {
        ensure_session (MM_BROADBAND_MODEM_MTK_CCCI (modem), (GAsyncReadyCallback) initial_session_ready, task);
        return;
    }
    g_object_unref (task);
}

static void
parent_initialization_ready (MMBroadbandModem *modem, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (modem);
    GError *error = NULL;
    gpointer ports;
    g_autofree gchar *cmd = NULL;
    ports = MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->initialization_started_finish (
        modem, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_object_set_data (G_OBJECT (task), "parent-ports", ports);
    if (self->priv->sim2_role) {
        /* +ESUO selects the stack of the physical AT port (wwan0at0) only;
         * the tunnel channel is addressed by the MIPC slot */
        ensure_session (self, (GAsyncReadyCallback) initial_session_ready, task);
        return;
    }
    cmd = g_strdup_printf ("+ESUO=%u", 3 + self->priv->sim_slot);
    mm_base_modem_at_command (MM_BASE_MODEM (self), cmd, 10, FALSE,
                              (GAsyncReadyCallback) initial_stack_ready, task);
}

static void
initialization_started (MMBroadbandModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    load_port_configuration (MM_BROADBAND_MODEM_MTK_CCCI (self));
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->initialization_started (
        self, (GAsyncReadyCallback) parent_initialization_ready, g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Setup ports (Broadband modem class) */

static void
setup_ports (MMBroadbandModem *_self)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);

    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->setup_ports (_self);

    load_port_configuration (self);

    /* handlers installed disabled; the interfaces enable them */
    set_vendor_unsolicited_events_handlers (self, FALSE);
    set_voice_unsolicited_events_handlers (self, FALSE);
}

/*****************************************************************************/

MMBroadbandModemMtkCcci *
mm_broadband_modem_mtk_ccci_new (const gchar  *device,
                                 const gchar  *physdev,
                                 const gchar **drivers,
                                 const gchar  *plugin,
                                 guint16       vendor_id,
                                 guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_MTK_CCCI,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_PHYSDEV,    physdev,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* The data interfaces are wwan links created at
                          * connection time; no net port exists before. The
                          * AT port stands in as the "data port" so that the
                          * port organizer accepts the modem; the bearer never
                          * uses it for data. */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, TRUE,
                         /* +ECPI/+CLCC give the call states */
                         MM_IFACE_MODEM_VOICE_PERIODIC_CALL_LIST_CHECK_DISABLED, FALSE,
                         NULL);
}

MMBroadbandModemMtkCcci *
mm_broadband_modem_mtk_ccci_new_sim2 (const gchar     *device,
                                      const gchar     *physdev,
                                      const gchar    **drivers,
                                      const gchar     *plugin,
                                      MMKernelDevice  *anchor,
                                      GError         **error)
{
    MMBroadbandModemMtkCcci         *self;
    g_autoptr(MMKernelDevice)        kdev = NULL;
    g_autoptr(MMKernelEventProperties) props = NULL;
    g_autofree gchar                *mipc_name = NULL;
    const gchar                     *name;

    /* the MIPC channel of the SIM1 object (Android's data and
     * control channel, ttyCMIPC1): from the anchor's properties, else
     * "<wwan device>mipc1" */
    name = mm_kernel_device_get_property (anchor, ID_MM_MTK_CCCI_MIPC_PORT);
    if (name && name[0])
        mipc_name = g_strdup (name);
    else {
        const gchar *anchor_name = mm_kernel_device_get_name (anchor);
        const gchar *p = anchor_name ? strstr (anchor_name, "mipc") : NULL;

        if (!p || p == anchor_name) {
            g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                         "cannot derive the MIPC port from %s", anchor_name ? anchor_name : "(null)");
            return NULL;
        }
        mipc_name = g_strdup_printf ("%.*smipc1", (gint) (p - anchor_name), anchor_name);
    }

    self = g_object_new (MM_TYPE_BROADBAND_MODEM_MTK_CCCI,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_PHYSDEV,    physdev,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  0,
                         MM_BASE_MODEM_PRODUCT_ID, 0,
                         /* the AT port stands in as the data port, see _new() */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_VOICE_PERIODIC_CALL_LIST_CHECK_DISABLED, FALSE,
                         NULL);
    self->priv->sim2_role = TRUE;
    self->priv->dual = TRUE;
    self->priv->sim_slot = 2;
    self->priv->served = MM_MIPC_SLOT_SIM2;
    self->priv->config_kdev = g_object_ref (anchor);
    self->priv->shared = mm_mtk_ccci_shared_acquire (mipc_name);
    self->priv->mipc = g_object_ref (mm_mtk_ccci_shared_peek_mipc (self->priv->shared));
    g_signal_connect_object (self->priv->mipc, "forced-close", G_CALLBACK (mipc_port_lost), self, 0);
    g_signal_connect_object (self->priv->mipc, MM_PORT_MIPC_SIGNAL_INDICATION, G_CALLBACK (voice_mipc_indication), self, 0);

    self->priv->bridge = mm_mtk_ccci_at_bridge_new (self->priv->mipc, MM_MIPC_SLOT_SIM2, error);
    if (!self->priv->bridge) {
        g_object_unref (self);
        return NULL;
    }

    /* the primary AT port: ModemManager's "virtual" (unix socket) AT port,
     * connected to the bridge (mm-base-modem.c base_modem_create_virtual_port,
     * mm-port-serial.c "abstract:" addresses) */
    props = mm_kernel_event_properties_new ();
    mm_kernel_event_properties_set_action (props, "add");
    mm_kernel_event_properties_set_subsystem (props, "virtual");
    mm_kernel_event_properties_set_name (props, mm_mtk_ccci_at_bridge_get_name (self->priv->bridge));
    kdev = mm_kernel_device_generic_new_with_rules (props, NULL, error);
    if (!kdev ||
        !mm_base_modem_grab_port (MM_BASE_MODEM (self), kdev, MM_PORT_GROUP_USED, MM_PORT_TYPE_AT,
                                  MM_PORT_SERIAL_AT_FLAG_PRIMARY, error)) {
        g_prefix_error (error, "SIM2 AT port: ");
        g_object_unref (self);
        return NULL;
    }
    mm_obj_msg (self, "SIM2 modem object: AT port %s over the MIPC tunnel of %s",
                mm_mtk_ccci_at_bridge_get_name (self->priv->bridge), mipc_name);
    return self;
}

static void
mm_broadband_modem_mtk_ccci_init (MMBroadbandModemMtkCcci *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_BROADBAND_MODEM_MTK_CCCI,
                                              MMBroadbandModemMtkCcciPrivate);
    self->priv->radio_mask     = DEFAULT_RADIO_MASK;
    self->priv->sim_slot       = DEFAULT_SIM_SLOT;
    self->priv->served         = MM_MIPC_SLOT_SIM1 | MM_MIPC_SLOT_SIM2;
    self->priv->speechd_socket = g_strdup (DEFAULT_SPEECHD_SOCKET);
    self->priv->start_rat = TRUE;
    trace_process_marker (self);

    /* +ECPI: <call_id>,<msg_type>,<is_ibt>,<is_tch>,<dir>,<call_mode>,"<number>",<type>,"<pau>"[,<cause>] */
    self->priv->ecpi_regex = g_regex_new ("\\r\\n\\+ECPI:\\s*(\\d+),\\s*(\\d+)(?:,\\s*([^\\r\\n]*))?\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    /* +EAIC: <call_id>,"<number>",<type>,<call_mode>,<seq_no>[,...] */
    self->priv->eaic_regex = g_regex_new ("\\r\\n\\+EAIC:\\s*(\\d+),\\s*\"([^\"]*)\",\\s*(\\d+),\\s*(\\d+),\\s*(\\d+)[^\\r\\n]*\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    /* +ESPEECH: <speech_on>,<x>,<y> */
    self->priv->espeech_regex = g_regex_new ("\\r\\n\\+ESPEECH:\\s*(\\d+)[^\\r\\n]*\\r\\n",
                                             G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    /* +EVOCD:<codec> */
    self->priv->evocd_regex = g_regex_new ("\\r\\n\\+EVOCD:\\s*(\\d+)\\r\\n",
                                           G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    /* +EIND (start-up indication), +EFUN, +ESUO reports */
    self->priv->eind_regex = g_regex_new ("\\r\\n(\\+(?:EIND|EFUN|ESUO):[^\\r\\n]*)\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    /* IMS reports, forwarded to the log only */
    self->priv->ims_regex = g_regex_new ("\\r\\n(\\+(?:EIMSPDN|EIMSPDIS|EIMS|EIMCFLAG|EIMSPDNOOS|CIREGU|EIF):[^\\r\\n]*)\\r\\n",
                                         G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

static void
dispose (GObject *object)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (object);

    mm_mtk_ccci_sms_poll_stop (MM_BASE_MODEM (self));

    if (self->priv->trace_timer) {
        g_source_remove (self->priv->trace_timer);
        self->priv->trace_timer = 0;
    }
    reg_read_stop (self);
    if (self->priv->mipc) {
        g_autofree gchar *stamp = mm_mtk_ccci_trace_stamp ();
        mm_obj_msg (self, "rat-trace %s %s: modem object released (MM stop, reprobe or removal)",
                    trace_sim_name (trace_slots (self)), stamp);
    }
    if (self->priv->trace_skeleton) {
        g_signal_handlers_disconnect_by_data (self->priv->trace_skeleton, self);
        g_clear_object (&self->priv->trace_skeleton);
    }
    g_clear_pointer (&self->priv->bridge, mm_mtk_ccci_at_bridge_free);
    if (self->priv->mipc && self->priv->mipc_opened) {
        mm_port_mipc_close (self->priv->mipc);
        self->priv->mipc_opened = FALSE;
    }
    g_clear_object (&self->priv->mipc);
    if (self->priv->shared) {
        mm_mtk_ccci_shared_unset_voice_client (self->priv->shared, G_OBJECT (self));
        /* a call of this object can no longer end through it */
        if (mm_mtk_ccci_shared_speech_get (self->priv->shared) & self->priv->served &&
            mm_mtk_ccci_shared_speech_update (self->priv->shared, self->priv->served, FALSE) == -1)
            mm_obj_warn (self, "modem object gone during a call: the voice audio path may stay on");
        g_clear_object (&self->priv->shared);
    }
    g_clear_object (&self->priv->config_kdev);

    G_OBJECT_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (object);

    g_free (self->priv->wwan_device);
    g_free (self->priv->speechd_socket);
    if (self->priv->trace_reason)
        g_string_free (self->priv->trace_reason, TRUE);
    g_regex_unref (self->priv->ecpi_regex);
    g_regex_unref (self->priv->eaic_regex);
    g_regex_unref (self->priv->espeech_regex);
    g_regex_unref (self->priv->evocd_regex);
    g_regex_unref (self->priv->eind_regex);
    g_regex_unref (self->priv->ims_regex);

    G_OBJECT_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->finalize (object);
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    iface->modem_power_up = modem_power_up;
    iface->modem_power_up_finish = modem_power_up_finish;
    iface->modem_power_down = modem_power_down;
    iface->modem_power_down_finish = modem_power_down_finish;
    iface->create_bearer = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;
    iface->load_sim_slots = modem_load_sim_slots;
    iface->load_sim_slots_finish = modem_load_sim_slots_finish;
    iface->set_primary_sim_slot = modem_set_primary_sim_slot;
    iface->set_primary_sim_slot_finish = modem_set_primary_sim_slot_finish;
    iface->load_supported_modes = modem_load_supported_modes;
    iface->load_supported_modes_finish = modem_load_supported_modes_finish;
    iface->load_current_modes = modem_load_current_modes;
    iface->load_current_modes_finish = modem_load_current_modes_finish;
    iface->set_current_modes = modem_set_current_modes;
    iface->set_current_modes_finish = modem_set_current_modes_finish;
}

/* the generic registration reporting setup: on the SIM2 AT port (the slot-2
 * MIPC AT channel) it sets <n> = 2, so the reject causes (step 6) are set
 * again after it */
static gboolean
modem_3gpp_enable_registration_events_finish (MMIfaceModem3gpp *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_enable_registration_events_ready (MMIfaceModem3gpp *_self, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    GError                  *error = NULL;

    if (!iface_modem_3gpp_parent->enable_unsolicited_registration_events_finish (_self, res, &error))
        g_task_return_error (task, error);
    else {
        if (self->priv->bridge && self->priv->shared)
            mm_mtk_ccci_shared_lte_reg_cause (self->priv->shared, mm_broadband_modem_mtk_ccci_get_mipc_slot (self),
                                              "after the generic <n> = 2 setting of this SIM's AT port");
        reg_read_start (self);
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
modem_3gpp_enable_registration_events (MMIfaceModem3gpp    *self,
                                       gboolean             cs_supported,
                                       gboolean             ps_supported,
                                       gboolean             eps_supported,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    iface_modem_3gpp_parent->enable_unsolicited_registration_events (
        self, cs_supported, ps_supported, eps_supported,
        (GAsyncReadyCallback) parent_enable_registration_events_ready,
        g_task_new (self, NULL, callback, user_data));
}

static void
iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

    iface->enable_unsolicited_registration_events = modem_3gpp_enable_registration_events;
    iface->enable_unsolicited_registration_events_finish = modem_3gpp_enable_registration_events_finish;

    iface->setup_unsolicited_events = modem_3gpp_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_3gpp_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
}

static void
iface_modem_voice_init (MMIfaceModemVoiceInterface *iface)
{
    iface_modem_voice_parent = g_type_interface_peek_parent (iface);

    iface->setup_unsolicited_events = modem_voice_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_voice_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_voice_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_voice_setup_cleanup_unsolicited_events_finish;
    iface->enable_unsolicited_events = modem_voice_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_voice_enable_unsolicited_events_finish;
    iface->load_call_list = modem_voice_load_call_list;
    iface->load_call_list_finish = modem_voice_load_call_list_finish;
    iface->create_call = modem_voice_create_call;
    iface->hangup_all = modem_voice_hangup_all;
    iface->hangup_all_finish = modem_voice_hangup_all_finish;
    iface->setup_in_call_audio_channel = modem_voice_setup_in_call_audio_channel;
    iface->setup_in_call_audio_channel_finish = modem_voice_setup_in_call_audio_channel_finish;
    iface->cleanup_in_call_audio_channel = modem_voice_cleanup_in_call_audio_channel;
    iface->cleanup_in_call_audio_channel_finish = modem_voice_cleanup_in_call_audio_channel_finish;
}

static void
iface_modem_messaging_init (MMIfaceModemMessagingInterface *iface)
{
    iface_modem_messaging_parent = g_type_interface_peek_parent (iface);

    iface->enable_unsolicited_events = modem_messaging_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_messaging_enable_unsolicited_events_finish;
}

static void
mm_broadband_modem_mtk_ccci_class_init (MMBroadbandModemMtkCcciClass *klass)
{
    GObjectClass          *object_class = G_OBJECT_CLASS (klass);
    MMBaseModemClass      *base_modem_class = MM_BASE_MODEM_CLASS (klass);
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemMtkCcciPrivate));

    object_class->dispose = dispose;
    object_class->finalize = finalize;

    base_modem_class->create_wwan_port = create_wwan_port;

    broadband_modem_class->initialization_started = initialization_started;
    broadband_modem_class->initialization_started_finish = initialization_started_finish;
    broadband_modem_class->setup_ports = setup_ports;
    broadband_modem_class->enabling_started = enabling_started;
    broadband_modem_class->enabling_started_finish = enabling_started_finish;
    broadband_modem_class->disabling_stopped = disabling_stopped;
}

gboolean
mm_broadband_modem_mtk_ccci_ims_owns_link (MMBroadbandModemMtkCcci *self, guint link_id)
{
    return self->priv->shared && mm_mtk_ccci_shared_ims_owns_link (self->priv->shared, link_id);
}
