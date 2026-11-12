// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: everything MediaTek-specific in one place. The facts come from
 * what was observed on the phone with our own driver: AT+EFUN=<mask> powers the radios of
 * SIM1 (1), SIM2 (2) or both (3); AT+ESUO=4/5 selects the SIM1/SIM2 protocol
 * stack on the AT port; +EIND is an indication at start-up. Everything else
 * in the daemon is 3GPP. Unknown vendor facts are questions in QUESTIONS.md
 * and configurable hooks here.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modemd.h"

/* radio on: mask 1 = SIM1 (3GPP AT+CFUN=1 does the same, observed), 2 =
 * SIM2, 3 = both (AT+EFUN=3). Radio off: AT+CFUN=4 (3GPP flight mode; the
 * modem starts in that state). AT+EFUN=0 has not been tried: QUESTIONS.md. */
int vendor_radio_cmd(struct md *m, int on, const char *mask, char *cmd, size_t cap)
{
	int v;

	(void)m;
	if (!on) {
		snprintf(cmd, cap, "AT+CFUN=4");
		return 0;
	}
	if (parse_int(mask, 1, 3, &v))
		return -1;
	if (v == 1)
		snprintf(cmd, cap, "AT+CFUN=1");
	else
		snprintf(cmd, cap, "AT+EFUN=%d", v);
	return 0;
}

/* protocol stack served by this AT port: one at a time */
int vendor_stack_cmd(int sim, char *cmd, size_t cap)
{
	if (sim == 1)
		snprintf(cmd, cap, "AT+ESUO=4");
	else if (sim == 2)
		snprintf(cmd, cap, "AT+ESUO=5");
	else
		return -1;
	return 0;
}

/*
 * PDN -> data interface bind hook (AT backend).
 *
 * The command that makes the modem carry a PDN's packets on data interface
 * N (wwan0-N) is NOT known on the clean side. The factory software does this
 * over MIPC (see mipc.c, data_backend=mipc). Until the protocol description arrives, the
 * configuration key data_bind_cmds runs AT templates with {cid}, {linkid}
 * and {apn} substituted (data.c). To implement it in code instead, fill in
 * this function and return 1 after writing the result into status.
 *
 * Return: 0 = nothing done here (use the configured templates), 1 = done.
 */
int vendor_data_bind(struct md *m, int cid, int linkid, const char *apn,
		     char *status, size_t cap)
{
	(void)m;
	(void)cid;
	(void)linkid;
	(void)apn;
	(void)status;
	(void)cap;
	/* TODO (fact pending): the MTK PDN bind command */
	return 0;
}

/* vendor URCs: logged and forwarded as events; returns 1 when consumed.
 * IMS: +EIMSPDN, +EIMSPDNOOS, +EIMS, +EIMCFLAG go to
 * ims.c (state, events, the AT notify fallback); +EIMSPDIS (P-CSCF discovery,
 * step 6, answer unverified) is forwarded as an "ims ..." event only. */
int vendor_urc(struct md *m, const char *line)
{
	if (ims_urc(m, line))
		return 1;
	if (!strncmp(line, "+EIND:", 6)) {
		md_log(m, 1, "vendor indication %s", line);
		md_event(m, "urc %s", line);
		return 1;
	}
	if (!strncmp(line, "+EFUN:", 6) || !strncmp(line, "+ESUO:", 6)) {
		md_event(m, "urc %s", line);
		return 1;
	}
	if (!strncmp(line, "+EIMSPDN:", 9) || !strncmp(line, "+EIMSPDIS:", 10) ||
	    !strncmp(line, "+EIMS:", 6) || !strncmp(line, "+EIMCFLAG:", 10) ||
	    !strncmp(line, "+EIMSPDNOOS:", 12)) {
		md_log(m, 0, "ims: %s (forwarded, not handled)", line);
		md_event(m, "ims %s", line);
		return 1;
	}
	return 0;
}

/* --- voice (the formats are the modem's) --- */

/* +ECPI: <call_id>,<msg_type>,<is_ibt>,<is_tch>,<dir>,<call_mode>,"<number>",<type>,"<pau>"[,<cause>] */
int vendor_parse_ecpi(const char *args, struct ecpi *e)
{
	char buf[AT_LINE_MAX], *av[12];
	int n;

	memset(e, 0, sizeof(*e));
	e->cause = -1;
	e->type = -1;
	str_copy(buf, sizeof(buf), args);
	n = at_split(buf, av, 12);
	if (n < 2 || parse_int(av[0], 0, 255, &e->call_id) ||
	    parse_int(av[1], 0, 255, &e->msg_type))
		return -1;
	if (n > 2)
		parse_int(av[2], 0, 9, &e->is_ibt);
	if (n > 3)
		parse_int(av[3], 0, 9, &e->is_tch);
	if (n > 4)
		parse_int(av[4], 0, 9, &e->dir);
	if (n > 5)
		parse_int(av[5], 0, 255, &e->call_mode);
	if (n > 6) {
		str_copy(e->number, sizeof(e->number), av[6]);
		str_sanitize(e->number);
	}
	if (n > 7)
		parse_int(av[7], 0, 255, &e->type);
	if (n > 9)
		parse_int(av[9], 0, 65535, &e->cause);
	return 0;
}

/* +EAIC: <call_id>,"<number>",<type>,<call_mode>,<seq_no>[,"<redirect>","<pau>",<rq>] */
int vendor_parse_eaic(const char *args, struct eaic *e)
{
	char buf[AT_LINE_MAX], *av[10];
	int n;

	memset(e, 0, sizeof(*e));
	str_copy(buf, sizeof(buf), args);
	n = at_split(buf, av, 10);
	if (n < 5 || parse_int(av[0], 0, 255, &e->call_id) || parse_int(av[2], 0, 255, &e->type) ||
	    parse_int(av[3], 0, 255, &e->call_mode) || parse_int(av[4], 0, 65535, &e->seq))
		return -1;
	str_copy(e->number, sizeof(e->number), av[1]);
	str_sanitize(e->number);
	return 0;
}

/* +ESPEECH: <speech_on>,<x>,<y>: only the first field is used */
int vendor_parse_espeech(const char *args)
{
	int v;

	if (parse_int(args, 0, 1, &v))
		return -1;
	return v;
}

/* AT+EAIC=<mode>,<call_id>,<seq>[,<cause>]: 0 accept, 1 reject with 21 */
void vendor_eaic_cmd(int accept, int call_id, int seq, char *cmd, size_t cap)
{
	if (accept)
		snprintf(cmd, cap, "AT+EAIC=0,%d,%d", call_id, seq);
	else
		snprintf(cmd, cap, "AT+EAIC=1,%d,%d,21", call_id, seq);
}

/* ATD<clir><number>; / ATDE<number>;: clir 1 invoke (#31#), 2 suppress (*31#) */
void vendor_dial_cmd(const char *number, int clir, int emergency, char *cmd, size_t cap)
{
	snprintf(cmd, cap, "ATD%s%s%s;", emergency ? "E" : "",
		 clir == 1 ? "#31#" : clir == 2 ? "*31#" : "", number);
}

/* hang up one call: AT+CHLD=1<id>; all: AT+CHLD=6 */
void vendor_hangup_cmd(int call_id, char *cmd, size_t cap)
{
	if (call_id > 0)
		snprintf(cmd, cap, "AT+CHLD=1%d", call_id);
	else
		snprintf(cmd, cap, "AT+CHLD=6");
}

int vendor_ecpi_is_ims(int call_mode)
{
	return call_mode >= 20 && call_mode <= 25;
}

/* --- IMS --- */

/* +EIMSPDN: "init" | "notify",<aid>,<action>,"ims"|"emergency" |
 * "reject",<cause>,"ims"; returns -1 when the line is not understood */
int vendor_parse_eimspdn(const char *args, struct eimspdn *e)
{
	char buf[AT_LINE_MAX], *av[6];
	int n;

	memset(e, 0, sizeof(*e));
	e->aid = e->action = e->cause = -1;
	str_copy(buf, sizeof(buf), args);
	n = at_split(buf, av, 6);
	if (n < 1 || !av[0][0])
		return -1;
	str_copy(e->kind, sizeof(e->kind), av[0]);
	str_sanitize(e->kind);
	if (!strcmp(av[0], "init"))
		return 0;
	if (!strcmp(av[0], "notify")) {
		if (n < 3 || parse_int(av[1], 0, 255, &e->aid) || parse_int(av[2], 0, 255, &e->action))
			return -1;
		str_copy(e->type, sizeof(e->type), n > 3 ? av[3] : "ims");
		str_sanitize(e->type);
		return 0;
	}
	if (!strcmp(av[0], "reject")) {
		if (n > 1)
			parse_int(av[1], 0, 65535, &e->cause);
		str_copy(e->type, sizeof(e->type), n > 2 ? av[2] : "");
		str_sanitize(e->type);
		return 0;
	}
	str_copy(e->kind, sizeof(e->kind), "other");
	return 0;
}

/* AT+EIMSPDN="onoff",<0|1>: the modem raises the IMS PDN / reports it. The
 * factory RIL sent AT+EIMSPDN= "onoff",1 (space after '=', observed through the
 * AT tunnel); the plain form is the 27.007 style
 * (equivalence unverified) */
void vendor_ims_onoff_cmd(int on, int observed_form, char *cmd, size_t cap)
{
	if (observed_form)
		snprintf(cmd, cap, "AT+EIMSPDN= \"onoff\",%d", on ? 1 : 0);
	else
		snprintf(cmd, cap, "AT+EIMSPDN=\"onoff\",%d", on ? 1 : 0);
}

/* AT+EIMSPDN="confirm",<aid>,<result>: 0 = success; only the AT fallback
 * for a +EIMSPDN "notify" uses it (unverified: never observed) */
void vendor_ims_confirm_cmd(int aid, int result, char *cmd, size_t cap)
{
	snprintf(cmd, cap, "AT+EIMSPDN=\"confirm\",%d,%d", aid, result);
}

/* AT+EIMSCFG=<volte>,<vilte>,<vowifi>,<viwifi>,<sms>,<eims> */
void vendor_ims_cfg_cmd(const char *cfg, char *cmd, size_t cap)
{
	snprintf(cmd, cap, "AT+EIMSCFG=%s", cfg);
}

/* AT+EIMS=<0|1>: IMS stack of this SIM off/on */
void vendor_ims_eims_cmd(int on, char *cmd, size_t cap)
{
	snprintf(cmd, cap, "AT+EIMS=%d", on ? 1 : 0);
}

/* extra vendor commands at init: none for now (the service t2-modem sends
 * AT+EFUN; radio_mask in the config does it here if wanted) */
void vendor_init_cmds(struct md *m)
{
	(void)m;
}

/*
 * Raw AT passthrough filter. The daemon must never change IMEI, NV or
 * calibration data, SIM or carrier lock state or factory modes. The default
 * deny list (config raw_at_deny) holds the 3GPP commands that touch locks,
 * PINs and raw SIM access; vendor commands that write NV/IMEI are not known
 * here (QUESTIONS.md) — add them to raw_at_deny when they are.
 */
int vendor_raw_allowed(struct md *m, const char *cmd, char *why, size_t cap)
{
	char list[sizeof(m->cfg.raw_at_deny)], *save, *t;
	const char *c = cmd;

	if (strncasecmp(c, "AT", 2)) {
		snprintf(why, cap, "not an AT command");
		return 0;
	}
	c += 2;
	str_copy(list, sizeof(list), m->cfg.raw_at_deny);
	for (t = strtok_r(list, ";, ", &save); t; t = strtok_r(NULL, ";, ", &save)) {
		size_t l = strlen(t);

		if (!l)
			continue;
		if (!strncasecmp(c, t, l) && (c[l] == 0 || c[l] == '=' || c[l] == '?')) {
			snprintf(why, cap, "AT%s is on the deny list (locks, PIN, SIM access, NV)", t);
			return 0;
		}
	}
	return 1;
}
