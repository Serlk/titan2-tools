/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ModemManager plugin "mtk-ccci": the MediaTek MT6878 integrated modem behind
 * the mtk_ccci WWAN driver (Unihertz Titan 2, mainline Linux).
 *
 * Ports (all in the "wwan" subsystem, created by the driver when the modem is
 * READY; the udev rules file tags them ID_MM_MTK_CCCI):
 *   wwan0at0        AT, the primary port (3GPP control, voice, SMS)
 *   wwan0mipc1      the MIPC data port (ID_MM_MTK_CCCI_MIPC_DATA): not AT,
 *                   grabbed with an unknown type and turned into MMPortMipc
 *   wwan0mipc2      ignored, and in the dual-modem mode the anchor of a
 *                   second device (ID_MM_PHYSDEV_UID mtk-ccci-sim2,
 *                   ID_MM_MTK_CCCI_SIM_ANCHOR=2): never opened; its modem
 *                   object is the SIM2 stack, with a virtual AT port over
 *                   the slot-2 MIPC AT tunnel of wwan0mipc1 (DESIGN.md §11)
 *   wwan0mipc0,3-9  other MIPC channels: ignored
 *   wwan0-N         the data links (net), created by the bearer at connect time
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

#include <string.h>
#include <gmodule.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-plugin-common.h"
#include "mm-broadband-modem-mtk-ccci.h"
#include "mm-log-object.h"

#define MM_TYPE_PLUGIN_MTK_CCCI mm_plugin_mtk_ccci_get_type ()
MM_DEFINE_PLUGIN (MTK_CCCI, mtk_ccci, MtkCcci)

/*****************************************************************************/

static gboolean
is_mipc_port (MMKernelDevice *port)
{
    const gchar *name;

    if (g_strcmp0 (mm_kernel_device_get_subsystem (port), "wwan") != 0)
        return FALSE;
    /* the kernel names them wwan<D>mipc<N>; Linux >= 6.9 also reports type "MIPC" */
    name = mm_kernel_device_get_name (port);
    return name && strstr (name, "mipc") != NULL;
}

static gboolean
is_mipc_data_port (MMKernelDevice *port)
{
    const gchar *name;

    if (!is_mipc_port (port))
        return FALSE;
    if (mm_kernel_device_has_property (port, ID_MM_MTK_CCCI_MIPC_DATA))
        return mm_kernel_device_get_property_as_boolean (port, ID_MM_MTK_CCCI_MIPC_DATA);
    /* without the udev rules: the data channel Android uses is mipc1 */
    name = mm_kernel_device_get_name (port);
    return g_str_has_suffix (name, "mipc1");
}

static gboolean
is_sim2_anchor (MMKernelDevice *port)
{
    return g_strcmp0 (mm_kernel_device_get_subsystem (port), "wwan") == 0 &&
           mm_kernel_device_has_property (port, ID_MM_MTK_CCCI_SIM_ANCHOR) &&
           mm_kernel_device_get_property_as_int (port, ID_MM_MTK_CCCI_SIM_ANCHOR) == 2;
}

/*****************************************************************************/
/* Custom init: keep the AT probing away from the MIPC ports */

static gboolean
mtk_ccci_custom_init_finish (MMPortProbe   *probe,
                             GAsyncResult  *result,
                             GError       **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}

static void
mtk_ccci_custom_init (MMPortProbe         *probe,
                      MMPortSerialAt      *port,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    GTask *task;

    task = g_task_new (probe, cancellable, callback, user_data);

    if (is_mipc_port (mm_port_probe_peek_port (probe))) {
        /* a binary protocol port: never send AT to it; the probe ends with no
         * type and the plugin's grab_port() decides */
        mm_obj_dbg (probe, "MIPC port: AT probing skipped");
        mm_port_probe_set_result_at (probe, FALSE);
        mm_port_probe_set_result_qcdm (probe, FALSE);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*****************************************************************************/

static gboolean
grab_port (MMPlugin     *self,
           MMBaseModem  *modem,
           MMPortProbe  *probe,
           GError      **error)
{
    MMKernelDevice     *port;
    MMPortGroup         port_group;
    MMPortType          port_type;
    MMPortSerialAtFlag  pflags = MM_PORT_SERIAL_AT_FLAG_NONE;

    port       = mm_port_probe_peek_port (probe);
    port_group = mm_port_probe_get_port_group (probe);
    port_type  = mm_port_probe_get_port_type (probe);

    if (is_mipc_port (port)) {
        if (is_mipc_data_port (port)) {
            mm_obj_dbg (self, "%s is the MIPC data port", mm_kernel_device_get_name (port));
            port_group = MM_PORT_GROUP_USED;
            port_type  = MM_PORT_TYPE_UNKNOWN; /* the modem's create_wwan_port() makes it a MMPortMipc */
        } else {
            mm_obj_dbg (self, "%s is an unused MIPC port, ignored", mm_kernel_device_get_name (port));
            port_group = MM_PORT_GROUP_IGNORED;
        }
    } else if (port_type == MM_PORT_TYPE_AT) {
        /* the one AT port is the primary one; the rules file says so too */
        pflags = MM_PORT_SERIAL_AT_FLAG_PRIMARY;
    }

    return mm_base_modem_grab_port (modem, port, port_group, port_type, pflags, error);
}

static MMBaseModem *
create_modem (MMPlugin     *self,
              const gchar  *uid,
              const gchar  *physdev,
              const gchar **drivers,
              guint16       vendor,
              guint16       product,
              guint16       subsystem_vendor,
              guint16       subsystem_device,
              GList        *probes,
              GError      **error)
{
    GList *l;

    if (!mm_port_probe_list_has_at_port (probes)) {
        /* the dual-modem mode's SIM2 device: only the anchor port */
        for (l = probes; l; l = g_list_next (l)) {
            MMKernelDevice *port = mm_port_probe_peek_port (MM_PORT_PROBE (l->data));

            if (!is_sim2_anchor (port))
                continue;
            if (mm_kernel_device_has_property (port, ID_MM_MTK_CCCI_DUAL_MODEM) &&
                !mm_kernel_device_get_property_as_boolean (port, ID_MM_MTK_CCCI_DUAL_MODEM)) {
                g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                             "single-modem mode (" ID_MM_MTK_CCCI_DUAL_MODEM "=0): SIM2 is served by the SIM1 modem object");
                return NULL;
            }
            mm_obj_dbg (self, "MediaTek CCCI integrated modem, SIM2 stack (anchor %s)...",
                        mm_kernel_device_get_name (port));
            return MM_BASE_MODEM (mm_broadband_modem_mtk_ccci_new_sim2 (uid, physdev, drivers,
                                                                        mm_plugin_get_name (self),
                                                                        port, error));
        }
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Unsupported device: the AT port is required");
        return NULL;
    }

    mm_obj_dbg (self, "MediaTek CCCI integrated modem found...");
    return MM_BASE_MODEM (mm_broadband_modem_mtk_ccci_new (uid,
                                                           physdev,
                                                           drivers,
                                                           mm_plugin_get_name (self),
                                                           vendor,
                                                           product));
}

/*****************************************************************************/

MM_PLUGIN_NAMED_CREATOR_SCOPE MMPlugin *
mm_plugin_create_mtk_ccci (void)
{
    static const gchar *subsystems[] = { "wwan", "net", NULL };
    static const gchar *udev_tags[] = { "ID_MM_MTK_CCCI", NULL };
    static const MMAsyncMethod custom_init = {
        .async  = G_CALLBACK (mtk_ccci_custom_init),
        .finish = G_CALLBACK (mtk_ccci_custom_init_finish),
    };

    return MM_PLUGIN (
        g_object_new (MM_TYPE_PLUGIN_MTK_CCCI,
                      MM_PLUGIN_NAME,               MM_MODULE_NAME,
                      MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                      MM_PLUGIN_ALLOWED_UDEV_TAGS,  udev_tags,
                      MM_PLUGIN_ALLOWED_AT,         TRUE,
                      MM_PLUGIN_CUSTOM_INIT,        &custom_init,
                      NULL));
}

static void
mm_plugin_mtk_ccci_init (MMPluginMtkCcci *self)
{
}

static void
mm_plugin_mtk_ccci_class_init (MMPluginMtkCcciClass *klass)
{
    MMPluginClass *plugin_class = MM_PLUGIN_CLASS (klass);

    plugin_class->create_modem = create_modem;
    plugin_class->grab_port    = grab_port;
}
