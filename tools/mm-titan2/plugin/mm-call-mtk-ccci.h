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
#endif
