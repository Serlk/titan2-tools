/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMMtkCcciAtBridge: a per-SIM AT port over the MIPC SYS_AT tunnel (see the
 * header). Wire format of the tunnel
 * (SYS_AT_REQ 0x0003, header slot = SIM slot bit, TLV 0x8100 = the AT
 * command; CNF 0x0004 with result 0x0000 and the response text in 0x8100;
 * SYS_AT_IND 0x4001 = URC text). Everything else here is the 3GPP/V.250 line
 * discipline ModemManager's serial AT port expects. No proprietary code.
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

#include <config.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-log-object.h"
#include "mm-mtk-ccci-at-bridge.h"
#include "mm-modem-helpers-mtk-ccci.h"

#define BRIDGE_MAX_INPUT 8192   /* a PDU of 176 octets is 352 hex characters */
#define MAX_COMMAND      2048   /* mm_port_mipc_at_command's limit too */
#define DEFAULT_TIMEOUT  30

typedef enum {
    ITEM_TUNNEL,
    ITEM_LOCAL_OK,
    ITEM_LOCAL_ERROR,
    ITEM_PROMPT,
} ItemType;

typedef struct {
    ItemType type;
    gchar   *text;
} Item;

struct _MMMtkCcciAtBridge {
    gint        refs;
    gboolean    closed;
    MMPortMipc *port;
    guint8      slot;
    gchar      *name;
    GSocket    *listener;
    GSource    *accept_source;
    GSocket    *client;
    GSource    *client_source;
    GSource    *write_source;
    GByteArray *in;
    GByteArray *out;
    gboolean    pdu_mode;
    gchar      *pdu_command;
    GQueue      items;
    gboolean    busy;
    guint       epoch;
};

static guint bridge_counter;

static void run_queue (MMMtkCcciAtBridge *self);
static void flush_output (MMMtkCcciAtBridge *self);

static void
item_free (Item *item)
{
    g_free (item->text);
    g_free (item);
}

static void
items_clear (GQueue *items)
{
    Item *item;

    while ((item = g_queue_pop_head (items)))
        item_free (item);
}

static MMMtkCcciAtBridge *
bridge_ref (MMMtkCcciAtBridge *self)
{
    self->refs++;
    return self;
}

static void
bridge_unref (MMMtkCcciAtBridge *self)
{
    if (--self->refs)
        return;
    g_byte_array_unref (self->in);
    g_byte_array_unref (self->out);
    items_clear (&self->items);
    g_free (self->pdu_command);
    g_free (self->name);
    g_clear_object (&self->port);
    g_free (self);
}

/*****************************************************************************/
/* Pure helpers (unit tested) */

/* Only what has a handler on the port from setup_ports on (MMBroadbandModem
 * installs +CREG/+CGREG/+CEREG/+C5GREG, +CIEV, +CMTI, +CUSD handlers, the
 * plugin its vendor ones), so that unsolicited text is never taken into
 * the reply of the next command. Calls, IMS and the other SMS notices
 * (+CDSI, SMS indications) are handled from MIPC directly. */
static const gchar *wanted_urcs[] = {
    "+CREG:", "+CGREG:", "+CEREG:", "+C5GREG:",
    "+CMTI:", "+CUSD:", "+CIEV:",
    "+EIND:", "+EFUN:", "+ESUO:",
};

gboolean
mm_mtk_ccci_at_bridge_urc_wanted (const gchar *line)
{
    guint i;

    if (!line)
        return FALSE;
    while (g_ascii_isspace (*line))
        line++;
    for (i = 0; i < G_N_ELEMENTS (wanted_urcs); i++)
        if (g_str_has_prefix (line, wanted_urcs[i]))
            return TRUE;
    return FALSE;
}

static gboolean
is_final_result (const gchar *line)
{
    return g_str_equal (line, "OK") || g_str_equal (line, "ERROR") ||
           g_str_has_prefix (line, "+CME ERROR:") || g_str_has_prefix (line, "+CMS ERROR:") ||
           g_str_equal (line, "NO CARRIER") || g_str_equal (line, "BUSY") ||
           g_str_equal (line, "NO ANSWER") || g_str_equal (line, "NO DIALTONE") ||
           g_str_has_prefix (line, "CONNECT");
}

static GPtrArray *
split_lines (const gchar *text)
{
    GPtrArray     *lines = g_ptr_array_new_with_free_func (g_free);
    g_auto(GStrv)  parts = NULL;
    guint          i;

    if (!text)
        return lines;
    parts = g_strsplit_set (text, "\r\n", -1);
    for (i = 0; parts[i]; i++) {
        gchar *line = g_strstrip (parts[i]);
        if (line[0])
            g_ptr_array_add (lines, g_strdup (line));
    }
    return lines;
}

gchar *
mm_mtk_ccci_at_bridge_frame_reply (const gchar *text, guint32 result)
{
    g_autoptr(GPtrArray) lines = split_lines (text);
    GString             *out = g_string_new (NULL);
    const gchar         *final = NULL;
    guint                n = lines->len, i;

    if (n && is_final_result (g_ptr_array_index (lines, n - 1)))
        final = g_ptr_array_index (lines, --n);
    for (i = 0; i < n; i++)
        g_string_append_printf (out, "\r\n%s\r\n", (const gchar *) g_ptr_array_index (lines, i));
    /* no final result code in the text: the CNF result decides */
    g_string_append_printf (out, "\r\n%s\r\n", final ? final : (result == 0 ? "OK" : "ERROR"));
    return g_string_free (out, FALSE);
}

/* V.250 settings of the serial line itself: answered here. ATZ in
 * particular would reset the tunnel channel's settings (+EURCRPT, +CNMI,
 * +CREG ... of this stack) behind ModemManager's back. */
static gboolean
is_local_command (const gchar *cmd)
{
    const gchar *p;

    if (g_ascii_strcasecmp (cmd, "AT") == 0)
        return TRUE;
    p = cmd + 2;
    if (!*p)
        return TRUE;
    /* every element of a basic command line must be one of Z E V X &C &F Q0 */
    while (*p) {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (strchr ("ZzEeVvXxQq", *p)) {
            p++;
            while (g_ascii_isdigit (*p))
                p++;
            continue;
        }
        if (*p == '&' && p[1] && strchr ("CcFfDd", p[1])) {
            p += 2;
            while (g_ascii_isdigit (*p))
                p++;
            continue;
        }
        return FALSE;
    }
    return TRUE;
}

static gboolean
is_pdu_command (const gchar *cmd)
{
    return g_ascii_strncasecmp (cmd, "AT+CMGS=", 8) == 0 || g_ascii_strncasecmp (cmd, "AT+CMGW=", 8) == 0;
}

static guint
command_timeout (const gchar *cmd)
{
    if (g_ascii_strncasecmp (cmd, "AT+COPS=?", 9) == 0)
        return 180;
    if (g_ascii_strncasecmp (cmd, "AT+CMGS=", 8) == 0 || g_ascii_strncasecmp (cmd, "AT+CMSS=", 8) == 0)
        return 120;
    if (g_ascii_strncasecmp (cmd, "ATD", 3) == 0 || g_ascii_strncasecmp (cmd, "AT+CUSD", 7) == 0 ||
        g_ascii_strncasecmp (cmd, "AT+COPS=", 8) == 0)
        return 60;
    return DEFAULT_TIMEOUT;
}

/*****************************************************************************/
/* Output */

static gboolean
write_ready (GSocket *socket, GIOCondition condition, MMMtkCcciAtBridge *self)
{
    g_clear_pointer (&self->write_source, g_source_unref);
    flush_output (self);
    return G_SOURCE_REMOVE;
}

static void
flush_output (MMMtkCcciAtBridge *self)
{
    while (self->client && self->out->len) {
        g_autoptr(GError) error = NULL;
        gssize n = g_socket_send (self->client, (const gchar *) self->out->data, self->out->len, NULL, &error);

        if (n < 0) {
            if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                if (!self->write_source) {
                    self->write_source = g_socket_create_source (self->client, G_IO_OUT, NULL);
                    g_source_set_callback (self->write_source, (GSourceFunc) write_ready, self, NULL);
                    g_source_attach (self->write_source, g_main_context_get_thread_default ());
                }
                return;
            }
            mm_obj_dbg (self->port, "AT bridge SIM%u: send: %s", self->slot, error->message);
            g_byte_array_set_size (self->out, 0);
            return;
        }
        g_byte_array_remove_range (self->out, 0, (guint) n);
    }
}

static void
write_text (MMMtkCcciAtBridge *self, const gchar *text)
{
    if (!self->client)
        return; /* nobody listens: ModemManager has the port closed */
    g_byte_array_append (self->out, (const guint8 *) text, strlen (text));
    flush_output (self);
}

/*****************************************************************************/
/* Tunnel requests */

typedef struct {
    MMMtkCcciAtBridge *self;
    guint              epoch;
    gchar             *command;
} Request;

/* a read response with level 3 registration lines, in the <n> = 2 form for
 * the generic parsers; NULL: nothing to change */
static gchar *
bridge_strip_reg_causes (const gchar *text)
{
    g_auto(GStrv) lines = NULL;
    GString      *out;
    gboolean      changed = FALSE;
    guint         i;

    if (!text || !strstr (text, "REG:"))
        return NULL;
    lines = g_strsplit (text, "\n", -1);
    out = g_string_new (NULL);
    for (i = 0; lines[i]; i++) {
        g_autofree gchar *line = g_strdup (lines[i]);
        g_autofree gchar *plain = NULL;
        gboolean          cr = g_str_has_suffix (line, "\r");

        if (cr)
            line[strlen (line) - 1] = 0;
        plain = mm_mtk_ccci_reg_strip_cause (line, TRUE);
        if (i)
            g_string_append_c (out, '\n');
        if (plain && strcmp (plain, g_strstrip (line))) {
            g_string_append_printf (out, "%s%s", plain, cr ? "\r" : "");
            changed = TRUE;
        } else
            g_string_append (out, lines[i]);
    }
    if (!changed) {
        g_string_free (out, TRUE);
        return NULL;
    }
    return g_string_free (out, FALSE);
}

static void
tunnel_ready (MMPortMipc *port, GAsyncResult *res, Request *req)
{
    MMMtkCcciAtBridge    *self = req->self;
    g_autoptr(GError)     error = NULL;
    g_autoptr(MMMipcCnf)  cnf = mm_port_mipc_request_finish (port, res, &error);
    g_autofree gchar     *text = NULL;
    g_autofree gchar     *reply = NULL;

    if (self->closed || req->epoch != self->epoch) {
        /* the client went away: the answer belongs to nobody */
        bridge_unref (self);
        g_free (req->command);
        g_free (req);
        return;
    }
    if (!cnf) {
        mm_obj_dbg (self->port, "AT bridge SIM%u: tunnel: %s", self->slot, error->message);
        reply = g_strdup ("\r\nERROR\r\n");
    } else {
        g_autofree gchar *reg = NULL;

        g_autofree gchar *plain = NULL;

        text = mm_mipc_tlv_find_at_string (cnf->payload->data, cnf->payload->len);
        plain = bridge_strip_reg_causes (text);
        reply = mm_mtk_ccci_at_bridge_frame_reply (plain ? plain : text, cnf->result);
        /* LTE-loss diagnostics: a registration read response on this port
         * is logged as a response, so that it is not taken for the URC the
         * port's unsolicited handlers may turn it into (the AT reply text
         * can then be empty, e.g. for mmcli --command) */
        reg = mm_mtk_ccci_reg_describe_text (text, TRUE);
        if (reg) {
            g_autofree gchar *stamp = mm_mtk_ccci_trace_stamp ();
            g_autofree gchar *line = mm_mtk_ccci_at_one_line (text);

            mm_obj_msg (self->port, "rat-trace SIM%u %s: read response to %s on the AT port: %s [%s]",
                        self->slot, stamp, req->command ? req->command : "?", line, reg);
        }
    }
    self->busy = FALSE;
    write_text (self, reply);
    run_queue (self);
    bridge_unref (self);
    g_free (req->command);
    g_free (req);
}

static void
run_queue (MMMtkCcciAtBridge *self)
{
    while (!self->busy && !self->closed && !g_queue_is_empty (&self->items)) {
        Item                 *item = g_queue_pop_head (&self->items);
        g_autoptr(GByteArray) tlvs = NULL;
        Request              *req;

        switch (item->type) {
        case ITEM_LOCAL_OK:
            write_text (self, "\r\nOK\r\n");
            break;
        case ITEM_LOCAL_ERROR:
            write_text (self, "\r\nERROR\r\n");
            break;
        case ITEM_PROMPT:
            /* 27.005 §3.5.1: the TE sends the PDU after "<CR><LF>> " */
            write_text (self, "\r\n> ");
            break;
        case ITEM_TUNNEL:
            if (!mm_port_mipc_get_session_ready (self->port)) {
                mm_obj_dbg (self->port, "AT bridge SIM%u: no MIPC session for %s", self->slot, item->text);
                write_text (self, "\r\nERROR\r\n");
                break;
            }
            tlvs = g_byte_array_new ();
            mm_mipc_tlv_add_str (tlvs, MM_MIPC_T_AT_STRING, item->text);
            req = g_new0 (Request, 1);
            req->self = bridge_ref (self);
            req->epoch = self->epoch;
            req->command = g_strdup (item->text);
            self->busy = TRUE;
            mm_port_mipc_request (self->port, MM_MIPC_SYS_AT_REQ, self->slot, tlvs,
                                  command_timeout (item->text), NULL,
                                  (GAsyncReadyCallback) tunnel_ready, req);
            break;
        default:
            g_assert_not_reached ();
        }
        item_free (item);
    }
}

static void
queue_item (MMMtkCcciAtBridge *self, ItemType type, gchar *text)
{
    Item *item = g_new0 (Item, 1);

    item->type = type;
    item->text = text;
    g_queue_push_tail (&self->items, item);
}

/*****************************************************************************/
/* Input */

static void
process_input (MMMtkCcciAtBridge *self)
{
    for (;;) {
        guint i;

        if (self->pdu_mode) {
            g_autofree gchar *pdu = NULL;
            guint8            end = 0;

            for (i = 0; i < self->in->len; i++)
                if (self->in->data[i] == 0x1a || self->in->data[i] == 0x1b) {
                    end = self->in->data[i];
                    break;
                }
            if (!end)
                break;
            pdu = g_strndup ((const gchar *) self->in->data, i);
            g_byte_array_remove_range (self->in, 0, i + 1);
            self->pdu_mode = FALSE;
            g_strstrip (pdu);
            if (end == 0x1b) {
                /* 27.005: ESC cancels the message */
                g_clear_pointer (&self->pdu_command, g_free);
                queue_item (self, ITEM_LOCAL_OK, NULL);
                continue;
            }
            if (strspn (pdu, "0123456789ABCDEFabcdef") != strlen (pdu) ||
                strlen (pdu) + strlen (self->pdu_command) + 2 > MAX_COMMAND) {
                g_clear_pointer (&self->pdu_command, g_free);
                queue_item (self, ITEM_LOCAL_ERROR, NULL);
                continue;
            }
            queue_item (self, ITEM_TUNNEL, g_strdup_printf ("%s\r%s\x1a", self->pdu_command, pdu));
            g_clear_pointer (&self->pdu_command, g_free);
            continue;
        }

        for (i = 0; i < self->in->len; i++)
            if (self->in->data[i] == '\r' || self->in->data[i] == '\n')
                break;
        if (i == self->in->len)
            break;
        {
            g_autofree gchar *line = g_strndup ((const gchar *) self->in->data, i);

            g_byte_array_remove_range (self->in, 0, i + 1);
            g_strstrip (line);
            if (!line[0])
                continue;
            if (g_ascii_strncasecmp (line, "AT", 2) != 0 || strlen (line) > MAX_COMMAND ||
                strpbrk (line, "\x1a\x1b")) {
                queue_item (self, ITEM_LOCAL_ERROR, NULL);
                continue;
            }
            if (is_local_command (line)) {
                queue_item (self, ITEM_LOCAL_OK, NULL);
                continue;
            }
            if (is_pdu_command (line)) {
                self->pdu_mode = TRUE;
                g_free (self->pdu_command);
                self->pdu_command = g_strdup (line);
                queue_item (self, ITEM_PROMPT, NULL);
                continue;
            }
            queue_item (self, ITEM_TUNNEL, g_steal_pointer (&line));
        }
    }
    if (self->in->len > BRIDGE_MAX_INPUT) {
        mm_obj_warn (self->port, "AT bridge SIM%u: input overflow, dropped", self->slot);
        g_byte_array_set_size (self->in, 0);
        self->pdu_mode = FALSE;
        queue_item (self, ITEM_LOCAL_ERROR, NULL);
    }
    run_queue (self);
}

static void
drop_client (MMMtkCcciAtBridge *self)
{
    if (self->client_source) {
        g_source_destroy (self->client_source);
        g_clear_pointer (&self->client_source, g_source_unref);
    }
    if (self->write_source) {
        g_source_destroy (self->write_source);
        g_clear_pointer (&self->write_source, g_source_unref);
    }
    if (self->client) {
        g_socket_close (self->client, NULL);
        g_clear_object (&self->client);
    }
    g_byte_array_set_size (self->in, 0);
    g_byte_array_set_size (self->out, 0);
    items_clear (&self->items);
    self->pdu_mode = FALSE;
    g_clear_pointer (&self->pdu_command, g_free);
    /* an answer still in flight is for the old client */
    self->busy = FALSE;
    self->epoch++;
}

static gboolean
client_ready (GSocket *socket, GIOCondition condition, MMMtkCcciAtBridge *self)
{
    gchar  buf[1024];
    gssize n;

    if (condition & G_IO_IN) {
        g_autoptr(GError) error = NULL;

        n = g_socket_receive (socket, buf, sizeof (buf), NULL, &error);
        if (n > 0) {
            g_byte_array_append (self->in, (const guint8 *) buf, (guint) n);
            process_input (self);
            return G_SOURCE_CONTINUE;
        }
        if (n < 0 && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            return G_SOURCE_CONTINUE;
    }
    /* EOF, error or hang-up: ModemManager closed the port */
    mm_obj_dbg (self->port, "AT bridge SIM%u: client closed", self->slot);
    g_clear_pointer (&self->client_source, g_source_unref);
    drop_client (self);
    return G_SOURCE_REMOVE;
}

static gboolean
accept_ready (GSocket *listener, GIOCondition condition, MMMtkCcciAtBridge *self)
{
    g_autoptr(GError)       error = NULL;
    g_autoptr(GSocket)      client = NULL;
    g_autoptr(GCredentials) creds = NULL;
    pid_t                   pid;

    client = g_socket_accept (listener, NULL, &error);
    if (!client)
        return G_SOURCE_CONTINUE;
    creds = g_socket_get_credentials (client, &error);
    pid = creds ? g_credentials_get_unix_pid (creds, NULL) : -1;
    if (pid != getpid ()) {
        /* the abstract namespace is visible to the whole network namespace:
         * only ModemManager's own port may talk to the modem here */
        mm_obj_warn (self->port, "AT bridge SIM%u: connection from pid %d refused", self->slot, (gint) pid);
        g_socket_close (client, NULL);
        return G_SOURCE_CONTINUE;
    }
    drop_client (self);
    g_socket_set_blocking (client, FALSE);
    self->client = g_steal_pointer (&client);
    self->client_source = g_socket_create_source (self->client, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback (self->client_source, (GSourceFunc) client_ready, self, NULL);
    g_source_attach (self->client_source, g_main_context_get_thread_default ());
    mm_obj_dbg (self->port, "AT bridge SIM%u: client connected", self->slot);
    return G_SOURCE_CONTINUE;
}

/*****************************************************************************/

gboolean
mm_mtk_ccci_at_bridge_push_urc (MMMtkCcciAtBridge *self, const gchar *text)
{
    g_autoptr(GPtrArray) lines = split_lines (text);
    GString             *out;
    guint                i;

    if (!lines->len || !mm_mtk_ccci_at_bridge_urc_wanted (g_ptr_array_index (lines, 0)))
        return FALSE;
    /* the whole indication: +CMT/+CDS carry the PDU on a second line;
     * registration lines in the <n> = 2 form the generic parsers read (the
     * plugin logs the reject causes of level 3 before this) */
    out = g_string_new (NULL);
    for (i = 0; i < lines->len; i++) {
        g_autofree gchar *plain = mm_mtk_ccci_reg_strip_cause (g_ptr_array_index (lines, i), FALSE);

        g_string_append_printf (out, "\r\n%s\r\n", plain ? plain : (const gchar *) g_ptr_array_index (lines, i));
    }
    write_text (self, out->str);
    g_string_free (out, TRUE);
    return TRUE;
}

const gchar *
mm_mtk_ccci_at_bridge_get_name (MMMtkCcciAtBridge *self)
{
    return self->name;
}

guint8
mm_mtk_ccci_at_bridge_get_slot (MMMtkCcciAtBridge *self)
{
    return self->slot;
}

MMMtkCcciAtBridge *
mm_mtk_ccci_at_bridge_new (MMPortMipc *port, guint8 slot, GError **error)
{
    MMMtkCcciAtBridge        *self;
    g_autoptr(GSocketAddress) address = NULL;

    g_return_val_if_fail (MM_IS_PORT_MIPC (port), NULL);
    g_return_val_if_fail (slot == MM_MIPC_SLOT_SIM1 || slot == MM_MIPC_SLOT_SIM2, NULL);

    self = g_new0 (MMMtkCcciAtBridge, 1);
    self->refs = 1;
    self->port = g_object_ref (port);
    self->slot = slot;
    self->in = g_byte_array_new ();
    self->out = g_byte_array_new ();
    g_queue_init (&self->items);
    /* the same string is the address ModemManager's unix-socket serial port
     * connects to (mm-port-serial.c: "abstract:" prefix = abstract socket) */
    self->name = g_strdup_printf ("abstract:mm-mtk-ccci-sim%u-at-%d-%u", slot == MM_MIPC_SLOT_SIM2 ? 2 : 1,
                                  (gint) getpid (), ++bridge_counter);

    self->listener = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, error);
    if (!self->listener)
        goto fail;
    g_socket_set_blocking (self->listener, FALSE);
    address = g_unix_socket_address_new_with_type (self->name, -1, G_UNIX_SOCKET_ADDRESS_ABSTRACT);
    if (!g_socket_bind (self->listener, address, FALSE, error) || !g_socket_listen (self->listener, error))
        goto fail;
    self->accept_source = g_socket_create_source (self->listener, G_IO_IN, NULL);
    g_source_set_callback (self->accept_source, (GSourceFunc) accept_ready, self, NULL);
    g_source_attach (self->accept_source, g_main_context_get_thread_default ());
    return self;

fail:
    g_prefix_error (error, "AT bridge for SIM%u: ", slot == MM_MIPC_SLOT_SIM2 ? 2 : 1);
    mm_mtk_ccci_at_bridge_free (self);
    return NULL;
}

void
mm_mtk_ccci_at_bridge_free (MMMtkCcciAtBridge *self)
{
    if (!self)
        return;
    drop_client (self);
    if (self->accept_source) {
        g_source_destroy (self->accept_source);
        g_clear_pointer (&self->accept_source, g_source_unref);
    }
    if (self->listener) {
        g_socket_close (self->listener, NULL);
        g_clear_object (&self->listener);
    }
    self->closed = TRUE;
    bridge_unref (self);
}
