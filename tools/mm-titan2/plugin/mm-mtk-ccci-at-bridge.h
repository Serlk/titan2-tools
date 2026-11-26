/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMMtkCcciAtBridge: an AT command port for one SIM stack, carried by the
 * slot-addressed MIPC AT tunnel (SYS_AT_REQ 0x0003 / CNF 0x0004 / IND 0x4001).
 * It listens on an abstract unix socket inside the
 * ModemManager process; the SIM2 modem object's primary AT port is
 * ModemManager's own "virtual" AT port (MMPortSerialAt on a unix socket,
 * mm-base-modem.c) connected to it. Generic ModemManager code (+CPIN, +CSQ,
 * +CREG/+CEREG, +COPS, SMS in PDU mode ...) thus runs unchanged per stack.
 *
 * Line handling (towards ModemManager): commands end with CR (or LF); the
 * tunnel reply is reframed as "\r\n<line>\r\n"... with a final result code
 * (the CNF result decides OK/ERROR when the text has none). V.250 settings
 * that only concern a serial line (ATZ, ATE, ATV1, ATX, AT&C, AT&F, AT) are
 * answered here and never sent to the modem. AT+CMGS=/AT+CMGW= get the "> "
 * prompt here; the PDU up to Ctrl-Z is then sent with the command as one
 * tunnel string "AT+CMGS=<n>\r<pdu>\x1a" (unverified that the tunnel accepts
 * it; Android's RIL sends SMS through the MIPC SMS group, which is not described).
 * Unsolicited text of the slot (SYS_AT_IND) is written to the client when
 * the port always has a handler for it (registration, +CMTI, +CUSD, +CIEV,
 * the plugin's +EIND/+EFUN/+ESUO); anything else would end up in the reply
 * of the next command.
 *
 * Only a peer in the same process (SO_PEERCRED pid) is accepted.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

#ifndef MM_MTK_CCCI_AT_BRIDGE_H
#define MM_MTK_CCCI_AT_BRIDGE_H

#include <glib.h>
#include "mm-port-mipc.h"

typedef struct _MMMtkCcciAtBridge MMMtkCcciAtBridge;

/* listens at once; @slot is the MIPC slot bit (0x01/0x02) */
MMMtkCcciAtBridge *mm_mtk_ccci_at_bridge_new  (MMPortMipc   *port,
                                               guint8        slot,
                                               GError      **error);
void               mm_mtk_ccci_at_bridge_free (MMMtkCcciAtBridge *self);

/* the port name for a "virtual" ModemManager port: "abstract:..." */
const gchar *mm_mtk_ccci_at_bridge_get_name (MMMtkCcciAtBridge *self);
guint8       mm_mtk_ccci_at_bridge_get_slot (MMMtkCcciAtBridge *self);

/* a SYS_AT_IND text of this slot; forwarded when it is a wanted URC.
 * Returns TRUE when (at least one line was) forwarded. */
gboolean mm_mtk_ccci_at_bridge_push_urc (MMMtkCcciAtBridge *self, const gchar *text);

/* exposed for unit tests: whether a URC line is forwarded */
gboolean mm_mtk_ccci_at_bridge_urc_wanted (const gchar *line);
/* exposed for unit tests: the reframed response for a tunnel reply */
gchar   *mm_mtk_ccci_at_bridge_frame_reply (const gchar *text, guint32 result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMMtkCcciAtBridge, mm_mtk_ccci_at_bridge_free)

#endif /* MM_MTK_CCCI_AT_BRIDGE_H */
