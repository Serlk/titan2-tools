/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 * IMS (VoLTE) set-up over MIPC. Written from a clean-room behavioural description of the protocol; no code of the phone's Android software was used.
 * Values marked unverified there remain unverified here.
 */
#include <config.h>
#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include "mm-mtk-ccci-ims.h"
#include "mm-base-modem-at.h"
#include "mm-modem-helpers.h"
#include "mm-log-object.h"
#include <mm-errors-types.h>
#include "mm-wwan-link.h"
#include "mm-modem-helpers-mtk-ccci.h"

#define MAX_PDNS 32
#define MAX_SPIS 4096

typedef struct {
    guint tid;
    gboolean labelled, up, created;
    guint mtu;
    gchar *address, *link;
} ImsPdn;

struct _MMMtkCcciIms {
    /* owner: the source object of every result (GTask) and the log object.
     * The shared context (mm-mtk-ccci-shared.c) in the plugin; never assume
     * it is a modem in a callback (DESIGN.md §11.2). Without a radio hook the
     * radio-off step is an AT command of the owner, which then must be an
     * MMBaseModem (the host tests' owner). */
    gpointer owner;
    MMPortMipc *port;
    guint8 slot;
    gchar *wwan, *internet_apn, *ims_apn, *sos_apn;
    gboolean enabled, configured, registered, poll_busy, profiles_complete, radio_owner;
    gboolean ia_enabled;   /* experiment B: APN_SET_IA_REQ after the profile list */
    gboolean ia_done;      /* sent and confirmed in this setup epoch */
    const gchar *internet_source; /* where internet_apn came from (static text) */
    guint    start_steps;          /* MM_MTK_CCCI_IMS_STEP_* */
    MMMtkCcciImsProfiles profiles_mode;
    gboolean profiles_held;        /* the list waits for registration (or a bearer) */
    guint epoch, timer;
    gint64 next_poll;
    GHashTable *pdns, *spis;
    /* the radio-off step of a cold setup, when the owner provides one
     * (the shared radio coordinator, mm-mtk-ccci-shared.c) */
    MMMtkCcciImsRadioHook radio_hook;
    gpointer radio_hook_data;
    /* callers of the setup in flight (NULL: none in flight) */
    GPtrArray *setup_waiters;
};

static void pdn_free (ImsPdn *p)
{
    if (p->created && p->link)
        mm_wwan_link_del (p->link, NULL);
    g_free (p->address);
    g_free (p->link);
    g_free (p);
}

static ImsPdn *pdn_get (MMMtkCcciIms *ims, guint tid)
{
    ImsPdn *p;
    if (tid % 100 > 20 || tid > G_MAXINT)
        return NULL;
    p = g_hash_table_lookup (ims->pdns, GUINT_TO_POINTER (tid));
    if (!p && g_hash_table_size (ims->pdns) < MAX_PDNS) {
        p = g_new0 (ImsPdn, 1);
        p->tid = tid;
        p->mtu = 1500;
        g_hash_table_insert (ims->pdns, GUINT_TO_POINTER (tid), p);
    }
    return p;
}

static void pdn_configure (MMMtkCcciIms *ims, ImsPdn *p)
{
    g_autoptr(GError) error = NULL;
    gboolean existed = FALSE;

    if (!ims->enabled || !p->labelled || !p->up)
        return;
    if (!p->link) {
        p->link = g_strdup_printf ("%s-%u", ims->wwan, p->tid % 100);
        if (!mm_wwan_link_add (ims->wwan, p->tid % 100, p->link, &existed, &error)) {
            mm_obj_warn (ims->owner, "IMS link: %s", error->message);
            g_clear_pointer (&p->link, g_free);
            return;
        }
        if (existed) {
            /* Ownership cannot be inferred from a reused name. Do not configure
             * or delete a link that may belong to an internet bearer. */
            mm_obj_warn (ims->owner, "IMS link %s already exists; leaving it untouched", p->link);
            g_clear_pointer (&p->link, g_free);
            return;
        }
        p->created = TRUE;
    }
    if (!mm_wwan_link_configure (p->link, p->mtu, p->address, &error))
        mm_obj_warn (ims->owner, "IMS link %s: %s", p->link, error->message);
    else
        mm_obj_msg (ims->owner, "IMS SIM%u PDN %u on %s, address %s (no default route or DNS)",
                    ims->slot, p->tid, p->link, p->address ? p->address : "pending");
}

/* LTE-loss diagnostics: the profile steps of the slot on the rat-trace
 * timeline (mm-broadband-modem-mtk-ccci.c) */
static void ims_trace (MMMtkCcciIms *ims, const gchar *fmt, ...) G_GNUC_PRINTF (2, 3);

static void ims_trace (MMMtkCcciIms *ims, const gchar *fmt, ...)
{
    g_autofree gchar *stamp = mm_mtk_ccci_trace_stamp ();
    g_autofree gchar *what = NULL;
    va_list ap;

    va_start (ap, fmt);
    what = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    mm_obj_msg (ims->owner, "rat-trace SIM%u %s: %s", ims->slot, stamp, what);
}

static void trace_cid0 (MMMtkCcciIms *ims, const gchar *reply, const gchar *source)
{
    g_autofree gchar *line = mm_mtk_ccci_at_one_line (reply);

    ims_trace (ims, "CID 0 (%s, AT+CGDCONT? read response): %s; internet APN of this SIM: %s%s%s%s%s%s",
               source, reply ? line : "no reply", ims->internet_apn ? "'" : "",
               ims->internet_apn ? ims->internet_apn : "none (no CID 0 internet context, no preset)",
               ims->internet_apn ? "'" : "", ims->internet_apn ? " (from " : "",
               ims->internet_apn ? ims->internet_source : "", ims->internet_apn ? ")" : "");
}

static void trace_profiles_request (MMMtkCcciIms *ims, const gchar *internet, const gchar *source)
{
    ims_trace (ims, "APN profile list request APN_ADD_PROFILE_LIST_REQ 0x0113 (%s): internet %s%s%s, ims '%s', sos '%s'",
               source, internet ? "'" : "", internet ? internet : "none", internet ? "'" : "",
               ims->ims_apn, ims->sos_apn);
}

static void trace_confirm (MMMtkCcciIms *ims, const gchar *what, MMMipcCnf *cnf, const GError *error)
{
    if (!cnf)
        ims_trace (ims, "%s: no confirm: %s", what, error ? error->message : "unknown error");
    else
        ims_trace (ims, "%s: confirm 0x%04x slot 0x%02x txid %u result %u%s", what, cnf->msg_id, cnf->slot,
                   cnf->txid, cnf->result,
                   mm_mipc_tlv_find (cnf->payload->data, cnf->payload->len, MM_MIPC_T_RESULT, NULL) ?
                       "" : " (no result TLV)");
}

void mm_mtk_ccci_ims_set_initial_attach (MMMtkCcciIms *ims, gboolean enabled)
{
    if (ims->ia_enabled != enabled)
        mm_obj_msg (ims->owner, "SIM%u initial-attach experiment %s", ims->slot,
                    enabled ? "B: APN_SET_IA_REQ after the profile list" : "A: profile list only");
    ims->ia_enabled = enabled;
}

/* experiment B: the slot's internet profile as the initial-attach APN.
 * Returns FALSE (and logs why) when nothing was sent. */
static gboolean ia_send (MMMtkCcciIms *ims, const gchar *source, GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GByteArray) tlvs = NULL;

    if (!ims->ia_enabled) {
        ims_trace (ims, "initial attach (%s): no separate APN_SET_IA_REQ (experiment A)", source);
        return FALSE;
    }
    if (ims->ia_done)
        return FALSE;
    tlvs = mm_mipc_build_set_ia (ims->internet_apn, MM_MIPC_APN_INTERNET_PDP, MM_MIPC_APN_INTERNET_PDP,
                                 MM_MIPC_APN_INTERNET_AUTH, NULL, NULL);
    if (!tlvs) {
        ims_trace (ims, "initial attach (%s): APN_SET_IA_REQ not sent: %s", source,
                   ims->internet_apn ? "APN too long" : "no internet APN of this SIM known");
        return FALSE;
    }
    ims_trace (ims, "initial attach (%s): APN_SET_IA_REQ 0x0101: APN '%s' (from %s), PDP type %u, roaming PDP type %u, "
               "auth type %u, user empty, password empty", source, ims->internet_apn, ims->internet_source,
               MM_MIPC_APN_INTERNET_PDP, MM_MIPC_APN_INTERNET_PDP, MM_MIPC_APN_INTERNET_AUTH);
    mm_port_mipc_request (ims->port, MM_MIPC_APN_SET_IA_REQ, ims->slot, tlvs, 15, NULL, callback, user_data);
    return TRUE;
}

/* the IA confirm: only APN_SET_IA_CNF of this slot with result 0 counts
 * (the port matches the transaction id) */
static void ia_finish (MMMtkCcciIms *ims, MMPortMipc *port, GAsyncResult *res)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    trace_confirm (ims, "initial attach APN_SET_IA_REQ", cnf, error);
    ims->ia_done = cnf && cnf->msg_id == MM_MIPC_APN_SET_IA_CNF && cnf->slot == ims->slot && !cnf->result;
    if (!ims->ia_done)
        mm_obj_warn (ims->owner, "SIM%u initial-attach APN not accepted: %s", ims->slot,
                     error ? error->message : "refused");
}

static void set_registration (MMMtkCcciIms *ims, guint registered, guint services)
{
    gboolean state = registered == 1;
    if (ims->registered != state)
        mm_obj_msg (ims->owner, "IMS SIM%u %s, services 0x%x", ims->slot, state ? "registered" : "not registered", services);
    ims->registered = state;
}

void mm_mtk_ccci_ims_urc (MMMtkCcciIms *ims, const gchar *line)
{
    guint tid, kind, reg, services;
    gchar state[16], address[64];
    ImsPdn *p;
    g_autoptr(GInetAddress) parsed = NULL;

    if (!ims || !ims->enabled)
        return;
    if (sscanf (line, "+CIREGU: %u , %x", &reg, &services) == 2) {
        set_registration (ims, reg, services);
        return;
    }
    if (sscanf (line, "+EIF: %u , %15[^,], %u , %63[^,\r\n]", &tid, state, &kind, address) != 4)
        return;
    if (!g_str_equal (g_strstrip (state), "ifup") && !g_str_equal (state, "ipchg"))
        return;
    parsed = g_inet_address_new_from_string (g_strstrip (address));
    if (!parsed || g_inet_address_get_is_any (parsed))
        return;
    p = pdn_get (ims, tid);
    if (!p)
        return;
    g_free (p->address);
    p->address = g_inet_address_to_string (parsed);
    pdn_configure (ims, p);
}

static void spi_command (MMMtkCcciIms *ims, guint slot, guint txid, GByteArray *payload)
{
    guint32 index = 0, lo = MM_MIPC_SPI_DEFAULT_MIN, hi = MM_MIPC_SPI_DEFAULT_MAX, spi = 0;
    guint attempts;
    gboolean ok = FALSE;
    g_autoptr(GByteArray) reply = g_byte_array_new ();
    g_autoptr(GError) error = NULL;

    mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_SPI_INDEX, &index);
    mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_SPI_MIN, &lo);
    mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_SPI_MAX, &hi);
    /* SPI 0 is reserved. Bounded search; never reuse an SPI in this session.
     * Kernel IPsec reservation and the failure value 1 are unverified. */
    lo = MAX (lo, 0x100);
    if (lo <= hi && g_hash_table_size (ims->spis) < MAX_SPIS) {
        guint64 range = (guint64) hi - lo + 1;
        guint64 limit = (G_GUINT64_CONSTANT (1) << 32) / range * range;
        for (attempts = 0; attempts < MAX_SPIS * 2; attempts++) {
            guint32 random = g_random_int ();
            if ((guint64) random >= limit)
                continue;
            spi = lo + random % range;
            if (!g_hash_table_contains (ims->spis, GUINT_TO_POINTER (spi))) {
                ok = TRUE;
                break;
            }
        }
    }
    if (ok)
        g_hash_table_add (ims->spis, GUINT_TO_POINTER (spi));
    mm_mipc_tlv_add_u32 (reply, MM_MIPC_T_SPI_RSP_INDEX, index);
    mm_mipc_tlv_add_u8 (reply, MM_MIPC_T_SPI_RSP_RESULT, ok ? 0 : 1);
    mm_mipc_tlv_add_u32 (reply, MM_MIPC_T_SPI_RSP_SPI, ok ? spi : 0);
    if (!mm_port_mipc_send_response (ims->port, MM_MIPC_INTERNAL_EIPSPI_RSP, txid, slot, reply, &error))
        mm_obj_warn (ims->owner, "IMS SPI reply: %s", error->message);
}

static void indication (MMPortMipc *port, guint id, guint slot, guint txid,
                         GByteArray *payload, MMMtkCcciIms *ims)
{
    guint32 tid, command = 0, type = 0, action = 0, mtu;
    ImsPdn *p;
    const guint8 *record;
    gsize length;

    if (!ims->enabled)
        return;
    if (id == MM_MIPC_INTERNAL_EIPSPI_CMD) {
        /* Byte 8 can be 0x11, not the active SIM bit: echo it unchanged. */
        if ((slot & 3) == ims->slot || ((slot & 3) == 3 && ims->radio_owner))
            spi_command (ims, slot, txid, payload);
        return;
    }
    if (!(slot & ims->slot))
        return;
    if (id == MM_MIPC_SYS_AT_IND) {
        g_autofree gchar *text = mm_mipc_tlv_find_at_string (payload->data, payload->len);
        if (text) {
            g_auto(GStrv) lines = g_strsplit_set (text, "\r\n", -1);
            guint i;
            for (i = 0; lines[i]; i++)
                mm_mtk_ccci_ims_urc (ims, g_strstrip (lines[i]));
        }
        return;
    }
    if (id == MM_MIPC_IMS_PDN_IND) {
        if (!mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_IMSPDN_IFACE, &tid))
            return; /* short bookkeeping reset: no interface is identified */
        if (!mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_IMSPDN_TYPE, &type) ||
            !mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_IMSPDN_ACTION, &action))
            return;
        if (type != MM_MIPC_IMSPDN_TYPE_IMS)
            return; /* emergency type coding is not established */
        if (!action) {
            g_hash_table_remove (ims->pdns, GUINT_TO_POINTER (tid));
            return;
        }
        p = pdn_get (ims, tid);
        if (p) { p->labelled = TRUE; pdn_configure (ims, p); }
        return;
    }
    if (id != MM_MIPC_INTERNAL_EIF_IND ||
        !mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_EIF_TRANS_INTF_ID, &tid) ||
        !mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_EIF_CMD, &command))
        return;
    if (command == MM_MIPC_EIF_DOWN) {
        g_hash_table_remove (ims->pdns, GUINT_TO_POINTER (tid));
        return;
    }
    if (command != MM_MIPC_EIF_UP && command != MM_MIPC_EIF_CHANGE &&
        command != MM_MIPC_EIF_HO_ADDR && command != MM_MIPC_EIF_MTU)
        return;
    p = pdn_get (ims, tid);
    if (!p)
        return;
    if (command != MM_MIPC_EIF_MTU)
        p->up = TRUE;
    if (mm_mipc_tlv_find_uint (payload->data, payload->len, MM_MIPC_T_EIF_MTU, &mtu) && mtu >= 68 && mtu <= 3568)
        p->mtu = mtu;
    record = mm_mipc_tlv_find_record (payload->data, payload->len, MM_MIPC_T_EIF_V4_RECORDS, &length);
    if (record && length >= 4 && (record[0] | record[1] | record[2] | record[3])) {
        g_free (p->address);
        p->address = g_strdup_printf ("%u.%u.%u.%u", record[0], record[1], record[2], record[3]);
    }
    pdn_configure (ims, p);
}

/* All per-stack commands use the explicitly addressed tunnel. CFUN=4 is a
 * physical-radio operation and is issued only by the primary startup owner. */
static void
ims_at_command (MMMtkCcciIms *ims, const gchar *command, guint timeout,
                GAsyncReadyCallback callback, gpointer user_data)
{
    g_autofree gchar *full = NULL;
    if (g_str_equal (command, "+CFUN=4")) {
        if (ims->radio_hook) {
            GTask *task = g_task_new (ims->owner, NULL, callback, user_data);
            g_task_set_source_tag (task, mm_mtk_ccci_ims_set_radio_hook);
            ims->radio_hook (ims, ims->slot, task, ims->radio_hook_data);
            return;
        }
        mm_base_modem_at_command ((MMBaseModem *) ims->owner, command, timeout, FALSE, callback, user_data);
        return;
    }
    full = g_strdup_printf ("AT%s", command);
    mm_port_mipc_at_command (ims->port, ims->slot, full, timeout, callback, user_data);
}

static gchar *
ims_at_finish (GObject *source, GAsyncResult *res, GError **error)
{
    gchar *text;
    if (g_async_result_is_tagged (res, mm_port_mipc_at_command))
        text = mm_port_mipc_at_command_finish (MM_PORT_MIPC (source), res, error);
    else if (g_async_result_is_tagged (res, mm_mtk_ccci_ims_set_radio_hook))
        text = g_task_propagate_pointer (G_TASK (res), error);
    else {
        const gchar *borrowed = mm_base_modem_at_command_finish ((MMBaseModem *) source, res, error);
        text = borrowed ? g_strdup (borrowed) : NULL;
    }
    return text ? g_strstrip (text) : NULL;
}

/* All startup operations run before the normal MM radio-power-up step. */
typedef struct {
    MMMtkCcciIms *ims;
    guint epoch, step;
    gboolean warm, fallback;
} Setup;

static void setup_step (GTask *task);

static void read_apn (MMMtkCcciIms *ims, const gchar *response)
{
    g_autoptr(GError) error = NULL;
    GList *contexts, *l;
    if (!response || ims->internet_apn)
        return;
    contexts = mm_3gpp_parse_cgdcont_read_response (response, &error);
    for (l = contexts; l; l = l->next) {
        MM3gppPdpContext *p = l->data;
        /* On this modem the attach context is CID 0. After data teardown it
         * may be absent while CID 1 (IMS) remains; that is not an internet APN. */
        if (p->cid == 0 && p->apn && p->apn[0] &&
            g_ascii_strcasecmp (p->apn, ims->ims_apn) &&
            g_ascii_strcasecmp (p->apn, ims->sos_apn)) {
            g_free (ims->internet_apn);
            ims->internet_apn = g_strdup (p->apn);
            ims->internet_source = "CID 0";
        }
    }
    mm_3gpp_pdp_context_list_free (contexts);
}

static void setup_at_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    Setup *ctx = g_task_get_task_data (task);
    GError *error = NULL;
    g_autofree gchar *reply = ims_at_finish (source, res, &error);
    guint n, reg, services;

    if (ctx->epoch != ctx->ims->epoch || !ctx->ims->enabled) {
        g_clear_error (&error);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "IMS setup superseded");
        g_object_unref (task);
        return;
    }
    if (!reply && ctx->step == 7 && !ctx->fallback) {
        ctx->fallback = TRUE;
        g_clear_error (&error);
        ims_at_command (ctx->ims, "+EIMSPDN=\"onoff\",1", 10,
                        (GAsyncReadyCallback) setup_at_ready, task);
        return;
    }
    if (!reply && ctx->step != 0 && ctx->step != 4) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (ctx->step == 0 && reply && sscanf (reply, "+CIREG: %u , %u , %x", &n, &reg, &services) == 3)
        ctx->warm = reg == 1;
    if (ctx->step == 4) {
        read_apn (ctx->ims, reply);
        trace_cid0 (ctx->ims, reply, "start-up");
    }
    g_clear_error (&error);
    ctx->step++;
    setup_step (task);
}

static void setup_ia_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    Setup *ctx = g_task_get_task_data (task);

    if (ctx->epoch != ctx->ims->epoch || !ctx->ims->enabled) {
        g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, NULL);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "IMS setup superseded");
        g_object_unref (task);
        return;
    }
    /* not fatal: the experiment must not stop the start-up */
    ia_finish (ctx->ims, port, res);
    ctx->step++;
    setup_step (task);
}

static void setup_mipc_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    Setup *ctx = g_task_get_task_data (task);
    g_autoptr(MMMipcCnf) cnf = NULL;
    GError *error = NULL;
    g_autofree gchar *reply = NULL;

    cnf = mm_port_mipc_request_finish (port, res, &error);
    if (ctx->epoch != ctx->ims->epoch || !ctx->ims->enabled) {
        g_clear_error (&error);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "IMS setup superseded");
        g_object_unref (task);
        return;
    }
    if (cnf && ctx->step == 9)
        reply = mm_mipc_tlv_find_at_string (cnf->payload->data, cnf->payload->len);
    if (ctx->step == 5)
        trace_confirm (ctx->ims, "APN profile list (start-up)", cnf, error);
    if (!cnf || cnf->result || (reply && strstr (reply, "ERROR"))) {
        if (!error)
            error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "IMS setup step %u refused: result %u%s%s", ctx->step,
                                 cnf ? cnf->result : 0, reply ? ": " : "", reply ? reply : "");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* the list confirmed: experiment B sends the initial-attach APN now */
    if (ctx->step == 5 && ia_send (ctx->ims, "start-up", (GAsyncReadyCallback) setup_ia_ready, task))
        return;
    ctx->step++;
    setup_step (task);
}

typedef struct { MMMtkCcciIms *ims; guint epoch; } Poll;
static gboolean tick (gpointer user_data);

/* the late IA of experiment B, between the data bearer's profile list and
 * its activation; not fatal for the bearer */
static void data_ia_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);

    if (ctx->epoch == ctx->ims->epoch && ctx->ims->enabled) {
        ia_finish (ctx->ims, port, res);
        g_task_return_boolean (task, TRUE);
    } else {
        g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, NULL);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Data profile update superseded");
    }
    g_object_unref (task);
}

static void data_profiles_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);
    GError *error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    trace_confirm (ctx->ims, "APN profile list (data bearer)", cnf, error);
    if (ctx->epoch != ctx->ims->epoch || !ctx->ims->enabled) {
        g_clear_error (&error);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Data profile update superseded");
    } else if (!cnf || cnf->result) {
        ctx->ims->profiles_complete = FALSE;
        if (!error)
            error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "Data APN profiles refused: result %u", cnf->result);
        g_task_return_error (task, error);
    } else {
        ctx->ims->profiles_complete = TRUE;
        /* experiment B when the start-up had no internet APN: the first
         * moment it is known is this list */
        if (ia_send (ctx->ims, "late: the internet APN became known with the data bearer",
                     (GAsyncReadyCallback) data_ia_ready, task))
            return;
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

void mm_mtk_ccci_ims_prepare_data (MMMtkCcciIms *ims, const gchar *apn,
                                  GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (ims->owner, NULL, callback, user_data);
    g_autoptr(GByteArray) tlvs = NULL;
    Poll *ctx;

    if (!ims->enabled || !ims->configured) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "IMS profiles are not ready");
        g_object_unref (task);
        return;
    }
    tlvs = apn && apn[0] ? mm_mipc_build_apn_profiles (apn, ims->ims_apn, ims->sos_apn) : NULL;
    if (!tlvs) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS, "Invalid internet APN profiles");
        g_object_unref (task);
        return;
    }
    /* NM's requested APN is authoritative. Keep it for any background refresh,
     * which must not replace it with a remaining IMS-only context. Always wait
     * for this request's confirm before letting the bearer activate data. */
    if (!ims->internet_apn || g_strcmp0 (ims->internet_apn, apn))
        ims->internet_source = "the data bearer (NetworkManager's APN)";
    g_free (ims->internet_apn);
    ims->internet_apn = g_strdup (apn);
    ims->profiles_complete = FALSE;
    ctx = g_new0 (Poll, 1);
    ctx->ims = ims;
    ctx->epoch = ims->epoch;
    g_task_set_task_data (task, ctx, g_free);
    trace_profiles_request (ims, apn, "data bearer");
    mm_port_mipc_request (ims->port, MM_MIPC_APN_ADD_PROFILE_LIST_REQ, ims->slot,
                          tlvs, 15, NULL, (GAsyncReadyCallback) data_profiles_ready, task);
}

gboolean mm_mtk_ccci_ims_prepare_data_finish (GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static const struct {
    guint        step;
    guint        bit;
    const gchar *what;
    const gchar *property;
} start_switches[] = {
    { 2, MM_MTK_CCCI_IMS_STEP_MIPC_CONFIG, "IMS_SET_CONFIG key form", "ID_MM_MTK_CCCI_IMS_MIPC_CONFIG" },
    { 3, MM_MTK_CCCI_IMS_STEP_MIPC_CONFIG, "IMS_SET_CONFIG flags form", "ID_MM_MTK_CCCI_IMS_MIPC_CONFIG" },
    { 6, MM_MTK_CCCI_IMS_STEP_CIREG,       "AT+CIREG=2", "ID_MM_MTK_CCCI_CIREG" },
    { 7, MM_MTK_CCCI_IMS_STEP_EIMSPDN,     "AT+EIMSPDN", "ID_MM_MTK_CCCI_EIMSPDN" },
    { 8, MM_MTK_CCCI_IMS_STEP_EIMSCFG,     "AT+EIMSCFG", "ID_MM_MTK_CCCI_EIMSCFG" },
    { 9, MM_MTK_CCCI_IMS_STEP_EIMSRCS,     "AT+EIMSRCS", "ID_MM_MTK_CCCI_EIMSRCS" },
};

/* a start-up step switched off: traced and skipped (the order of the
 * others is unchanged) */
static gboolean setup_step_off (MMMtkCcciIms *ims, guint step)
{
    guint i;

    if (step == 5 && ims->profiles_mode != MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO) {
        ims->profiles_held = TRUE;
        ims_trace (ims, "start-up step APN profile list: %s (ID_MM_MTK_CCCI_START_PROFILES)",
                   ims->profiles_mode == MM_MTK_CCCI_IMS_PROFILES_OFF ? "off, only with a data bearer"
                                                                       : "held until the first registration");
        return TRUE;
    }
    for (i = 0; i < G_N_ELEMENTS (start_switches); i++)
        if (start_switches[i].step == step && !(ims->start_steps & start_switches[i].bit)) {
            ims_trace (ims, "start-up step %s: off (%s=0)", start_switches[i].what, start_switches[i].property);
            return TRUE;
        }
    return FALSE;
}

static void setup_step (GTask *task)
{
    Setup *ctx = g_task_get_task_data (task);
    MMMtkCcciIms *ims = ctx->ims;
    g_autoptr(GByteArray) tlvs = NULL;
    const gchar *at = NULL;
    guint16 id = 0;
    static const guint8 flags[6] = { 1, 0, 0, 0, 1, 1 };

    if (!ims->enabled || ctx->epoch != ims->epoch) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "IMS startup stopped");
        g_object_unref (task);
        return;
    }
    if (setup_step_off (ims, ctx->step)) {
        ctx->step++;
        setup_step (task);
        return;
    }
    switch (ctx->step) {
    case 0: at = "+CIREG?"; break;
    case 1:
        if (ctx->warm || !ims->radio_owner) { ctx->step++; setup_step (task); return; }
        at = "+CFUN=4"; break;
    case 2: id = MM_MIPC_IMS_SET_CONFIG_REQ; tlvs = mm_mipc_build_ims_config (TRUE, flags); break;
    case 3: id = MM_MIPC_IMS_SET_CONFIG_REQ; tlvs = mm_mipc_build_ims_config (FALSE, flags); break;
    case 4: at = "+CGDCONT?"; break;
    case 5:
        id = MM_MIPC_APN_ADD_PROFILE_LIST_REQ;
        tlvs = mm_mipc_build_apn_profiles (ims->internet_apn, ims->ims_apn, ims->sos_apn);
        ims->profiles_complete = ims->internet_apn != NULL;
        trace_profiles_request (ims, ims->internet_apn, "start-up");
        break;
    case 6: at = "+CIREG=2"; break;
    case 7: at = "+EIMSPDN= \"onoff\",1"; break;
    case 8: at = "+EIMSCFG=1,0,0,0,1,1"; break;
    case 9:
        id = MM_MIPC_SYS_AT_REQ;
        tlvs = g_byte_array_new ();
        mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, "AT+EIMSRCS=0,0");
        break;
    default:
        ims->configured = TRUE;
        ims->next_poll = 0;
        if (!ims->timer)
            ims->timer = g_timeout_add_seconds (5, tick, ims);
        mm_obj_msg (ims->owner, "IMS SIM%u configured before radio-on%s", ims->slot, ctx->warm ? " (already registered, radio kept on)" : "");
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    if (at)
        ims_at_command (ims, at, 15, (GAsyncReadyCallback) setup_at_ready, task);
    else if (tlvs)
        mm_port_mipc_request (ims->port, id, ims->slot, tlvs, 15, NULL, (GAsyncReadyCallback) setup_mipc_ready, task);
    else {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS, "Invalid IMS APN profiles");
        g_object_unref (task);
    }
}

static void poll_done (GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);
    if (ctx->epoch == ctx->ims->epoch)
        ctx->ims->poll_busy = FALSE;
    g_object_unref (task);
}

static void refresh_ia_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);

    if (ctx->epoch == ctx->ims->epoch)
        ia_finish (ctx->ims, port, res);
    else {
        g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, NULL);
    }
    poll_done (task);
}

static void profiles_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);
    if (ctx->epoch == ctx->ims->epoch) {
        trace_confirm (ctx->ims, "APN profile list (refresh)", cnf, error);
        ctx->ims->profiles_complete = cnf && !cnf->result;
        if (!ctx->ims->profiles_complete)
            mm_obj_warn (ctx->ims->owner, "IMS APN refresh failed: %s", error ? error->message : "refused");
        /* experiment B, when the start-up had no internet APN for it */
        else if (ctx->ims->enabled &&
                 ia_send (ctx->ims, "refresh", (GAsyncReadyCallback) refresh_ia_ready, task))
            return;
    }
    poll_done (task);
}

static void apn_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = ims_at_finish (source, res, &error);
    if (ctx->epoch == ctx->ims->epoch && ctx->ims->enabled) {
        g_autoptr(GByteArray) tlvs = NULL;
        read_apn (ctx->ims, reply);
        trace_cid0 (ctx->ims, reply, "refresh");
        if (ctx->ims->internet_apn) {
            tlvs = mm_mipc_build_apn_profiles (ctx->ims->internet_apn, ctx->ims->ims_apn, ctx->ims->sos_apn);
            if (tlvs) {
                trace_profiles_request (ctx->ims, ctx->ims->internet_apn, "refresh");
                mm_port_mipc_request (ctx->ims->port, MM_MIPC_APN_ADD_PROFILE_LIST_REQ, ctx->ims->slot,
                                      tlvs, 15, NULL, (GAsyncReadyCallback) profiles_ready, task);
                return;
            }
        }
    }
    poll_done (task);
}

static void registration_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = ims_at_finish (source, res, &error);
    guint n, reg, services;
    if (ctx->epoch == ctx->ims->epoch && ctx->ims->enabled) {
        if (reply && sscanf (reply, "+CIREG: %u , %u , %x", &n, &reg, &services) == 3)
            set_registration (ctx->ims, reg, services);
        ctx->ims->next_poll = g_get_monotonic_time () + (ctx->ims->registered ? 60 : 5) * G_USEC_PER_SEC;
        if (!ctx->ims->profiles_complete && !ctx->ims->profiles_held) {
            ims_at_command (ctx->ims, "+CGDCONT?", 5, (GAsyncReadyCallback) apn_ready, task);
            return;
        }
    }
    poll_done (task);
}

static gboolean tick (gpointer user_data)
{
    MMMtkCcciIms *ims = user_data;
    GTask *task;
    Poll *ctx;
    if (ims->poll_busy || g_get_monotonic_time () < ims->next_poll)
        return G_SOURCE_CONTINUE;
    ims->poll_busy = TRUE;
    task = g_task_new (ims->owner, NULL, NULL, NULL);
    ctx = g_new0 (Poll, 1);
    ctx->ims = ims;
    ctx->epoch = ims->epoch;
    g_task_set_task_data (task, ctx, g_free);
    ims_at_command (ims, "+CIREG?", 5, (GAsyncReadyCallback) registration_ready, task);
    return G_SOURCE_CONTINUE;
}

static void setup_internal_ready (GObject *source, GAsyncResult *res, MMMtkCcciIms *ims)
{
    g_autoptr(GPtrArray) waiters = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    g_task_propagate_boolean (G_TASK (res), &error);
    waiters = g_steal_pointer (&ims->setup_waiters);
    for (i = 0; waiters && i < waiters->len; i++) {
        GTask *task = g_ptr_array_index (waiters, i);
        if (error)
            g_task_return_error (task, g_error_copy (error));
        else
            g_task_return_boolean (task, TRUE);
        g_object_unref (task);
    }
}

void mm_mtk_ccci_ims_setup (MMMtkCcciIms *ims, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (ims->owner, NULL, callback, user_data);
    GTask *internal;
    Setup *ctx;
    if (ims->enabled && ims->configured) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    /* Two modem objects (one per SIM) and the radio coordinator may ask for
     * the same stack's setup at once: one sequence runs, all get its result. */
    if (ims->setup_waiters) {
        g_ptr_array_add (ims->setup_waiters, task);
        return;
    }
    ims->setup_waiters = g_ptr_array_new ();
    g_ptr_array_add (ims->setup_waiters, task);
    internal = g_task_new (ims->owner, NULL, (GAsyncReadyCallback) setup_internal_ready, ims);
    ctx = g_new0 (Setup, 1);
    ims->enabled = TRUE;
    ctx->ims = ims;
    ctx->epoch = ims->epoch;
    g_task_set_task_data (internal, ctx, g_free);
    setup_step (internal);
}

gboolean mm_mtk_ccci_ims_setup_finish (GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

gboolean mm_mtk_ccci_ims_owns_link (MMMtkCcciIms *ims, guint link_id)
{
    GHashTableIter iter;
    gpointer value;
    if (!ims)
        return FALSE;
    g_hash_table_iter_init (&iter, ims->pdns);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        ImsPdn *p = value;
        if (p->labelled && p->tid % 100 == link_id)
            return TRUE;
    }
    return FALSE;
}

void mm_mtk_ccci_ims_stop (MMMtkCcciIms *ims)
{
    if (!ims)
        return;
    ims->epoch++;
    ims->enabled = ims->configured = ims->registered = ims->poll_busy = FALSE;
    ims->ia_done = FALSE;
    ims->profiles_held = FALSE;
    if (ims->timer) { g_source_remove (ims->timer); ims->timer = 0; }
    g_hash_table_remove_all (ims->pdns);
    g_hash_table_remove_all (ims->spis);
}

static void port_lost (MMPortMipc *port, MMMtkCcciIms *ims)
{
    mm_mtk_ccci_ims_stop (ims);
}

MMMtkCcciIms *mm_mtk_ccci_ims_new (gpointer owner, MMPortMipc *port, guint8 slot, const gchar *wwan)
{
    MMMtkCcciIms *ims = g_new0 (MMMtkCcciIms, 1);
    ims->radio_owner = TRUE;
    ims->start_steps = MM_MTK_CCCI_IMS_STEPS_ALL;
    ims->owner = owner;
    ims->port = g_object_ref (port);
    ims->slot = slot;
    ims->wwan = g_strdup (wwan);
    ims->ims_apn = g_strdup ("ims");
    ims->sos_apn = g_strdup ("sos");
    ims->pdns = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) pdn_free);
    ims->spis = g_hash_table_new (g_direct_hash, g_direct_equal);
    g_signal_connect (port, MM_PORT_MIPC_SIGNAL_INDICATION, G_CALLBACK (indication), ims);
    g_signal_connect (port, "forced-close", G_CALLBACK (port_lost), ims);
    return ims;
}

void mm_mtk_ccci_ims_free (MMMtkCcciIms *ims)
{
    if (!ims)
        return;
    mm_mtk_ccci_ims_stop (ims);
    g_signal_handlers_disconnect_by_data (ims->port, ims);
    g_object_unref (ims->port);
    g_hash_table_unref (ims->pdns);
    g_hash_table_unref (ims->spis);
    g_free (ims->wwan);
    g_free (ims->internet_apn);
    g_free (ims->ims_apn);
    g_free (ims->sos_apn);
    g_free (ims);
}

void mm_mtk_ccci_ims_set_profiles (MMMtkCcciIms *ims, const gchar *internet, const gchar *internet_source,
                                   const gchar *ims_apn, const gchar *sos)
{
    if (internet && internet[0]) {
        g_free (ims->internet_apn);
        ims->internet_apn = g_strdup (internet);
        ims->internet_source = internet_source ? internet_source : "configuration";
    }
    if (ims_apn) { g_free (ims->ims_apn); ims->ims_apn = g_strdup (ims_apn); }
    if (sos) { g_free (ims->sos_apn); ims->sos_apn = g_strdup (sos); }
}

void mm_mtk_ccci_ims_set_radio_owner (MMMtkCcciIms *ims, gboolean owner)
{
    ims->radio_owner = owner;
}

void mm_mtk_ccci_ims_set_radio_hook (MMMtkCcciIms *ims, MMMtkCcciImsRadioHook hook, gpointer data)
{
    ims->radio_hook = hook;
    ims->radio_hook_data = data;
}

guint8 mm_mtk_ccci_ims_get_slot (MMMtkCcciIms *ims)
{
    return ims->slot;
}

gboolean mm_mtk_ccci_ims_is_configured (MMMtkCcciIms *ims)
{
    return ims && ims->enabled && ims->configured;
}

gboolean mm_mtk_ccci_ims_is_registered (MMMtkCcciIms *ims)
{
    return ims && ims->registered;
}

void mm_mtk_ccci_ims_set_start_steps (MMMtkCcciIms *ims, guint steps, MMMtkCcciImsProfiles profiles)
{
    if (ims->start_steps != steps || ims->profiles_mode != profiles)
        mm_obj_msg (ims->owner, "SIM%u start-up steps: IMS MIPC config %s, CIREG=2 %s, EIMSPDN %s, EIMSCFG %s, "
                    "EIMSRCS %s, APN profile list %s", ims->slot,
                    steps & MM_MTK_CCCI_IMS_STEP_MIPC_CONFIG ? "on" : "off",
                    steps & MM_MTK_CCCI_IMS_STEP_CIREG ? "on" : "off",
                    steps & MM_MTK_CCCI_IMS_STEP_EIMSPDN ? "on" : "off",
                    steps & MM_MTK_CCCI_IMS_STEP_EIMSCFG ? "on" : "off",
                    steps & MM_MTK_CCCI_IMS_STEP_EIMSRCS ? "on" : "off",
                    profiles == MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO ? "before radio-on" :
                    profiles == MM_MTK_CCCI_IMS_PROFILES_AFTER_REGISTRATION ? "after the first registration" : "off");
    ims->start_steps = steps;
    ims->profiles_mode = profiles;
}

/* the held list: sent like the start-up one (IMS and SOS records even
 * without an internet APN), then experiment B's IA */
static void held_profiles_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autoptr(MMMipcCnf) cnf = mm_port_mipc_request_finish (port, res, &error);

    if (ctx->epoch == ctx->ims->epoch && ctx->ims->enabled) {
        trace_confirm (ctx->ims, "APN profile list (after the first registration)", cnf, error);
        ctx->ims->profiles_complete = cnf && !cnf->result && ctx->ims->internet_apn;
        if (cnf && !cnf->result &&
            ia_send (ctx->ims, "after the first registration", (GAsyncReadyCallback) refresh_ia_ready, task))
            return;
    }
    poll_done (task);
}

static void held_apn_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    Poll *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *reply = ims_at_finish (source, res, &error);
    g_autoptr(GByteArray) tlvs = NULL;

    if (ctx->epoch == ctx->ims->epoch && ctx->ims->enabled) {
        read_apn (ctx->ims, reply);
        trace_cid0 (ctx->ims, reply, "after the first registration");
        tlvs = mm_mipc_build_apn_profiles (ctx->ims->internet_apn, ctx->ims->ims_apn, ctx->ims->sos_apn);
        if (tlvs) {
            trace_profiles_request (ctx->ims, ctx->ims->internet_apn, "after the first registration");
            mm_port_mipc_request (ctx->ims->port, MM_MIPC_APN_ADD_PROFILE_LIST_REQ, ctx->ims->slot,
                                  tlvs, 15, NULL, (GAsyncReadyCallback) held_profiles_ready, task);
            return;
        }
    }
    poll_done (task);
}

void mm_mtk_ccci_ims_registered (MMMtkCcciIms *ims)
{
    GTask *task;
    Poll *ctx;

    if (!ims || !ims->enabled || !ims->profiles_held ||
        ims->profiles_mode != MM_MTK_CCCI_IMS_PROFILES_AFTER_REGISTRATION)
        return;
    ims->profiles_held = FALSE;
    ims_trace (ims, "first registration: the held APN profile list now");
    if (ims->poll_busy) {
        /* the poll in flight reads CGDCONT? and sends the list itself */
        ims->next_poll = 0;
        return;
    }
    ims->poll_busy = TRUE;
    task = g_task_new (ims->owner, NULL, NULL, NULL);
    ctx = g_new0 (Poll, 1);
    ctx->ims = ims;
    ctx->epoch = ims->epoch;
    g_task_set_task_data (task, ctx, g_free);
    ims_at_command (ims, "+CGDCONT?", 5, (GAsyncReadyCallback) held_apn_ready, task);
}
