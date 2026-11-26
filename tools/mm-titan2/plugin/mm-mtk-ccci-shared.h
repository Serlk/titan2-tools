/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMMtkCcciShared: what the modem objects of one MT6878 modem share.
 *
 * With both SIM stacks exposed as two ModemManager modems (DESIGN.md §11)
 * the modem is still one device: one MIPC session on wwan0mipc1 (Android's
 * RIL drives both slots over one MIPC channel too), one radio power command
 * for both stacks (AT+EFUN takes a SIM mask), one speech path, one responder
 * for the modem's incoming-call approval commands. This object owns those,
 * outlives a reprobe of either modem object and is found again by the MIPC
 * port name. The legacy single-modem mode uses it the same way with one
 * client.
 *
 * Written from tools/modemd, a clean-room behavioural description of the protocol (no code of the phone's Android software was used)
 * and the ModemManager tree.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

#ifndef MM_MTK_CCCI_SHARED_H
#define MM_MTK_CCCI_SHARED_H

#include <glib-object.h>
#include <gio/gio.h>

#include "mm-port-mipc.h"
#include "mm-port-serial-at.h"
#include "mm-mtk-ccci-ims.h"

#define MM_TYPE_MTK_CCCI_SHARED (mm_mtk_ccci_shared_get_type ())
G_DECLARE_FINAL_TYPE (MMMtkCcciShared, mm_mtk_ccci_shared, MM, MTK_CCCI_SHARED, GObject)

/* the context of the MIPC port @mipc_port ("wwan0mipc1"): a new reference;
 * created (with its MMPortMipc, not opened) on first use */
MMMtkCcciShared *mm_mtk_ccci_shared_acquire (const gchar *mipc_port);
/* the existing context, if any (no new reference); for tests and logs */
MMMtkCcciShared *mm_mtk_ccci_shared_peek (const gchar *mipc_port);

MMPortMipc   *mm_mtk_ccci_shared_peek_mipc        (MMMtkCcciShared *self);
const gchar  *mm_mtk_ccci_shared_get_wwan_device  (MMMtkCcciShared *self);
/* the IMS controller of @slot (MIPC slot bit 0x01/0x02); owned here */
MMMtkCcciIms *mm_mtk_ccci_shared_peek_ims         (MMMtkCcciShared *self, guint8 slot);
gboolean      mm_mtk_ccci_shared_ims_owns_link    (MMMtkCcciShared *self, guint link_id);

/* dual-modem mode: a stack's IMS radio-off step must not switch the other
 * stack's radio off; legacy mode keeps the old behaviour (CFUN=4) */
void          mm_mtk_ccci_shared_set_dual         (MMMtkCcciShared *self, gboolean dual);
gboolean      mm_mtk_ccci_shared_get_dual         (MMMtkCcciShared *self);

/*****************************************************************************/
/* Radio coordinator: the radios are on for configured & ~low_power slots.
 *
 * AT+EFUN=<mask> powers the stacks of <mask> (3 both, observed; 1 SIM1,
 * observed as equivalent to CFUN=1; 2 SIM2 alone unverified); no slot
 * wanted -> AT+CFUN=4 (the start-up state; whether it switches the SIM2
 * stack off too is unverified, AT+EFUN=0 is untried). The commands go to
 * the SIM1 modem's AT port (wwan0at0) when it is registered and open, else
 * through SYS_AT slot 1 (unverified for radio power). Requests run one at a
 * time. Before a stack's radio goes on its IMS configuration is done
 * (radio_prepare), as Android's RIL does before radio-on. */

void   mm_mtk_ccci_shared_set_radio_mask (MMMtkCcciShared *self, guint configured);
guint  mm_mtk_ccci_shared_get_radio_mask (MMMtkCcciShared *self);
/* the SIM1 modem's primary AT port (weak); NULL unregisters */
void   mm_mtk_ccci_shared_set_radio_port (MMMtkCcciShared *self, MMPortSerialAt *port);
/* wait this long for the SIM1 AT port before using SYS_AT (default 0) */
void   mm_mtk_ccci_shared_set_radio_port_wait (MMMtkCcciShared *self, guint seconds);
/* what a modem learnt (AT+CFUN? on its own stack) */
void   mm_mtk_ccci_shared_radio_note     (MMMtkCcciShared *self, guint8 slots, gboolean on);
/* bit mask of the slots known to be on / known at all */
guint8 mm_mtk_ccci_shared_radio_get_on   (MMMtkCcciShared *self);
guint8 mm_mtk_ccci_shared_radio_get_known (MMMtkCcciShared *self);
guint8 mm_mtk_ccci_shared_radio_get_low_power (MMMtkCcciShared *self);

/* IMS setup of every slot whose radio is about to come on (wanted and not
 * known on). The requester's own failure is an error; the other slot's is
 * logged (it is set up again by its own modem). Nothing when !ims_enabled. */
void     mm_mtk_ccci_shared_radio_prepare        (MMMtkCcciShared     *self,
                                                  guint8               requester,
                                                  gboolean             ims_enabled,
                                                  GAsyncReadyCallback  callback,
                                                  gpointer             user_data);
gboolean mm_mtk_ccci_shared_radio_prepare_finish (MMMtkCcciShared     *self,
                                                  GAsyncResult        *res,
                                                  GError             **error);

/* The LTE start-up contract, one switch per
 * trial group. Nothing of it runs before mm_mtk_ccci_shared_set_lte_config.
 *  - data_allow (group 1, step 8): DATA_SET_DATA_ALLOW_REQ on the data
 *    SIM's slot after every radio-on of that slot (three sends, the observed
 *    timing), when the data SIM changes, and once per MIPC session when the
 *    radio was already on; independent of any bearer;
 *  - radio_config (group 2, steps 2, 4, 5): NW_SET_RADIO_CAPABILITY (both),
 *    SYS_MULTI_SIM_CONFIG and SYS_SET_DSBP (header slot 1), before radio-on;
 *  - prefer_sbp (group 3, steps 1, 3): CALL_DATA_PREFER_SET (both) and the
 *    ESBP/EIAREG/ECCAUSE AT commands (both), before radio-on;
 *  - reg_cause (step 6): AT+CREG=3, +CGREG=3, +CEREG=3 where AT+CEREG=?
 *    offers 3, else n stays as it is.
 * Read-only checks (AT+CEREG=? per slot) run whenever the contract is
 * configured. Steps before radio-on run once per MIPC session and slot and
 * only while the stack's radio is off; every confirm is traced. */
typedef struct {
    gboolean data_allow;
    guint8   data_slot;      /* slot bit of the data SIM (0x01 SIM1) */
    gboolean radio_config;
    gboolean prefer_sbp;
    gboolean reg_cause;
    /* NW_SET_RADIO_STATE_REQ 0 on
     * each configured slot instead of AT+CFUN=4 (the IMS radio-off step and
     * the low power of both stacks) */
    gboolean radio_off_mipc;
    /* read-only: the network indications registered and traced on
     * both slots, NW_GET_PS_REQ every second from each radio-on until the
     * first registration result */
    gboolean nw_diag;
} MMMtkCcciLteConfig;

void     mm_mtk_ccci_shared_set_lte_config (MMMtkCcciShared *self, const MMMtkCcciLteConfig *config);
/* enabling with the radio on (warm start): the checks, step 6 and the
 * session's data-allow for @slots; fire and forget */
void     mm_mtk_ccci_shared_lte_session    (MMMtkCcciShared *self, guint8 slots);
/* step 6 again on @slots (after the generic code set its own <n> on that
 * channel, i.e. the SIM2 AT port); fire and forget */
void     mm_mtk_ccci_shared_lte_reg_cause  (MMMtkCcciShared *self, guint8 slots, const gchar *why);

/* @slots on (power up: forces the command) or off (low power) */
void     mm_mtk_ccci_shared_radio_set            (MMMtkCcciShared     *self,
                                                  guint8               slots,
                                                  gboolean             on,
                                                  GAsyncReadyCallback  callback,
                                                  gpointer             user_data);
gboolean mm_mtk_ccci_shared_radio_set_finish     (MMMtkCcciShared     *self,
                                                  GAsyncResult        *res,
                                                  GError             **error);

/*****************************************************************************/
/* The single speech path (t2-speechd): +1 switch it on now, -1 switch it
 * off now, 0 nothing to do (the other SIM's call keeps it on). DSDS: one
 * call is active at a time; what the modem does with the other SIM's
 * incoming call during a call is unverified. */
gint   mm_mtk_ccci_shared_speech_update  (MMMtkCcciShared *self, guint8 slot, gboolean in_call);
guint8 mm_mtk_ccci_shared_speech_get     (MMMtkCcciShared *self);

/*****************************************************************************/
/* Incoming-call approval policy: CALL_APPROVE_INCOMING_CMD 0x8901 (answered
 * here) and the +EAIC of the SIM1 AT port (answered by that modem object)
 * ask the same question. DSDS (Android's RIL rejects
 * by itself when another SIM already has a call): with the other slot's call
 * in any non-terminated state the call is rejected (approve 0, cause 21),
 * otherwise approved. Same-SIM call waiting is the modem's own (+CCWA) and
 * does not come through here as a rejection.
 *
 * The modem objects are the voice clients of their slots (the single-object
 * mode: one client for both): @calls_present counts the slot's calls that
 * are not terminated, @rejected shows a rejected call of the slot (a Call
 * object that is terminated from the start). Clients are weak references. */
typedef guint (*MMMtkCcciSharedCallsPresentFn) (GObject *client, guint8 slot);
typedef void  (*MMMtkCcciSharedCallRejectedFn) (GObject *client, guint8 slot, guint call_id, const gchar *number);

void     mm_mtk_ccci_shared_set_voice_client   (MMMtkCcciShared               *self,
                                                guint8                         slots,
                                                GObject                       *client,
                                                MMMtkCcciSharedCallsPresentFn  calls_present,
                                                MMMtkCcciSharedCallRejectedFn  rejected);
void     mm_mtk_ccci_shared_unset_voice_client (MMMtkCcciShared *self, GObject *client);
/* udev ID_MM_MTK_CCCI_DSDS_BUSY: TRUE (default) the DSDS rejection, FALSE
 * approve every call (the behaviour before) */
void     mm_mtk_ccci_shared_set_dsds_busy      (MMMtkCcciShared *self, gboolean enabled);
gboolean mm_mtk_ccci_shared_get_dsds_busy      (MMMtkCcciShared *self);
/* the decision for an incoming call on @slot (1/2): TRUE approve. The same
 * request (slot, call id, seq) seen again within 30 s, e.g. on the other
 * channel, gets the same answer and is shown only once. */
gboolean mm_mtk_ccci_shared_incoming_decide    (MMMtkCcciShared *self,
                                                guint8           slot,
                                                guint            call_id,
                                                guint            seq,
                                                const gchar     *number);

#endif /* MM_MTK_CCCI_SHARED_H */
