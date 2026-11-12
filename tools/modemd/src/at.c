// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: the AT engine. One command at a time on the WWAN AT port
 * (/dev/wwan0at0: one write = one command, reads return response bytes,
 * unsolicited result codes at any time). Commands wait in a queue; each has
 * a timeout; final result codes end a command; everything that is not part
 * of the pending command's response is an unsolicited result code (URC) and
 * is dispatched by prefix.
 *
 * Rules for the rest of the daemon: URC handlers and command callbacks run
 * from inside the port loop and may only at_submit() (asynchronous); the
 * synchronous at_cmd() is for the control-socket handlers and the init
 * sequence, which run from the main loop.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "modemd.h"

static const struct {
	const char *s;
	enum at_result r;
	int call_only;	/* final only for ATD/ATA, a URC otherwise */
	int prefix;	/* match the prefix, the rest is a code */
} finals[] = {
	{ "OK", AT_OK, 0, 0 },
	{ "ERROR", AT_ERROR, 0, 0 },
	{ "+CME ERROR:", AT_CME, 0, 1 },
	{ "+CMS ERROR:", AT_CMS, 0, 1 },
	{ "NO CARRIER", AT_NO_CARRIER, 1, 0 },
	{ "BUSY", AT_BUSY, 1, 0 },
	{ "NO ANSWER", AT_NO_ANSWER, 1, 0 },
	{ "NO DIALTONE", AT_NO_DIALTONE, 1, 0 },
	{ "CONNECT", AT_CONNECT, 0, 0 },
};

/* URC prefixes (3GPP 27.007/27.005 and the vendor ones we know of). A line
 * starting with one of these is a URC unless it answers the pending command
 * of the same name ("AT+CREG?" -> "+CREG: 2,1"). */
static const char *const urc_prefixes[] = {
	"RING", "+CRING:", "+CLIP:", "+CCWA:", "+COLP:", "+CSSI:", "+CSSU:",
	"+CUSD:", "+CCCM:", "+CMT:", "+CMTI:", "+CDS:", "+CDSI:", "+CBM:",
	"+CBMI:", "+CGEV:", "+CREG:", "+CEREG:", "+CGREG:", "+CIREG:",
	"+CIREP:", "+CTZV:", "+CTZE:", "+CSQ:", "+CESQ:", "+CMTE:", "+EPDN:", "+EIF:",
	"+ECPI:", "+EAIC:", "+ESPEECH:", "+EVOCD:", "+EVADSREP:", "+CIREPH:", "+CIREGU:",
	"+EIMSPDN:", "+EIMSPDNOOS:", "+EIMSPDIS:", "+EIMS:", "+EIMCFLAG:", "+CNAP:", "+CDIP:",
	NULL
};

/* URCs whose payload is the next line */
static const char *const urc_two_line[] = { "+CMT:", "+CDS:", "+CBM:", NULL };

const char *at_result_str(enum at_result r)
{
	switch (r) {
	case AT_PENDING: return "pending";
	case AT_OK: return "OK";
	case AT_ERROR: return "ERROR";
	case AT_CME: return "+CME ERROR";
	case AT_CMS: return "+CMS ERROR";
	case AT_NO_CARRIER: return "NO CARRIER";
	case AT_BUSY: return "BUSY";
	case AT_NO_ANSWER: return "NO ANSWER";
	case AT_NO_DIALTONE: return "NO DIALTONE";
	case AT_CONNECT: return "CONNECT";
	case AT_TIMEOUT: return "timeout";
	case AT_LOST: return "modem lost";
	case AT_REFUSED: return "refused";
	}
	return "?";
}

int at_is_error(enum at_result r)
{
	return r != AT_OK && r != AT_CONNECT;
}

void at_resp_error(const struct at_resp *r, char *out, size_t cap)
{
	if ((r->res == AT_CME || r->res == AT_CMS) && r->err_text[0])
		snprintf(out, cap, "%s: %s", at_result_str(r->res), r->err_text);
	else if (r->res == AT_CME || r->res == AT_CMS)
		snprintf(out, cap, "%s: %d", at_result_str(r->res), r->err_code);
	else
		snprintf(out, cap, "%s", at_result_str(r->res));
}

void at_init(struct md *m, int fd)
{
	struct at *a = &m->at;

	memset(a, 0, sizeof(*a));
	a->fd = fd;
	a->cur.err_code = -1;
}

static void resp_reset(struct at_resp *r)
{
	r->res = AT_PENDING;
	r->err_code = -1;
	r->err_text[0] = 0;
	r->len = 0;
	r->buf[0] = 0;
	r->nlines = 0;
	r->truncated = 0;
}

static void resp_add(struct at_resp *r, const char *line)
{
	size_t l = strlen(line);

	if (r->len + l + 2 > sizeof(r->buf)) {
		r->truncated = 1;
		return;
	}
	memcpy(r->buf + r->len, line, l);
	r->len += l;
	r->buf[r->len++] = '\n';
	r->buf[r->len] = 0;
	r->nlines++;
}

/* --- response helpers --- */

/* iterate over the lines of a response buffer: *p advances */
int at_next_line(const char **p, char *line, size_t cap)
{
	const char *s = *p, *e;
	size_t l;

	if (!s || !*s)
		return 0;
	e = strchr(s, '\n');
	l = e ? (size_t)(e - s) : strlen(s);
	if (l >= cap)
		l = cap - 1;
	memcpy(line, s, l);
	line[l] = 0;
	*p = e ? e + 1 : s + strlen(s);
	return 1;
}

/* first line starting with prefix; returns the text after the prefix with
 * leading spaces skipped (pointer into the response buffer, up to '\n') */
const char *at_find(const struct at_resp *r, const char *prefix)
{
	const char *p = r->buf;
	size_t pl = strlen(prefix);

	while (p && *p) {
		if (!strncmp(p, prefix, pl)) {
			p += pl;
			while (*p == ' ')
				p++;
			return p;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

/* split a comma-separated parameter list in place; quotes are removed,
 * commas inside quotes kept, parentheses are not special. Returns the count.
 * Stops at '\n'. */
int at_split(char *s, char **argv, int max)
{
	int n = 0;

	if (!s)
		return 0;
	while (n < max) {
		char *start, *out;
		int q = 0;

		while (*s == ' ')
			s++;
		start = out = s;
		argv[n++] = start;
		while (*s && *s != '\n' && (q || *s != ',')) {
			if (*s == '"')
				q = !q;
			else
				*out++ = *s;
			s++;
		}
		if (*s == ',') {
			*out = 0;
			s++;
			continue;
		}
		*out = 0;
		break;
	}
	return n;
}

/* --- the queue --- */

static struct at_cmd *q_at(struct at *a, unsigned i)
{
	return &a->q[(a->qhead + i) % AT_QUEUE];
}

int at_submit(struct md *m, const char *cmd, const char *pdu, unsigned flags,
	      int timeout_ms, at_cb cb, void *ctx)
{
	struct at *a = &m->at;
	struct at_cmd *c;

	if (a->fd < 0 || m->stopping) {
		md_log(m, 1, "at: %s not queued: port closed", cmd);
		return -1;
	}
	if (a->qlen >= AT_QUEUE) {
		md_log(m, 0, "at: queue full, dropping %s", cmd);
		return -1;
	}
	if (strlen(cmd) >= AT_CMD_MAX || (pdu && strlen(pdu) >= AT_PDU_MAX - 1) ||
	    strchr(cmd, '\r') || strchr(cmd, '\n')) {
		md_log(m, 0, "at: bad command");
		return -1;
	}
	c = q_at(a, a->qlen++);
	memset(c, 0, sizeof(*c));
	str_copy(c->text, sizeof(c->text), cmd);
	if (pdu)
		str_copy(c->pdu, sizeof(c->pdu), pdu);
	c->flags = flags;
	c->timeout_ms = timeout_ms > 0 ? timeout_ms : m->cfg.cmd_timeout_ms;
	c->cb = cb;
	c->ctx = ctx;
	return 0;
}

static int write_all(struct md *m, const char *buf, size_t len)
{
	struct at *a = &m->at;

	for (int i = 0; i < 50; i++) {
		ssize_t n = write(a->fd, buf, len);

		if (n == (ssize_t)len)
			return 0;
		if (n >= 0) {
			/* the WWAN port takes whole commands; a short write
			 * would split one. Treat it as an error. */
			md_log(m, 0, "at: short write %zd of %zu", n, len);
			return -1;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN) {
			struct pollfd p = { .fd = a->fd, .events = POLLOUT };

			poll(&p, 1, 100);
			continue;
		}
		md_log(m, 0, "at: write: %s", strerror(errno));
		if (errno == EIO || errno == ENODEV || errno == ENXIO ||
		    errno == EBADF)
			return -2;
		return -1;
	}
	return -1;
}

/* complete the pending command, dequeue it, then tell the owner */
static void at_complete(struct md *m, enum at_result res)
{
	struct at *a = &m->at;
	struct at_cmd c;
	struct at_resp *r = &a->cur;

	if (!a->qlen)
		return;
	c = *q_at(a, 0);
	a->qhead = (a->qhead + 1) % AT_QUEUE;
	a->qlen--;
	a->busy = 0;
	a->prompt_sent = 0;
	r->res = res;
	if (at_is_error(res))
		a->n_errors++;
	if (res == AT_TIMEOUT) {
		a->n_timeouts++;
		a->settle_until = mono_ms() + m->cfg.settle_ms;
	}
	md_log(m, 1, "at: %s -> %s%s", (c.flags & AT_F_SILENT) ? "(hidden)" : c.text,
	       at_result_str(res), r->nlines ? "" : "");
	if (c.out)
		*c.out = *r;
	if (c.cb)
		c.cb(m, r, c.ctx);
	resp_reset(r);
}

void at_pump(struct md *m)
{
	struct at *a = &m->at;
	struct at_cmd *c;
	char buf[AT_CMD_MAX + 2];
	size_t l;
	int ret;

	if (a->fd < 0 || a->busy || !a->qlen)
		return;
	if (a->settle_until && mono_ms() < a->settle_until)
		return;
	a->settle_until = 0;
	c = q_at(a, 0);
	l = strlen(c->text);
	memcpy(buf, c->text, l);
	buf[l++] = '\r';
	resp_reset(&a->cur);
	a->busy = 1;
	a->prompt_sent = 0;
	a->deadline = mono_ms() + c->timeout_ms;
	a->n_cmds++;
	md_log(m, 1, "at: > %s", (c->flags & AT_F_SILENT) ? "(hidden)" : c->text);
	ret = write_all(m, buf, l);
	if (ret == -2) {
		modem_lost(m, "write error");
		return;
	}
	if (ret < 0)
		at_complete(m, AT_ERROR);
}

int64_t at_next_deadline(struct md *m)
{
	struct at *a = &m->at;

	if (a->busy)
		return a->deadline;
	if (a->qlen && a->settle_until)
		return a->settle_until;
	return INT64_MAX;
}

void at_check_timeout(struct md *m, int64_t now)
{
	struct at *a = &m->at;

	if (a->busy && now >= a->deadline) {
		md_log(m, 0, "at: %s: no final result in %d ms", q_at(a, 0)->text,
		       q_at(a, 0)->timeout_ms);
		at_complete(m, AT_TIMEOUT);
	}
}

/* the port is gone: fail everything that is queued */
void at_lost(struct md *m)
{
	struct at *a = &m->at;

	a->busy = 1;	/* so that at_complete does not need a sent command */
	while (a->qlen)
		at_complete(m, AT_LOST);
	a->busy = 0;
	a->rxlen = 0;
	a->urc_cont[0] = 0;
}

void at_close(struct md *m)
{
	struct at *a = &m->at;

	at_lost(m);
	if (a->fd >= 0)
		close(a->fd);
	a->fd = -1;
}

/* --- lines --- */

static int match_final(const char *line, int call_pending, enum at_result *res,
		       const char **rest)
{
	for (size_t i = 0; i < sizeof(finals) / sizeof(finals[0]); i++) {
		size_t l = strlen(finals[i].s);

		if (finals[i].prefix ? strncmp(line, finals[i].s, l) != 0 :
				       strcmp(line, finals[i].s) != 0)
			continue;
		if (finals[i].call_only && !call_pending)
			return 0;
		*res = finals[i].r;
		*rest = line + l;
		return 1;
	}
	return 0;
}

static const char *match_urc_prefix(const char *line)
{
	for (int i = 0; urc_prefixes[i]; i++) {
		size_t l = strlen(urc_prefixes[i]);

		if (!strncmp(line, urc_prefixes[i], l) &&
		    (line[l] == 0 || line[l] == ' ' || line[l - 1] == ':'))
			return urc_prefixes[i];
	}
	return NULL;
}

/* does "+CREG:" answer the pending "AT+CREG?" / "AT+CREG=..."? */
static int answers_pending(const struct at *a, const char *prefix)
{
	const char *t;
	size_t l;

	if (!a->busy || !a->qlen)
		return 0;
	t = a->q[a->qhead].text;
	if (strncasecmp(t, "AT", 2))
		return 0;
	t += 2;
	l = strlen(prefix);
	if (l && prefix[l - 1] == ':')
		l--;
	if (strncasecmp(t, prefix, l))
		return 0;
	return t[l] == '?' || t[l] == '=' || t[l] == 0;
}

static void dispatch_urc(struct md *m, const char *line, const char *payload)
{
	struct at *a = &m->at;

	a->n_urcs++;
	md_log(m, 1, "at: urc %s%s%s", line, payload ? " / " : "", payload ? payload : "");
	if (!strncmp(line, "+CMT:", 5) || !strncmp(line, "+CMTI:", 6) ||
	    !strncmp(line, "+CDS:", 5) || !strncmp(line, "+CDSI:", 6) ||
	    !strncmp(line, "+CBM:", 5) || !strncmp(line, "+CBMI:", 6)) {
		sms_urc(m, line, payload);
		return;
	}
	if (!strncmp(line, "+CGEV:", 6) || !strncmp(line, "+EPDN:", 6) ||
	    !strncmp(line, "+EIF:", 5)) {
		data_urc(m, line);
		return;
	}
	if (!strcmp(line, "RING") || !strncmp(line, "+CRING:", 7) ||
	    !strncmp(line, "+CLIP:", 6) || !strncmp(line, "+CCWA:", 6) ||
	    !strncmp(line, "+COLP:", 6) || !strncmp(line, "+CSSI:", 6) ||
	    !strncmp(line, "+CSSU:", 6) || !strcmp(line, "NO CARRIER") ||
	    !strcmp(line, "BUSY") || !strcmp(line, "NO ANSWER") ||
	    !strcmp(line, "NO DIALTONE") || !strncmp(line, "+ECPI:", 6) ||
	    !strncmp(line, "+EAIC:", 6) || !strncmp(line, "+ESPEECH:", 9) ||
	    !strncmp(line, "+EVOCD:", 7) || !strncmp(line, "+EVADSREP:", 10) ||
	    !strncmp(line, "+CIREPH:", 8)) {
		call_urc(m, line);
		return;
	}
	if (!strncmp(line, "+CREG:", 6) || !strncmp(line, "+CEREG:", 7) ||
	    !strncmp(line, "+CGREG:", 7) || !strncmp(line, "+CIREG:", 7) ||
	    !strncmp(line, "+CIREGU:", 8) ||
	    !strncmp(line, "+CSQ:", 5) || !strncmp(line, "+CPIN:", 6) ||
	    !strncmp(line, "+CFUN:", 6)) {
		modem_urc(m, line);
		return;
	}
	if (vendor_urc(m, line))
		return;
	md_event(m, "urc %s", line);
}

static void at_line(struct md *m, char *line)
{
	struct at *a = &m->at;
	enum at_result res;
	const char *rest, *pfx;
	int call_pending;

	str_sanitize(line);
	if (!*line)
		return;
	if (a->urc_cont[0]) {
		/* the payload (PDU) of a two-line URC. A final result code or
		 * another URC here means the payload never came. */
		char first[AT_LINE_MAX];

		str_copy(first, sizeof(first), a->urc_first);
		a->urc_cont[0] = 0;
		if (!match_final(line, 1, &res, &rest) && !match_urc_prefix(line)) {
			dispatch_urc(m, first, line);
			return;
		}
		md_log(m, 0, "at: %s without payload", first);
		dispatch_urc(m, first, "");
		/* fall through: process the line itself */
	}
	call_pending = a->busy && (q_at(a, 0)->flags & AT_F_CALL);
	if (a->busy && !strcmp(line, q_at(a, 0)->text)) {
		md_log(m, 1, "at: echo dropped");
		return;
	}
	if (match_final(line, call_pending, &res, &rest)) {
		if (!a->busy) {
			a->n_stray++;
			md_log(m, 0, "at: stray final result '%s'", line);
			if (res == AT_NO_CARRIER)
				call_urc(m, line);
			return;
		}
		if (res == AT_CME || res == AT_CMS) {
			char *end;
			long v;

			while (*rest == ' ')
				rest++;
			errno = 0;
			v = strtol(rest, &end, 10);
			if (!errno && end != rest && *end == 0)
				a->cur.err_code = (int)v;
			else
				str_copy(a->cur.err_text, sizeof(a->cur.err_text), rest);
		}
		if (res == AT_NO_CARRIER || res == AT_BUSY || res == AT_NO_ANSWER ||
		    res == AT_NO_DIALTONE)
			call_urc(m, line);	/* also a call event */
		at_complete(m, res);
		return;
	}
	pfx = match_urc_prefix(line);
	if (pfx && !answers_pending(a, pfx)) {
		for (int i = 0; urc_two_line[i]; i++) {
			if (!strcmp(pfx, urc_two_line[i])) {
				str_copy(a->urc_cont, sizeof(a->urc_cont), pfx);
				str_copy(a->urc_first, sizeof(a->urc_first), line);
				return;
			}
		}
		dispatch_urc(m, line, NULL);
		return;
	}
	if (a->busy) {
		resp_add(&a->cur, line);
		return;
	}
	/* no command pending: an unsolicited line */
	dispatch_urc(m, line, NULL);
}

/* "> " prompt after AT+CMGS: send the PDU and Ctrl-Z */
static int check_prompt(struct md *m)
{
	struct at *a = &m->at;
	struct at_cmd *c;
	size_t i = 0;
	char buf[AT_PDU_MAX + 1];
	size_t l;

	if (!a->busy || a->prompt_sent || !q_at(a, 0)->pdu[0])
		return 0;
	while (i < a->rxlen && (a->rx[i] == '\r' || a->rx[i] == '\n'))
		i++;
	if (a->rxlen - i < 2 || a->rx[i] != '>' || a->rx[i + 1] != ' ')
		return 0;
	i += 2;
	memmove(a->rx, a->rx + i, a->rxlen - i);
	a->rxlen -= i;
	c = q_at(a, 0);
	a->prompt_sent = 1;
	l = strlen(c->pdu);
	memcpy(buf, c->pdu, l);
	buf[l++] = 0x1a;
	md_log(m, 1, "at: > (pdu %zu hex chars)", l - 1);
	if (write_all(m, buf, l) == -2) {
		modem_lost(m, "write error");
		return 1;
	}
	return 1;
}

static void at_feed(struct md *m, const char *buf, size_t n)
{
	struct at *a = &m->at;

	if (a->rxlen + n > sizeof(a->rx)) {
		md_log(m, 0, "at: input without line end, %zu bytes dropped", a->rxlen);
		a->rxlen = 0;
		if (n > sizeof(a->rx))
			return;
	}
	memcpy(a->rx + a->rxlen, buf, n);
	a->rxlen += n;

	for (;;) {
		size_t i;
		char line[AT_LINE_MAX];

		if (check_prompt(m) && a->fd < 0)
			return;
		for (i = 0; i < a->rxlen && a->rx[i] != '\n' && a->rx[i] != '\r'; i++)
			;
		if (i == a->rxlen) {
			if (a->rxlen >= sizeof(a->rx)) {
				md_log(m, 0, "at: line too long, dropped");
				a->rxlen = 0;
			}
			return;
		}
		if (i >= sizeof(line)) {
			md_log(m, 0, "at: line of %zu bytes dropped", i);
			line[0] = 0;
		} else {
			memcpy(line, a->rx, i);
			line[i] = 0;
		}
		/* swallow the line end: \r, \n or \r\n */
		i++;
		if (i < a->rxlen && a->rx[i - 1] == '\r' && a->rx[i] == '\n')
			i++;
		memmove(a->rx, a->rx + i, a->rxlen - i);
		a->rxlen -= i;
		if (line[0])
			at_line(m, line);
		if (a->fd < 0)
			return;
	}
}

void at_read(struct md *m)
{
	struct at *a = &m->at;
	char buf[2048];
	ssize_t n;

	if (a->fd < 0)
		return;
	n = read(a->fd, buf, sizeof(buf));
	if (n > 0) {
		at_feed(m, buf, (size_t)n);
		return;
	}
	if (n == 0) {
		modem_lost(m, "port closed (EOF)");
		return;
	}
	if (errno == EAGAIN || errno == EINTR)
		return;
	md_log(m, 0, "at: read: %s", strerror(errno));
	if (errno == EIO || errno == ENODEV || errno == ENXIO || errno == EBADF)
		modem_lost(m, "read error");
}

/* --- synchronous commands --- */

struct sync_ctx {
	int done;
};

static void sync_cb(struct md *m, struct at_resp *r, void *ctx)
{
	(void)m;
	(void)r;
	((struct sync_ctx *)ctx)->done = 1;
}

enum at_result at_cmd_ex(struct md *m, const char *cmd, const char *pdu,
			 unsigned flags, int timeout_ms, struct at_resp *out)
{
	struct at *a = &m->at;
	struct sync_ctx sc = { 0 };
	struct at_cmd *c;
	static struct at_resp scratch;

	if (!out)
		out = &scratch;
	resp_reset(out);
	if (at_submit(m, cmd, pdu, flags, timeout_ms, sync_cb, &sc)) {
		out->res = a->fd < 0 ? AT_LOST : AT_REFUSED;
		return out->res;
	}
	c = q_at(a, a->qlen - 1);
	c->out = out;
	while (!sc.done) {
		struct pollfd p = { .fd = a->fd, .events = POLLIN };
		int64_t now, dl;
		int to;

		if (a->fd < 0)
			break;
		at_pump(m);
		if (sc.done || a->fd < 0)
			break;
		now = mono_ms();
		dl = at_next_deadline(m);
		to = dl == INT64_MAX ? 1000 : dl <= now ? 0 : (int)(dl - now);
		if (to > 1000)
			to = 1000;
		if (poll(&p, 1, to) > 0) {
			if (p.revents & (POLLIN | POLLHUP | POLLERR))
				at_read(m);
		}
		at_check_timeout(m, mono_ms());
	}
	if (!sc.done) {
		/* the port was lost before the command completed */
		out->res = AT_LOST;
	}
	return out->res;
}

enum at_result at_cmd(struct md *m, const char *cmd, struct at_resp *out)
{
	return at_cmd_ex(m, cmd, NULL, 0, 0, out);
}
