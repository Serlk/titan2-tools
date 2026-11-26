/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Response helpers of the mtk-ccci plugin (pure string handling, unit tested).
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

#ifndef MM_MODEM_HELPERS_MTK_CCCI_H
#define MM_MODEM_HELPERS_MTK_CCCI_H

#include <glib.h>
#include "mm-modem-helpers.h"

G_BEGIN_DECLS

/* +CLCC <mode> values of IMS (VoLTE) calls on this modem: 20 voice, 21 video, 22/23 conference host, 24/25 participant */
#define MM_MTK_CCCI_CLCC_MODE_IMS_FIRST 20
#define MM_MTK_CCCI_CLCC_MODE_IMS_LAST  25

/* Rewrites the <mode> field of every "+CLCC: <id>,<dir>,<stat>,<mode>,..."
 * line from 20..25 to 0 so that ModemManager's generic +CLCC parser, which
 * accepts voice calls (mode 0) only, lists the IMS calls too. Everything else
 * is returned unchanged. Never returns NULL for a non-NULL input. */
gchar *mm_mtk_ccci_clcc_normalize (const gchar *response);

/* Append one UNIX-stream fragment. 0: wait, 1: final ok, -1: error/EOF/limit. */
gint mm_mtk_ccci_speech_reply_feed (gchar **reply, const guint8 *data, gsize len, GError **error);

/* Index namespace: SIM1 1..7; SIM2 101..107. Wire ids are never modified. */
guint mm_mtk_ccci_call_index (guint8 slot, guint id);
gboolean mm_mtk_ccci_call_parse (guint8 slot, const guint8 *payload, gsize len, MMCallInfo *info);

/* RAT mode: the
 * <rat>,<prefer_rat> pair of AT+ERAT / MIPC NW_SET_RAT_REQ (one-byte TLVs
 * 0x0100, 0x0101, addressed to the slot). <rat> is a RAT bit mask minus one
 * with GSM 1, UMTS 2, LTE 4, C2K 8, NR 16 (22 = GSM|UMTS|LTE|NR, 6 =
 * GSM|UMTS|LTE, 3 = LTE only). The documented pairs for a normal SIM:
 *   NR/LTE/WCDMA/GSM (the automatic mode)    22 / 128
 *   LTE with GSM/WCDMA fallback (LTE first)   6 / 4
 *   LTE only                                  3 / 0
 * <prefer_rat> 4 is LTE; it is not the 3 of the <rat> field for LTE only
 * (the old LTE-first pair 6/3 was rejected by the modem). 128 goes with 22
 * ("NR first" here, like Android's NR/LTE/WCDMA/GSM mode). The <prefer_rat>
 * values 1 (GSM) and 2 (UMTS) come from ModemManager's mtk-legacy plugin and
 * are unverified on this modem, as is 6/0 (no preference). */
#define MM_MTK_CCCI_RAT_STOCK_DEFAULT     22
#define MM_MTK_CCCI_RAT_GSM_UMTS_LTE      6
#define MM_MTK_CCCI_RAT_LTE_ONLY          3
#define MM_MTK_CCCI_RAT_PREFER_NONE       0
#define MM_MTK_CCCI_RAT_PREFER_LTE        4
#define MM_MTK_CCCI_RAT_PREFER_STOCK      128

/* the mode combinations offered for the modes the modem reports (@mask:
 * the union of 2G/3G/4G/5G from the generic probing) */
GArray  *mm_mtk_ccci_rat_build_combinations (MMModemMode mask);
/* the default (start-up, ANY/NONE): always NR first, 22/128, whatever the
 * generic probing reported; LTE first only as the retry after a refusal */
void     mm_mtk_ccci_rat_default (MMModemMode *allowed, MMModemMode *preferred);
/* the one retry after the start-up pair @rejected_rat/@rejected_prefer was
 * rejected: NR first (when @mask allows 5G or is not known yet), else LTE
 * first, never the rejected pair itself. FALSE: no other pair to try. */
gboolean mm_mtk_ccci_rat_fallback (MMModemMode mask, guint8 rejected_rat, guint8 rejected_prefer,
                                   MMModemMode *allowed, MMModemMode *preferred);
/* MM combination -> rat/prefer; ANY/NONE gives the default (any @mask); FALSE when
 * the combination is not one of the offered ones */
gboolean mm_mtk_ccci_rat_from_modes (MMModemMode mask, MMModemMode allowed, MMModemMode preferred,
                                     guint8 *rat, guint8 *prefer);
/* rat/prefer -> MM modes; FALSE for an impossible rat */
gboolean mm_mtk_ccci_rat_to_modes (guint rat, guint prefer, MMModemMode *allowed, MMModemMode *preferred);
/* "+ERAT: <curr_rat>,<gprs_status>,<rat>,<prefer_rat>" (anywhere in @reply) */
gboolean mm_mtk_ccci_parse_erat (const gchar *reply, guint *curr_rat, guint *rat, guint *prefer);

/* LTE loss diagnostics. */

/* "mono <s.ms> boot <s.ms>": CLOCK_MONOTONIC (the kernel log's clock, stops
 * in suspend) and CLOCK_BOOTTIME (runs on in suspend) */
gchar *mm_mtk_ccci_trace_stamp (void);
/* AT text on one log line: CR/LF runs become " | " */
gchar *mm_mtk_ccci_at_one_line (const gchar *text);

/* one +CREG/+CGREG/+CEREG line (3GPP TS 27.007); @solicited: the read
 * response (<n> first), else the unsolicited form. -1: field absent. */
typedef struct {
    const gchar *domain;        /* "CS" (+CREG), "PS" (+CGREG), "EPS" (+CEREG) */
    gint         n;             /* reporting level, read response only */
    gint         stat;
    gint         act;
    gint         cause_type;    /* registration reject: only when the modem gives it */
    gint         reject_cause;
} MMMtkCcciReg;

gboolean mm_mtk_ccci_reg_parse (const gchar *line, gboolean solicited, MMMtkCcciReg *reg);
/* "EPS stat 1 (home) AcT 7, reject cause not reported" */
gchar   *mm_mtk_ccci_reg_describe (const MMMtkCcciReg *reg);
/* every registration line of @text described, "; "-separated; NULL: none */
gchar   *mm_mtk_ccci_reg_describe_text (const gchar *text, gboolean solicited);

/* a +CREG/+CGREG/+CEREG/+C5GREG line as ModemManager's generic code reads
 * it (mm_3gpp_creg_regex_get, mm_3gpp_parse_creg_response): which state
 * it updates, the state, AcT, LAC or TAC, cell id */
typedef struct {
    gboolean                     cgreg, cereg, c5greg;   /* all FALSE: CS (+CREG) */
    MMModem3gppRegistrationState state;
    MMModemAccessTechnology      act;
    gulong                       lac, tac, ci;
} MMMtkCcciRegUpdate;

gboolean mm_mtk_ccci_reg_update_parse (const gchar *line, gboolean solicited, MMMtkCcciRegUpdate *u);
/* "+CEREG: (0-5)" style test response: whether @n is offered after @prefix */
gboolean mm_mtk_ccci_test_range_has (const gchar *reply, const gchar *prefix, guint n);
/* a registration line without the fields after <AcT> (+CGREG: after <rac>),
 * i.e. the <n> = 2 form; NULL: not a registration line */
gchar   *mm_mtk_ccci_reg_strip_cause (const gchar *line, gboolean solicited);
/* the AcT of a +COPS? read response; FALSE when it has none */
gboolean mm_mtk_ccci_cops_act (const gchar *reply, MMModemAccessTechnology *act);

/* the registration block of NW_GET_PS_CNF (0x8106), NW_PS_IND (0x8108) and
 * NW_CS_IND (0x8102): +0 u8 stat, +1 PLMN (7 bytes
 * ASCII), +8 u32 cell id, +12 u32 EARFCN, +16 u32 PCI, +20 u32 TAC, then
 * signal words; EARFCN and PCI offsets inferred, not confirmed */
typedef struct {
    guint   stat;
    gchar   plmn[8];
    guint32 cell, earfcn, pci, tac;
} MMMtkCcciNwBlock;

gboolean mm_mtk_ccci_nw_block_parse    (const guint8 *p, gsize len, MMMtkCcciNwBlock *b);
gchar   *mm_mtk_ccci_nw_block_describe (const MMMtkCcciNwBlock *b);
/* NW_CSCON_IND 0x0100 (4 bytes): hex and the reading of the documented examples */
gchar   *mm_mtk_ccci_cscon_describe    (const guint8 *v, gsize len);

G_END_DECLS

#endif /* MM_MODEM_HELPERS_MTK_CCCI_H */
