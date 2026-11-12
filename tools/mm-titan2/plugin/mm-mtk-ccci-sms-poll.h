/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef MM_MTK_CCCI_SMS_POLL_H
#define MM_MTK_CCCI_SMS_POLL_H
#include "mm-base-modem.h"
void mm_mtk_ccci_sms_poll_start (MMBaseModem *modem);
void mm_mtk_ccci_sms_poll_stop (MMBaseModem *modem);
/* scan SM/ME now (quietly); a request during a scan runs one more after it */
void mm_mtk_ccci_sms_poll_now (MMBaseModem *modem);
/* the same, logged with the reason (a +CMTI, an SMS indication, ...) */
void mm_mtk_ccci_sms_poll_notify (MMBaseModem *modem, const gchar *reason);
/* call on modem activity: scans once if the system slept since the last call */
void mm_mtk_ccci_sms_poll_check_resume (MMBaseModem *modem);
/* keep the system out of suspend for ms (timed /sys/power/wake_lock) */
void mm_mtk_ccci_sms_wake_hold (MMBaseModem *modem, guint ms);
#endif
