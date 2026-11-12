// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: voice calls. 3GPP 27.007 commands (ATD/ATA/+CHLD/+VTS/+CLCC)
 * plus the MediaTek call-progress model:
 * +ECPI drives the call state (130 created, 0 incoming,
 * 2 alerting, 6 connected, 133 disconnected with cause, 7 all released),
 * +EAIC incoming calls are approved with AT+EAIC=0,<id>,<seq> before they
 * ring, +ESPEECH reports the modem's speech path. +CLCC is polled after
 * every event and periodically while calls exist to reconcile.
 *
 * Speech (t2-speechd call-start/call-stop): speech_on=dial sends
 * call-start right after ATD/ATA is accepted (factory timing) and call-stop
 * when the last call is gone (+ECPI 133 for the last id, +ESPEECH: 0, or a
 * hang-up answered OK with an empty list); speech_on=espeech sends
 * call-start only on +ESPEECH: 1 (modem-driven, unverified: in-band
 * ring-back may be inaudible).
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "modemd.h"

#define SPEECHD_MS	3000
#define NUMBER_DIAL_MAX	40

/* +ECPI msg_type */
#define ECPI_SETUP	0
#define ECPI_ALERT	2
#define ECPI_SYNC	4
#define ECPI_PROGRESS	5
#define ECPI_CONNECTED	6
#define ECPI_RELEASED_ALL 7
#define ECPI_CREATED	130
#define ECPI_HELD_A	131	/* held/active: which is which is unverified */
#define ECPI_HELD_B	132
#define ECPI_DISCONNECT	133

const char *call_stat_str(enum call_stat st)
{
	switch (st) {
	case CALL_ACTIVE: return "active";
	case CALL_HELD: return "held";
	case CALL_DIALING: return "dialing";
	case CALL_ALERTING: return "alerting";
	case CALL_INCOMING: return "incoming";
	case CALL_WAITING: return "waiting";
	case CALL_ENDED: return "ended";
	}
	return "?";
}

void call_init(struct md *m)
{
	struct call_state *c = &m->call;

	memset(c, 0, sizeof(*c));
	c->codec = -1;
	c->last_cause = -1;
	c->espeech = -1;
}

/* --- t2-speechd client --- */

int speechd_send(struct md *m, const char *cmd, char *reply, size_t cap)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char line[256], *last;
	size_t len = 0;
	ssize_t n;
	int fd, ret;
	struct timeval tv = { SPEECHD_MS / 1000, (SPEECHD_MS % 1000) * 1000 };

	if (cap)
		reply[0] = 0;
	if (!m->cfg.audio) {
		if (cap)
			snprintf(reply, cap, "audio disabled\n");
		return 0;
	}
	if (strlen(m->cfg.speechd_sock) >= sizeof(a.sun_path))
		return -1;
	strcpy(a.sun_path, m->cfg.speechd_sock);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		md_log(m, 0, "t2-speechd %s: %s", m->cfg.speechd_sock, strerror(errno));
		if (cap)
			snprintf(reply, cap, "error: t2-speechd: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	snprintf(line, sizeof(line), "%s\n", cmd);
	if (send(fd, line, strlen(line), MSG_NOSIGNAL) != (ssize_t)strlen(line)) {
		close(fd);
		return -1;
	}
	shutdown(fd, SHUT_WR);
	while (len < cap - 1 && (n = read(fd, reply + len, cap - 1 - len)) > 0)
		len += (size_t)n;
	close(fd);
	reply[len] = 0;
	md_log(m, 1, "t2-speechd: %s -> %s", cmd, reply);
	while (len && reply[len - 1] == '\n')
		reply[--len] = 0;
	last = strrchr(reply, '\n');
	last = last ? last + 1 : reply;
	ret = strncmp(last, "ok", 2) ? -1 : 0;
	if (ret)
		md_log(m, 0, "t2-speechd: %s failed: %s", cmd, reply);
	return ret;
}

static void audio_start(struct md *m, const char *why)
{
	char cmd[64], reply[512];

	if (m->call.audio_on)
		return;
	m->call.audio_on = 1;
	snprintf(cmd, sizeof(cmd), "call-start --rate %d", m->cfg.call_rate);
	if (speechd_send(m, cmd, reply, sizeof(reply)))
		md_event(m, "audio error call-start (%s)", why);
	else
		md_event(m, "audio on (%s)", why);
}

static void audio_stop(struct md *m, const char *why)
{
	char reply[512];

	if (!m->call.audio_on)
		return;
	m->call.audio_on = 0;
	if (speechd_send(m, "call-stop", reply, sizeof(reply)))
		md_event(m, "audio error call-stop (%s)", why);
	else
		md_event(m, "audio off (%s)", why);
}

/* --- the call model --- */

static struct call *find_call(struct call_state *c, int id)
{
	for (int i = 0; i < c->ncalls; i++)
		if (c->calls[i].id == id)
			return &c->calls[i];
	return NULL;
}

static struct call *add_call(struct md *m, int id, int dir, enum call_stat st, int mode,
			     const char *number)
{
	struct call_state *c = &m->call;
	struct call *k = find_call(c, id);

	if (!k) {
		if (c->ncalls >= CALL_MAX)
			return NULL;
		k = &c->calls[c->ncalls++];
		memset(k, 0, sizeof(*k));
		k->id = id;
		k->dir = dir;
		k->stat = st;
		k->mode = mode;
		if (number && *number)
			str_copy(k->number, sizeof(k->number), number);
		md_event(m, "call %d %s %s", id, call_stat_str(st), k->number[0] ? k->number : "-");
		return k;
	}
	if (number && *number && !k->number[0])
		str_copy(k->number, sizeof(k->number), number);
	if (k->stat != st) {
		k->stat = st;
		md_event(m, "call %d %s %s", id, call_stat_str(st), k->number[0] ? k->number : "-");
	}
	return k;
}

static void remove_call(struct md *m, int id, int cause)
{
	struct call_state *c = &m->call;

	for (int i = 0; i < c->ncalls; i++) {
		if (c->calls[i].id != id)
			continue;
		if (cause >= 0)
			md_event(m, "call %d ended %s cause %d", id,
				 c->calls[i].number[0] ? c->calls[i].number : "-", cause);
		else
			md_event(m, "call %d ended %s", id,
				 c->calls[i].number[0] ? c->calls[i].number : "-");
		c->calls[i] = c->calls[--c->ncalls];
		return;
	}
}

/* speech policy after a model change */
static void apply_audio(struct md *m, const char *why)
{
	struct call_state *c = &m->call;

	if (!c->ncalls) {
		audio_stop(m, why);
		c->ring_number[0] = 0;
		return;
	}
	if (!strcmp(m->cfg.speech_on, "dial") && !c->audio_on) {
		/* recovery: a call already up (daemon restarted mid-call) */
		for (int i = 0; i < c->ncalls; i++)
			if (c->calls[i].stat == CALL_ACTIVE || c->calls[i].stat == CALL_HELD ||
			    c->calls[i].stat == CALL_DIALING || c->calls[i].stat == CALL_ALERTING)
				audio_start(m, why);
	}
}

/* +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,<number>,<type>[,<alpha>]] */
static int parse_clcc(const char *args, struct call *k)
{
	char buf[AT_LINE_MAX], *av[8];
	int n, v;

	str_copy(buf, sizeof(buf), args);
	buf[strcspn(buf, "\n")] = 0;
	n = at_split(buf, av, 8);
	if (n < 5)
		return -1;
	memset(k, 0, sizeof(*k));
	if (parse_int(av[0], 1, 99, &k->id) || parse_int(av[1], 0, 1, &k->dir) ||
	    parse_int(av[2], 0, 5, &v) || parse_int(av[3], 0, 99, &k->mode) ||
	    parse_int(av[4], 0, 1, &k->mpty))
		return -1;
	k->stat = (enum call_stat)v;
	if (n > 5) {
		str_copy(k->number, sizeof(k->number), av[5]);
		str_sanitize(k->number);
		if (strspn(k->number, "+*#pw0123456789") != strlen(k->number))
			k->number[0] = 0;	/* dropped by the RIL */
	}
	return 0;
}

/* reconcile the model with a +CLCC list */
static void update_calls(struct md *m, struct call *list, int n)
{
	struct call_state *c = &m->call;
	int had = c->ncalls;

	for (int i = 0; i < c->ncalls;) {
		int found = 0;

		for (int j = 0; j < n; j++)
			found |= list[j].id == c->calls[i].id;
		if (found) {
			i++;
			continue;
		}
		md_event(m, "call %d ended %s", c->calls[i].id,
			 c->calls[i].number[0] ? c->calls[i].number : "-");
		c->calls[i] = c->calls[--c->ncalls];
	}
	for (int j = 0; j < n; j++) {
		struct call *k = &list[j], *o = find_call(c, k->id);

		if (!k->number[0] && c->ring_number[0] && k->dir == 1 &&
		    (k->stat == CALL_INCOMING || k->stat == CALL_WAITING))
			str_copy(k->number, sizeof(k->number), c->ring_number);
		if (!o) {
			if (c->ncalls < CALL_MAX) {
				c->calls[c->ncalls++] = *k;
				md_event(m, "call %d %s %s", k->id, call_stat_str(k->stat),
					 k->number[0] ? k->number : "-");
			}
			continue;
		}
		if (o->stat != k->stat || (k->number[0] && strcmp(o->number, k->number))) {
			o->stat = k->stat;
			if (k->number[0])
				str_copy(o->number, sizeof(o->number), k->number);
			md_event(m, "call %d %s %s", k->id, call_stat_str(k->stat),
				 o->number[0] ? o->number : "-");
		}
		o->mode = k->mode;
		o->mpty = k->mpty;
		o->dir = k->dir;
	}
	c->watch_until = mono_ms() + (n ? 0 : 3000);
	/* an empty list stops the audio only when tracked calls disappeared;
	 * right after ATD/ATA the modem may not list the call yet (+ECPI 130
	 * comes first), so a grace period applies */
	if (n || had || mono_ms() >= c->dial_grace)
		apply_audio(m, "clcc");
}

static void cb_clcc(struct md *m, struct at_resp *r, void *ctx)
{
	struct call_state *c = &m->call;
	struct call list[CALL_MAX];
	const char *q = r->buf;
	char line[AT_LINE_MAX];
	int n = 0;

	(void)ctx;
	c->clcc_pending = 0;
	c->n_clcc++;
	if (r->res != AT_OK) {
		if (r->res != AT_LOST)
			md_log(m, 0, "AT+CLCC: %s", at_result_str(r->res));
		return;
	}
	while (n < CALL_MAX && at_next_line(&q, line, sizeof(line))) {
		if (strncmp(line, "+CLCC:", 6))
			continue;
		if (!parse_clcc(line + 6, &list[n]))
			n++;
	}
	update_calls(m, list, n);
	if (c->clcc_wanted) {
		c->clcc_wanted = 0;
		c->next_poll = mono_ms() + 100;
	}
}

static void request_clcc(struct md *m)
{
	struct call_state *c = &m->call;

	if (m->state != MD_READY && m->at.fd < 0)
		return;
	if (c->clcc_pending) {
		c->clcc_wanted = 1;
		return;
	}
	if (!at_submit(m, "AT+CLCC", NULL, 0, 0, cb_clcc, NULL))
		c->clcc_pending = 1;
	c->watch_until = mono_ms() + 3000;
}

/* --- URCs --- */

static void handle_ecpi(struct md *m, const char *args)
{
	struct call_state *c = &m->call;
	struct ecpi e;

	if (vendor_parse_ecpi(args, &e)) {
		md_log(m, 0, "+ECPI: unparsable: %s", args);
		return;
	}
	c->n_ecpi++;
	md_log(m, 1, "ecpi: call %d type %d ibt %d tch %d dir %d mode %d num %s cause %d",
	       e.call_id, e.msg_type, e.is_ibt, e.is_tch, e.dir, e.call_mode, e.number, e.cause);
	switch (e.msg_type) {
	case ECPI_CREATED:
		add_call(m, e.call_id, 0, CALL_DIALING, vendor_ecpi_is_ims(e.call_mode) ? 20 : 0,
			 e.number);
		break;
	case ECPI_SETUP:
		add_call(m, e.call_id, 1, c->ncalls ? CALL_WAITING : CALL_INCOMING,
			 vendor_ecpi_is_ims(e.call_mode) ? 20 : 0,
			 e.number[0] ? e.number : c->ring_number);
		break;
	case ECPI_ALERT:
		add_call(m, e.call_id, 0, CALL_ALERTING, 0, e.number);
		if (e.is_ibt)
			md_event(m, "call %d ringback in-band", e.call_id);
		break;
	case ECPI_CONNECTED:
		add_call(m, e.call_id, 0, CALL_ACTIVE, 0, e.number);
		break;
	case ECPI_DISCONNECT:
		c->last_cause = e.cause;
		remove_call(m, e.call_id, e.cause);
		if (!c->ncalls)
			audio_stop(m, "ecpi 133 last call");
		break;
	case ECPI_RELEASED_ALL:
		for (int i = c->ncalls - 1; i >= 0; i--)
			remove_call(m, c->calls[i].id, -1);
		audio_stop(m, "ecpi 7");
		break;
	case ECPI_HELD_A:
	case ECPI_HELD_B:
		md_event(m, "call %d state-change %d", e.call_id, e.msg_type);
		break;
	case ECPI_SYNC:
	case ECPI_PROGRESS:
		if (e.is_ibt)
			md_event(m, "call %d ringback in-band", e.call_id);
		break;
	default:
		md_event(m, "call %d ecpi %d", e.call_id, e.msg_type);
		break;
	}
	request_clcc(m);
}

static void handle_eaic(struct md *m, const char *args)
{
	struct call_state *c = &m->call;
	struct eaic e;
	char cmd[AT_CMD_MAX];
	int accept = m->cfg.eaic_auto_accept;

	if (vendor_parse_eaic(args, &e)) {
		md_log(m, 0, "+EAIC: unparsable: %s", args);
		return;
	}
	c->n_eaic++;
	/* the described self-rejections: a call on the other stack is not visible
	 * here; nothing else applies */
	vendor_eaic_cmd(accept, e.call_id, e.seq, cmd, sizeof(cmd));
	at_submit(m, cmd, NULL, 0, 0, NULL, NULL);
	str_copy(c->ring_number, sizeof(c->ring_number), e.number);
	if (accept) {
		add_call(m, e.call_id, 1, c->ncalls ? CALL_WAITING : CALL_INCOMING,
			 vendor_ecpi_is_ims(e.call_mode) ? 20 : 0, e.number);
	} else {
		md_event(m, "call %d rejected %s", e.call_id, e.number[0] ? e.number : "-");
	}
}

/* RING, +CRING, +CLIP, +CCWA, NO CARRIER, +ECPI, +EAIC, +ESPEECH, +EVOCD ... */
void call_urc(struct md *m, const char *line)
{
	struct call_state *c = &m->call;

	if (!strncmp(line, "+ECPI:", 6)) {
		handle_ecpi(m, line + 6);
		return;
	}
	if (!strncmp(line, "+EAIC:", 6)) {
		handle_eaic(m, line + 6);
		return;
	}
	if (!strncmp(line, "+ESPEECH:", 9)) {
		int on = vendor_parse_espeech(line + 9);

		if (on < 0)
			return;
		c->espeech = on;
		md_event(m, "call speech %d", on);
		if (on && !strcmp(m->cfg.speech_on, "espeech"))
			audio_start(m, "espeech 1");
		else if (!on)
			audio_stop(m, "espeech 0");
		return;
	}
	if (!strncmp(line, "+EVOCD:", 7)) {
		int v;

		if (!parse_int(line + 7, 0, 65535, &v)) {
			c->codec = v;
			md_event(m, "call codec %d", v);
		}
		return;
	}
	if (!strncmp(line, "+EVADSREP:", 10) || !strncmp(line, "+CIREPH:", 8)) {
		md_event(m, "call %s", line + 1);
		if (!strncmp(line, "+CIREPH:", 8))
			request_clcc(m);
		return;
	}
	if (!strncmp(line, "+CLIP:", 6) || !strncmp(line, "+CCWA:", 6)) {
		char buf[AT_LINE_MAX], *av[4];

		str_copy(buf, sizeof(buf), line + 6);
		if (at_split(buf, av, 4) >= 1 && av[0][0]) {
			str_copy(c->ring_number, sizeof(c->ring_number), av[0]);
			str_sanitize(c->ring_number);
			for (int i = 0; i < c->ncalls; i++)
				if (c->calls[i].dir == 1 && !c->calls[i].number[0] &&
				    (c->calls[i].stat == CALL_INCOMING ||
				     c->calls[i].stat == CALL_WAITING)) {
					str_copy(c->calls[i].number, sizeof(c->calls[i].number),
						 c->ring_number);
					md_event(m, "call %d %s %s", c->calls[i].id,
						 call_stat_str(c->calls[i].stat), c->ring_number);
				}
		}
		md_event(m, "call ring %s", c->ring_number[0] ? c->ring_number : "-");
	} else if (!strcmp(line, "RING") || !strncmp(line, "+CRING:", 7)) {
		if (!c->ring_seen)
			md_event(m, "call ring %s", c->ring_number[0] ? c->ring_number : "-");
		c->ring_seen = 1;
	} else {
		md_event(m, "call result %s", line);
	}
	request_clcc(m);
}

void call_tick(struct md *m, int64_t now)
{
	struct call_state *c = &m->call;

	if (m->state != MD_READY)
		return;
	if ((c->ncalls || now < c->watch_until || c->clcc_wanted) && now >= c->next_poll &&
	    !c->clcc_pending) {
		c->next_poll = now + m->cfg.call_poll_ms;
		request_clcc(m);
	}
	if (!c->ncalls)
		c->ring_seen = 0;
}

int64_t call_next_deadline(struct md *m)
{
	struct call_state *c = &m->call;

	if (c->ncalls || mono_ms() < c->watch_until || c->clcc_wanted)
		return c->next_poll;
	return INT64_MAX;
}

void call_modem_lost(struct md *m)
{
	struct call_state *c = &m->call;

	for (int i = 0; i < c->ncalls; i++)
		md_event(m, "call %d ended %s", c->calls[i].id,
			 c->calls[i].number[0] ? c->calls[i].number : "-");
	c->ncalls = 0;
	c->clcc_pending = 0;
	c->clcc_wanted = 0;
	c->ring_number[0] = 0;
	audio_stop(m, "modem lost");
}

void call_shutdown(struct md *m)
{
	struct call_state *c = &m->call;
	char cmd[AT_CMD_MAX];

	if (c->ncalls && m->at.fd >= 0) {
		md_log(m, 0, "exit: hanging up");
		vendor_hangup_cmd(0, cmd, sizeof(cmd));
		if (at_cmd(m, cmd, NULL) != AT_OK)
			at_cmd(m, "ATH", NULL);
	}
	c->ncalls = 0;
	audio_stop(m, "exit");
}

/* once per stack: each command optional */
void call_stack_init(struct md *m)
{
	char list[sizeof(m->cfg.voice_init_cmds)], *save, *t;
	struct at_resp r;

	str_copy(list, sizeof(list), m->cfg.voice_init_cmds);
	for (t = strtok_r(list, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
		while (*t == ' ')
			t++;
		if (!*t)
			continue;
		at_cmd(m, t, &r);
		if (r.res == AT_LOST)
			return;
		if (r.res != AT_OK)
			md_log(m, 0, "voice init: %s refused (%s)", t, at_result_str(r.res));
	}
}

void call_status(struct md *m, char *out, size_t cap)
{
	struct call_state *c = &m->call;

	str_append(out, cap, "calls: %d, audio %s (speech_on=%s), espeech %d, codec %d, "
		   "last cause %d, clcc %lu ecpi %lu eaic %lu\n", c->ncalls,
		   c->audio_on ? "on" : "off", m->cfg.speech_on, c->espeech, c->codec,
		   c->last_cause, c->n_clcc, c->n_ecpi, c->n_eaic);
	for (int i = 0; i < c->ncalls; i++)
		str_append(out, cap, "call %d: %s %s %s%s%s\n", c->calls[i].id,
			   call_stat_str(c->calls[i].stat), c->calls[i].dir ? "MT" : "MO",
			   c->calls[i].number[0] ? c->calls[i].number : "-",
			   c->calls[i].mpty ? " (multiparty)" : "",
			   vendor_ecpi_is_ims(c->calls[i].mode) ? " (ims)" : "");
}

/* --- commands --- */

static int valid_number(const char *s)
{
	size_t l = strlen(s);

	if (!l || l > NUMBER_DIAL_MAX)
		return 0;
	if (*s == '+') {
		s++;
		l--;
	}
	return l && strspn(s, "0123456789*#ABCDabcd") == l;
}

/* refresh the list synchronously (control commands) */
static void refresh_sync(struct md *m)
{
	struct at_resp r;
	struct call list[CALL_MAX];
	const char *q;
	char line[AT_LINE_MAX];
	int n = 0;

	if (at_cmd(m, "AT+CLCC", &r) != AT_OK)
		return;
	q = r.buf;
	while (n < CALL_MAX && at_next_line(&q, line, sizeof(line))) {
		if (!strncmp(line, "+CLCC:", 6) && !parse_clcc(line + 6, &list[n]))
			n++;
	}
	m->call.n_clcc++;
	update_calls(m, list, n);
}

static int simple(struct md *m, const char *cmd, int timeout, unsigned flags,
		  char *out, size_t cap)
{
	struct at_resp r;

	at_cmd_ex(m, cmd, NULL, flags, timeout, &r);
	if (r.res != AT_OK) {
		char e[96];

		at_resp_error(&r, e, sizeof(e));
		str_append(out, cap, "error: %s: %s\n", cmd, e);
		return -1;
	}
	return 0;
}

int call_command(struct md *m, int argc, char **argv, char *out, size_t cap)
{
	struct call_state *c = &m->call;
	char cmd[AT_CMD_MAX];
	int rc;

	if (argc < 1) {
		str_append(out, cap, "error: call dial|answer|hangup|hold|resume|swap|dtmf|list|device|volume|mute\n");
		return -1;
	}
	if (!strcmp(argv[0], "list")) {
		if (m->state == MD_READY)
			refresh_sync(m);
		call_status(m, out, cap);
		return 0;
	}
	if (!strcmp(argv[0], "device") || !strcmp(argv[0], "volume") ||
	    !strcmp(argv[0], "mute")) {
		char reply[2048], *last;

		if (argc < 2) {
			str_append(out, cap, "error: %s needs an argument\n", argv[0]);
			return -1;
		}
		snprintf(cmd, sizeof(cmd), "%s %s", argv[0], argv[1]);
		rc = speechd_send(m, cmd, reply, sizeof(reply));
		/* t2-speechd's lines without its final ok/error */
		last = strrchr(reply, '\n');
		if (last)
			*last = 0;
		else
			reply[0] = 0;
		if (reply[0])
			str_append(out, cap, "%s\n", reply);
		if (rc)
			str_append(out, cap, "error: t2-speechd %s failed\n", argv[0]);
		return rc;
	}
	if (m->state != MD_READY) {
		str_append(out, cap, "error: modem not ready\n");
		return -1;
	}
	if (!strcmp(argv[0], "dial")) {
		int clir = 0, emergency = 0;

		if (argc < 2 || !valid_number(argv[1])) {
			str_append(out, cap, "error: call dial <number> [--clir|--no-clir] [--emergency]\n");
			return -1;
		}
		for (int i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "--clir"))
				clir = 1;
			else if (!strcmp(argv[i], "--no-clir"))
				clir = 2;
			else if (!strcmp(argv[i], "--emergency"))
				emergency = 1;
			else {
				str_append(out, cap, "error: unknown option %s\n", argv[i]);
				return -1;
			}
		}
		vendor_dial_cmd(argv[1], clir, emergency, cmd, sizeof(cmd));
		rc = simple(m, cmd, m->cfg.call_timeout_ms, AT_F_CALL, out, cap);
		if (!rc) {
			/* factory timing: speech on as soon as the dial is accepted */
			if (!strcmp(m->cfg.speech_on, "dial"))
				audio_start(m, "dial");
			c->watch_until = mono_ms() + 10000;
			c->dial_grace = mono_ms() + 5000;
			str_append(out, cap, "dialing %s%s\n", argv[1], emergency ? " (emergency)" : "");
		}
		refresh_sync(m);
		return rc;
	}
	if (!strcmp(argv[0], "answer")) {
		rc = simple(m, "ATA", m->cfg.call_timeout_ms, AT_F_CALL, out, cap);
		if (!rc && !strcmp(m->cfg.speech_on, "dial"))
			audio_start(m, "answer");
		if (!rc)
			c->dial_grace = mono_ms() + 5000;
		refresh_sync(m);
		return rc;
	}
	if (!strcmp(argv[0], "hangup")) {
		int id = 0;

		if (argc >= 2 && parse_int(argv[1], 1, 7, &id)) {
			str_append(out, cap, "error: call id 1..7\n");
			return -1;
		}
		if (!id && c->ncalls == 1)
			id = c->calls[0].id;
		vendor_hangup_cmd(id, cmd, sizeof(cmd));
		rc = simple(m, cmd, 0, 0, out, cap);
		if (rc && !id) {
			out[0] = 0;
			rc = simple(m, "ATH", 0, 0, out, cap);
		}
		if (rc && !id) {
			out[0] = 0;
			rc = simple(m, "AT+CHUP", 0, 0, out, cap);
		}
		refresh_sync(m);
		if (!rc && !c->ncalls)
			audio_stop(m, "hangup");
		return rc;
	}
	if (!strcmp(argv[0], "hold") || !strcmp(argv[0], "resume") ||
	    !strcmp(argv[0], "swap")) {
		/* +CHLD=2: active calls on hold, held or waiting call accepted */
		rc = simple(m, "AT+CHLD=2", 0, 0, out, cap);
		refresh_sync(m);
		return rc;
	}
	if (!strcmp(argv[0], "dtmf")) {
		size_t l;

		if (argc < 2) {
			str_append(out, cap, "error: call dtmf <digits>\n");
			return -1;
		}
		l = strlen(argv[1]);
		if (!l || l > 32 || strspn(argv[1], "0123456789*#ABCDabcd") != l) {
			str_append(out, cap, "error: DTMF digits are 0-9 * # A-D\n");
			return -1;
		}
		if (!c->ncalls) {
			str_append(out, cap, "error: no call\n");
			return -1;
		}
		for (size_t i = 0; i < l; i++) {
			snprintf(cmd, sizeof(cmd), "AT+VTS=%c", argv[1][i]);
			if (simple(m, cmd, 0, 0, out, cap))
				return -1;
		}
		str_append(out, cap, "sent %zu digit%s\n", l, l == 1 ? "" : "s");
		return 0;
	}
	str_append(out, cap, "error: unknown call command %s\n", argv[0]);
	return -1;
}
