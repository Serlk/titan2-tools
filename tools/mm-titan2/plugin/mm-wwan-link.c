/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WWAN link add/del over rtnetlink (RTM_NEWLINK with IFLA_LINKINFO kind "wwan",
 * IFLA_PARENT_DEV_NAME and IFLA_WWAN_LINK_ID, as the kernel's wwan core expects;
 * RTM_DELLINK by name). Written from the kernel UAPI (linux/if_link.h,
 * linux/rtnetlink.h, linux/wwan.h).
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

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_addr.h>

#include <glib.h>
#include <gio/gio.h>

#include "mm-wwan-link.h"

/* older UAPI headers may lack these */
#ifndef IFLA_PARENT_DEV_NAME
# define IFLA_PARENT_DEV_NAME 56
#endif
#ifndef IFLA_WWAN_LINK_ID
/* linux/wwan.h: IFLA_WWAN_UNSPEC = 0, IFLA_WWAN_LINK_ID = 1 */
# define IFLA_WWAN_LINK_ID 1
#endif

#define NL_BUF_SIZE 1024

typedef struct {
    struct nlmsghdr  hdr;
    struct ifinfomsg ifi;
    guint8           attrs[NL_BUF_SIZE];
} NlRequest;

static struct rtattr *
attr_begin (NlRequest *req, guint16 type)
{
    struct rtattr *rta;

    rta = (struct rtattr *) ((guint8 *) req + NLMSG_ALIGN (req->hdr.nlmsg_len));
    rta->rta_type = type;
    rta->rta_len  = RTA_LENGTH (0);
    req->hdr.nlmsg_len = NLMSG_ALIGN (req->hdr.nlmsg_len) + RTA_ALIGN (rta->rta_len);
    return rta;
}

static void
attr_end (NlRequest *req, struct rtattr *rta)
{
    rta->rta_len = (guint8 *) req + NLMSG_ALIGN (req->hdr.nlmsg_len) - (guint8 *) rta;
}

static gboolean
attr_put (NlRequest *req, guint16 type, gconstpointer data, gsize len)
{
    struct rtattr *rta;
    gsize          need = RTA_ALIGN (RTA_LENGTH (len));

    if (NLMSG_ALIGN (req->hdr.nlmsg_len) + need > sizeof (NlRequest))
        return FALSE;
    rta = (struct rtattr *) ((guint8 *) req + NLMSG_ALIGN (req->hdr.nlmsg_len));
    rta->rta_type = type;
    rta->rta_len  = RTA_LENGTH (len);
    if (len)
        memcpy (RTA_DATA (rta), data, len);
    req->hdr.nlmsg_len = NLMSG_ALIGN (req->hdr.nlmsg_len) + need;
    return TRUE;
}

/* one request, wait for its NLMSG_ERROR acknowledgement; *out_errno gets the
 * kernel's error code (0 on success) */
static gboolean
nl_transact (NlRequest  *req,
             gint       *out_errno,
             GError    **error)
{
    gint               fd;
    struct sockaddr_nl sa;
    guint8             buf[4096];
    gssize             n;
    struct nlmsghdr   *hdr;
    gboolean           acked = FALSE;

    *out_errno = 0;

    fd = socket (AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "netlink socket: %s", g_strerror (errno));
        return FALSE;
    }

    memset (&sa, 0, sizeof (sa));
    sa.nl_family = AF_NETLINK;
    if (bind (fd, (struct sockaddr *) &sa, sizeof (sa)) < 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "netlink bind: %s", g_strerror (errno));
        close (fd);
        return FALSE;
    }

    req->hdr.nlmsg_seq = 1;
    req->hdr.nlmsg_pid = 0;
    if (sendto (fd, req, req->hdr.nlmsg_len, 0, (struct sockaddr *) &sa, sizeof (sa)) < 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "netlink send: %s", g_strerror (errno));
        close (fd);
        return FALSE;
    }

    /* the kernel answers a request with NLM_F_ACK synchronously */
    while (!acked) {
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

        setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
        n = recv (fd, buf, sizeof (buf), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                         "netlink receive: %s", g_strerror (errno));
            close (fd);
            return FALSE;
        }
        for (hdr = (struct nlmsghdr *) buf; NLMSG_OK (hdr, (gsize) n); hdr = NLMSG_NEXT (hdr, n)) {
            if (hdr->nlmsg_seq != 1)
                continue;
            if (hdr->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA (hdr);

                *out_errno = -err->error;
                acked = TRUE;
                break;
            }
            if (hdr->nlmsg_type == NLMSG_DONE) {
                acked = TRUE;
                break;
            }
        }
    }
    close (fd);
    return TRUE;
}

gboolean
mm_wwan_link_add (const gchar  *parent_dev,
                  guint         link_id,
                  const gchar  *ifname,
                  gboolean     *out_existed,
                  GError      **error)
{
    NlRequest      req;
    struct rtattr *linkinfo, *infodata;
    guint32        id = link_id;
    gint           err;
    static const gchar kind[] = "wwan";

    g_return_val_if_fail (parent_dev != NULL, FALSE);
    g_return_val_if_fail (ifname != NULL, FALSE);

    if (out_existed)
        *out_existed = FALSE;
    if (strlen (ifname) >= IFNAMSIZ) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "interface name '%s' too long", ifname);
        return FALSE;
    }

    memset (&req, 0, sizeof (req));
    req.hdr.nlmsg_len   = NLMSG_LENGTH (sizeof (struct ifinfomsg));
    req.hdr.nlmsg_type  = RTM_NEWLINK;
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    req.ifi.ifi_family  = AF_UNSPEC;

    if (!attr_put (&req, IFLA_IFNAME, ifname, strlen (ifname) + 1) ||
        !attr_put (&req, IFLA_PARENT_DEV_NAME, parent_dev, strlen (parent_dev) + 1)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "netlink message too long");
        return FALSE;
    }
    linkinfo = attr_begin (&req, IFLA_LINKINFO);
    attr_put (&req, IFLA_INFO_KIND, kind, sizeof (kind));
    infodata = attr_begin (&req, IFLA_INFO_DATA);
    attr_put (&req, IFLA_WWAN_LINK_ID, &id, sizeof (id));
    attr_end (&req, infodata);
    attr_end (&req, linkinfo);

    if (!nl_transact (&req, &err, error))
        return FALSE;
    if (err == EEXIST) {
        if (out_existed)
            *out_existed = TRUE;
        return TRUE;
    }
    if (err) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                     "cannot create wwan link %s (parent %s, link id %u): %s",
                     ifname, parent_dev, link_id, g_strerror (err));
        return FALSE;
    }
    return TRUE;
}

gboolean
mm_wwan_link_del (const gchar  *ifname,
                  GError      **error)
{
    NlRequest req;
    gint      err;

    g_return_val_if_fail (ifname != NULL, FALSE);

    memset (&req, 0, sizeof (req));
    req.hdr.nlmsg_len   = NLMSG_LENGTH (sizeof (struct ifinfomsg));
    req.hdr.nlmsg_type  = RTM_DELLINK;
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.ifi.ifi_family  = AF_UNSPEC;
    if (!attr_put (&req, IFLA_IFNAME, ifname, strlen (ifname) + 1)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "interface name too long");
        return FALSE;
    }

    if (!nl_transact (&req, &err, error))
        return FALSE;
    if (err && err != ENODEV) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                     "cannot delete link %s: %s", ifname, g_strerror (err));
        return FALSE;
    }
    return TRUE;
}


gboolean
mm_wwan_link_configure (const gchar *ifname, guint mtu, const gchar *address, GError **error)
{
    NlRequest req;
    gint err;
    guint index = if_nametoindex (ifname);
    guint32 value = mtu;

    if (!index) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No interface %s", ifname);
        return FALSE;
    }
    memset (&req, 0, sizeof (req));
    req.hdr.nlmsg_len = NLMSG_LENGTH (sizeof (req.ifi));
    req.hdr.nlmsg_type = RTM_NEWLINK;
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.ifi.ifi_index = index;
    req.ifi.ifi_flags = IFF_UP;
    req.ifi.ifi_change = IFF_UP;
    attr_put (&req, IFLA_MTU, &value, sizeof (value));
    if (!nl_transact (&req, &err, error))
        return FALSE;
    if (err) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err), "Cannot configure %s: %s", ifname, g_strerror (err));
        return FALSE;
    }
    if (address && address[0]) {
        struct ifaddrmsg *ifa;
        guint8 bytes[16];
        gint family = strchr (address, ':') ? AF_INET6 : AF_INET;
        gsize size = family == AF_INET ? 4 : 16;
        if (inet_pton (family, address, bytes) != 1) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid IMS address");
            return FALSE;
        }
        memset (&req, 0, sizeof (req));
        req.hdr.nlmsg_len = NLMSG_LENGTH (sizeof (struct ifaddrmsg));
        req.hdr.nlmsg_type = RTM_NEWADDR;
        req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE;
        ifa = NLMSG_DATA (&req.hdr);
        ifa->ifa_family = family;
        ifa->ifa_prefixlen = family == AF_INET ? 32 : 64;
        ifa->ifa_scope = RT_SCOPE_UNIVERSE;
        ifa->ifa_index = index;
        attr_put (&req, IFA_LOCAL, bytes, size);
        attr_put (&req, IFA_ADDRESS, bytes, size);
        if (!nl_transact (&req, &err, error))
            return FALSE;
        if (err && err != EEXIST) {
            g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err), "Cannot set IMS address: %s", g_strerror (err));
            return FALSE;
        }
    }
    return TRUE;
}
