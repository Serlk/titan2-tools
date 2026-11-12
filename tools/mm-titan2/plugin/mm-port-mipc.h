/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMPortMipc: a MIPC control port (/dev/wwan0mipcN) of the MediaTek MT6878
 * integrated modem, driven by the mtk_ccci WWAN driver. Built on MMPort with nonblocking I/O and independent
 * transmit/request queues. This class pairs requests with confirms by
 * transaction id, dispatches indications with their original headers and runs
 * the session start.
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

#ifndef MM_PORT_MIPC_H
#define MM_PORT_MIPC_H

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include "mm-port.h"
#include "mm-mipc-proto.h"

#define MM_TYPE_PORT_MIPC            (mm_port_mipc_get_type ())
#define MM_PORT_MIPC(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_PORT_MIPC, MMPortMipc))
#define MM_PORT_MIPC_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_PORT_MIPC, MMPortMipcClass))
#define MM_IS_PORT_MIPC(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_PORT_MIPC))
#define MM_IS_PORT_MIPC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_PORT_MIPC))
#define MM_PORT_MIPC_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_PORT_MIPC, MMPortMipcClass))

/* signal: an indication (0x4xxx) or a modem->AP command (0x8xxx, odd) arrived.
 * Arguments: guint msg_id, guint slot, guint txid, GByteArray *payload. */
#define MM_PORT_MIPC_SIGNAL_INDICATION "indication"

typedef struct _MMPortMipc MMPortMipc;
typedef struct _MMPortMipcClass MMPortMipcClass;
typedef struct _MMPortMipcPrivate MMPortMipcPrivate;

struct _MMPortMipc {
    MMPort parent;
    MMPortMipcPrivate *priv;
};

struct _MMPortMipcClass {
    MMPortClass parent;

    /* signals */
    void (* indication) (MMPortMipc *self, guint msg_id, guint slot, guint txid, GByteArray *payload);
};

/* a confirm, as returned by mm_port_mipc_request_finish() */
typedef struct {
    guint16     msg_id;
    guint16     txid;
    guint8      slot;
    guint32     result;   /* TLV 0x0000, MM_MIPC_RESULT_MISSING when absent */
    GByteArray *payload;  /* the TLVs */
} MMMipcCnf;

void mm_mipc_cnf_free (MMMipcCnf *cnf);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMMipcCnf, mm_mipc_cnf_free)

GType mm_port_mipc_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMPortMipc, g_object_unref)

MMPortMipc *mm_port_mipc_new (const gchar *name);
gboolean mm_port_mipc_open (MMPortMipc *self, GError **error);
void mm_port_mipc_close (MMPortMipc *self);
gboolean mm_port_mipc_is_open (MMPortMipc *self);
gboolean mm_port_mipc_send_response (MMPortMipc *self, guint16 id, guint16 txid,
                                     guint8 slot, GByteArray *tlvs, GError **error);

/* One request (REQ class): the frame is built here with the next
 * transaction id; the confirm with the same txid completes the operation.
 * Multiple requests may await confirms; their replies may arrive in any order.
 * Cancellation before submission sends nothing. After submission the confirm
 * is drained (or times out) before cancellation is propagated.
 * @tlvs may be NULL for an empty payload. The confirm's result TLV is NOT
 * checked here: callers decide what a non-zero result means. */
void       mm_port_mipc_request        (MMPortMipc          *self,
                                        guint16              msg_id,
                                        guint8               slot,
                                        GByteArray          *tlvs,
                                        guint                timeout_seconds,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
MMMipcCnf *mm_port_mipc_request_finish (MMPortMipc          *self,
                                        GAsyncResult        *res,
                                        GError             **error);

/* Slot-addressed, noninteractive AT command over SYS_AT. Result text is owned
 * by the caller. No global ESUO switch is performed. */
void mm_port_mipc_at_command (MMPortMipc *self, guint8 slot, const gchar *command,
                              guint timeout, GAsyncReadyCallback callback, gpointer user_data);
gchar *mm_port_mipc_at_command_finish (MMPortMipc *self, GAsyncResult *res, GError **error);

/* Session start: INTERNAL_TEST_REQ until answered, INTERNAL_OPEN_REQ,
 * then REGISTER_IND on @slot or REGISTER_CMD on slot 0xff for each id.
 * The port must be open. OPEN and all registrations must succeed. */
void     mm_port_mipc_open_session        (MMPortMipc          *self,
                                           guint8               slot,
                                           const guint16       *indications,
                                           guint                n_indications,
                                           const gchar         *client_name,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
gboolean mm_port_mipc_open_session_finish (MMPortMipc          *self,
                                           GAsyncResult        *res,
                                           GError             **error);

/* TRUE between a successful session start and the port being closed */
gboolean mm_port_mipc_get_session_ready (MMPortMipc *self);

/* the default request timeout the modem announced in the open confirm (ms), 0 if none */
guint    mm_port_mipc_get_modem_timeout_ms (MMPortMipc *self);

#endif /* MM_PORT_MIPC_H */
