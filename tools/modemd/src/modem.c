// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: the modem state machine. Opens the AT port (with retries),
 * waits for the modem to be ready (the driver's state file or -a), runs the
 * init sequence, tracks radio, SIM, registration and signal, and handles the
 * loss of the modem.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "modemd.h"

#define SIM_BUSY_RETRY_MS	1000

const char *md_state_str(enum md_state st)
{
	switch (st) {
	case MD_CLOSED: return "closed";
	case MD_OPEN: return "init";
	case MD_READY: return "ready";
	}
	return "?";
}

const char *reg_stat_str(int stat)
{
	switch (stat) {
	case 0: return "not-registered";
	case 1: return "home";
	case 2: return "searching";
	case 3: return "denied";
	case 4: return "unknown";
	case 5: return "roaming";
	case 6: return "home-sms-only";
	case 7: return "roaming-sms-only";
	case 8: return "emergency-only";
	case 9: return "home-csfb-not-preferred";
	case 10: return "roaming-csfb-not-preferred";
	case 11: return "emergency-only";
	}
	return "-";
}

/* 27.007 <AcT> */
const char *act_str(int act)
{
	switch (act) {
	case 0: return "GSM";
	case 1: return "GSM-compact";
	case 2: return "UTRAN";
	case 3: return "EDGE";
	case 4: return "HSDPA";
	case 5: return "HSUPA";
	case 6: return "HSPA";
	case 7: return "LTE";
	case 8: return "EC-GSM-IoT";
	case 9: return "LTE-NB";
	case 10: return "LTE-5GCN";
	case 11: return "NR";
	case 12: return "NG-RAN";
	case 13: return "LTE-NR-DC";
	}
	return "-";
}

/* --- readiness --- */

/* 1 ready, 0 not ready, -1 no state source */
static int state_file_ready(struct md *m)
{
	char line[64];
	FILE *f;
	int ret = -1;

	if (!m->cfg.state_file[0])
		return -1;
	f = fopen(m->cfg.state_file, "re");
	if (!f)
		return -1;
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "state: ", 7))
			ret = !strcmp(line + 7, "ready");
		else
			ret = 0;
	}
	fclose(f);
	return ret;
}

int md_ready(struct md *m)
{
	return m->state == MD_READY;
}

static int modem_is_ready_src(struct md *m)
{
	int r = state_file_ready(m);

	if (r >= 0)
		return r;
	return m->cfg.assume_ready;
}

/* --- the port --- */

static int open_port(struct md *m)
{
	struct stat st;
	int fd = open(m->cfg.port, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);

	if (fd < 0) {
		md_log(m, 1, "%s: %s", m->cfg.port, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
		md_log(m, 0, "%s: not a character device, refusing", m->cfg.port);
		close(fd);
		return -2;
	}
	if (isatty(fd)) {
		/* a tty (tests on a pty, a serial adapter): raw, no echo */
		struct termios t;

		if (!tcgetattr(fd, &t)) {
			cfmakeraw(&t);
			t.c_cflag |= CLOCAL | CREAD;
			tcsetattr(fd, TCSANOW, &t);
		}
	}
	return fd;
}

static void reset_facts(struct md *m)
{
	m->cfun = -1;
	m->cpin[0] = 0;
	m->cs.stat = m->eps.stat = m->gprs.stat = -1;
	m->cs.act = m->eps.act = m->gprs.act = -1;
	m->cs.lac[0] = m->cs.ci[0] = m->eps.lac[0] = m->eps.ci[0] = 0;
	m->ims_reg = -1;
	m->ims.ext = -1;
	m->sim_retry_at = 0;
	m->oper[0] = 0;
	m->oper_act = -1;
	m->rssi = m->ber = 99;
}

void modem_lost(struct md *m, const char *why)
{
	if (m->state == MD_CLOSED && m->at.fd < 0)
		return;
	md_log(m, 0, "modem lost: %s", why);
	m->state = MD_CLOSED;
	call_modem_lost(m);
	data_modem_lost(m);
	ims_modem_lost(m);
	mipc_close(m, why);
	at_close(m);
	reset_facts(m);
	m->reopen_at = mono_ms() + m->cfg.reopen_ms;
	m->init_tries = 0;
	md_event(m, "modem lost %s", why);
}

/* --- parsing of the tracked facts --- */

static void parse_reg(struct md *m, const char *name, struct reg *r,
		      const char *args, int is_urc)
{
	char buf[AT_LINE_MAX], *av[8];
	int n, i = 0, stat, act = -1, old = r->stat, old_act = r->act;

	str_copy(buf, sizeof(buf), args);
	n = at_split(buf, av, 8);
	/* response: <n>,<stat>[,<lac>,<ci>[,<AcT>]]; URC: <stat>[,...] */
	if (!is_urc) {
		if (n < 2)
			return;
		i = 1;
	}
	if (n <= i || parse_int(av[i], 0, 11, &stat))
		return;
	r->stat = stat;
	if (n > i + 2) {
		str_copy(r->lac, sizeof(r->lac), av[i + 1]);
		str_copy(r->ci, sizeof(r->ci), av[i + 2]);
	}
	if (n > i + 3 && !parse_int(av[i + 3], 0, 13, &act))
		r->act = act;
	else if (n <= i + 1 || stat == 0 || stat == 2 || stat == 3 || stat == 4)
		r->act = -1;	/* no AcT given: unknown */
	if (r->stat != old || r->act != old_act) {
		if (r->act >= 0)
			md_event(m, "reg %s %s %s", name, reg_stat_str(r->stat),
				 act_str(r->act));
		else
			md_event(m, "reg %s %s", name, reg_stat_str(r->stat));
	}
}

/* +CIREG: <n>,<reg_info>[,<ext_info>] (response), +CIREGU: <reg_info>[,<ext_info>]
 * (URC); <ext_info> is a hexadecimal bit mask of the registered services
 * (27.007 §8.71) */
static void parse_cireg(struct md *m, const char *args, int is_urc)
{
	char buf[AT_LINE_MAX], *av[4];
	int n, i = is_urc ? 0 : 1, v, old = m->ims_reg;

	str_copy(buf, sizeof(buf), args);
	n = at_split(buf, av, 4);
	if (n <= i || parse_int(av[i], 0, 1, &v))
		return;
	m->ims_reg = v;
	if (v != old)
		md_event(m, "reg ims %s", v ? "registered" : "not-registered");
	if (n > i + 1 && av[i + 1][0] && strspn(av[i + 1], "0123456789abcdefABCDEF") ==
	    strlen(av[i + 1]) && strlen(av[i + 1]) <= 6)
		ims_set_ext(m, (int)strtol(av[i + 1], NULL, 16));
	else if (!v)
		ims_set_ext(m, 0);
}

static void parse_csq(struct md *m, const char *args)
{
	char buf[64], *av[2];
	int rssi, ber;

	str_copy(buf, sizeof(buf), args);
	if (at_split(buf, av, 2) < 2 || parse_int(av[0], 0, 99, &rssi) ||
	    parse_int(av[1], 0, 99, &ber))
		return;
	if (rssi != m->rssi || ber != m->ber) {
		m->rssi = rssi;
		m->ber = ber;
		if (rssi == 99)
			md_event(m, "signal unknown");
		else
			md_event(m, "signal %d dBm rssi=%d ber=%d", -113 + 2 * rssi,
				 rssi, ber);
	}
}

const char *cpin_str(const char *code)
{
	if (!strcmp(code, "READY"))
		return "ready";
	if (!strcmp(code, "SIM PIN"))
		return "pin";
	if (!strcmp(code, "SIM PUK"))
		return "puk";
	if (!strcmp(code, "SIM PIN2"))
		return "pin2";
	if (!strcmp(code, "SIM PUK2"))
		return "puk2";
	if (!strncmp(code, "PH-", 3))
		return "locked";
	return "other";
}

static void set_cpin(struct md *m, const char *code)
{
	char old[sizeof(m->cpin)];

	str_copy(old, sizeof(old), m->cpin);
	str_copy(m->cpin, sizeof(m->cpin), code);
	if (strcmp(old, m->cpin))
		md_event(m, "sim %s %s", cpin_str(m->cpin), m->cpin);
}

static void parse_cpin(struct md *m, const char *args)
{
	char buf[64];

	str_copy(buf, sizeof(buf), args);
	buf[strcspn(buf, "\n")] = 0;
	set_cpin(m, buf);
}

static void parse_cops(struct md *m, const char *args)
{
	char buf[AT_LINE_MAX], *av[4];
	char old[sizeof(m->oper)];
	int n, act = -1;

	str_copy(old, sizeof(old), m->oper);
	str_copy(buf, sizeof(buf), args);
	n = at_split(buf, av, 4);
	if (n >= 3)
		str_copy(m->oper, sizeof(m->oper), av[2]);
	else
		m->oper[0] = 0;
	if (n >= 4 && !parse_int(av[3], 0, 13, &act))
		m->oper_act = act;
	else
		m->oper_act = -1;
	if (strcmp(old, m->oper))
		md_event(m, "reg operator %s %s", m->oper[0] ? m->oper : "-",
			 act_str(m->oper_act));
}

/* URCs: +CREG/+CEREG/+CGREG/+CIREG/+CSQ/+CPIN/+CFUN */
void modem_urc(struct md *m, const char *line)
{
	if (!strncmp(line, "+CREG:", 6))
		parse_reg(m, "cs", &m->cs, line + 6, 1);
	else if (!strncmp(line, "+CEREG:", 7))
		parse_reg(m, "eps", &m->eps, line + 7, 1);
	else if (!strncmp(line, "+CGREG:", 7))
		parse_reg(m, "gprs", &m->gprs, line + 7, 1);
	else if (!strncmp(line, "+CIREG:", 7))
		parse_cireg(m, line + 7, 1);
	else if (!strncmp(line, "+CIREGU:", 8))
		parse_cireg(m, line + 8, 1);
	else if (!strncmp(line, "+CSQ:", 5))
		parse_csq(m, line + 5);
	else if (!strncmp(line, "+CPIN:", 6)) {
		const char *p = line + 6;

		while (*p == ' ')
			p++;
		parse_cpin(m, p);
	} else if (!strncmp(line, "+CFUN:", 6)) {
		int v;

		if (!parse_int(line + 7, 0, 127, &v))
			m->cfun = v;
	}
}

/* --- queries (synchronous, from the main loop or a control command) --- */

static void cb_csq(struct md *m, struct at_resp *r, void *ctx)
{
	const char *p;

	(void)ctx;
	if (r->res != AT_OK)
		return;
	p = at_find(r, "+CSQ:");
	if (p)
		parse_csq(m, p);
}

void modem_refresh_signal(struct md *m)
{
	if (m->state != MD_READY)
		return;
	at_submit(m, "AT+CSQ", NULL, 0, 0, cb_csq, NULL);
}

void modem_refresh_sim(struct md *m)
{
	struct at_resp r;
	const char *p;

	at_cmd(m, "AT+CPIN?", &r);
	if (r.res == AT_OK && (p = at_find(&r, "+CPIN:"))) {
		m->sim_retry_at = 0;
		parse_cpin(m, p);
	} else if (r.res == AT_CME && (r.err_code == 14 || strcasestr(r.err_text, "busy"))) {
		/* 27.007 CME 14 "SIM busy", right after a modem restart: not a
		 * SIM state, read again later */
		md_log(m, 0, "sim: busy, reading again in %d ms", SIM_BUSY_RETRY_MS);
		m->sim_retry_at = mono_ms() + SIM_BUSY_RETRY_MS;
	} else if (r.res == AT_CME || r.res == AT_ERROR) {
		char e[96];

		at_resp_error(&r, e, sizeof(e));
		set_cpin(m, e);
	}
	at_cmd(m, "AT+CFUN?", &r);
	if (r.res == AT_OK && (p = at_find(&r, "+CFUN:"))) {
		int v;

		if (!parse_int(p, 0, 127, &v))
			m->cfun = v;
	}
}

/* AT+CIREG? alone: on mainline +CIREGU did not reach the AT port */
void modem_refresh_ims(struct md *m)
{
	struct at_resp r;
	const char *p;

	if (m->state == MD_READY && at_cmd(m, "AT+CIREG?", &r) == AT_OK &&
	    (p = at_find(&r, "+CIREG:")))
		parse_cireg(m, p, 0);
}

void modem_refresh_reg(struct md *m)
{
	struct at_resp r;
	const char *p;

	if (at_cmd(m, "AT+CREG?", &r) == AT_OK && (p = at_find(&r, "+CREG:")))
		parse_reg(m, "cs", &m->cs, p, 0);
	if (at_cmd(m, "AT+CEREG?", &r) == AT_OK && (p = at_find(&r, "+CEREG:")))
		parse_reg(m, "eps", &m->eps, p, 0);
	if (at_cmd(m, "AT+CGREG?", &r) == AT_OK && (p = at_find(&r, "+CGREG:")))
		parse_reg(m, "gprs", &m->gprs, p, 0);
	if (at_cmd(m, "AT+CIREG?", &r) == AT_OK && (p = at_find(&r, "+CIREG:")))
		parse_cireg(m, p, 0);
	if (at_cmd(m, "AT+COPS?", &r) == AT_OK && (p = at_find(&r, "+COPS:")))
		parse_cops(m, p);
	if (at_cmd(m, "AT+CSQ", &r) == AT_OK && (p = at_find(&r, "+CSQ:")))
		parse_csq(m, p);
}

/* --- init --- */

/* optional: a refusal is logged, not fatal */
static int init_opt(struct md *m, const char *cmd)
{
	struct at_resp r;

	at_cmd(m, cmd, &r);
	if (r.res == AT_LOST || r.res == AT_TIMEOUT)
		return -1;
	if (r.res != AT_OK)
		md_log(m, 0, "init: %s refused (%s), continuing", cmd, at_result_str(r.res));
	return 0;
}

static int modem_init(struct md *m)
{
	struct at_resp r;
	char cmd[AT_CMD_MAX];

	/* echo off: the first command may be mangled by a stale partial line */
	at_cmd(m, "ATE0", &r);
	if (r.res == AT_TIMEOUT)
		at_cmd(m, "ATE0", &r);
	if (r.res == AT_LOST || r.res == AT_TIMEOUT)
		return -1;
	if (init_opt(m, "AT+CMEE=2"))
		return -1;
	if (at_cmd(m, "ATI", &r) == AT_OK) {
		const char *q = r.buf;
		char line[AT_LINE_MAX];

		m->model[0] = 0;
		while (at_next_line(&q, line, sizeof(line))) {
			if (!m->model[0])
				str_copy(m->model, sizeof(m->model), line);
			else if (!m->revision[0])
				str_copy(m->revision, sizeof(m->revision), line);
		}
	}
	if (at_cmd(m, "AT+CGMR", &r) == AT_OK && r.nlines) {
		char line[AT_LINE_MAX];
		const char *q = r.buf;

		if (at_next_line(&q, line, sizeof(line)))
			str_copy(m->revision, sizeof(m->revision), line);
	}
	/* stack select before anything SIM related */
	if (m->cfg.sim != 1 && !vendor_stack_cmd(m->cfg.sim, cmd, sizeof(cmd))) {
		if (at_cmd(m, cmd, &r) == AT_OK)
			m->sim_sel = m->cfg.sim;
		else
			md_log(m, 0, "init: %s failed (%s)", cmd, at_result_str(r.res));
	}
	/* with radio_on_at_start the radio goes on after the IMS/APN start-up
	 * (ims.c), which must come first */
	if (m->cfg.radio_mask[0] && !m->cfg.radio_on_at_start &&
	    !vendor_radio_cmd(m, 1, m->cfg.radio_mask, cmd, sizeof(cmd))) {
		at_cmd_ex(m, cmd, NULL, 0, 15000, &r);
		if (r.res != AT_OK)
			md_log(m, 0, "init: %s: %s", cmd, at_result_str(r.res));
	}
	/* registration and call URCs */
	if (init_opt(m, "AT+CREG=2") || init_opt(m, "AT+CEREG=2") ||
	    init_opt(m, "AT+CGREG=2") || init_opt(m, "AT+CIREG=2") ||
	    init_opt(m, "AT+COPS=3,0") || init_opt(m, "AT+CLIP=1") ||
	    init_opt(m, "AT+CCWA=1") || init_opt(m, "AT+CRC=1") ||
	    init_opt(m, "AT+CGEREP=2,1") || init_opt(m, "AT+CGPIAF=1,0,0,0"))
		return -1;
	/* SMS: PDU mode, deliver directly (+CMT) */
	if (init_opt(m, "AT+CMGF=0"))
		return -1;
	at_cmd(m, "AT+CNMI=2,2,0,1,0", &r);
	if (r.res == AT_LOST || r.res == AT_TIMEOUT)
		return -1;
	if (r.res != AT_OK && init_opt(m, "AT+CNMI=1,2,0,0,0"))
		return -1;
	vendor_init_cmds(m);
	call_stack_init(m);
	/* extra commands from the configuration */
	if (m->cfg.init_cmds[0]) {
		char list[sizeof(m->cfg.init_cmds)], *save, *t;

		str_copy(list, sizeof(list), m->cfg.init_cmds);
		for (t = strtok_r(list, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
			while (*t == ' ')
				t++;
			if (*t && init_opt(m, t))
				return -1;
		}
	}
	modem_refresh_sim(m);
	modem_refresh_reg(m);
	return m->at.fd >= 0 ? 0 : -1;
}

/* --- the tick --- */

void modem_tick(struct md *m, int64_t now)
{
	switch (m->state) {
	case MD_CLOSED:
		if (now < m->reopen_at)
			return;
		{
			int fd = open_port(m);

			if (fd == -2) {
				m->reopen_at = now + 10 * m->cfg.reopen_ms;
				return;
			}
			if (fd < 0) {
				m->reopen_at = now + m->cfg.reopen_ms;
				return;
			}
			at_init(m, fd);
			reset_facts(m);
			m->sim_sel = 1;
			m->state = MD_OPEN;
			m->next_ready_check = now;
			m->next_init = now;
			md_log(m, 0, "%s open", m->cfg.port);
		}
		/* fall through */
	case MD_OPEN:
		if (now < m->next_ready_check)
			return;
		m->next_ready_check = now + m->cfg.ready_poll_ms;
		if (!modem_is_ready_src(m))
			return;
		if (now < m->next_init)
			return;
		md_log(m, 0, "modem ready, init (try %d)", m->init_tries + 1);
		if (modem_init(m)) {
			if (m->at.fd < 0)
				return;	/* lost during init */
			m->init_tries++;
			m->next_init = mono_ms() + 2000;
			if (m->init_tries == 5)
				md_log(m, 0, "init keeps failing: the modem does not answer AT");
			return;
		}
		m->state = MD_READY;
		m->init_tries = 0;
		m->next_signal = mono_ms() + m->cfg.signal_poll_ms;
		m->next_ready_check = mono_ms() + m->cfg.ready_poll_ms;
		md_event(m, "modem ready");
		return;
	case MD_READY:
		if (now >= m->next_ready_check) {
			m->next_ready_check = now + m->cfg.ready_poll_ms;
			if (!modem_is_ready_src(m)) {
				modem_lost(m, "state file not ready");
				return;
			}
		}
		if (m->cfg.signal_poll_ms > 0 && now >= m->next_signal) {
			m->next_signal = now + m->cfg.signal_poll_ms;
			modem_refresh_signal(m);
		}
		if (m->sim_retry_at && now >= m->sim_retry_at) {
			m->sim_retry_at = 0;
			modem_refresh_sim(m);
			if (m->state != MD_READY)
				return;
		}
		call_tick(m, now);
		sms_tick(m, now);
		data_tick(m);
		mipc_tick(m, now);
		ims_tick(m, mono_ms());
		return;
	}
}

static int64_t min64(int64_t a, int64_t b)
{
	return a < b ? a : b;
}

int64_t modem_next_deadline(struct md *m)
{
	int64_t dl = INT64_MAX;

	switch (m->state) {
	case MD_CLOSED:
		dl = m->reopen_at;
		break;
	case MD_OPEN:
		dl = min64(m->next_ready_check, m->next_init);
		break;
	case MD_READY:
		dl = m->next_ready_check;
		if (m->cfg.signal_poll_ms > 0)
			dl = min64(dl, m->next_signal);
		dl = min64(dl, call_next_deadline(m));
		dl = min64(dl, mipc_next_deadline(m));
		dl = min64(dl, ims_next_deadline(m));
		if (m->sim_retry_at)
			dl = min64(dl, m->sim_retry_at);
		break;
	}
	return min64(dl, at_next_deadline(m));
}

/* --- commands --- */

int modem_set_radio(struct md *m, int on, const char *mask, char *err, size_t cap)
{
	struct at_resp r;
	char cmd[AT_CMD_MAX];

	if (m->state != MD_READY) {
		snprintf(err, cap, "modem not ready");
		return -1;
	}
	if (!mask || !*mask)
		mask = m->cfg.radio_mask[0] ? m->cfg.radio_mask : "1";
	if (vendor_radio_cmd(m, on, mask, cmd, sizeof(cmd))) {
		snprintf(err, cap, "bad radio mask '%s'", mask);
		return -1;
	}
	at_cmd_ex(m, cmd, NULL, 0, 15000, &r);
	if (r.res != AT_OK) {
		char e[96];

		at_resp_error(&r, e, sizeof(e));
		snprintf(err, cap, "%s: %s", cmd, e);
		return -1;
	}
	m->radio_user_off = !on;	/* radio_on_at_start respects a "radio off" */
	modem_refresh_sim(m);
	return 0;
}

int modem_select_sim(struct md *m, int sim, char *err, size_t cap)
{
	struct at_resp r;
	char cmd[AT_CMD_MAX];

	if (m->state != MD_READY) {
		snprintf(err, cap, "modem not ready");
		return -1;
	}
	if (vendor_stack_cmd(sim, cmd, sizeof(cmd))) {
		snprintf(err, cap, "bad SIM %d", sim);
		return -1;
	}
	if (m->data.up) {
		snprintf(err, cap, "data is up on this stack; run 'data down' first");
		return -1;
	}
	if (m->call.ncalls) {
		snprintf(err, cap, "a call is in progress");
		return -1;
	}
	/* the IMS PDNs belong to the old stack: down, set up again on the new one */
	ims_stack_change(m);
	at_cmd(m, cmd, &r);
	if (r.res != AT_OK) {
		char e[96];

		at_resp_error(&r, e, sizeof(e));
		snprintf(err, cap, "%s: %s", cmd, e);
		return -1;
	}
	m->sim_sel = sim;
	reset_facts(m);
	call_stack_init(m);
	modem_refresh_sim(m);
	modem_refresh_reg(m);
	return 0;
}

int modem_enter_pin(struct md *m, const char *pin, char *err, size_t cap)
{
	struct at_resp r;
	char cmd[AT_CMD_MAX];
	size_t l = strlen(pin);

	if (m->state != MD_READY) {
		snprintf(err, cap, "modem not ready");
		return -1;
	}
	if (l < 4 || l > 8 || strspn(pin, "0123456789") != l) {
		snprintf(err, cap, "a PIN is 4 to 8 digits");
		return -1;
	}
	if (strcmp(m->cpin, "SIM PIN") && strcmp(m->cpin, "SIM PIN2")) {
		/* never feed a PIN where a PUK is asked for: a wrong "PUK"
		 * attempt counts against the SIM */
		snprintf(err, cap, "the SIM does not ask for a PIN (%s)",
			 m->cpin[0] ? m->cpin : "unknown");
		return -1;
	}
	snprintf(cmd, sizeof(cmd), "AT+CPIN=\"%s\"", pin);
	at_cmd_ex(m, cmd, NULL, AT_F_SILENT, 15000, &r);
	if (r.res != AT_OK) {
		char e[96];

		at_resp_error(&r, e, sizeof(e));
		snprintf(err, cap, "AT+CPIN: %s", e);
		return -1;
	}
	modem_refresh_sim(m);
	return 0;
}

static void reg_line(char *out, size_t cap, const char *name, const struct reg *r)
{
	str_append(out, cap, "reg %s: %s", name, reg_stat_str(r->stat));
	if (r->act >= 0)
		str_append(out, cap, " %s", act_str(r->act));
	if (r->lac[0])
		str_append(out, cap, " lac/tac=%s ci=%s", r->lac, r->ci);
	str_append(out, cap, "\n");
}

static void net_lines(struct md *m, char *out, size_t cap)
{
	str_append(out, cap, "operator: %s%s%s\n", m->oper[0] ? m->oper : "-",
		   m->oper_act >= 0 ? " " : "", m->oper_act >= 0 ? act_str(m->oper_act) : "");
	reg_line(out, cap, "cs", &m->cs);
	reg_line(out, cap, "eps", &m->eps);
	reg_line(out, cap, "gprs", &m->gprs);
	str_append(out, cap, "reg ims: %s", m->ims_reg < 0 ? "-" :
		   m->ims_reg ? "registered" : "not-registered");
	if (m->ims.ext > 0) {
		char sv[64];

		ims_services_str(m->ims.ext, sv, sizeof(sv));
		str_append(out, cap, " (%s, ext 0x%x)", sv, m->ims.ext);
	}
	str_append(out, cap, "\n");
	if (m->rssi == 99)
		str_append(out, cap, "signal: unknown\n");
	else
		str_append(out, cap, "signal: %d dBm (rssi %d, ber %d)\n",
			   -113 + 2 * m->rssi, m->rssi, m->ber);
}

void modem_status(struct md *m, char *out, size_t cap)
{
	str_append(out, cap, "modem: %s\n", md_state_str(m->state));
	str_append(out, cap, "port: %s%s\n", m->cfg.port,
		   m->at.fd >= 0 ? " (open)" : " (closed)");
	if (m->model[0])
		str_append(out, cap, "model: %s\n", m->model);
	if (m->revision[0])
		str_append(out, cap, "revision: %s\n", m->revision);
	str_append(out, cap, "radio: %s\n", m->cfun < 0 ? "-" : m->cfun == 1 ? "on (CFUN 1)" :
		   m->cfun == 4 ? "off (CFUN 4)" : m->cfun == 0 ? "minimum (CFUN 0)" : "other");
	str_append(out, cap, "stack: SIM%d\n", m->sim_sel ? m->sim_sel : 1);
	str_append(out, cap, "sim: %s%s%s\n", m->cpin[0] ? cpin_str(m->cpin) : "-",
		   m->cpin[0] ? " " : "", m->cpin);
	net_lines(m, out, cap);
	ims_status(m, out, cap);
	data_status(m, out, cap);
	mipc_status(m, out, cap);
	call_status(m, out, cap);
	str_append(out, cap, "sms: received %lu sent %lu bad %lu\n", m->sms.n_rx,
		   m->sms.n_tx, m->sms.n_bad);
	str_append(out, cap, "at: commands %lu timeouts %lu errors %lu urcs %lu stray %lu queued %u\n",
		   m->at.n_cmds, m->at.n_timeouts, m->at.n_errors, m->at.n_urcs,
		   m->at.n_stray, m->at.qlen);
}

void modem_net(struct md *m, char *out, size_t cap)
{
	if (m->state == MD_READY)
		modem_refresh_reg(m);
	net_lines(m, out, cap);
}

/* AT+COPS=? : one line per network */
int modem_scan(struct md *m, char *out, size_t cap)
{
	struct at_resp r;
	const char *p;

	if (m->state != MD_READY) {
		str_append(out, cap, "error: modem not ready\n");
		return -1;
	}
	at_cmd_ex(m, "AT+COPS=?", NULL, 0, m->cfg.scan_timeout_ms, &r);
	if (r.res != AT_OK) {
		char e[96];

		at_resp_error(&r, e, sizeof(e));
		str_append(out, cap, "error: AT+COPS=?: %s\n", e);
		return -1;
	}
	p = at_find(&r, "+COPS:");
	if (!p) {
		str_append(out, cap, "no networks\n");
		return 0;
	}
	/* (stat,"long","short","numeric",act),(...),,(modes),(formats) */
	while (*p && *p != '\n') {
		const char *e;
		char item[256], *av[8];
		int n, stat, act = -1;

		if (*p != '(') {
			p++;
			continue;
		}
		e = strchr(p, ')');
		if (!e)
			break;
		if ((size_t)(e - p - 1) >= sizeof(item))
			break;
		memcpy(item, p + 1, (size_t)(e - p - 1));
		item[e - p - 1] = 0;
		p = e + 1;
		n = at_split(item, av, 8);
		if (n < 4 || parse_int(av[0], 0, 3, &stat))
			break;	/* the trailing (modes),(formats) lists */
		if (n >= 5)
			parse_int(av[4], 0, 13, &act);
		str_append(out, cap, "%s \"%s\" \"%s\" %s%s%s\n", stat == 0 ? "unknown" :
			   stat == 1 ? "available" : stat == 2 ? "current" : "forbidden",
			   av[1], av[2], av[3], act >= 0 ? " " : "",
			   act >= 0 ? act_str(act) : "");
	}
	return 0;
}

/* SIGTERM: calls ended, data down unless configured otherwise */
void modem_shutdown(struct md *m)
{
	if (m->at.fd < 0)
		return;
	call_shutdown(m);
	data_shutdown(m);
	ims_shutdown(m);
	mipc_close(m, "exit");
	at_close(m);
	m->state = MD_CLOSED;
}
