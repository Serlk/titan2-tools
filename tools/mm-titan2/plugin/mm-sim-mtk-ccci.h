/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef MM_SIM_MTK_CCCI_H
#define MM_SIM_MTK_CCCI_H

#include "mm-base-sim.h"
#include "mm-port-mipc.h"

/* Read the non-primary SIM without changing the physical AT port's stack.
 * A successful NULL result means CPIN explicitly reported SIM-not-inserted.
 * PIN operations remain unsupported here: MMBaseSim checks the primary
 * modem lock before invoking per-SIM hooks (see QUESTIONS.md). */
void mm_sim_mtk_ccci_new (MMBaseModem *modem, MMPortMipc *port, guint8 slot,
                          GAsyncReadyCallback callback, gpointer user_data);
MMBaseSim *mm_sim_mtk_ccci_new_finish (GAsyncResult *res, GError **error);

#endif
