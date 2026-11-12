/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 * Public 3GPP AT SIM reads over the slot-addressed SYS_AT tunnel of the modem.
 * No ESUO switch is performed.
 */
#include <config.h>
#include <string.h>
#include <mm-errors-types.h>
#include "mm-sim-mtk-ccci.h"
#include "mm-modem-helpers.h"
#include "mm-bind.h"

typedef struct { MMBaseSim parent; MMPortMipc *port; guint8 slot; } MMSimMtkCcci;
typedef struct { MMBaseSimClass parent; } MMSimMtkCcciClass;
GType mm_sim_mtk_ccci_get_type (void);
G_DEFINE_TYPE (MMSimMtkCcci, mm_sim_mtk_ccci, MM_TYPE_BASE_SIM)

static gchar *parse_iccid (const gchar *text, GError **error)
{
    guint sw1, sw2;
    g_autofree gchar *hex = NULL;
    if (!mm_3gpp_parse_crsm_response (text, &sw1, &sw2, &hex, error))
        return NULL;
    if ((sw1 == 0x90 && sw2 == 0) || sw1 == 0x91 || sw1 == 0x92 || sw1 == 0x9f)
        return mm_3gpp_parse_iccid (hex, error);
    g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "SIM CRSM status %02x/%02x", sw1, sw2);
    return NULL;
}

static gchar *parse_imsi (const gchar *text, GError **error)
{
    g_auto(GStrv) lines = g_strsplit_set (text, "\r\n", -1);
    guint i;
    for (i = 0; lines[i]; i++) {
        const gchar *line = mm_strip_tag (g_strstrip (lines[i]), "+CIMI:");
        gsize len = strlen (line);
        if (len >= 5 && len <= 15 && strspn (line, "0123456789") == len)
            return g_strdup (line);
    }
    g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Invalid SIM IMSI response");
    return NULL;
}

static gchar *string_finish (MMBaseSim *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void read_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    g_autofree gchar *text = mm_port_mipc_at_command_finish (port, res, &error);
    gchar *value;
    if (text)
        g_strstrip (text);
    value = text ? (g_task_get_task_data (task) ? parse_imsi (text, &error) : parse_iccid (text, &error)) : NULL;
    if (value)
        g_task_return_pointer (task, value, g_free);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

static void read_sim_identifier (MMBaseSim *base, GAsyncReadyCallback cb, gpointer data)
{
    MMSimMtkCcci *self = (MMSimMtkCcci *) base;
    mm_port_mipc_at_command (self->port, self->slot, "AT+CRSM=176,12258,0,0,10", 20,
                              (GAsyncReadyCallback) read_ready, g_task_new (self, NULL, cb, data));
}

static void read_imsi (MMBaseSim *base, GAsyncReadyCallback cb, gpointer data)
{
    MMSimMtkCcci *self = (MMSimMtkCcci *) base;
    GTask *task = g_task_new (self, NULL, cb, data);
    g_task_set_task_data (task, GUINT_TO_POINTER (1), NULL);
    mm_port_mipc_at_command (self->port, self->slot, "AT+CIMI", 10,
                              (GAsyncReadyCallback) read_ready, task);
}

static void dispose (GObject *object)
{
    g_clear_object (&((MMSimMtkCcci *) object)->port);
    G_OBJECT_CLASS (mm_sim_mtk_ccci_parent_class)->dispose (object);
}

static void mm_sim_mtk_ccci_init (MMSimMtkCcci *self) { }
static void mm_sim_mtk_ccci_class_init (MMSimMtkCcciClass *klass)
{
    MMBaseSimClass *base = MM_BASE_SIM_CLASS (klass);
    G_OBJECT_CLASS (klass)->dispose = dispose;
    base->load_sim_identifier = read_sim_identifier;
    base->load_sim_identifier_finish = string_finish;
    base->load_imsi = read_imsi;
    base->load_imsi_finish = string_finish;
    /* Inherited generic hooks use the physical AT port and therefore the
     * primary SIM. Never let an operation on this object target that SIM. */
    base->load_operator_identifier = NULL;
    base->load_operator_identifier_finish = NULL;
    base->load_operator_name = NULL;
    base->load_operator_name_finish = NULL;
    base->load_emergency_numbers = NULL;
    base->load_emergency_numbers_finish = NULL;
    base->load_preferred_networks = NULL;
    base->load_preferred_networks_finish = NULL;
    base->load_gid1 = NULL; base->load_gid1_finish = NULL;
    base->load_gid2 = NULL; base->load_gid2_finish = NULL;
    base->set_preferred_networks = NULL; base->set_preferred_networks_finish = NULL;
    base->send_pin = NULL; base->send_pin_finish = NULL;
    base->send_puk = NULL; base->send_puk_finish = NULL;
    base->change_pin = NULL; base->change_pin_finish = NULL;
    base->enable_pin = NULL; base->enable_pin_finish = NULL;
}

typedef struct { MMPortMipc *port; guint8 slot; MMBaseSim *sim; } NewContext;
static void new_context_free (NewContext *ctx)
{
    g_clear_object (&ctx->port);
    g_clear_object (&ctx->sim);
    g_free (ctx);
}

static void sim_initialized (MMBaseSim *sim, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    if (!mm_base_sim_initialize_finish (sim, res, &error))
        g_task_return_error (task, error);
    else {
        mm_base_sim_export (sim);
        g_task_return_pointer (task, g_object_ref (sim), g_object_unref);
    }
    g_object_unref (task);
}

static void pin_ready (MMPortMipc *port, GAsyncResult *res, GTask *task)
{
    NewContext *ctx = g_task_get_task_data (task);
    MMBaseModem *modem = g_task_get_source_object (task);
    GError *error = NULL;
    g_autofree gchar *text = mm_port_mipc_at_command_finish (port, res, &error);
    if (g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_SIM_NOT_INSERTED)) {
        g_clear_error (&error);
        g_task_return_pointer (task, NULL, NULL);
        g_object_unref (task);
        return;
    }
    if (!text || mm_parse_cpin_response (text, TRUE) == MM_MODEM_LOCK_UNKNOWN) {
        if (!error)
            error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "SIM%u state is unknown", ctx->slot);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    ctx->sim = g_object_new (mm_sim_mtk_ccci_get_type (), MM_BASE_SIM_MODEM, modem,
                             MM_BIND_TO, modem, MM_BASE_SIM_SLOT_NUMBER, ctx->slot,
                             "active", TRUE, NULL);
    ((MMSimMtkCcci *) ctx->sim)->port = g_object_ref (port);
    ((MMSimMtkCcci *) ctx->sim)->slot = ctx->slot;
    mm_base_sim_initialize (ctx->sim, NULL, (GAsyncReadyCallback) sim_initialized, task);
}

void mm_sim_mtk_ccci_new (MMBaseModem *modem, MMPortMipc *port, guint8 slot,
                          GAsyncReadyCallback cb, gpointer data)
{
    GTask *task = g_task_new (modem, NULL, cb, data);
    NewContext *ctx = g_new0 (NewContext, 1);
    ctx->port = g_object_ref (port);
    ctx->slot = slot;
    g_task_set_task_data (task, ctx, (GDestroyNotify) new_context_free);
    mm_port_mipc_at_command (port, slot, "AT+CPIN?", 10, (GAsyncReadyCallback) pin_ready, task);
}

MMBaseSim *mm_sim_mtk_ccci_new_finish (GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}
