/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef MM_MTK_CCCI_SMS_READY_H
#define MM_MTK_CCCI_SMS_READY_H

#include "mm-base-modem.h"

/* CPIN READY does not imply that the SMS service has initialized. Probe before
 * generic MM caches SMS storage support or falls back from PDU to text mode. */
void mm_mtk_ccci_sms_wait_ready (MMBaseModem *modem, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data);
gboolean mm_mtk_ccci_sms_wait_ready_finish (GAsyncResult *res, GError **error);

#endif
