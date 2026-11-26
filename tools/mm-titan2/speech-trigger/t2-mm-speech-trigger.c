/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * t2-mm-speech-trigger: switches the Titan 2 voice audio path (t2-speechd)
 * on and off from ModemManager's Call objects on D-Bus. ModemManager-
 * independent alternative to the in-plugin trigger of the mtk-ccci plugin
 * (mm-broadband-modem-mtk-ccci.c, in-call audio channel hooks); use one of
 * the two, not both.
 *
 * Rule (Android's timing): speech on when the first call
 * is dialing, ringing out, held or active (not for a ringing incoming call),
 * speech off when no call is in one of these states any more. t2-speechd's
 * line protocol: tools/speechd/README.md.
 *
 *   t2-mm-speech-trigger [-s /run/t2-speechd.sock] [-r 48000] [-v]
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <libmm-glib.h>

static const gchar *speechd_socket = "/run/t2-speechd.sock";
static guint        call_rate = 48000;
static gboolean     verbose;

static MMManager   *manager;
static GHashTable  *calls;      /* call D-Bus path -> MMCall */
static GList       *voices;     /* MMModemVoice of every modem */
static gboolean     speech_on;

static void
logv (const gchar *fmt, ...)
{
    va_list ap;

    if (!verbose)
        return;
    va_start (ap, fmt);
    vfprintf (stderr, fmt, ap);
    va_end (ap);
    fputc ('\n', stderr);
}

/*****************************************************************************/
/* t2-speechd client: one command, the reply's last line is "ok" or "error: ..." */

static gboolean
speechd_command (const gchar *command)
{
    gint               fd;
    struct sockaddr_un addr;
    gchar              buf[1024];
    gssize             n, total = 0;
    g_autofree gchar  *line = NULL;
    g_auto(GStrv)      lines = NULL;
    guint              nl;

    memset (&addr, 0, sizeof (addr));
    addr.sun_family = AF_UNIX;
    if (strlen (speechd_socket) >= sizeof (addr.sun_path)) {
        g_printerr ("socket path too long\n");
        return FALSE;
    }
    strcpy (addr.sun_path, speechd_socket);

    fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
        g_printerr ("t2-speechd %s: %s\n", speechd_socket, g_strerror (errno));
        if (fd >= 0)
            close (fd);
        return FALSE;
    }
    line = g_strdup_printf ("%s\n", command);
    if (write (fd, line, strlen (line)) != (gssize) strlen (line)) {
        g_printerr ("t2-speechd write: %s\n", g_strerror (errno));
        close (fd);
        return FALSE;
    }
    while (total < (gssize) sizeof (buf) - 1) {
        n = read (fd, buf + total, sizeof (buf) - 1 - total);
        if (n <= 0)
            break;
        total += n;
        buf[total] = 0;
        if (strstr (buf, "\nok\n") || g_str_has_prefix (buf, "ok\n") || strstr (buf, "\nerror:") || g_str_has_prefix (buf, "error:"))
            break;
    }
    close (fd);
    buf[total > 0 ? total : 0] = 0;
    g_strstrip (buf);
    lines = g_strsplit (buf, "\n", -1);
    nl = g_strv_length (lines);
    logv ("t2-speechd %s -> %s", command, buf);
    if (nl && g_str_equal (lines[nl - 1], "ok"))
        return TRUE;
    g_printerr ("t2-speechd %s failed: %s\n", command, buf[0] ? buf : "no reply");
    return FALSE;
}

/*****************************************************************************/
/* the rule */

static gboolean
state_is_in_call (MMCallState state)
{
    switch (state) {
    case MM_CALL_STATE_DIALING:
    case MM_CALL_STATE_RINGING_OUT:
    case MM_CALL_STATE_HELD:
    case MM_CALL_STATE_ACTIVE:
        return TRUE;
    case MM_CALL_STATE_UNKNOWN:
    case MM_CALL_STATE_RINGING_IN:
    case MM_CALL_STATE_WAITING:
    case MM_CALL_STATE_TERMINATED:
    default:
        return FALSE;
    }
}

static void
evaluate (void)
{
    GHashTableIter iter;
    gpointer       value;
    guint          n = 0;

    g_hash_table_iter_init (&iter, calls);
    while (g_hash_table_iter_next (&iter, NULL, &value))
        if (state_is_in_call (mm_call_get_state (MM_CALL (value))))
            n++;

    if (n > 0 && !speech_on) {
        g_autofree gchar *cmd = g_strdup_printf ("call-start --rate %u", call_rate);

        g_print ("call in progress: speech on\n");
        speech_on = TRUE;
        speechd_command (cmd);
    } else if (n == 0 && speech_on) {
        g_print ("no call left: speech off\n");
        speech_on = FALSE;
        speechd_command ("call-stop");
    }
}

/*****************************************************************************/
/* calls */

static void
call_state_changed (MMCall      *call,
                    MMCallState  old,
                    MMCallState  new,
                    MMCallStateReason reason,
                    gpointer     user_data)
{
    logv ("call %s: %s -> %s", mm_call_get_path (call),
          mm_call_state_get_string (old), mm_call_state_get_string (new));
    evaluate ();
}

static void
track_call (MMCall *call)
{
    const gchar *path = mm_call_get_path (call);

    if (g_hash_table_contains (calls, path))
        return;
    logv ("tracking call %s (%s, %s)", path, mm_call_get_number (call),
          mm_call_state_get_string (mm_call_get_state (call)));
    g_signal_connect (call, "state-changed", G_CALLBACK (call_state_changed), NULL);
    g_hash_table_insert (calls, g_strdup (path), g_object_ref (call));
    evaluate ();
}

static void
untrack_call (const gchar *path)
{
    if (g_hash_table_remove (calls, path)) {
        logv ("call %s gone", path);
        evaluate ();
    }
}

static void
list_calls_ready (MMModemVoice *voice,
                  GAsyncResult *res,
                  gpointer      user_data)
{
    g_autoptr(GError) error = NULL;
    GList            *list, *l;

    list = mm_modem_voice_list_calls_finish (voice, res, &error);
    if (error) {
        g_printerr ("could not list the calls of %s: %s\n", mm_modem_voice_get_path (voice), error->message);
        return;
    }
    for (l = list; l; l = g_list_next (l))
        track_call (MM_CALL (l->data));
    g_list_free_full (list, g_object_unref);
}

static void
call_added (MMModemVoice *voice,
            const gchar  *path,
            gpointer      user_data)
{
    /* the Call object is listed again: cheap, and gives us the proxy */
    mm_modem_voice_list_calls (voice, NULL, (GAsyncReadyCallback) list_calls_ready, NULL);
}

static void
call_deleted (MMModemVoice *voice,
              const gchar  *path,
              gpointer      user_data)
{
    untrack_call (path);
}

/*****************************************************************************/
/* modems */

static void
add_modem (MMObject *object)
{
    MMModemVoice *voice;

    voice = mm_object_get_modem_voice (object);
    if (!voice) {
        logv ("modem %s has no voice interface", mm_object_get_path (object));
        return;
    }
    logv ("watching the calls of modem %s", mm_object_get_path (object));
    g_signal_connect (voice, "call-added", G_CALLBACK (call_added), NULL);
    g_signal_connect (voice, "call-deleted", G_CALLBACK (call_deleted), NULL);
    voices = g_list_prepend (voices, voice); /* owns the ref */
    mm_modem_voice_list_calls (voice, NULL, (GAsyncReadyCallback) list_calls_ready, NULL);
}

static void
remove_modem (MMObject *object)
{
    GList          *l;
    GHashTableIter  iter;
    gpointer        key, value;
    const gchar    *modem_path = mm_object_get_path (object);
    GList          *gone = NULL;

    for (l = voices; l; l = g_list_next (l))
        if (g_str_equal (mm_modem_voice_get_path (MM_MODEM_VOICE (l->data)), modem_path)) {
            g_object_unref (l->data);
            voices = g_list_delete_link (voices, l);
            break;
        }
    /* its calls are gone too (call paths start with the modem path) */
    g_hash_table_iter_init (&iter, calls);
    while (g_hash_table_iter_next (&iter, &key, &value))
        if (g_str_has_prefix ((const gchar *) key, "/org/freedesktop/ModemManager1/Call/") ||
            g_str_has_prefix ((const gchar *) key, modem_path))
            gone = g_list_prepend (gone, g_strdup (key));
    for (l = gone; l; l = g_list_next (l))
        untrack_call (l->data);
    g_list_free_full (gone, g_free);
    logv ("modem %s gone", modem_path);
    evaluate ();
}

static void
object_added (GDBusObjectManager *mgr, GDBusObject *object, gpointer user_data)
{
    add_modem (MM_OBJECT (object));
}

static void
object_removed (GDBusObjectManager *mgr, GDBusObject *object, gpointer user_data)
{
    remove_modem (MM_OBJECT (object));
}

static void
name_owner_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    g_autofree gchar *owner = NULL;

    owner = g_dbus_object_manager_client_get_name_owner (G_DBUS_OBJECT_MANAGER_CLIENT (manager));
    if (!owner) {
        /* ModemManager went away: no calls can exist; make sure the path is off */
        g_print ("ModemManager is gone\n");
        g_hash_table_remove_all (calls);
        g_list_free_full (voices, g_object_unref);
        voices = NULL;
        evaluate ();
    } else {
        GList *objects, *l;

        g_print ("ModemManager is running (%s)\n", owner);
        objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (manager));
        for (l = objects; l; l = g_list_next (l))
            add_modem (MM_OBJECT (l->data));
        g_list_free_full (objects, g_object_unref);
    }
}

/*****************************************************************************/

static gboolean
on_signal (gpointer loop)
{
    if (speech_on) {
        speech_on = FALSE;
        speechd_command ("call-stop");
    }
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
    g_autoptr(GError)          error = NULL;
    g_autoptr(GDBusConnection) bus = NULL;
    g_autoptr(GMainLoop)       loop = NULL;
    gint                       opt;

    while ((opt = getopt (argc, argv, "s:r:vh")) != -1) {
        switch (opt) {
        case 's': speechd_socket = optarg; break;
        case 'r': call_rate = (guint) strtoul (optarg, NULL, 10); break;
        case 'v': verbose = TRUE; break;
        default:
            fprintf (stderr, "usage: %s [-s SOCKET] [-r RATE] [-v]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        g_printerr ("system bus: %s\n", error->message);
        return 1;
    }
    manager = mm_manager_new_sync (bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, NULL, &error);
    if (!manager) {
        g_printerr ("ModemManager client: %s\n", error->message);
        return 1;
    }
    calls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

    g_signal_connect (manager, "notify::name-owner", G_CALLBACK (name_owner_changed), NULL);
    g_signal_connect (manager, "object-added", G_CALLBACK (object_added), NULL);
    g_signal_connect (manager, "object-removed", G_CALLBACK (object_removed), NULL);
    name_owner_changed (G_OBJECT (manager), NULL, NULL);

    loop = g_main_loop_new (NULL, FALSE);
    g_unix_signal_add (SIGTERM, on_signal, loop);
    g_unix_signal_add (SIGINT, on_signal, loop);
    g_main_loop_run (loop);

    g_hash_table_unref (calls);
    g_list_free_full (voices, g_object_unref);
    g_object_unref (manager);
    return 0;
}
