// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: the control socket (/run/t2-modemd.sock, mode 0660). Line
 * protocol like t2-speechd: one command per connection, reply lines, a final
 * "ok" or "error: ...". "monitor" keeps the connection open and streams
 * events. The client mode of the binary talks to it.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "modemd.h"

#define CLIENT_MS	1000
#define LINE_MAX_IN	1024

/* --- events --- */

void md_event(struct md *m, const char *fmt, ...)
{
	struct ctl_state *c = &m->ctl;
	char line[4096];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line) - 1)
		n = (int)sizeof(line) - 2;
	md_log(m, 1, "event: %s", line);
	line[n++] = '\n';
	for (int i = 0; i < c->nmon;) {
		ssize_t w = send(c->mon[i], line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (w == n) {
			i++;
			continue;
		}
		/* slow or gone: drop the monitor */
		close(c->mon[i]);
		c->mon[i] = c->mon[--c->nmon];
	}
}

/* --- socket --- */

int ctl_listen(struct md *m, const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	struct stat st;
	mode_t old;
	int fd;

	m->ctl.lfd = -1;
	if (strlen(path) >= sizeof(a.sun_path)) {
		md_log(m, 0, "%s: path too long", path);
		return -1;
	}
	strcpy(a.sun_path, path);
	if (!lstat(path, &st)) {
		if (!S_ISSOCK(st.st_mode)) {
			md_log(m, 0, "%s exists and is not a socket", path);
			return -1;
		}
		unlink(path);
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	old = umask(0117);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 8)) {
		umask(old);
		md_log(m, 0, "%s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	umask(old);
	m->ctl.lfd = fd;
	return 0;
}

void ctl_close(struct md *m)
{
	struct ctl_state *c = &m->ctl;

	for (int i = 0; i < c->nmon; i++)
		close(c->mon[i]);
	c->nmon = 0;
	if (c->lfd >= 0)
		close(c->lfd);
	c->lfd = -1;
	unlink(m->cfg.sock);
}

static void send_all(int fd, const char *buf, size_t n)
{
	for (size_t off = 0; off < n;) {
		struct pollfd p = { .fd = fd, .events = POLLOUT };
		ssize_t w;

		if (poll(&p, 1, CLIENT_MS) <= 0)
			return;
		w = send(fd, buf + off, n - off, MSG_NOSIGNAL);
		if (w <= 0)
			return;
		off += (size_t)w;
	}
}

static void add_monitor(struct md *m, int fd)
{
	struct ctl_state *c = &m->ctl;
	static char snap[REPLY_MAX];

	if (c->nmon >= CTL_MON_MAX) {
		send_all(fd, "error: too many monitors\n", 25);
		close(fd);
		return;
	}
	/* a snapshot first (same formats as the events), so the client knows
	 * where it starts */
	snprintf(snap, sizeof(snap), "modem %s\n", md_state_str(m->state));
	if (m->cpin[0])
		str_append(snap, sizeof(snap), "sim %s %s\n", cpin_str(m->cpin), m->cpin);
	if (m->cs.stat >= 0)
		str_append(snap, sizeof(snap), "reg cs %s%s%s\n", reg_stat_str(m->cs.stat),
			   m->cs.act >= 0 ? " " : "", m->cs.act >= 0 ? act_str(m->cs.act) : "");
	if (m->eps.stat >= 0)
		str_append(snap, sizeof(snap), "reg eps %s%s%s\n", reg_stat_str(m->eps.stat),
			   m->eps.act >= 0 ? " " : "", m->eps.act >= 0 ? act_str(m->eps.act) : "");
	if (m->ims_reg >= 0)
		str_append(snap, sizeof(snap), "reg ims %s\n", m->ims_reg ? "registered" : "not-registered");
	if (m->oper[0])
		str_append(snap, sizeof(snap), "reg operator %s %s\n", m->oper, act_str(m->oper_act));
	if (m->rssi != 99)
		str_append(snap, sizeof(snap), "signal %d dBm rssi=%d ber=%d\n", -113 + 2 * m->rssi,
			   m->rssi, m->ber);
	if (m->mipc.enabled)
		str_append(snap, sizeof(snap), "mipc %s\n", mipc_ready(m) ? "ready" :
			   m->mipc.fd >= 0 ? "open" : "closed");
	ims_snapshot(m, snap, sizeof(snap));
	if (m->data.up)
		str_append(snap, sizeof(snap), "data up aid=%d link=%s addr=%s\n", m->data.aid,
			   m->data.ifname, m->data.v4[0] ? m->data.v4 : m->data.v6);
	for (int i = 0; i < m->call.ncalls; i++)
		str_append(snap, sizeof(snap), "call %d %s %s\n", m->call.calls[i].id,
			   call_stat_str(m->call.calls[i].stat),
			   m->call.calls[i].number[0] ? m->call.calls[i].number : "-");
	if (m->call.audio_on)
		str_append(snap, sizeof(snap), "audio on (snapshot)\n");
	send_all(fd, snap, strlen(snap));
	c->mon[c->nmon++] = fd;
}

void ctl_accept(struct md *m)
{
	static char reply[REPLY_MAX];
	char line[LINE_MAX_IN];
	size_t len = 0;
	int64_t dl = mono_ms() + CLIENT_MS;
	int fd = accept4(m->ctl.lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

	if (fd < 0)
		return;
	while (len < sizeof(line) - 1) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int64_t t = mono_ms();
		ssize_t n;

		if (t >= dl || poll(&p, 1, (int)(dl - t)) <= 0)
			break;
		n = read(fd, line + len, sizeof(line) - 1 - len);
		if (n <= 0)
			break;
		len += (size_t)n;
		if (memchr(line, '\n', len))
			break;
	}
	line[len] = 0;
	line[strcspn(line, "\r\n")] = 0;
	if (!strcmp(line, "monitor")) {
		add_monitor(m, fd);
		return;
	}
	ctl_command(m, line, reply, sizeof(reply));
	send_all(fd, reply, strlen(reply));
	close(fd);
}

/* --- commands --- */

static void finish(char *out, size_t cap, int rc)
{
	/* an "error:" line produced by the handler is the final line */
	if (rc) {
		size_t l = strlen(out);
		const char *last;

		while (l && out[l - 1] == '\n')
			l--;
		out[l] = 0;
		last = strrchr(out, '\n');
		last = last ? last + 1 : out;
		if (strncmp(last, "error:", 6))
			str_append(out, cap, "\nerror: failed");
		str_append(out, cap, "\n");
	} else {
		str_append(out, cap, "ok\n");
	}
}

void ctl_command(struct md *m, const char *line, char *out, size_t cap)
{
	char buf[LINE_MAX_IN], *av[16], *save, *t, *rest = NULL;
	char err[256];
	int ac = 0, rc = 0;

	out[0] = 0;
	str_copy(buf, sizeof(buf), line);
	md_log(m, 1, "command: %s", !strncmp(buf, "pin ", 4) ? "pin ****" : buf);
	for (t = strtok_r(buf, " \t", &save); t && ac < 16; t = strtok_r(NULL, " \t", &save))
		av[ac++] = t;
	if (!ac) {
		str_append(out, cap, "error: empty command\n");
		finish(out, cap, -1);
		return;
	}
	/* the rest of the line after the third word (sms send <num> <text>) */
	if (ac >= 3) {
		const char *p = line;

		for (int w = 0; w < 3; w++) {
			while (*p == ' ' || *p == '\t')
				p++;
			while (*p && *p != ' ' && *p != '\t')
				p++;
		}
		while (*p == ' ' || *p == '\t')
			p++;
		rest = (char *)p;
	}

	if (!strcmp(av[0], "status")) {
		modem_status(m, out, cap);
	} else if (!strcmp(av[0], "net")) {
		modem_net(m, out, cap);
	} else if (!strcmp(av[0], "scan")) {
		rc = modem_scan(m, out, cap);
	} else if (!strcmp(av[0], "radio")) {
		if (ac < 2 || (strcmp(av[1], "on") && strcmp(av[1], "off"))) {
			str_append(out, cap, "error: radio on|off [mask]\n");
			rc = -1;
		} else {
			rc = modem_set_radio(m, !strcmp(av[1], "on"), ac > 2 ? av[2] : NULL, err,
					     sizeof(err));
			if (rc)
				str_append(out, cap, "error: %s\n", err);
			else
				str_append(out, cap, "radio %s, sim %s\n", av[1],
					   m->cpin[0] ? m->cpin : "-");
		}
	} else if (!strcmp(av[0], "sim")) {
		int sim;

		if (ac < 2 || parse_int(av[1], 1, 2, &sim)) {
			str_append(out, cap, "error: sim 1|2\n");
			rc = -1;
		} else {
			rc = modem_select_sim(m, sim, err, sizeof(err));
			if (rc)
				str_append(out, cap, "error: %s\n", err);
			else {
				str_append(out, cap, "stack: SIM%d\nsim: %s\n", sim,
					   m->cpin[0] ? m->cpin : "-");
				modem_net(m, out, cap);
			}
		}
	} else if (!strcmp(av[0], "pin")) {
		if (ac < 2) {
			str_append(out, cap, "error: pin <PIN>\n");
			rc = -1;
		} else {
			rc = modem_enter_pin(m, av[1], err, sizeof(err));
			if (rc)
				str_append(out, cap, "error: %s\n", err);
			else
				str_append(out, cap, "sim: %s\n", m->cpin);
		}
	} else if (!strcmp(av[0], "data")) {
		if (ac >= 2 && !strcmp(av[1], "up")) {
			int cid = -1;

			if (ac > 3 && parse_int(av[3], 0, 99, &cid)) {
				str_append(out, cap, "error: data up [apn] [cid]\n");
				rc = -1;
			} else {
				rc = data_up(m, ac > 2 ? av[2] : NULL, cid, out, cap);
			}
		} else if (ac >= 2 && !strcmp(av[1], "down")) {
			rc = data_down(m, out, cap);
		} else if (ac >= 2 && !strcmp(av[1], "status")) {
			data_status(m, out, cap);
		} else {
			str_append(out, cap, "error: data up [apn] [cid] | down | status\n");
			rc = -1;
		}
	} else if (!strcmp(av[0], "apn")) {
		if (ac >= 2 && !strcmp(av[1], "profiles")) {
			rc = ims_apn_command(m, out, cap);
		} else {
			str_append(out, cap, "error: apn profiles\n");
			rc = -1;
		}
	} else if (!strcmp(av[0], "ims")) {
		rc = ims_command(m, ac - 1, av + 1, out, cap);
	} else if (!strcmp(av[0], "call")) {
		rc = call_command(m, ac - 1, av + 1, out, cap);
	} else if (!strcmp(av[0], "sms")) {
		rc = sms_command(m, ac - 1, av + 1, rest, out, cap);
	} else if (!strcmp(av[0], "at")) {
		const char *raw = line + 2;
		struct at_resp r;
		char why[128];

		while (*raw == ' ' || *raw == '\t')
			raw++;
		if (!m->cfg.allow_raw_at) {
			str_append(out, cap, "error: raw AT is disabled (allow_raw_at=0)\n");
			rc = -1;
		} else if (!*raw || strlen(raw) >= AT_CMD_MAX) {
			str_append(out, cap, "error: at <command>\n");
			rc = -1;
		} else if (!vendor_raw_allowed(m, raw, why, sizeof(why))) {
			md_log(m, 0, "raw AT refused: %s (%s)", raw, why);
			str_append(out, cap, "error: refused: %s\n", why);
			rc = -1;
		} else if (m->state != MD_READY && m->at.fd < 0) {
			str_append(out, cap, "error: port not open\n");
			rc = -1;
		} else {
			md_log(m, 0, "raw AT: %s", raw);
			at_cmd_ex(m, raw, NULL, 0, m->cfg.call_timeout_ms, &r);
			if (r.len)
				str_append(out, cap, "%s", r.buf);
			if (r.truncated)
				str_append(out, cap, "[response truncated]\n");
			if (r.res == AT_OK || r.res == AT_CONNECT) {
				str_append(out, cap, "%s\n", at_result_str(r.res));
			} else {
				char e[96];

				at_resp_error(&r, e, sizeof(e));
				str_append(out, cap, "error: %s\n", e);
				rc = -1;
			}
		}
	} else if (!strcmp(av[0], "help")) {
		str_append(out, cap,
			   "status | net | scan | radio on|off [mask] | sim 1|2 | pin <PIN>\n"
			   "data up [apn] [cid] | data down | data status | ims on|off|config|status\n"
			   "apn profiles\n"
			   "call dial <number> [--clir] | answer | hangup [id] | hold | resume | swap\n"
			   "call dtmf <digits> | list | device earpiece|speaker | volume 0..6 | mute on|off\n"
			   "sms send <number> <text> | sms list\n"
			   "monitor | at <command>\n");
	} else {
		str_append(out, cap, "error: unknown command '%s' (try help)\n", av[0]);
		rc = -1;
	}
	finish(out, cap, rc);
}

/* --- client --- */

int client_main(const char *sock, int argc, char **argv)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	static char resp[REPLY_MAX];
	char line[LINE_MAX_IN], *last;
	struct timeval tv = { 180, 0 };
	size_t len = 0;
	ssize_t n;
	int fd, monitor;

	for (int i = 0; i < argc; i++) {
		int w = snprintf(line + len, sizeof(line) - len, "%s%s", i ? " " : "", argv[i]);

		if (w < 0 || (size_t)w >= sizeof(line) - len - 1) {
			fprintf(stderr, "t2-modemd: command too long\n");
			return 2;
		}
		len += (size_t)w;
	}
	monitor = !strcmp(line, "monitor");
	line[len++] = '\n';
	if (strlen(sock) >= sizeof(a.sun_path))
		return 2;
	strcpy(a.sun_path, sock);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		fprintf(stderr, "t2-modemd: %s: %s\n", sock, strerror(errno));
		return 1;
	}
	if (!monitor)
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (send(fd, line, len, MSG_NOSIGNAL) != (ssize_t)len) {
		fprintf(stderr, "t2-modemd: send: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	shutdown(fd, SHUT_WR);
	if (monitor) {
		char buf[4096];

		while ((n = read(fd, buf, sizeof(buf))) > 0) {
			fwrite(buf, 1, (size_t)n, stdout);
			fflush(stdout);
		}
		close(fd);
		return 0;
	}
	len = 0;
	while (len < sizeof(resp) - 1 && (n = read(fd, resp + len, sizeof(resp) - 1 - len)) > 0)
		len += (size_t)n;
	close(fd);
	resp[len] = 0;
	fputs(resp, stdout);
	while (len && resp[len - 1] == '\n')
		resp[--len] = 0;
	last = strrchr(resp, '\n');
	last = last ? last + 1 : resp;
	return strncmp(last, "ok", 2) ? 1 : 0;
}
