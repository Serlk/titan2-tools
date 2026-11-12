/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMBroadbandModemMtkCcci: MT6878 integrated modem on the mtk_ccci driver.
 *
 * Everything 3GPP comes from MMBroadbandModem (registration, SIM, signal, SMS
 * in PDU mode, the generic AT voice calls). This class adds the MediaTek
 * behaviour established on the phone with our own tools (t2-modemd,
 * tools/modemd/src/vendor.c) and a clean-room behavioural description of the
 * vendor AT/MIPC protocol (no code of the factory firmware was used):
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
 *    in-call audio channel hooks (factory timing: at dial / answer and when the
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
    MMPortMipc *mipc;          /* the data port, created by create_wwan_port() */
    MMMtkCcciIms *ims[2];
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

/*****************************************************************************/
/* Port creation (MMBaseModem class): the MIPC port */

static void
mipc_port_lost (MMPortMipc *port, MMBroadbandModemMtkCcci *self)
{
    self->priv->mipc_opened = FALSE;
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
    guint32 event = 0, call_id = 0, seq = 0;
    /* any modem traffic after a suspend: one fallback scan (lost notices) */
    mm_mtk_ccci_sms_poll_check_resume (MM_BASE_MODEM (self));
    if (slot != 1 && slot != 2)
        return;
    if (id == MM_MIPC_SYS_AT_IND) {
        g_autofree gchar *text = mm_mipc_tlv_find_at_string (payload->data, payload->len);
        if (!text)
            return;
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
        g_autoptr(GByteArray) reply = g_byte_array_new ();
        g_autoptr(GError) error = NULL;
        if (!mm_mipc_tlv_find_uint (payload->data, payload->len, 0x0100, &call_id) ||
            !mm_mipc_tlv_find_uint (payload->data, payload->len, 0x0103, &seq) ||
            !mm_mtk_ccci_call_index (slot, call_id))
            return;
        mm_mipc_tlv_add_u8 (reply, 0x0100, 1); /* allow ringing, not auto-answer */
        mm_mipc_tlv_add_u32 (reply, 0x0102, call_id);
        mm_mipc_tlv_add_u32 (reply, 0x0103, seq);
        if (!mm_port_mipc_send_response (port, MM_MIPC_CALL_APPROVE_RSP, txid, routing, reply, &error))
            mm_obj_warn (self, "SIM%u incoming-call approval: %s", slot, error->message);
    } else if (id == MM_MIPC_CALL_STATUS_IND) {
        MMCallInfo info;
        if (mm_mtk_ccci_call_parse (slot, payload->data, payload->len, &info)) {
            mm_obj_dbg (self, "SIM%u call index %u state %u", slot, info.index, info.state);
            mm_call_mtk_ccci_report (MM_IFACE_MODEM_VOICE (self), port, slot, &info);
            g_free (info.number);
        }
    } else if (id == MM_MIPC_CALL_EVENT_IND &&
               mm_mipc_tlv_find_uint (payload->data, payload->len, 0x0100, &event))
        mm_obj_dbg (self, "SIM%u call event %u", slot, event);
    else if (id == MM_MIPC_NW_CURRENT_RAT_IND) {
        guint32 rat = 0, prefer = 0;
        if (mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_CURRENT_RAT_RAT, &rat) &&
            mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_CURRENT_RAT_PREFER, &prefer))
            mm_obj_msg (self, "SIM%u RAT mode now rat %u prefer_rat %u (NW_CURRENT_RAT_IND)", slot, rat, prefer);
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
        self->priv->mipc = mm_port_mipc_new (name);
        g_signal_connect_object (self->priv->mipc, "forced-close", G_CALLBACK (mipc_port_lost), self, 0);
        g_signal_connect_object (self->priv->mipc, MM_PORT_MIPC_SIGNAL_INDICATION, G_CALLBACK (voice_mipc_indication), self, 0);
        mm_obj_dbg (self, "MIPC data port created: %s", name);
        return MM_PORT (g_object_ref (self->priv->mipc));
    }

    return MM_BASE_MODEM_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->create_wwan_port (_self, name, ptype);
}

/*****************************************************************************/
/* Configuration from the AT port's udev properties */

static void
load_port_configuration (MMBroadbandModemMtkCcci *self)
{
    MMPortSerialAt *primary;
    MMKernelDevice *kdev;
    const gchar    *name, *p;
    gint            v;

    primary = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));
    if (!primary)
        return;
    kdev = mm_port_peek_kernel_device (MM_PORT (primary));
    if (!kdev)
        return;

    self->priv->ims_enabled = !mm_kernel_device_has_property (kdev, "ID_MM_MTK_CCCI_IMS") ||
                             mm_kernel_device_get_property_as_boolean (kdev, "ID_MM_MTK_CCCI_IMS");
    if (mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_RADIO_MASK)) {
        v = mm_kernel_device_get_property_as_int (kdev, ID_MM_MTK_CCCI_RADIO_MASK);
        if (v >= 1 && v <= 3)
            self->priv->radio_mask = (guint) v;
        else
            mm_obj_warn (self, "ignoring invalid " ID_MM_MTK_CCCI_RADIO_MASK " %d", v);
    }
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
    if (mm_kernel_device_has_property (kdev, ID_MM_MTK_CCCI_SPEECHD_SOCKET)) {
        g_free (self->priv->speechd_socket);
        self->priv->speechd_socket = g_strdup (mm_kernel_device_get_property (kdev, ID_MM_MTK_CCCI_SPEECHD_SOCKET));
    }

    /* the WWAN device the ports hang off: the wwan class device's name when the
     * kernel device backend knows it, else derived from the port name
     * ("wwan0at0" -> "wwan0") */
    g_clear_pointer (&self->priv->wwan_device, g_free);
    p = mm_kernel_device_get_wwandev_sysfs_path (kdev);
    if (p && *p)
        self->priv->wwan_device = g_path_get_basename (p);
    if (!self->priv->wwan_device) {
        name = mm_kernel_device_get_name (kdev);
        p = name ? strstr (name, "at") : NULL;
        if (p && p > name)
            self->priv->wwan_device = g_strndup (name, p - name);
    }
    mm_obj_dbg (self, "configuration: radio mask %u, SIM slot %u, WWAN device %s, speechd socket '%s'",
                self->priv->radio_mask, self->priv->sim_slot,
                mm_broadband_modem_mtk_ccci_get_wwan_device (self),
                self->priv->speechd_socket ? self->priv->speechd_socket : "");
}

/*****************************************************************************/
/* Power (Modem interface) */

static gboolean
modem_power_up_finish (MMIfaceModem  *self,
                       GAsyncResult  *res,
                       GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
cfun_power_up_ready (MMBaseModem *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!mm_base_modem_at_command_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean retry_radio (gpointer user_data);

static void
efun_power_up_ready (MMBaseModem *self, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;
    guint tries = GPOINTER_TO_UINT (g_task_get_task_data (task));
    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        if (g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_BUSY) && tries < 30) {
            g_task_set_task_data (task, GUINT_TO_POINTER (tries + 1), NULL);
            g_timeout_add_seconds (1, retry_radio, task);
            return;
        }
        if (MM_BROADBAND_MODEM_MTK_CCCI (self)->priv->radio_mask == 1) {
            mm_base_modem_at_command (self, "+CFUN=1", 10, FALSE,
                                      (GAsyncReadyCallback) cfun_power_up_ready, task);
            return;
        }
        /* CFUN=1 cannot express a SIM2/both-radio mask. */
        g_task_return_error (task, g_steal_pointer (&error));
    } else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
retry_radio (gpointer user_data)
{
    GTask *task = user_data;
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    g_autofree gchar *cmd = g_strdup_printf ("+EFUN=%u", self->priv->radio_mask);
    if (g_cancellable_is_cancelled (mm_base_modem_peek_cancellable (MM_BASE_MODEM (self)))) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Modem removed during radio start");
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }
    mm_base_modem_at_command (MM_BASE_MODEM (self), cmd, 15, FALSE,
                              (GAsyncReadyCallback) efun_power_up_ready, task);
    return G_SOURCE_REMOVE;
}

static void
modem_power_up (MMIfaceModem        *_self,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (_self);
    GTask                   *task;
    g_autofree gchar        *cmd = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    /* AT+EFUN=<mask>: 1 = SIM1 radio (equivalent to the 3GPP AT+CFUN=1,
     * observed), 2 = SIM2, 3 = both (observed on the phone) */
    cmd = g_strdup_printf ("+EFUN=%u", self->priv->radio_mask);
    mm_obj_dbg (self, "powering the radio up with AT%s", cmd);
    mm_base_modem_at_command (MM_BASE_MODEM (self), cmd, 15, FALSE,
                              (GAsyncReadyCallback) efun_power_up_ready, task);
}

static gboolean
modem_power_down_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
}

static void
modem_power_down (MMIfaceModem        *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    /* 3GPP flight mode, the modem's start-up state; AT+EFUN=0 is untried
     * (t2-modemd QUESTIONS C17) */
    mm_base_modem_at_command (MM_BASE_MODEM (self), "+CFUN=4", 15, FALSE, callback, user_data);
}

/*****************************************************************************/
/* Allowed/preferred modes (Modem interface): the RAT mode
 *
 * The factory firmware's RIL sets the RAT mode with MIPC
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
} SetRatContext;

static gboolean
set_rat_finish (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_rat_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = g_task_get_source_object (task);
    SetRatContext           *ctx = g_task_get_task_data (task);
    g_autoptr(MMMipcCnf)     cnf = NULL;
    GError                  *error = NULL;

    cnf = mm_port_mipc_request_finish (port, res, &error);
    if (!cnf) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (cnf->result) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "NW_SET_RAT rat %u prefer_rat %u: result %u",
                                 ctx->rat, ctx->prefer, cnf->result);
        g_object_unref (task);
        return;
    }
    self->priv->rat_known = TRUE;
    self->priv->rat_value = ctx->rat;
    self->priv->rat_prefer = ctx->prefer;
    self->priv->rat_allowed = ctx->allowed;
    self->priv->rat_preferred = ctx->preferred;
    mm_obj_msg (self, "SIM%u RAT mode set: rat %u prefer_rat %u",
                mm_broadband_modem_mtk_ccci_get_mipc_slot (self), ctx->rat, ctx->prefer);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* NW_SET_RAT_REQ on the selected slot; ANY/NONE means the factory default */
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
    if (allowed == MM_MODEM_MODE_ANY && preferred == MM_MODEM_MODE_NONE)
        mm_mtk_ccci_rat_default (self->priv->rat_mask, &allowed, &preferred);
    ctx->allowed = allowed;
    ctx->preferred = preferred;
    if (!mm_mtk_ccci_rat_from_modes (self->priv->rat_mask, allowed, preferred, &ctx->rat, &ctx->prefer)) {
        g_autofree gchar *allowed_str = mm_modem_mode_build_string_from_mask (allowed);
        g_autofree gchar *preferred_str = mm_modem_mode_build_string_from_mask (preferred);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "Requested mode (allowed: '%s', preferred: '%s') not supported by the modem",
                                 allowed_str, preferred_str);
        g_object_unref (task);
        return;
    }
    if (!self->priv->mipc || !mm_port_mipc_get_session_ready (self->priv->mipc)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "MIPC session not ready");
        g_object_unref (task);
        return;
    }
    tlvs = mm_mipc_build_set_rat (ctx->rat, ctx->prefer);
    mm_port_mipc_request (self->priv->mipc, MM_MIPC_NW_SET_RAT_REQ,
                          mm_broadband_modem_mtk_ccci_get_mipc_slot (self), tlvs, 15, NULL,
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

    if (!set_rat_finish (self, res, &error))
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

/* Before the radio comes up (cold start) and at every enable: the factory
 * default, or what a client chose for this modem object. Never fatal: the
 * modem then keeps the mode it has. */
static void
initial_rat_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task);

static void
apply_initial_rat (MMBroadbandModemMtkCcci *self, GTask *task)
{
    MMModemMode allowed = MM_MODEM_MODE_ANY;
    MMModemMode preferred = MM_MODEM_MODE_NONE;

    if (self->priv->rat_user_set) {
        allowed = self->priv->rat_allowed;
        preferred = self->priv->rat_preferred;
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

void
mm_broadband_modem_mtk_ccci_prepare_data (MMBroadbandModemMtkCcci *self, const gchar *apn,
                                         GAsyncReadyCallback callback, gpointer user_data)
{
    if (self->priv->ims_enabled) {
        mm_mtk_ccci_ims_prepare_data (self->priv->ims[self->priv->sim_slot - 1], apn, callback, user_data);
    } else {
        GTask *task = g_task_new (self, NULL, callback, user_data);
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
    }
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
    mm_mtk_ccci_ims_urc (self->priv->ims[self->priv->sim_slot - 1], str);
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
    /* the speech path is switched on MM's in-call state (factory timing), this
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
        /* 1, 3, 4, 5, 129, 134..137: progress/sync, not forwarded by the factory firmware's RIL either */
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
    mm_obj_msg (self, "incoming call indication: id %u from %s, approving", call_id, number);

    /* The call does not ring until the AP answers; mode 0 = let it proceed
     * (the modem then sends RING, +CLIP, +ECPI). Rejecting would be
     * AT+EAIC=1,<id>,<seq>,21. Policy: always approve; the user rejects the
     * ringing call through the Call object as with any modem. */
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
 * indications that the factory firmware's RIL registers (mm-mipc-proto.h) are registered
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
 * The modem reports IMS (VoLTE) calls with <mode> 20..25
 * (observed on the phone: "+CLCC: 1,0,2,20,0,...,145" for a dialing
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
    while (ctx->slot < 2 && !self->priv->sim_ready[ctx->slot])
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
    while (slot < 2 && !self->priv->sim_ready[slot]) slot++;
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
 * exactly the timing of the factory firmware ("speech on when ATD/ATA
 * is accepted, off when the last call is gone"). t2-speechd owns the audio
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
    mm_obj_msg (self, "call in progress: switching the voice audio path on");
    /* 48 kHz: the rate t2-speechd assumes (unverified) */
    speechd_command (MM_BROADBAND_MODEM_MTK_CCCI (self), "call-start --rate 48000", NULL,
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
    mm_obj_msg (self, "no call left: switching the voice audio path off");
    speechd_command (MM_BROADBAND_MODEM_MTK_CCCI (self), "call-stop", NULL,
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
secondary_ims_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!mm_mtk_ccci_ims_setup_finish (res, &error)) {
        if (g_object_get_data (G_OBJECT (task), "cold-start")) {
            mm_obj_warn (modem, "secondary IMS setup before SIM power: %s; retrying after SIM discovery", error->message);
            g_clear_error (&error);
            mm_base_modem_at_sequence (modem, voice_init_sequence, NULL, NULL,
                                       (GAsyncReadyCallback) voice_init_ready, task);
            return;
        }
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
    guint secondary = self->priv->sim_slot == 1 ? 1 : 0;
    if (reply && strstr (reply, "READY")) {
        mm_mtk_ccci_ims_setup (self->priv->ims[secondary], (GAsyncReadyCallback) secondary_ims_ready, task);
        return;
    }
    mm_obj_warn (self, "SIM%u not ready for IMS: %s", secondary + 1, error ? error->message : (reply ? reply : "unknown"));
    mm_base_modem_at_sequence (MM_BASE_MODEM (self), voice_init_sequence, NULL, NULL,
                               (GAsyncReadyCallback) voice_init_ready, task);
}

static void
ims_setup_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (modem);
    GError *error = NULL;
    guint8 secondary_slot = self->priv->sim_slot == 1 ? MM_MIPC_SLOT_SIM2 : MM_MIPC_SLOT_SIM1;
    if (!mm_mtk_ccci_ims_setup_finish (res, &error)) {
        g_prefix_error (&error, "primary IMS setup before radio-on: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (g_object_get_data (G_OBJECT (task), "cold-start")) {
        /* CPIN stays busy until EFUN on a cold modem. Configure both stacks
         * before powering SIMs; a missing secondary is checked after discovery. */
        mm_mtk_ccci_ims_setup (self->priv->ims[secondary_slot - 1],
                               (GAsyncReadyCallback) secondary_ims_ready, task);
        return;
    }
    mm_port_mipc_at_command (self->priv->mipc, secondary_slot, "AT+CPIN?", 10,
                              (GAsyncReadyCallback) secondary_pin_ready, task);
}

static void
prepare_ims_and_voice (MMBroadbandModemMtkCcci *self, GTask *task)
{
    MMPortMipc *mipc = self->priv->mipc;
    guint i;

    set_vendor_unsolicited_events_handlers (self, TRUE);
    if (self->priv->ims_enabled) {
        MMKernelDevice *kdev = mm_port_peek_kernel_device (MM_PORT (mm_base_modem_peek_port_primary (MM_BASE_MODEM (self))));
        for (i = 0; i < 2; i++) {
            g_autofree gchar *property = g_strdup_printf ("ID_MM_MTK_CCCI_DATA_APN_SIM%u", i + 1);
            const gchar *internet = kdev ? mm_kernel_device_get_property (kdev, property) : NULL;
            if (!self->priv->ims[i])
                self->priv->ims[i] = mm_mtk_ccci_ims_new (MM_BASE_MODEM (self), mipc, 1u << i,
                    mm_broadband_modem_mtk_ccci_get_wwan_device (self));
            mm_mtk_ccci_ims_set_radio_owner (self->priv->ims[i], i + 1 == self->priv->sim_slot);
            if (!internet && kdev && i + 1 == self->priv->sim_slot)
                internet = mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_DATA_APN");
            if (kdev)
                mm_mtk_ccci_ims_set_profiles (self->priv->ims[i], internet,
                    mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_IMS_APN"),
                    mm_kernel_device_get_property (kdev, "ID_MM_MTK_CCCI_SOS_APN"));
        }
        mm_mtk_ccci_ims_setup (self->priv->ims[self->priv->sim_slot - 1], (GAsyncReadyCallback) ims_setup_ready, task);
    } else
        mm_base_modem_at_sequence (MM_BASE_MODEM (self), voice_init_sequence, NULL, NULL,
                                   (GAsyncReadyCallback) voice_init_ready, task);
}

static void
initial_rat_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;

    if (!set_rat_finish (self, res, &error)) {
        if (!self->priv->rat_user_set && !g_object_get_data (G_OBJECT (task), "rat-lte-fallback")) {
            MMModemMode allowed, preferred;
            /* the NR-first default rejected (no NR in this firmware/SIM?):
             * the LTE-first default instead (user decision 2026-11-12) */
            mm_mtk_ccci_rat_default (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G,
                                     &allowed, &preferred);
            mm_obj_warn (self, "NR-first RAT mode not accepted: %s; trying LTE first", error->message);
            g_object_set_data (G_OBJECT (task), "rat-lte-fallback", GINT_TO_POINTER (TRUE));
            set_rat (self, allowed, preferred, (GAsyncReadyCallback) initial_rat_ready, task);
            return;
        }
        mm_obj_warn (self, "RAT mode not set: %s; the modem keeps its own", error->message);
    } else if (self->priv->rat_mask) {
        /* enabling: show the mode on D-Bus (the generic init read it before) */
        MmGdbusModem *skeleton = NULL;
        g_object_get (self, MM_IFACE_MODEM_DBUS_SKELETON, &skeleton, NULL);
        if (skeleton) {
            mm_gdbus_modem_set_current_modes (skeleton, g_variant_new ("(uu)", self->priv->rat_allowed,
                                                                       self->priv->rat_preferred));
            g_object_unref (skeleton);
        }
    }
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
    mm_mtk_ccci_ims_stop (self->priv->ims[0]);
    mm_mtk_ccci_ims_stop (self->priv->ims[1]);
    if (self->priv->mipc && self->priv->mipc_opened) {
        /* INTERNAL_CLOSE_REQ is not sent: a new session simply starts with
         * INTERNAL_TEST/OPEN next time, as after a client restart */
        mm_port_mipc_close (self->priv->mipc);
        self->priv->mipc_opened = FALSE;
    }

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
    if (state == MM_MODEM_POWER_STATE_ON) {
        initial_query_pin (task);
        return;
    }
    /* On this firmware CFUN=4 leaves both SIMs inaccessible. IMS must be
     * configured before EFUN, but EFUN must precede generic SIM discovery. */
    mm_obj_msg (modem, "cold start: preparing IMS before powering SIMs for discovery");
    prepare = g_task_new (modem, NULL, (GAsyncReadyCallback) initial_ims_ready, task);
    g_object_set_data (G_OBJECT (prepare), "cold-start", GINT_TO_POINTER (TRUE));
    /* the RAT mode first, so that the radio camps with it from the start */
    apply_initial_rat (MM_BROADBAND_MODEM_MTK_CCCI (modem), prepare);
}

static void
initial_session_ready (MMBroadbandModemMtkCcci *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        initial_complete (task, error);
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

static void
mm_broadband_modem_mtk_ccci_init (MMBroadbandModemMtkCcci *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_BROADBAND_MODEM_MTK_CCCI,
                                              MMBroadbandModemMtkCcciPrivate);
    self->priv->radio_mask     = DEFAULT_RADIO_MASK;
    self->priv->sim_slot       = DEFAULT_SIM_SLOT;
    self->priv->speechd_socket = g_strdup (DEFAULT_SPEECHD_SOCKET);

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

    g_clear_pointer (&self->priv->ims[0], mm_mtk_ccci_ims_free);
    g_clear_pointer (&self->priv->ims[1], mm_mtk_ccci_ims_free);
    if (self->priv->mipc && self->priv->mipc_opened) {
        mm_port_mipc_close (self->priv->mipc);
        self->priv->mipc_opened = FALSE;
    }
    g_clear_object (&self->priv->mipc);

    G_OBJECT_CLASS (mm_broadband_modem_mtk_ccci_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemMtkCcci *self = MM_BROADBAND_MODEM_MTK_CCCI (object);

    g_free (self->priv->wwan_device);
    g_free (self->priv->speechd_socket);
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

static void
iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

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
    return mm_mtk_ccci_ims_owns_link (self->priv->ims[0], link_id) ||
           mm_mtk_ccci_ims_owns_link (self->priv->ims[1], link_id);
}
