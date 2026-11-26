/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef MM_MTK_CCCI_IMS_H
#define MM_MTK_CCCI_IMS_H
#include "mm-base-modem.h"
#include "mm-port-mipc.h"
typedef struct _MMMtkCcciIms MMMtkCcciIms;
/* @owner (a GObject and MMLogObject) is the source object of every result
 * of the controller, also of mm_mtk_ccci_ims_setup/prepare_data: a caller's
 * callback must take its own object from its own task, not from @source. */
MMMtkCcciIms *mm_mtk_ccci_ims_new (gpointer owner, MMPortMipc *port, guint8 slot, const gchar *wwan);
void mm_mtk_ccci_ims_set_radio_owner (MMMtkCcciIms *ims, gboolean owner);
/* The radio-off step (CFUN=4) of a cold setup of a radio owner goes through
 * @hook instead of the owner modem's AT port when set: the hook completes
 * @task (source tag mm_mtk_ccci_ims_set_radio_hook) with a gchar * reply
 * (g_task_return_pointer, g_free) or an error. */
typedef void (*MMMtkCcciImsRadioHook) (MMMtkCcciIms *ims, guint8 slot, GTask *task, gpointer data);
void mm_mtk_ccci_ims_set_radio_hook (MMMtkCcciIms *ims, MMMtkCcciImsRadioHook hook, gpointer data);
guint8 mm_mtk_ccci_ims_get_slot (MMMtkCcciIms *ims);
gboolean mm_mtk_ccci_ims_is_configured (MMMtkCcciIms *ims);
gboolean mm_mtk_ccci_ims_is_registered (MMMtkCcciIms *ims);
/* @internet_source: where @internet comes from, for the trace (static text) */
void mm_mtk_ccci_ims_set_profiles (MMMtkCcciIms *ims, const gchar *internet, const gchar *internet_source,
                                   const gchar *ims_apn, const gchar *sos);
/* the initial-attach experiment: FALSE (A, default) sends the profile list only;
 * TRUE (B) also sends APN_SET_IA_REQ with the slot's internet profile once
 * the list is confirmed */
void mm_mtk_ccci_ims_set_initial_attach (MMMtkCcciIms *ims, gboolean enabled);

/* the start-up steps before radio-on, each switchable; all on and the profile list before radio-on by default */
#define MM_MTK_CCCI_IMS_STEP_MIPC_CONFIG 0x01   /* IMS_SET_CONFIG key and flags forms */
#define MM_MTK_CCCI_IMS_STEP_CIREG       0x02   /* AT+CIREG=2 */
#define MM_MTK_CCCI_IMS_STEP_EIMSPDN     0x04   /* AT+EIMSPDN= "onoff",1 */
#define MM_MTK_CCCI_IMS_STEP_EIMSCFG     0x08   /* AT+EIMSCFG=1,0,0,0,1,1 */
#define MM_MTK_CCCI_IMS_STEP_EIMSRCS     0x10   /* AT+EIMSRCS=0,0 */
#define MM_MTK_CCCI_IMS_STEPS_ALL        0x1f
typedef enum {
    MM_MTK_CCCI_IMS_PROFILES_BEFORE_RADIO,   /* in the start-up sequence (default) */
    MM_MTK_CCCI_IMS_PROFILES_AFTER_REGISTRATION, /* after the slot's first registration */
    MM_MTK_CCCI_IMS_PROFILES_OFF,            /* only with a data bearer */
} MMMtkCcciImsProfiles;
void mm_mtk_ccci_ims_set_start_steps (MMMtkCcciIms *ims, guint steps, MMMtkCcciImsProfiles profiles);
/* the slot's first registration result (stat 1 or 5): a profile list held
 * back (AFTER_REGISTRATION) is sent now */
void mm_mtk_ccci_ims_registered (MMMtkCcciIms *ims);
void mm_mtk_ccci_ims_free (MMMtkCcciIms *ims);
void mm_mtk_ccci_ims_stop (MMMtkCcciIms *ims);
void mm_mtk_ccci_ims_setup (MMMtkCcciIms *ims, GAsyncReadyCallback callback, gpointer user_data);
gboolean mm_mtk_ccci_ims_setup_finish (GAsyncResult *res, GError **error);
void mm_mtk_ccci_ims_prepare_data (MMMtkCcciIms *ims, const gchar *apn,
                                  GAsyncReadyCallback callback, gpointer user_data);
gboolean mm_mtk_ccci_ims_prepare_data_finish (GAsyncResult *res, GError **error);
void mm_mtk_ccci_ims_urc (MMMtkCcciIms *ims, const gchar *line);
gboolean mm_mtk_ccci_ims_owns_link (MMMtkCcciIms *ims, guint link_id);
#endif
