/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef MM_CALL_MTK_CCCI_H
#define MM_CALL_MTK_CCCI_H
#include "mm-base-call.h"
#include "mm-base-modem.h"
#include "mm-port-mipc.h"
#include "mm-iface-modem-voice.h"
#include "mm-modem-helpers.h"
MMBaseCall *mm_call_mtk_ccci_new (MMBaseModem *modem, MMPortMipc *port, guint8 slot,
                                 MMCallDirection direction, const gchar *number, guint dtmf_duration);
void mm_call_mtk_ccci_report (MMIfaceModemVoice *modem, MMPortMipc *port, guint8 slot, const MMCallInfo *info);
/* the calls of MIPC slot @slot (1/2) in @modem's list that are not
 * terminated (dialing, ringing-out, active, held, ringing-in, waiting) */
guint mm_call_mtk_ccci_count_present (MMIfaceModemVoice *modem, guint8 slot);
/* an incoming call of @slot that the plugin rejected (DSDS): a new incoming
 * Call object, terminated from the start (REFUSED_OR_BUSY), no index; it
 * never rings and never enters the in-call state. FALSE when @modem has no
 * call list (voice not set up) */
gboolean mm_call_mtk_ccci_report_rejected (MMIfaceModemVoice *modem, MMPortMipc *port, guint8 slot, const gchar *number);
#endif
