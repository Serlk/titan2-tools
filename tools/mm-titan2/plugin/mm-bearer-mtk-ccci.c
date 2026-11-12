/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMBearerMtkCcci: mobile data over a MIPC data call (the
 * sequence is the one t2-modemd uses, tools/modemd/src/data.c).
 *
 * Connect:  DATA_ACT_CALL_REQ (0x0201) -> INTERNAL_EIF_IND (0x4302, usually
 *           first) + DATA_ACT_CALL_CNF (0x0202): aid, transIntfId, address,
 *           DNS, MTU -> "ip link add <wwan>-<transIntfId mod 100> type wwan"
 *           -> wait for the netdev -> link up -> static IPv4 config.
 *           Nothing is confirmed back to the modem.
 * Disconnect: DATA_DEACT_CALL_REQ (0x0203, cause 1) -> link down -> link del.
 * Indications DATA_DEACT_CALL_IND (0x4202) for our aid and INTERNAL_EIF_IND
 * cmd 1 (down) for our interface end the connection.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-bearer-mtk-ccci.h"
#include "mm-broadband-modem-mtk-ccci.h"
#include "mm-port-mipc.h"
#include "mm-mipc-proto.h"
#include "mm-wwan-link.h"
#include "mm-port-net.h"
#include "mm-base-modem-at.h"
#include "mm-modem-helpers.h"
#include "mm-bind.h"
#include "mm-log-object.h"

/* the factory firmware's RIL waits the modem's default (300 s) for the activation confirm;
 * t2-modemd uses 30 s; MM's own connection timeout is 180 s */
#define ACT_CALL_TIMEOUT_SECS    60
/* the factory firmware's RIL allows 900 s for a deactivation; MM's disconnection timeout is 120 s */
#define DEACT_CALL_TIMEOUT_SECS  60
#define WAIT_LINK_PORT_TIMEOUT_MS 5000
#define CGCONTRDP_TIMEOUT_SECS   10
#define CGDCONT_TIMEOUT_SECS     10
/* the factory software sets 1500 when the modem gives no MTU
 * (observed on the phone: TLV 0x0123 = 0) */
#define DEFAULT_MTU              1500

G_DEFINE_TYPE (MMBearerMtkCcci, mm_bearer_mtk_ccci, MM_TYPE_BASE_BEARER)

struct _MMBearerMtkCcciPrivate {
    /* state of the active connection */
    gint      aid;        /* modem PDN id, -1 none */
    gint      tid;        /* transIntfId, -1 none */
    gint      link_id;    /* tid mod 100 */
    gchar    *link_name;  /* "<wwan device>-<link id>" */
    MMPort   *link;       /* the link netdev port, while connected */
    gboolean  link_created;
    gboolean  lost;
    guint8    slot;

    /* the MIPC port whose indications we follow, and the handler */
    MMPortMipc *mipc;
    gulong      indication_id;

    /* INTERNAL_EIF_IND "up" seen while a connection attempt is running: ours
     * arrives before the confirm and may carry the address the confirm
     * lacks, but the modem also brings PDNs up on its own (the IMS PDN),
     * so every indication is kept by transIntfId
     * and matched against the confirm */
    GPtrArray *eif_seen; /* EifInfo */
};

typedef struct {
    guint32  tid;
    gchar   *v4;
    gchar   *v6;
} EifInfo;

static void
eif_info_free (EifInfo *info)
{
    g_free (info->v4);
    g_free (info->v6);
    g_slice_free (EifInfo, info);
}

static EifInfo *
eif_find (MMBearerMtkCcci *self, guint32 tid)
{
    guint i;

    for (i = 0; i < self->priv->eif_seen->len; i++) {
        EifInfo *info = g_ptr_array_index (self->priv->eif_seen, i);

        if (info->tid == tid)
            return info;
    }
    return NULL;
}

/*****************************************************************************/
/* Helpers */

static gchar *
ipv4_to_string (const guint8 *b)
{
    return g_strdup_printf ("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static gchar *
ipv6_to_string (const guint8 *b)
{
    gchar buf[INET6_ADDRSTRLEN];

    if (!inet_ntop (AF_INET6, b, buf, sizeof (buf)))
        return NULL;
    return g_strdup (buf);
}

/* a global unicast IPv6 address: not link-local, not an interface id alone */
static gboolean
ipv6_is_global (const guint8 *b)
{
    guint i;

    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80)
        return FALSE;
    for (i = 0; i < 8; i++)
        if (b[i])
            return TRUE;
    return FALSE;
}

static guint8
pdp_type_from_ip_family (MMBearerIpFamily family)
{
    switch (family) {
    case MM_BEARER_IP_FAMILY_IPV6:
        return MM_MIPC_PDP_IPV6;
    case MM_BEARER_IP_FAMILY_IPV4V6:
        return MM_MIPC_PDP_IPV4V6;
    case MM_BEARER_IP_FAMILY_IPV4:
    case MM_BEARER_IP_FAMILY_NONE:
    case MM_BEARER_IP_FAMILY_ANY:
    case MM_BEARER_IP_FAMILY_NON_IP:
    default:
        /* IPv4 is the only path exercised so far */
        return MM_MIPC_PDP_IP;
    }
}

static void
reset_connection_state (MMBearerMtkCcci *self)
{
    self->priv->lost = FALSE;
    self->priv->aid = -1;
    self->priv->tid = -1;
    self->priv->link_id = -1;
    g_clear_pointer (&self->priv->link_name, g_free);
    g_clear_object (&self->priv->link);
    self->priv->link_created = FALSE;
    g_ptr_array_set_size (self->priv->eif_seen, 0);
}

static void
delete_link (MMBearerMtkCcci *self)
{
    g_autoptr(GError) error = NULL;

    if (!self->priv->link_name)
        return;
    if (!mm_wwan_link_del (self->priv->link_name, &error))
        mm_obj_warn (self, "could not delete link %s: %s", self->priv->link_name, error->message);
    else
        mm_obj_dbg (self, "link %s deleted", self->priv->link_name);
    self->priv->link_created = FALSE;
}

/*****************************************************************************/
/* Indications from the MIPC port */

static EifInfo *
eif_record (MMBearerMtkCcci *self,
            guint32          tid,
            const guint8    *payload,
            gsize            len)
{
    const guint8 *rec;
    gsize         vlen;
    EifInfo      *info;

    info = eif_find (self, tid);
    if (!info) {
        info = g_slice_new0 (EifInfo);
        info->tid = tid;
        g_ptr_array_add (self->priv->eif_seen, info);
    }
    g_clear_pointer (&info->v4, g_free);
    g_clear_pointer (&info->v6, g_free);
    /* IPv4 record: 12 bytes, the address in bytes 0..3 (rest unverified) */
    rec = mm_mipc_tlv_find_record (payload, len, MM_MIPC_T_EIF_V4_RECORDS, &vlen);
    if (rec && vlen >= 4)
        info->v4 = ipv4_to_string (rec);
    /* IPv6 record: size unverified; the first 16 bytes taken as the address */
    rec = mm_mipc_tlv_find_record (payload, len, MM_MIPC_T_EIF_V6_RECORDS, &vlen);
    if (rec && vlen >= 16 && ipv6_is_global (rec))
        info->v6 = ipv6_to_string (rec);
    return info;
}

static void
mipc_indication_cb (MMPortMipc      *port,
                    guint            msg_id,
                    guint            slot,
                    guint            txid,
                    GByteArray      *payload,
                    MMBearerMtkCcci *self)
{
    guint32 v, cmd = 0, cause = 0;

    if (!(slot & self->priv->slot))
        return;

    if (msg_id == MM_MIPC_INTERNAL_EIF_IND) {
        gint tid = -1;

        if (mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_EIF_TRANS_INTF_ID, &v))
            tid = (gint) v;
        mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_EIF_CMD, &cmd);

        if (cmd == MM_MIPC_EIF_UP || cmd == MM_MIPC_EIF_HO_ADDR || cmd == MM_MIPC_EIF_CHANGE) {
            if (mm_base_bearer_get_status (MM_BASE_BEARER (self)) == MM_BEARER_STATUS_CONNECTING && tid >= 0) {
                /* while connecting we do not know our transIntfId yet: keep
                 * every "up" and match it against the confirm; the modem may
                 * bring its own PDNs (IMS) up meanwhile */
                EifInfo *info;

                info = eif_record (self, (guint32) tid, payload->data, payload->len);
                mm_obj_dbg (self, "interface indication while connecting: transIntfId %d %s, address %s",
                            tid, cmd == MM_MIPC_EIF_UP ? "up" : "changed", info->v4 ? info->v4 : "-");
            } else if (self->priv->tid >= 0 && tid == self->priv->tid && cmd != MM_MIPC_EIF_UP)
                /* address change on the connected PDN: reported, not applied (unverified layout) */
                mm_obj_msg (self, "interface %d changed (cmd %u): address update not applied", tid, cmd);
            else
                /* an interface this bearer did not ask for: a modem-initiated
                 * PDN (IMS, emergency); not this bearer's business */
                mm_obj_dbg (self, "interface indication for transIntfId %d (cmd %u) ignored: not this bearer's", tid, cmd);
            return;
        }
        if (cmd == MM_MIPC_EIF_DOWN) {
            if (tid == self->priv->tid)
                self->priv->lost = TRUE;
            if (self->priv->tid >= 0 && tid == self->priv->tid &&
                mm_base_bearer_get_status (MM_BASE_BEARER (self)) == MM_BEARER_STATUS_CONNECTED) {
                mm_obj_msg (self, "interface %d down indication: connection lost", tid);
                mm_base_bearer_report_connection_status (MM_BASE_BEARER (self),
                                                         MM_BEARER_CONNECTION_STATUS_DISCONNECTED);
            } else
                mm_obj_dbg (self, "interface %d down indication ignored: not this bearer's", tid);
            return;
        }
        if (cmd == MM_MIPC_EIF_MTU) {
            if (mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_EIF_MTU, &v))
                mm_obj_msg (self, "interface %d: MTU change to %u (not applied)", tid, v);
            return;
        }
        mm_obj_dbg (self, "interface %d: indication command %u", tid, cmd);
        return;
    }

    if (msg_id == MM_MIPC_DATA_DEACT_CALL_IND) {
        gint aid = -1;

        if (mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_IND_AID, &v))
            aid = (gint) v;
        mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_IND_CAUSE, &cause);
        if (aid == self->priv->aid)
            self->priv->lost = TRUE;
        if (self->priv->aid >= 0 && aid == self->priv->aid &&
            mm_base_bearer_get_status (MM_BASE_BEARER (self)) == MM_BEARER_STATUS_CONNECTED) {
            g_autoptr(GError) error = NULL;

            mm_obj_msg (self, "PDN %d deactivated by the modem (cause %u)", aid, cause);
            error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "PDN deactivated by the modem, cause %u", cause);
            mm_base_bearer_report_connection_status_detailed (MM_BASE_BEARER (self),
                                                              MM_BEARER_CONNECTION_STATUS_DISCONNECTED,
                                                              error);
        }
        return;
    }
}

static void
forced_close_cb (MMPortMipc      *port,
                 MMBearerMtkCcci *self)
{
    self->priv->lost = TRUE;
    if (mm_base_bearer_get_status (MM_BASE_BEARER (self)) == MM_BEARER_STATUS_CONNECTED) {
        mm_obj_msg (self, "MIPC port closed: connection lost");
        mm_base_bearer_report_connection_status (MM_BASE_BEARER (self),
                                                 MM_BEARER_CONNECTION_STATUS_DISCONNECTED);
    }
}

static void
follow_mipc_port (MMBearerMtkCcci *self,
                  MMPortMipc      *mipc)
{
    if (self->priv->mipc == mipc)
        return;
    if (self->priv->mipc) {
        g_signal_handlers_disconnect_by_data (self->priv->mipc, self);
        g_clear_object (&self->priv->mipc);
    }
    if (mipc) {
        self->priv->mipc = g_object_ref (mipc);
        self->priv->indication_id = g_signal_connect (mipc, MM_PORT_MIPC_SIGNAL_INDICATION,
                                                      G_CALLBACK (mipc_indication_cb), self);
        g_signal_connect (mipc, "forced-close", G_CALLBACK (forced_close_cb), self);
    }
}

/*****************************************************************************/
/* Connection status reported from outside (indications, port loss) */

static void
report_connection_status (MMBaseBearer             *_self,
                          MMBearerConnectionStatus  status,
                          const GError             *connection_error)
{
    MMBearerMtkCcci *self = MM_BEARER_MTK_CCCI (_self);

    if (status == MM_BEARER_CONNECTION_STATUS_DISCONNECTED ||
        status == MM_BEARER_CONNECTION_STATUS_CONNECTION_FAILED) {
        /* the modem has already torn the PDN down; remove our link */
        if (self->priv->link)
            mm_port_set_connected (self->priv->link, FALSE);
        delete_link (self);
        reset_connection_state (self);
    }

    MM_BASE_BEARER_CLASS (mm_bearer_mtk_ccci_parent_class)->report_connection_status (_self, status, connection_error);
}

/*****************************************************************************/
/* Connect */

typedef enum {
    CONNECT_STEP_FIRST,
    CONNECT_STEP_CHECK,
    CONNECT_STEP_APN,
    CONNECT_STEP_PROFILES,
    CONNECT_STEP_ACTIVATE,
    CONNECT_STEP_IP_FALLBACK,
    CONNECT_STEP_LINK_ADD,
    CONNECT_STEP_WAIT_LINK_PORT,
    CONNECT_STEP_LINK_UP,
    CONNECT_STEP_LAST,
} ConnectStep;

typedef struct {
    ConnectStep               step;
    MMBroadbandModemMtkCcci  *modem;
    MMPortMipc               *mipc;
    MMBearerIpFamily          ip_family;
    gchar                    *apn;
    gchar                    *user;
    gchar                    *password;
    /* from the confirm */
    gint                      aid;
    gint                      tid;
    gint                      link_id;
    gchar                    *v4_addr;
    gchar                    *v6_addr;
    GPtrArray                *dns;
    guint                     mtu;
    /* the link */
    gchar                    *link_name;
    MMPort                   *link;
    gboolean                  link_created;
    gboolean                  activated;
    GError                   *failure;
} ConnectContext;

static void
connect_context_free (ConnectContext *ctx)
{
    g_clear_error (&ctx->failure);
    g_free (ctx->apn);
    g_free (ctx->user);
    g_free (ctx->password);
    g_free (ctx->v4_addr);
    g_free (ctx->v6_addr);
    g_free (ctx->link_name);
    if (ctx->dns)
        g_ptr_array_unref (ctx->dns);
    g_clear_object (&ctx->link);
    g_clear_object (&ctx->mipc);
    g_clear_object (&ctx->modem);
    g_slice_free (ConnectContext, ctx);
}

static MMBearerConnectResult *
connect_finish (MMBaseBearer  *self,
                GAsyncResult  *res,
                GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void connect_context_step (GTask *task);

/* Undo only the PDN and netdev created by this connection attempt. */
static void
connect_cleanup (GTask *task)
{
    MMBearerMtkCcci *self = g_task_get_source_object (task);
    ConnectContext *ctx = g_task_get_task_data (task);
    if (ctx->link_created && ctx->link_name) {
        g_autoptr(GError) error = NULL;
        if (!mm_wwan_link_del (ctx->link_name, &error))
            mm_obj_warn (self, "connection rollback: %s", error->message);
    }
    reset_connection_state (self);
    g_task_return_error (task, g_steal_pointer (&ctx->failure));
    g_object_unref (task);
}

static void
rollback_ready (MMPortMipc *mipc, GAsyncResult *res, GTask *task)
{
    MMBearerMtkCcci *self = g_task_get_source_object (task);
    g_autoptr(GError) error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (mipc, res, &error);
    if (!cnf || cnf->result)
        mm_obj_warn (self, "PDN rollback failed: %s (result %u)",
                     error ? error->message : "refused", cnf ? cnf->result : 0);
    connect_cleanup (task);
}

static void
connect_fail (GTask *task, GError *error)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    ctx->failure = error;
    if (ctx->activated && ctx->aid >= 0 && mm_port_mipc_get_session_ready (ctx->mipc)) {
        g_autoptr(GByteArray) tlvs = g_byte_array_new ();
        ctx->activated = FALSE;
        mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_DEACT_AID, ctx->aid);
        mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_DEACT_CAUSE, MM_MIPC_DEACT_CAUSE_NORMAL);
        mm_port_mipc_request (ctx->mipc, MM_MIPC_DATA_DEACT_CALL_REQ,
            mm_broadband_modem_mtk_ccci_get_mipc_slot (ctx->modem), tlvs,
            DEACT_CALL_TIMEOUT_SECS, NULL, (GAsyncReadyCallback) rollback_ready, task);
        return;
    }
    connect_cleanup (task);
}

static void
link_up_ready (MMPortNet    *link,
               GAsyncResult *res,
               GTask        *task)
{
    GError *error = NULL;

    if (!mm_port_net_link_setup_finish (link, res, &error)) {
        g_prefix_error (&error, "could not bring the link up: ");
        connect_fail (task, error);
        return;
    }
    ((ConnectContext *) g_task_get_task_data (task))->step++;
    connect_context_step (task);
}

static void
wait_link_port_ready (MMBaseModem  *modem,
                      GAsyncResult *res,
                      GTask        *task)
{
    ConnectContext *ctx;
    GError         *error = NULL;

    ctx = g_task_get_task_data (task);
    ctx->link = mm_base_modem_wait_link_port_finish (modem, res, &error);
    if (!ctx->link) {
        g_prefix_error (&error, "link %s did not appear: ", ctx->link_name);
        connect_fail (task, error);
        return;
    }
    ctx->step++;
    connect_context_step (task);
}

static void
cgcontrdp_ready (MMBaseModem  *modem,
                 GAsyncResult *res,
                 GTask        *task)
{
    MMBearerMtkCcci  *self;
    ConnectContext   *ctx;
    const gchar      *response;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *apn = NULL;
    g_autofree gchar *local_address = NULL;
    g_autofree gchar *subnet = NULL;
    g_autofree gchar *gateway = NULL;
    g_autofree gchar *dns1 = NULL;
    g_autofree gchar *dns2 = NULL;
    guint             cid = 0, bearer_id = 0;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response ||
        !mm_3gpp_parse_cgcontrdp_response (response, &cid, &bearer_id, &apn, &local_address,
                                           &subnet, &gateway, &dns1, &dns2, &error)) {
        mm_obj_warn (self, "could not read the PDN's dynamic parameters with +CGCONTRDP: %s",
                     error ? error->message : "unknown error");
    } else {
        if (!ctx->v4_addr && local_address)
            ctx->v4_addr = g_steal_pointer (&local_address);
        if (ctx->dns->len == 0) {
            if (dns1 && g_strcmp0 (dns1, "0.0.0.0"))
                g_ptr_array_add (ctx->dns, g_steal_pointer (&dns1));
            if (dns2 && g_strcmp0 (dns2, "0.0.0.0"))
                g_ptr_array_add (ctx->dns, g_steal_pointer (&dns2));
        }
    }
    ctx->step++;
    connect_context_step (task);
}

static void
act_call_ready (MMPortMipc   *mipc,
                GAsyncResult *res,
                GTask        *task)
{
    MMBearerMtkCcci      *self;
    ConnectContext       *ctx;
    g_autoptr(MMMipcCnf)  cnf = NULL;
    GError               *error = NULL;
    const guint8         *p, *val;
    gsize                 len, vlen, i, off = 0;
    guint32               v;
    MMMipcTlv             t;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    cnf = mm_port_mipc_request_finish (mipc, res, &error);
    if (!cnf) {
        connect_fail (task, error);
        return;
    }
    if (cnf->result) {
        /* result codes other than 0 are unexplored (questions 15) */
        connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                         "MIPC DATA_ACT_CALL failed: result %u", cnf->result));
        return;
    }

    p = cnf->payload->data;
    len = cnf->payload->len;


    if (mm_mipc_tlv_find_uint (p, len, MM_MIPC_T_CNF_AID, &v) && v <= 255) {
        ctx->aid = (gint) v;
        ctx->activated = TRUE;
    } else {
        connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Activation confirm has no valid aid"));
        return;
    }
    if (mm_mipc_tlv_find_uint (p, len, MM_MIPC_T_CNF_TRANS_INTF_ID, &v)) {
        ctx->tid = (gint) v;
        ctx->link_id = (gint) (v % MM_MIPC_TRANS_INTF_MOD);
    } else if (mm_mipc_tlv_find_uint (p, len, MM_MIPC_T_CNF_IF_ID, &v) && v <= 20) {
        ctx->link_id = (gint) v;
    } else if (self->priv->eif_seen->len == 1) {
        /* unverified fallback: the one interface that came up during our request */
        EifInfo *info = g_ptr_array_index (self->priv->eif_seen, 0);

        ctx->tid = (gint) info->tid;
        ctx->link_id = (gint) (info->tid % MM_MIPC_TRANS_INTF_MOD);
        mm_obj_warn (self, "no transIntfId in the confirm, using the only interface indication's: %d", ctx->tid);
    } else {
        connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                         "MIPC DATA_ACT_CALL confirm names no data interface"));
        return;
    }

    val = mm_mipc_tlv_find (p, len, MM_MIPC_T_CNF_V4_ADDRS, &vlen);
    if (val && vlen >= 4)
        ctx->v4_addr = ipv4_to_string (val); /* the first address; the order is unverified */
    val = mm_mipc_tlv_find (p, len, MM_MIPC_T_CNF_V4_DNS, &vlen);
    for (i = 0; val && i + 4 <= vlen; i += 4)
        g_ptr_array_add (ctx->dns, ipv4_to_string (val + i));
    val = mm_mipc_tlv_find (p, len, MM_MIPC_T_CNF_V6_DNS, &vlen);
    for (i = 0; val && i + 16 <= vlen; i += 16) {
        gchar *s = ipv6_to_string (val + i);

        if (s)
            g_ptr_array_add (ctx->dns, s);
    }
    if (mm_mipc_tlv_find_uint (p, len, MM_MIPC_T_CNF_MTU_V4, &v) && v >= 68 && v <= 9000)
        ctx->mtu = v;
    else
        ctx->mtu = DEFAULT_MTU;
    if (mm_mipc_tlv_find_uint (p, len, MM_MIPC_T_CNF_EPS_BEARER, &v))
        mm_obj_dbg (self, "EPS bearer id %u", v);

    /* the interface indication usually came first: take its address
     * when the confirm has none and it names the same interface */
    if (ctx->tid >= 0) {
        EifInfo *info = eif_find (self, (guint32) ctx->tid);

        if (info) {
            if (!ctx->v4_addr && info->v4)
                ctx->v4_addr = g_strdup (info->v4);
            if (!ctx->v6_addr && info->v6)
                ctx->v6_addr = g_strdup (info->v6);
        } else
            mm_obj_dbg (self, "no interface indication seen for transIntfId %d before the confirm", ctx->tid);
    }

    /* every TLV id of the confirm, for the open questions (masked ids 0x810F,
     * 0x8121, 0x812F, 0x8131, 0x0128, 0x0129) */
    while (mm_mipc_tlv_next (p, len, &off, &t) == 1)
        mm_obj_dbg (self, "confirm TLV 0x%04x len %" G_GSIZE_FORMAT, t.raw_id, t.len);

    mm_obj_msg (self, "PDN activated over MIPC: aid %d, transIntfId %d, link id %d, address %s, %u DNS, MTU %u",
                ctx->aid, ctx->tid, ctx->link_id, ctx->v4_addr ? ctx->v4_addr : "none",
                ctx->dns->len, ctx->mtu);

    /* remember it now: a modem-side deactivation can arrive any time from here */
    self->priv->aid = ctx->aid;
    self->priv->tid = ctx->tid;
    self->priv->link_id = ctx->link_id;

    ctx->step++;
    connect_context_step (task);
}

static void
cgdcont_ready (MMBaseModem  *modem,
               GAsyncResult *res,
               GTask        *task)
{
    MMBearerMtkCcci  *self;
    ConnectContext   *ctx;
    const gchar      *response;
    g_autoptr(GError) error = NULL;
    GList            *contexts, *l;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    contexts = response ? mm_3gpp_parse_cgdcont_read_response (response, &error) : NULL;
    if (!contexts) {
        connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                         "No APN given and the attach context could not be read: %s",
                                         error ? error->message : "no contexts"));
        return;
    }
    /* Only CID 0 is the attach context on this modem. A remaining IMS CID
     * after disconnect must not silently become the default internet APN. */
    for (l = contexts; l; l = g_list_next (l)) {
        MM3gppPdpContext *pdp = l->data;

        if (pdp->cid == 0 && pdp->apn && pdp->apn[0]) {
            g_free (ctx->apn);
            ctx->apn = g_strdup (pdp->apn);
        }
    }
    mm_3gpp_pdp_context_list_free (contexts);
    if (!ctx->apn) {
        connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                         "No APN given and no attach APN in the modem"));
        return;
    }
    mm_obj_msg (self, "using the attach context's APN: %s", ctx->apn);
    ctx->step++;
    connect_context_step (task);
}

static void
data_profiles_ready (MMBroadbandModemMtkCcci *modem, GAsyncResult *res, GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    GError *error = NULL;

    if (!mm_broadband_modem_mtk_ccci_prepare_data_finish (modem, res, &error)) {
        connect_fail (task, error);
        return;
    }
    ctx->step++;
    connect_context_step (task);
}

static void
connect_context_step (GTask *task)
{
    MMBearerMtkCcci *self;
    ConnectContext  *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    if (g_task_get_cancellable (task) && g_cancellable_is_cancelled (g_task_get_cancellable (task))) {
        connect_fail (task, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Connection cancelled"));
        return;
    }

    if (self->priv->lost) {
        connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "PDN lost during connection"));
        return;
    }

    switch (ctx->step) {
    case CONNECT_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case CONNECT_STEP_CHECK:
        if (!ctx->mipc) {
            connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                             "The modem has no MIPC data port"));
            return;
        }
        if (!mm_port_mipc_get_session_ready (ctx->mipc)) {
            connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                             "The MIPC session is not ready"));
            return;
        }
        self->priv->slot = mm_broadband_modem_mtk_ccci_get_mipc_slot (ctx->modem);
        follow_mipc_port (self, ctx->mipc);
        g_ptr_array_set_size (self->priv->eif_seen, 0);
        ctx->step++;
        /* fall through */

    case CONNECT_STEP_APN:
        if (!ctx->apn || !ctx->apn[0]) {
            /* no APN from the connection manager: the attach APN (AT port) */
            mm_base_modem_at_command (MM_BASE_MODEM (ctx->modem), "+CGDCONT?", CGDCONT_TIMEOUT_SECS, FALSE,
                                      (GAsyncReadyCallback) cgdcont_ready, task);
            return;
        }
        ctx->step++;
        /* fall through */

    case CONNECT_STEP_PROFILES:
        mm_broadband_modem_mtk_ccci_prepare_data (ctx->modem, ctx->apn,
                                                 (GAsyncReadyCallback) data_profiles_ready, task);
        return;

    case CONNECT_STEP_ACTIVATE: {
        g_autoptr(GByteArray) tlvs = g_byte_array_new ();
        guint8                pdp = pdp_type_from_ip_family (ctx->ip_family);

        /* the TLV set of the captured request (the 672-byte traffic
         * descriptor 0x010D is left out: t2-modemd defaults to that too) */
        mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_ACT_APN, ctx->apn);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_ACT_APN_TYPE, MM_MIPC_ACT_APN_TYPE_DEFAULT);
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_ACT_PDP_TYPE, pdp);
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_ACT_ROAM_PDP_TYPE, pdp);
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_ACT_AUTH_TYPE, 0); /* coding unverified */
        mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_ACT_USER, ctx->user ? ctx->user : "");
        mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_ACT_PASSWORD, ctx->password ? ctx->password : "");
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_ACT_CONST_7F, MM_MIPC_ACT_CONST_7F);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_ACT_BEARER_MASK, MM_MIPC_ACT_BEARER_ALL);
        mm_mipc_tlv_add_u8  (tlvs, MM_MIPC_T_ACT_SPECIAL, 0);
        mm_mipc_tlv_add_u32 (tlvs, MM_MIPC_T_ACT_NO_MATCH_ALL, 0);

        mm_obj_msg (self, "activating PDN over MIPC: APN '%s', PDP type %u, slot 0x%02x",
                    ctx->apn, pdp, mm_broadband_modem_mtk_ccci_get_mipc_slot (ctx->modem));
        /* Drain the confirm even after cancellation: its aid is needed for rollback. */
        mm_port_mipc_request (ctx->mipc, MM_MIPC_DATA_ACT_CALL_REQ,
                              mm_broadband_modem_mtk_ccci_get_mipc_slot (ctx->modem),
                              tlvs, ACT_CALL_TIMEOUT_SECS, NULL,
                              (GAsyncReadyCallback) act_call_ready, task);
        return;
    }

    case CONNECT_STEP_IP_FALLBACK:
        if ((!ctx->v4_addr || ctx->dns->len == 0) && ctx->aid >= 0) {
            g_autofree gchar *cmd = NULL;

            /* observed for the attach context: +CGCONTRDP lists address and DNS */
            cmd = g_strdup_printf ("+CGCONTRDP=%d", ctx->aid);
            mm_base_modem_at_command (MM_BASE_MODEM (ctx->modem), cmd, CGCONTRDP_TIMEOUT_SECS, FALSE,
                                      (GAsyncReadyCallback) cgcontrdp_ready, task);
            return;
        }
        ctx->step++;
        /* fall through */

    case CONNECT_STEP_LINK_ADD: {
        GError      *error = NULL;
        gboolean     existed = FALSE;
        const gchar *wwan_dev;

        if (!ctx->v4_addr && !ctx->v6_addr) {
            connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                             "The modem reported no IP address for the PDN"));
            return;
        }
        if (mm_broadband_modem_mtk_ccci_ims_owns_link (ctx->modem, ctx->link_id)) {
            connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Data link belongs to IMS"));
            return;
        }
        wwan_dev = mm_broadband_modem_mtk_ccci_get_wwan_device (ctx->modem);
        ctx->link_name = g_strdup_printf ("%s-%d", wwan_dev, ctx->link_id);
        if (!mm_wwan_link_add (wwan_dev, (guint) ctx->link_id, ctx->link_name, &existed, &error)) {
            connect_fail (task, error);
            return;
        }
        if (existed) {
            connect_fail (task, g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                            "Link %s already exists; refusing to take ownership", ctx->link_name));
            return;
        }
        /* Creation succeeded with EXCL, so any MM port under this name belongs
         * to a previously deleted netdev. Its MMPortNet caches the old ifindex.
         * Drop it before yielding to udev and waiting for the new link event. */
        if (mm_base_modem_release_link_port (MM_BASE_MODEM (ctx->modem), "net", ctx->link_name, NULL))
            mm_obj_dbg (self, "released stale link port %s before waiting for its replacement", ctx->link_name);
        ctx->link_created = TRUE;
        self->priv->link_name = g_strdup (ctx->link_name);
        self->priv->link_created = TRUE;
        mm_obj_msg (self, "wwan link %s (parent %s, link id %d) %s",
                    ctx->link_name, wwan_dev, ctx->link_id, existed ? "already existed" : "created");
        ctx->step++;
    }
        /* fall through */

    case CONNECT_STEP_WAIT_LINK_PORT:
        /* the netdev is announced by udev/netlink and grabbed as a link port */
        mm_base_modem_wait_link_port (MM_BASE_MODEM (ctx->modem), "net", ctx->link_name,
                                      WAIT_LINK_PORT_TIMEOUT_MS,
                                      (GAsyncReadyCallback) wait_link_port_ready, task);
        return;

    case CONNECT_STEP_LINK_UP:
        mm_port_net_link_setup (MM_PORT_NET (ctx->link), TRUE, ctx->mtu, g_task_get_cancellable (task),
                                (GAsyncReadyCallback) link_up_ready, task);
        return;

    case CONNECT_STEP_LAST: {
        MMBearerConnectResult       *result;
        g_autoptr(MMBearerIpConfig)  ipv4_config = NULL;
        g_autoptr(MMBearerIpConfig)  ipv6_config = NULL;

        if (ctx->v4_addr) {
            ipv4_config = mm_bearer_ip_config_new ();
            mm_bearer_ip_config_set_method (ipv4_config, MM_BEARER_IP_METHOD_STATIC);
            mm_bearer_ip_config_set_address (ipv4_config, ctx->v4_addr);
            /* point-to-point PDN: a host address; the factory software hands the
             * connection manager the PDN's own address as gateway */
            mm_bearer_ip_config_set_prefix (ipv4_config, 32);
            mm_bearer_ip_config_set_gateway (ipv4_config, ctx->v4_addr);
            if (ctx->mtu)
                mm_bearer_ip_config_set_mtu (ipv4_config, ctx->mtu);
            if (ctx->dns->len) {
                g_ptr_array_add (ctx->dns, NULL);
                mm_bearer_ip_config_set_dns (ipv4_config, (const gchar **) ctx->dns->pdata);
                g_ptr_array_remove_index (ctx->dns, ctx->dns->len - 1);
            }
        }
        if (ctx->v6_addr) {
            ipv6_config = mm_bearer_ip_config_new ();
            mm_bearer_ip_config_set_method (ipv6_config, MM_BEARER_IP_METHOD_STATIC);
            mm_bearer_ip_config_set_address (ipv6_config, ctx->v6_addr);
            mm_bearer_ip_config_set_prefix (ipv6_config, 64);
        } else if (pdp_type_from_ip_family (ctx->ip_family) != MM_MIPC_PDP_IP) {
            /* IPv6 requested but no address known: let the kernel's SLAAC
             * inside the PDN do it (unverified on this network) */
            ipv6_config = mm_bearer_ip_config_new ();
            mm_bearer_ip_config_set_method (ipv6_config, MM_BEARER_IP_METHOD_DHCP);
        }

        self->priv->link = g_object_ref (ctx->link);
        mm_port_set_connected (ctx->link, TRUE);

        result = mm_bearer_connect_result_new (ctx->link, ipv4_config, ipv6_config);
        mm_bearer_connect_result_set_multiplexed (result, TRUE);
        g_task_return_pointer (task, result, (GDestroyNotify) mm_bearer_connect_result_unref);
        g_object_unref (task);
        return;
    }

    default:
        g_assert_not_reached ();
    }
}

static void
_connect (MMBaseBearer        *_self,
         GCancellable        *cancellable,
         GAsyncReadyCallback  callback,
         gpointer             user_data)
{
    MMBearerMtkCcci    *self = MM_BEARER_MTK_CCCI (_self);
    ConnectContext     *ctx;
    GTask              *task;
    MMBaseModem        *modem = NULL;
    MMBearerProperties *config;

    g_object_get (self, MM_BASE_BEARER_MODEM, &modem, NULL);
    g_assert (modem);

    task = g_task_new (self, cancellable, callback, user_data);
    /* Complete only after rollback, including when the caller cancels. */
    g_task_set_check_cancellable (task, FALSE);

    if (!MM_IS_BROADBAND_MODEM_MTK_CCCI (modem)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Unexpected modem type");
        g_object_unref (task);
        g_object_unref (modem);
        return;
    }

    ctx = g_slice_new0 (ConnectContext);
    ctx->step    = CONNECT_STEP_FIRST;
    ctx->modem   = MM_BROADBAND_MODEM_MTK_CCCI (modem); /* ownership of the ref */
    ctx->aid     = -1;
    ctx->tid     = -1;
    ctx->link_id = -1;
    ctx->dns     = g_ptr_array_new_with_free_func (g_free);
    g_task_set_task_data (task, ctx, (GDestroyNotify) connect_context_free);

    ctx->mipc = mm_broadband_modem_mtk_ccci_peek_port_mipc (ctx->modem);
    if (ctx->mipc)
        g_object_ref (ctx->mipc);

    config = mm_base_bearer_peek_config (MM_BASE_BEARER (self));
    ctx->apn       = g_strdup (mm_bearer_properties_get_apn (config));
    ctx->user      = g_strdup (mm_bearer_properties_get_user (config));
    ctx->password  = g_strdup (mm_bearer_properties_get_password (config));
    ctx->ip_family = mm_bearer_properties_get_ip_type (config);

    connect_context_step (task);
}

/*****************************************************************************/
/* Disconnect */

typedef enum {
    DISCONNECT_STEP_FIRST,
    DISCONNECT_STEP_DEACTIVATE,
    DISCONNECT_STEP_LINK_DOWN,
    DISCONNECT_STEP_LINK_DEL,
    DISCONNECT_STEP_LAST,
} DisconnectStep;

typedef struct {
    DisconnectStep           step;
    MMBroadbandModemMtkCcci *modem;
} DisconnectContext;

static void
disconnect_context_free (DisconnectContext *ctx)
{
    g_clear_object (&ctx->modem);
    g_slice_free (DisconnectContext, ctx);
}

static gboolean
disconnect_finish (MMBaseBearer  *self,
                   GAsyncResult  *res,
                   GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void disconnect_context_step (GTask *task);

static void
deact_call_ready (MMPortMipc   *mipc,
                  GAsyncResult *res,
                  GTask        *task)
{
    MMBearerMtkCcci      *self;
    DisconnectContext    *ctx;
    g_autoptr(MMMipcCnf)  cnf = NULL;
    g_autoptr(GError)     error = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    cnf = mm_port_mipc_request_finish (mipc, res, &error);
    if (!cnf)
        mm_obj_warn (self, "MIPC DATA_DEACT_CALL failed: %s (removing the link anyway)", error->message);
    else if (cnf->result)
        mm_obj_warn (self, "MIPC DATA_DEACT_CALL: result %u (removing the link anyway)", cnf->result);
    else
        mm_obj_msg (self, "PDN %d deactivated", self->priv->aid);
    ctx->step++;
    disconnect_context_step (task);
}

static void
link_down_ready (MMPortNet    *link,
                 GAsyncResult *res,
                 GTask        *task)
{
    MMBearerMtkCcci  *self;
    g_autoptr(GError) error = NULL;

    self = g_task_get_source_object (task);
    if (!mm_port_net_link_setup_finish (link, res, &error))
        mm_obj_dbg (self, "could not bring the link down: %s", error->message);
    ((DisconnectContext *) g_task_get_task_data (task))->step++;
    disconnect_context_step (task);
}

static void
disconnect_context_step (GTask *task)
{
    MMBearerMtkCcci   *self;
    DisconnectContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case DISCONNECT_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case DISCONNECT_STEP_DEACTIVATE:
        if (self->priv->aid >= 0 && self->priv->mipc && mm_port_mipc_get_session_ready (self->priv->mipc)) {
            g_autoptr(GByteArray) tlvs = g_byte_array_new ();

            /* cause 1 = normal (coding unverified) */
            mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_DEACT_AID, (guint8) self->priv->aid);
            mm_mipc_tlv_add_u8 (tlvs, MM_MIPC_T_DEACT_CAUSE, MM_MIPC_DEACT_CAUSE_NORMAL);
            mm_obj_msg (self, "deactivating PDN %d over MIPC", self->priv->aid);
            mm_port_mipc_request (self->priv->mipc, MM_MIPC_DATA_DEACT_CALL_REQ,
                                  mm_broadband_modem_mtk_ccci_get_mipc_slot (ctx->modem),
                                  tlvs, DEACT_CALL_TIMEOUT_SECS, NULL,
                                  (GAsyncReadyCallback) deact_call_ready, task);
            return;
        }
        ctx->step++;
        /* fall through */

    case DISCONNECT_STEP_LINK_DOWN:
        if (self->priv->link) {
            mm_port_set_connected (self->priv->link, FALSE);
            mm_port_net_link_setup (MM_PORT_NET (self->priv->link), FALSE, 0, NULL,
                                    (GAsyncReadyCallback) link_down_ready, task);
            return;
        }
        ctx->step++;
        /* fall through */

    case DISCONNECT_STEP_LINK_DEL:
        delete_link (self);
        ctx->step++;
        /* fall through */

    case DISCONNECT_STEP_LAST:
        reset_connection_state (self);
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

static void
_disconnect (MMBaseBearer        *_self,
            GAsyncReadyCallback  callback,
            gpointer             user_data)
{
    MMBearerMtkCcci   *self = MM_BEARER_MTK_CCCI (_self);
    DisconnectContext *ctx;
    GTask             *task;
    MMBaseModem       *modem = NULL;

    g_object_get (self, MM_BASE_BEARER_MODEM, &modem, NULL);
    g_assert (modem);

    task = g_task_new (self, NULL, callback, user_data);
    ctx = g_slice_new0 (DisconnectContext);
    ctx->step  = DISCONNECT_STEP_FIRST;
    ctx->modem = MM_BROADBAND_MODEM_MTK_CCCI (modem);
    g_task_set_task_data (task, ctx, (GDestroyNotify) disconnect_context_free);

    disconnect_context_step (task);
}

/*****************************************************************************/
/* Connection status monitoring: the modem announces losses, no polling */

static MMBearerConnectionStatus
load_connection_status_finish (MMBaseBearer  *self,
                               GAsyncResult  *res,
                               GError       **error)
{
    gssize value;

    value = g_task_propagate_int (G_TASK (res), error);
    return value < 0 ? MM_BEARER_CONNECTION_STATUS_UNKNOWN : (MMBearerConnectionStatus) value;
}

static void
load_connection_status (MMBaseBearer        *self,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                             "Connection status polling not required: the modem reports losses");
    g_object_unref (task);
}

/*****************************************************************************/
/* Statistics: the link's kernel counters */

static gboolean
reload_stats_finish (MMBaseBearer  *self,
                     guint64       *bytes_rx,
                     guint64       *bytes_tx,
                     GAsyncResult  *res,
                     GError       **error)
{
    guint64 *stats;

    stats = g_task_propagate_pointer (G_TASK (res), error);
    if (!stats)
        return FALSE;
    if (bytes_rx)
        *bytes_rx = stats[0];
    if (bytes_tx)
        *bytes_tx = stats[1];
    g_free (stats);
    return TRUE;
}

static gboolean
read_sysfs_u64 (const gchar *ifname,
                const gchar *stat,
                guint64     *value)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;

    path = g_strdup_printf ("/sys/class/net/%s/statistics/%s", ifname, stat);
    if (!g_file_get_contents (path, &contents, NULL, NULL))
        return FALSE;
    *value = g_ascii_strtoull (contents, NULL, 10);
    return TRUE;
}

static void
reload_stats (MMBaseBearer        *_self,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
    MMBearerMtkCcci *self = MM_BEARER_MTK_CCCI (_self);
    GTask           *task;
    guint64         *stats;

    task = g_task_new (self, NULL, callback, user_data);
    if (!self->priv->link_name) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "Not connected");
        g_object_unref (task);
        return;
    }
    stats = g_new0 (guint64, 2);
    if (!read_sysfs_u64 (self->priv->link_name, "rx_bytes", &stats[0]) ||
        !read_sysfs_u64 (self->priv->link_name, "tx_bytes", &stats[1])) {
        g_free (stats);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "Could not read the statistics of %s", self->priv->link_name);
        g_object_unref (task);
        return;
    }
    g_task_return_pointer (task, stats, g_free);
    g_object_unref (task);
}

/*****************************************************************************/

MMBaseBearer *
mm_bearer_mtk_ccci_new (MMBroadbandModemMtkCcci *modem,
                        MMBearerProperties      *config)
{
    MMBaseBearer *bearer;

    /* inherits from MMBaseBearer (not MMBroadbandBearer), so not async-initable */
    bearer = g_object_new (MM_TYPE_BEARER_MTK_CCCI,
                           MM_BASE_BEARER_MODEM,  modem,
                           MM_BIND_TO,            G_OBJECT (modem),
                           MM_BASE_BEARER_CONFIG, config,
                           NULL);

    /* only export valid bearers */
    mm_base_bearer_export (bearer);

    return bearer;
}

static void
mm_bearer_mtk_ccci_init (MMBearerMtkCcci *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_BEARER_MTK_CCCI, MMBearerMtkCcciPrivate);
    self->priv->aid = -1;
    self->priv->tid = -1;
    self->priv->link_id = -1;
    self->priv->eif_seen = g_ptr_array_new_with_free_func ((GDestroyNotify) eif_info_free);
}

static void
dispose (GObject *object)
{
    MMBearerMtkCcci *self = MM_BEARER_MTK_CCCI (object);

    follow_mipc_port (self, NULL);
    if (self->priv->link_created)
        delete_link (self);
    reset_connection_state (self);

    G_OBJECT_CLASS (mm_bearer_mtk_ccci_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMBearerMtkCcci *self = MM_BEARER_MTK_CCCI (object);

    g_ptr_array_unref (self->priv->eif_seen);

    G_OBJECT_CLASS (mm_bearer_mtk_ccci_parent_class)->finalize (object);
}

static void
mm_bearer_mtk_ccci_class_init (MMBearerMtkCcciClass *klass)
{
    GObjectClass      *object_class = G_OBJECT_CLASS (klass);
    MMBaseBearerClass *base_bearer_class = MM_BASE_BEARER_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBearerMtkCcciPrivate));

    object_class->dispose = dispose;
    object_class->finalize = finalize;

    base_bearer_class->connect = _connect;
    base_bearer_class->connect_finish = connect_finish;
    base_bearer_class->disconnect = _disconnect;
    base_bearer_class->disconnect_finish = disconnect_finish;
    base_bearer_class->report_connection_status = report_connection_status;
    base_bearer_class->load_connection_status = load_connection_status;
    base_bearer_class->load_connection_status_finish = load_connection_status_finish;
    base_bearer_class->reload_stats = reload_stats;
    base_bearer_class->reload_stats_finish = reload_stats_finish;
}
