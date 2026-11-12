// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: mobile data. Two backends activate the PDN (data_backend):
 *
 *  "mipc" (default): DATA_ACT_CALL_REQ over the MIPC port; the data
 *          interface index comes from transIntfId mod 100 in the confirm / the
 *          INTERNAL_EIF_IND; nothing is confirmed back to the modem.
 *  "at":   AT command templates from data_bind_cmds (default: the MediaTek
 *          +EAPNACT / +EPDN / +EIF family, with {apn},
 *          {aid}, {tid}, {linkid} substituted and the aid/tid captured from
 *          the responses; "@linkup" marks where the link is created). With an
 *          empty data_bind_cmds the plain 3GPP route (+CGDCONT/+CGACT) is
 *          used and the LTE default bearer (cid 0) is reused.
 *
 * Everything after activation is shared: the WWAN link (ip link add dev
 * wwan0-N parentdev wwan0 type wwan linkid N), the /32 address, the default
 * route with a configurable metric, the DNS file; teardown on request, on
 * +CGEV/indications, on modem loss and at exit.
 *
 * An INTERNAL_EIF_IND for an interface we did not request is a PDN the
 * modem raised by itself (the IMS PDN): it goes to
 * ims.c, which creates its link with data_pdn_link() into its own struct
 * data_state: address only, no default route, no DNS; m->data, the internet
 * PDN, is never touched by it.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modemd.h"

#define IP_ARGS 16

static int ip(struct md *m, const char *a1, const char *a2, const char *a3,
	      const char *a4, const char *a5, const char *a6, const char *a7,
	      const char *a8, const char *a9, const char *a10, const char *a11)
{
	const char *argv[IP_ARGS] = { m->cfg.ip_cmd, a1, a2, a3, a4, a5, a6, a7, a8,
				      a9, a10, a11, NULL };
	int n = 0;

	while (argv[n])
		n++;
	argv[n] = NULL;
	return run_argv(m, (char *const *)argv);
}

#define IP4(m, a, b, c, d)		ip(m, a, b, c, d, NULL, NULL, NULL, NULL, NULL, NULL, NULL)
#define IP6(m, a, b, c, d, e, f)	ip(m, a, b, c, d, e, f, NULL, NULL, NULL, NULL, NULL)
#define IP8(m, a, b, c, d, e, f, g, h)	ip(m, a, b, c, d, e, f, g, h, NULL, NULL, NULL)

static int link_exists(const char *ifname)
{
	char path[128];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/net/%s", ifname);
	return !stat(path, &st);
}

/* --- address formats --- */

static int is_digits_dots(const char *s)
{
	return *s && strspn(s, "0123456789.") == strlen(s);
}

static int count_dots(const char *s)
{
	int n = 0;

	for (; *s; s++)
		n += *s == '.';
	return n;
}

static void bin_to_v6(const uint8_t *v, char *out, size_t cap)
{
	snprintf(out, cap, "%x:%x:%x:%x:%x:%x:%x:%x", v[0] << 8 | v[1], v[2] << 8 | v[3],
		 v[4] << 8 | v[5], v[6] << 8 | v[7], v[8] << 8 | v[9], v[10] << 8 | v[11],
		 v[12] << 8 | v[13], v[14] << 8 | v[15]);
}

/* 16 dot-separated decimal octets -> colon-hex IPv6 */
static int dotted16_to_v6(const char *s, char *out, size_t cap)
{
	uint8_t v[16];
	int n = 0;
	const char *p = s;

	while (n < 16) {
		char *e;
		unsigned long x = strtoul(p, &e, 10);

		if (e == p || x > 255)
			return -1;
		v[n++] = (uint8_t)x;
		if (*e == 0)
			break;
		if (*e != '.')
			return -1;
		p = e + 1;
	}
	if (n != 16)
		return -1;
	bin_to_v6(v, out, cap);
	return 0;
}

static int valid_v4(const char *s)
{
	unsigned a, b, c, d;
	char tail;

	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
		return 0;
	return a < 256 && b < 256 && c < 256 && d < 256;
}

static int valid_v6(const char *s)
{
	size_t l = strlen(s);

	return l >= 2 && l < 46 && strspn(s, "0123456789abcdefABCDEF:.") == l &&
	       strchr(s, ':');
}

/* store an address string from the modem as v4 or v6 (27.007 forms:
 * "a.b.c.d", "a.b.c.d.m.m.m.m", 16 or 32 dotted decimals, colon-hex, or
 * the +EIF "XX:XX:...:XX" byte form) */
static void take_addr(struct data_state *d, const char *s)
{
	char v6[48];
	int dots;

	if (!s || !*s || !strcmp(s, "0.0.0.0"))
		return;
	if (strchr(s, ':')) {
		if (valid_v6(s) && !d->v6[0])
			str_copy(d->v6, sizeof(d->v6), s);
		return;
	}
	if (!is_digits_dots(s))
		return;
	dots = count_dots(s);
	if (dots == 3) {
		if (valid_v4(s) && !d->v4[0])
			str_copy(d->v4, sizeof(d->v4), s);
	} else if (dots == 7 || dots == 31) {
		/* address + subnet mask: the first half */
		char a[128];
		const char *p = s;
		int half = dots == 7 ? 4 : 16;

		for (int i = 0; i < half && p; i++)
			p = strchr(p + 1, '.');
		if (!p || (size_t)(p - s) >= sizeof(a))
			return;
		memcpy(a, s, (size_t)(p - s));
		a[p - s] = 0;
		if (half == 4) {
			if (valid_v4(a) && !d->v4[0])
				str_copy(d->v4, sizeof(d->v4), a);
		} else if (!dotted16_to_v6(a, v6, sizeof(v6)) && !d->v6[0]) {
			str_copy(d->v6, sizeof(d->v6), v6);
		}
	} else if (dots == 15) {
		if (!dotted16_to_v6(s, v6, sizeof(v6)) && !d->v6[0])
			str_copy(d->v6, sizeof(d->v6), v6);
	}
}

static void take_dns(struct data_state *d, const char *s)
{
	char v6[48];

	if (!s || !*s || !strcmp(s, "0.0.0.0"))
		return;
	if (is_digits_dots(s) && count_dots(s) == 15) {
		if (dotted16_to_v6(s, v6, sizeof(v6)))
			return;
		s = v6;
	} else if (!(valid_v4(s) || valid_v6(s)))
		return;
	for (int i = 0; i < 2; i++) {
		if (!strcmp(d->dns[i], s))
			return;
		if (!d->dns[i][0]) {
			str_copy(d->dns[i], sizeof(d->dns[i]), s);
			return;
		}
	}
}

static void take_bin_addr(struct data_state *d, const uint8_t *v, size_t len, int dns)
{
	char s[48];

	if (len == 4)
		snprintf(s, sizeof(s), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
	else if (len == 16)
		bin_to_v6(v, s, sizeof(s));
	else
		return;
	if (dns)
		take_dns(d, s);
	else
		take_addr(d, s);
}

/* --- the lines the modem sends about data (responses and URCs) --- */

/* +CGCONTRDP: <cid>,<bearer>,<apn>[,<addr+mask>[,<gw>[,<dns1>[,<dns2>...]]]] */
static void parse_cgcontrdp(struct md *m, const char *args, int want_cid)
{
	struct data_state *d = &m->data;
	char buf[AT_LINE_MAX], *av[12];
	int na, c;

	str_copy(buf, sizeof(buf), args);
	na = at_split(buf, av, 12);
	if (na < 3 || parse_int(av[0], 0, 99, &c) || (want_cid >= 0 && c != want_cid))
		return;
	if (!d->apn[0])
		str_copy(d->apn, sizeof(d->apn), av[2]);
	if (na > 3)
		take_addr(d, av[3]);
	if (na > 4 && !d->gw[0] && (valid_v4(av[4]) || valid_v6(av[4])) && strcmp(av[4], "0.0.0.0"))
		str_copy(d->gw, sizeof(d->gw), av[4]);
	if (na > 5)
		take_dns(d, av[5]);
	if (na > 6)
		take_dns(d, av[6]);
}

/* +CGPADDR: <cid>,"<addr1>"[,"<addr2>"] */
static void parse_cgpaddr(struct md *m, const char *args, int want_cid)
{
	struct data_state *d = &m->data;
	char buf[AT_LINE_MAX], *av[4];
	int na, c;

	str_copy(buf, sizeof(buf), args);
	na = at_split(buf, av, 4);
	if (na < 2 || parse_int(av[0], 0, 99, &c) || (want_cid >= 0 && c != want_cid))
		return;
	take_addr(d, av[1]);
	if (na > 2)
		take_addr(d, av[2]);
}

/* +EPDN: <aid>,"new",<rat>,<tid>[,<mtu>] | "update",<tid>,<type>,"<a>"[,"<a2>"] |
 * "addr"[,<type>,"<a>"] | "mtu",<type>,<mtu> | "err"[,<cause>] | "deact_info",... */
static void parse_epdn(struct md *m, const char *args)
{
	struct data_state *d = &m->data;
	char buf[AT_LINE_MAX], *av[8];
	int na, aid, v;

	str_copy(buf, sizeof(buf), args);
	na = at_split(buf, av, 8);
	if (na < 2 || parse_int(av[0], 0, 99, &aid))
		return;
	if (d->aid >= 0 && aid != d->aid)
		return;
	if (!strcmp(av[1], "new") && na >= 4) {
		if (!parse_int(av[3], 0, 100000, &v)) {
			d->tid = v;
			d->linkid = v % MIPC_TRANS_INTF_MOD;
		}
		if (na >= 5 && !parse_int(av[4], 68, 9000, &v))
			d->mtu = v;
	} else if (!strcmp(av[1], "update") && na >= 5) {
		if (!parse_int(av[2], 0, 100000, &v)) {
			d->tid = v;
			d->linkid = v % MIPC_TRANS_INTF_MOD;
		}
		take_addr(d, av[4]);
		if (na >= 6)
			take_addr(d, av[5]);
	} else if (!strcmp(av[1], "addr") && na >= 4) {
		take_addr(d, av[3]);
	} else if (!strcmp(av[1], "mtu") && na >= 4) {
		if (!parse_int(av[3], 68, 9000, &v) && !strcmp(av[2], "1"))
			d->mtu = v;
	} else if (!strcmp(av[1], "err")) {
		snprintf(d->bind_status, sizeof(d->bind_status), "+EPDN err%s%s", na > 2 ? " " : "",
			 na > 2 ? av[2] : "");
		d->epdn_err = 1;
	}
}

/* +EIF: <tid>,ifup,<addrType hex>,<v4>[,<v6>],<reason> | <tid>,ifdown,<reason> |
 * ifchg/ipchg,<addrType>,<addr...> */
static void parse_eif(struct md *m, const char *args)
{
	struct data_state *d = &m->data;
	char buf[AT_LINE_MAX], *av[8];
	int na, tid;
	unsigned long type;

	str_copy(buf, sizeof(buf), args);
	na = at_split(buf, av, 8);
	if (na < 2 || parse_int(av[0], 0, 100000, &tid))
		return;
	if (!strcmp(av[1], "ifup") || !strcmp(av[1], "ifchg") || !strcmp(av[1], "ipchg")) {
		if (na < 4)
			return;
		type = strtoul(av[2], NULL, 16);
		d->eif_tid = tid;
		d->eif_v4[0] = d->eif_v6[0] = 0;
		if (type & 1) {
			if (valid_v4(av[3]))
				str_copy(d->eif_v4, sizeof(d->eif_v4), av[3]);
			if ((type & 2) && na > 4 && valid_v6(av[4]))
				str_copy(d->eif_v6, sizeof(d->eif_v6), av[4]);
		} else if (type & 2) {
			if (valid_v6(av[3]))
				str_copy(d->eif_v6, sizeof(d->eif_v6), av[3]);
		}
		md_event(m, "data if %d %s %s %s", tid, av[1], d->eif_v4[0] ? d->eif_v4 : "-",
			 d->eif_v6[0] ? d->eif_v6 : "-");
		/* +EIF: <tid>, ifup, <type>, <ipv4>, <x> (observed once, AT port and
		 * tunnel): the address of a PDN the modem raised by itself */
		if (!(d->up && tid == d->tid))
			ims_eif_urc(m, tid, d->eif_v4, d->eif_v6);
		if (d->up && tid == d->tid && !d->via_mipc && strcmp(av[1], "ifup"))
			md_log(m, 0, "data: %s on %s: address change not applied (manual 'data down'/'up')",
			       av[1], d->ifname);
	} else if (!strcmp(av[1], "ifdown")) {
		md_event(m, "data if %d ifdown", tid);
		if (d->up && tid == d->tid)
			d->remote_down = 1;
	}
}

static void link_down(struct md *m, struct data_state *d);
static void clear_state(struct data_state *d);

/* +CGEV: ME PDN ACT <aid>[,<reason>[,<other>]] / NW PDN DEACT <aid> / ... */
static void parse_cgev(struct md *m, const char *p)
{
	struct data_state *d = &m->data;
	int cid = -1;
	const char *q;

	while (*p == ' ')
		p++;
	/* the first number after the words */
	q = p;
	while (*q && !(*q >= '0' && *q <= '9'))
		q++;
	if (*q)
		parse_int(q, 0, 99, &cid);
	md_event(m, "data pdn %s", p);
	if (strstr(p, "PDN ACT") && cid >= 0)
		d->last_cgev_cid = cid;
	if (strstr(p, "DETACH") || (strstr(p, "DEACT") && cid == d->aid && d->up)) {
		if (d->up) {
			md_log(m, 0, "data: %s: taking the link down", p);
			link_down(m, d);
			md_event(m, "data down %s", p);
			clear_state(d);
		}
	}
}

/* a response line of the AT flow */
static void parse_data_line(struct md *m, const char *line, int want_cid)
{
	if (!strncmp(line, "+CGCONTRDP:", 11))
		parse_cgcontrdp(m, line + 11, want_cid);
	else if (!strncmp(line, "+CGPADDR:", 9))
		parse_cgpaddr(m, line + 9, want_cid);
	else if (!strncmp(line, "+EPDN:", 6))
		parse_epdn(m, line + 6);
	else if (!strncmp(line, "+EIF:", 5))
		parse_eif(m, line + 5);
	else if (!strncmp(line, "+CGEV:", 6))
		parse_cgev(m, line + 6);
}

/* URCs: +CGEV, +EPDN, +EIF */
void data_urc(struct md *m, const char *line)
{
	parse_data_line(m, line, -1);
}

/* --- the 3GPP route (empty data_bind_cmds) --- */

struct ctx_info {
	int cid;
	char type[16];
	char apn[101];
	char addr[64];
};

/* +CGDCONT: <cid>,"<type>","<apn>","<addr>",... */
static int list_contexts(struct md *m, struct ctx_info *out, int max)
{
	struct at_resp r;
	const char *q;
	char line[AT_LINE_MAX];
	int n = 0;

	if (at_cmd(m, "AT+CGDCONT?", &r) != AT_OK)
		return -1;
	q = r.buf;
	while (n < max && at_next_line(&q, line, sizeof(line))) {
		char *av[8];
		int na, cid;

		if (strncmp(line, "+CGDCONT:", 9))
			continue;
		na = at_split(line + 9, av, 8);
		if (na < 3 || parse_int(av[0], 0, 99, &cid))
			continue;
		out[n].cid = cid;
		str_copy(out[n].type, sizeof(out[n].type), av[1]);
		str_copy(out[n].apn, sizeof(out[n].apn), av[2]);
		str_copy(out[n].addr, sizeof(out[n].addr), na > 3 ? av[3] : "");
		n++;
	}
	return n;
}

static int lowest_cid(const struct ctx_info *c, int n)
{
	int best = -1;

	for (int i = 0; i < n; i++)
		if (best < 0 || c[i].cid < best)
			best = c[i].cid;
	return best;
}

/* +CGACT: <cid>,<state>; 1 active, 0 inactive, -1 unknown (observed empty) */
static int context_active(struct md *m, int cid)
{
	struct at_resp r;
	const char *q;
	char line[AT_LINE_MAX];

	if (at_cmd(m, "AT+CGACT?", &r) != AT_OK)
		return -1;
	q = r.buf;
	while (at_next_line(&q, line, sizeof(line))) {
		char *av[4];
		int c, st;

		if (strncmp(line, "+CGACT:", 7))
			continue;
		if (at_split(line + 7, av, 4) < 2 || parse_int(av[0], 0, 99, &c) ||
		    parse_int(av[1], 0, 1, &st))
			continue;
		if (c == cid)
			return st;
	}
	return -1;
}

static int run_and_parse(struct md *m, const char *cmd, int timeout, int want_cid,
			 struct at_resp *r)
{
	const char *q;
	char line[AT_LINE_MAX];

	at_cmd_ex(m, cmd, NULL, 0, timeout, r);
	if (r->res != AT_OK)
		return -1;
	q = r->buf;
	while (at_next_line(&q, line, sizeof(line)))
		parse_data_line(m, line, want_cid);
	return 0;
}

static int read_dynamic(struct md *m, int cid)
{
	struct at_resp r;
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+CGCONTRDP=%d", cid);
	if (run_and_parse(m, cmd, 0, cid, &r))
		return -1;
	return at_find(&r, "+CGCONTRDP:") ? 0 : -1;
}

static void read_addresses(struct md *m, int cid)
{
	struct at_resp r;
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+CGPADDR=%d", cid);
	run_and_parse(m, cmd, 0, cid, &r);
}

static int first_free_cid(const struct ctx_info *c, int n)
{
	for (int cid = 1; cid < 16; cid++) {
		int used = 0;

		for (int i = 0; i < n; i++)
			used |= c[i].cid == cid;
		if (!used)
			return cid;
	}
	return -1;
}

static int activate_3gpp(struct md *m, const char *apn, int cid, struct ctx_info *ctx, int n,
			 char *out, size_t cap)
{
	struct data_state *d = &m->data;
	struct at_resp r;
	char cmd[AT_CMD_MAX];
	int found = -1, act;

	if (cid >= 0) {
		for (int i = 0; i < n; i++)
			if (ctx[i].cid == cid)
				found = i;
	} else if (apn) {
		for (int i = 0; i < n; i++)
			if (!strcasecmp(ctx[i].apn, apn))
				found = i;
	} else if (n > 0) {
		for (int i = 0; i < n; i++)
			if (ctx[i].cid == lowest_cid(ctx, n))
				found = i;
	}
	if (found >= 0) {
		cid = ctx[found].cid;
		str_copy(d->apn, sizeof(d->apn), ctx[found].apn);
		if (apn && cid > 0 && strcasecmp(ctx[found].apn, apn)) {
			snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"%s\",\"%s\"", cid,
				 m->cfg.data_ip_type, apn);
			if (at_cmd(m, cmd, &r) != AT_OK) {
				str_append(out, cap, "error: %s: %s\n", cmd, at_result_str(r.res));
				return -1;
			}
			str_copy(d->apn, sizeof(d->apn), apn);
		}
	} else {
		if (!apn) {
			str_append(out, cap, "error: no context and no APN given\n");
			return -1;
		}
		if (cid < 0)
			cid = first_free_cid(ctx, n);
		if (cid < 0) {
			str_append(out, cap, "error: no free cid\n");
			return -1;
		}
		snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"%s\",\"%s\"", cid,
			 m->cfg.data_ip_type, apn);
		if (at_cmd(m, cmd, &r) != AT_OK) {
			char e[96];

			at_resp_error(&r, e, sizeof(e));
			str_append(out, cap, "error: %s: %s\n", cmd, e);
			return -1;
		}
		str_copy(d->apn, sizeof(d->apn), apn);
		str_append(out, cap, "context %d defined: %s \"%s\"\n", cid, m->cfg.data_ip_type, apn);
	}
	d->aid = cid;
	act = context_active(m, cid);
	if (act != 1 && read_dynamic(m, cid) == 0)
		act = 1;	/* +CGCONTRDP answers only for active contexts */
	if (act != 1) {
		snprintf(cmd, sizeof(cmd), "AT+CGACT=1,%d", cid);
		at_cmd_ex(m, cmd, NULL, 0, 60000, &r);
		if (r.res != AT_OK) {
			char e[96];

			at_resp_error(&r, e, sizeof(e));
			str_append(out, cap, "error: %s: %s\n", cmd, e);
			return -1;
		}
		d->activated_by_us = 1;
		str_append(out, cap, "context %d activated (AT+CGACT)\n", cid);
		read_dynamic(m, cid);
	} else {
		str_append(out, cap, "context %d is active\n", cid);
	}
	read_addresses(m, cid);
	snprintf(d->bind_status, sizeof(d->bind_status), "3GPP route, no bind command");
	return 0;
}

/* --- the template flow (data_bind_cmds / data_deact_cmds) --- */

static void subst(struct md *m, char *out, size_t cap, const char *tpl)
{
	struct data_state *d = &m->data;
	size_t o = 0;

	out[0] = 0;
	while (*tpl && o + 1 < cap) {
		const char *rep = NULL;
		char num[16];

		if (!strncmp(tpl, "{cid}", 5) || !strncmp(tpl, "{aid}", 5)) {
			snprintf(num, sizeof(num), "%d", d->aid);
			rep = num;
			tpl += 5;
		} else if (!strncmp(tpl, "{linkid}", 8)) {
			snprintf(num, sizeof(num), "%d", d->linkid);
			rep = num;
			tpl += 8;
		} else if (!strncmp(tpl, "{tid}", 5)) {
			snprintf(num, sizeof(num), "%d", d->tid);
			rep = num;
			tpl += 5;
		} else if (!strncmp(tpl, "{apn}", 5)) {
			rep = d->apn;
			tpl += 5;
		}
		if (rep) {
			size_t l = strlen(rep);

			if (o + l + 1 > cap)
				break;
			memcpy(out + o, rep, l);
			o += l;
		} else {
			out[o++] = *tpl++;
		}
	}
	out[o] = 0;
}

/* run the templates of list from index *idx up to "@linkup" (phase 0) or to
 * the end (phase 1). Returns -1 on a failed command. */
static int run_templates(struct md *m, const char *list_cfg, int *idx, int stop_at_linkup,
			 char *out, size_t cap)
{
	struct data_state *d = &m->data;
	char list[1024], *save, *t;
	int i = 0, failed = 0;

	str_copy(list, sizeof(list), list_cfg);
	for (t = strtok_r(list, ";", &save); t; t = strtok_r(NULL, ";", &save), i++) {
		char cmd[AT_CMD_MAX];
		struct at_resp r;
		int had_aid = d->aid;

		while (*t == ' ')
			t++;
		if (i < *idx || !*t)
			continue;
		if (!strcmp(t, "@linkup")) {
			*idx = i + 1;
			if (stop_at_linkup)
				return 0;
			continue;
		}
		if (strstr(t, "{aid}") && d->aid < 0) {
			str_append(out, cap, "error: %s: no aid known yet\n", t);
			return -1;
		}
		if (strstr(t, "{tid}") && d->tid < 0) {
			str_append(out, cap, "error: %s: no transIntfId known yet\n", t);
			return -1;
		}
		subst(m, cmd, sizeof(cmd), t);
		d->last_cgev_cid = -1;
		d->epdn_err = 0;
		if (run_and_parse(m, cmd, 60000, -1, &r)) {
			char e[96];

			at_resp_error(&r, e, sizeof(e));
			str_append(out, cap, "%s -> %s\n", cmd, e);
			failed++;
			if (r.res == AT_LOST)
				return -1;
			return -1;
		}
		if (had_aid < 0 && d->last_cgev_cid >= 0)
			d->aid = d->last_cgev_cid;	/* +CGEV: ME PDN ACT <aid> */
		str_append(out, cap, "%s -> OK%s%s", cmd, r.nlines ? "\n" : "", r.nlines ? r.buf : "\n");
		if (d->epdn_err) {
			str_append(out, cap, "error: %s\n", d->bind_status);
			return -1;
		}
		*idx = i + 1;
	}
	*idx = i;
	return failed ? -1 : 0;
}

static int activate_at(struct md *m, const char *apn, int cid, char *out, size_t cap)
{
	struct data_state *d = &m->data;
	struct ctx_info ctx[16];
	int n;

	n = list_contexts(m, ctx, 16);
	if (n < 0) {
		str_append(out, cap, "error: AT+CGDCONT? failed\n");
		return -1;
	}
	for (int i = 0; i < n; i++)
		str_append(out, cap, "context %d: %s \"%s\"%s%s\n", ctx[i].cid, ctx[i].type,
			   ctx[i].apn, ctx[i].addr[0] ? " " : "", ctx[i].addr);
	if (!m->cfg.data_bind_cmds[0])
		return activate_3gpp(m, apn, cid, ctx, n, out, cap);

	/* the MediaTek flow: the APN is needed, the aid comes from +CGEV */
	if (!apn) {
		int low = lowest_cid(ctx, n);

		for (int i = 0; i < n; i++)
			if (ctx[i].cid == low && ctx[i].apn[0])
				apn = ctx[i].apn;
		if (!apn) {
			str_append(out, cap, "error: no APN (argument, data_apn or attach context)\n");
			return -1;
		}
		str_append(out, cap, "apn from the attach context: %s\n", apn);
	}
	str_copy(d->apn, sizeof(d->apn), apn);
	d->aid = cid;		/* -1: taken from +CGEV: ME PDN ACT */
	d->tpl_idx = 0;
	if (run_templates(m, m->cfg.data_bind_cmds, &d->tpl_idx, 1, out, cap)) {
		snprintf(d->bind_status, sizeof(d->bind_status), "AT activation failed");
		return -1;
	}
	if (d->aid < 0) {
		d->aid = lowest_cid(ctx, n);
		str_append(out, cap, "warning: no +CGEV: ME PDN ACT seen, assuming aid %d\n", d->aid);
	}
	if (d->tid < 0)
		str_append(out, cap, "warning: no +EPDN \"new\" transIntfId, using data_linkid %d\n",
			   d->linkid);
	/* addresses from +EIF if the responses gave none */
	if (!d->v4[0] && !d->v6[0] && d->eif_tid >= 0 && (d->tid < 0 || d->eif_tid == d->tid)) {
		take_addr(d, d->eif_v4);
		take_addr(d, d->eif_v6);
		if (d->tid < 0) {
			d->tid = d->eif_tid;
			d->linkid = d->tid % MIPC_TRANS_INTF_MOD;
		}
	}
	if (!d->v4[0] && !d->v6[0]) {
		read_addresses(m, d->aid);
		read_dynamic(m, d->aid);
	}
	if (!d->dns[0][0])
		read_dynamic(m, d->aid);
	d->activated_by_us = 1;
	snprintf(d->bind_status, sizeof(d->bind_status), "AT templates ok, aid %d tid %d", d->aid,
		 d->tid);
	return 0;
}

/* --- the MIPC backend --- */

static struct data_state *pdn_activating;	/* our DATA_ACT_CALL_REQ is pending */

/* interface indications seen while our request was pending: the one with
 * the confirm's transIntfId is ours, any other one is a PDN the modem raised
 * by itself (the IMS PDN) and goes to ims.c */
#define ACT_EIF_MAX 4
static struct act_eif {
	unsigned cmd;
	int tid, mtu;
	char v4[48], v6[48];
} act_eif[ACT_EIF_MAX];
static int act_neif;

static void flush_act_eif(struct md *m, int own_tid)
{
	int n = act_neif;

	act_neif = 0;
	for (int i = 0; i < n; i++)
		if (act_eif[i].tid != own_tid)
			ims_eif_up(m, act_eif[i].cmd, act_eif[i].tid, act_eif[i].v4, act_eif[i].v6,
				   act_eif[i].mtu);
}

static int pdp_code(const char *ip_type)
{
	return !strcasecmp(ip_type, "IPV6") ? MIPC_PDP_IPV6 :
	       !strcasecmp(ip_type, "IPV4V6") ? MIPC_PDP_IPV4V6 : MIPC_PDP_IP;
}

/* DATA_ACT_CALL_REQ for one PDN and the confirm into d */
static int mipc_act(struct md *m, struct data_state *d, const char *apn, uint32_t apn_type,
		    int pdp, char *out, size_t cap)
{
	struct mipc_cnf c;
	struct mipc_buf b;
	struct mipc_tlv t;
	static uint8_t mem[MIPC_TRAFFIC_DESCR_SIZE + 512];
	uint32_t res = 0, v;
	size_t vlen, off = 0;
	const uint8_t *val;
	enum mipc_res mr;

	/* values of the captured request / the recommended sequence */
	mipc_buf_init(&b, mem, sizeof(mem));
	mipc_tlv_add_str(&b, MIPC_T_ACT_APN, apn);
	mipc_tlv_add_u32(&b, MIPC_T_ACT_APN_TYPE, apn_type);
	mipc_tlv_add_u8(&b, MIPC_T_ACT_PDP_TYPE, (uint8_t)pdp);
	mipc_tlv_add_u8(&b, MIPC_T_ACT_ROAM_PDP_TYPE, (uint8_t)pdp);
	mipc_tlv_add_u8(&b, MIPC_T_ACT_AUTH_TYPE, 0);
	mipc_tlv_add_str(&b, MIPC_T_ACT_USER, "");
	mipc_tlv_add_str(&b, MIPC_T_ACT_PASSWORD, "");
	mipc_tlv_add_u8(&b, MIPC_T_ACT_CONST_7F, MIPC_ACT_CONST_7F);
	mipc_tlv_add_u32(&b, MIPC_T_ACT_BEARER_MASK, MIPC_ACT_BEARER_ALL);
	mipc_tlv_add_u8(&b, MIPC_T_ACT_SPECIAL, 0);
	mipc_tlv_add_u32(&b, MIPC_T_ACT_NO_MATCH_ALL, 0);
	if (m->cfg.mipc_traffic_descriptor) {
		uint8_t td[MIPC_TRAFFIC_DESCR_SIZE];
		size_t l = strlen(apn);

		if (l > MIPC_TRAFFIC_DESCR_DNN_MAX)
			l = MIPC_TRAFFIC_DESCR_DNN_MAX;
		memset(td, 0, sizeof(td));
		td[0] = (uint8_t)l;
		memcpy(td + MIPC_TRAFFIC_DESCR_DNN_OFF, apn, l);
		mipc_tlv_add(&b, MIPC_T_ACT_TRAFFIC_DESCR, td, sizeof(td));
	}
	if (b.overflow) {
		str_append(out, cap, "error: MIPC request too long\n");
		return -1;
	}
	if (d->apn != apn)
		str_copy(d->apn, sizeof(d->apn), apn);
	d->eif_tid = -1;
	d->eif_v4[0] = d->eif_v6[0] = 0;
	str_append(out, cap, "mipc: DATA_ACT_CALL_REQ slot 0x%02x apn \"%s\" type 0x%x pdp %d (%zu bytes)\n",
		   mipc_slot(m), apn, apn_type, pdp, b.len);
	pdn_activating = d;
	act_neif = 0;
	mr = mipc_request(m, MIPC_DATA_ACT_CALL_REQ, mipc_slot(m), b.data, b.len,
			  m->cfg.mipc_timeout_ms, &c);
	pdn_activating = NULL;
	if (mr != MIPC_OK) {
		str_append(out, cap, "error: MIPC DATA_ACT_CALL: %s\n", mipc_res_str(c.res));
		return -1;
	}
	mipc_tlv_result(c.payload, c.len, &res);
	if (res) {
		str_append(out, cap, "error: MIPC DATA_ACT_CALL result %u\n", res);
		return -1;
	}

	if (!mipc_tlv_find_uint(c.payload, c.len, MIPC_T_CNF_AID, &v))
		d->aid = (int)v;
	if (!mipc_tlv_find_uint(c.payload, c.len, MIPC_T_CNF_TRANS_INTF_ID, &v)) {
		d->tid = (int)v;
		d->linkid = (int)(v % MIPC_TRANS_INTF_MOD);
	} else if (!mipc_tlv_find_uint(c.payload, c.len, MIPC_T_CNF_IF_ID, &v) && v <= 20) {
		d->linkid = (int)v;
	} else if (d->eif_tid >= 0) {
		d->tid = d->eif_tid;
		d->linkid = d->eif_tid % MIPC_TRANS_INTF_MOD;
		str_append(out, cap, "transIntfId from INTERNAL_EIF_IND: %d\n", d->tid);
	} else {
		str_append(out, cap, "warning: no transIntfId in the confirm, using data_linkid %d\n",
			   d->linkid);
	}
	val = mipc_tlv_find(c.payload, c.len, MIPC_T_CNF_V4_ADDRS, &vlen);
	for (size_t i = 0; val && i + 4 <= vlen; i += 4)
		take_bin_addr(d, val + i, 4, 0);
	val = mipc_tlv_find(c.payload, c.len, MIPC_T_CNF_V4_DNS, &vlen);
	for (size_t i = 0; val && i + 4 <= vlen; i += 4)
		take_bin_addr(d, val + i, 4, 1);
	val = mipc_tlv_find(c.payload, c.len, MIPC_T_CNF_V6_DNS, &vlen);
	for (size_t i = 0; val && i + 16 <= vlen; i += 16)
		take_bin_addr(d, val + i, 16, 1);
	if (!mipc_tlv_find_uint(c.payload, c.len, MIPC_T_CNF_MTU_V4, &v) && v >= 68 && v <= 9000)
		d->mtu = (int)v;
	if (!mipc_tlv_find_uint(c.payload, c.len, MIPC_T_CNF_EPS_BEARER, &v))
		str_append(out, cap, "eps bearer id: %u\n", v);
	/* the interface indication usually came first: its address */
	if (!d->v4[0] && !d->v6[0] && d->eif_tid >= 0 && (d->tid < 0 || d->eif_tid == d->tid)) {
		take_addr(d, d->eif_v4);
		take_addr(d, d->eif_v6);
	}
	/* unknown TLVs: listed for the questions file */
	while (mipc_tlv_next(c.payload, c.len, &off, &t) == 1)
		md_log(m, 1, "mipc: cnf tlv 0x%04x len %zu", t.raw_id, t.len);
	d->via_mipc = 1;
	d->activated_by_us = 1;
	snprintf(d->bind_status, sizeof(d->bind_status), "MIPC DATA_ACT_CALL ok, aid %d tid %d",
		 d->aid, d->tid);
	str_append(out, cap, "mipc: activated, aid %d, transIntfId %d -> linkid %d\n", d->aid, d->tid,
		   d->linkid);
	return 0;
}

static int activate_mipc(struct md *m, const char *apn, int cid, char *out, size_t cap)
{
	struct data_state *d = &m->data;

	(void)cid;
	if (!mipc_ready(m)) {
		str_append(out, cap, "error: MIPC session not ready on %s (%s)\n", m->cfg.mipc_port,
			   m->mipc.fd < 0 ? "port closed" : "session start pending");
		return -1;
	}
	if (!apn) {
		/* the attach APN from the AT port */
		struct ctx_info ctx[16];
		int n = list_contexts(m, ctx, 16), low = lowest_cid(ctx, n);

		for (int i = 0; i < n; i++)
			if (ctx[i].cid == low && ctx[i].apn[0])
				str_copy(d->apn, sizeof(d->apn), ctx[i].apn);
		if (!d->apn[0]) {
			str_append(out, cap, "error: no APN (argument, data_apn or attach context)\n");
			return -1;
		}
		apn = d->apn;
		str_append(out, cap, "apn from the attach context: %s\n", apn);
	}
	if (mipc_act(m, d, apn, MIPC_ACT_APN_TYPE_DEFAULT, pdp_code(m->cfg.data_ip_type), out, cap)) {
		flush_act_eif(m, -1);
		return -1;
	}
	flush_act_eif(m, d->tid);
	if (!d->dns[0][0] && m->at.fd >= 0 && d->aid >= 0) {
		/* DNS from the AT port if the confirm had none (observed for cid 0) */
		read_dynamic(m, d->aid);
	}
	return 0;
}

static int deactivate_mipc(struct md *m, struct data_state *d)
{
	struct mipc_cnf c;
	struct mipc_buf b;
	uint8_t mem[64];
	uint32_t res = 0;

	if (!mipc_ready(m) || d->aid < 0)
		return -1;
	mipc_buf_init(&b, mem, sizeof(mem));
	mipc_tlv_add_u8(&b, MIPC_T_DEACT_AID, (uint8_t)d->aid);
	mipc_tlv_add_u8(&b, MIPC_T_DEACT_CAUSE, MIPC_DEACT_CAUSE_NORMAL);
	if (mipc_request(m, MIPC_DATA_DEACT_CALL_REQ, mipc_slot(m), b.data, b.len, 60000, &c) !=
	    MIPC_OK)
		return -1;
	mipc_tlv_result(c.payload, c.len, &res);
	return res ? -1 : 0;
}

/* indications */
void data_mipc_ind(struct md *m, const struct mipc_msg *msg)
{
	struct data_state *d = &m->data;
	uint32_t v, cmd = 0, cause = 0;

	if (msg->msg_id == MIPC_INTERNAL_EIF_IND) {
		const uint8_t *rec;
		size_t vlen;
		int tid = -1;

		if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_EIF_TRANS_INTF_ID, &v))
			tid = (int)v;
		mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_EIF_CMD, &cmd);
		if (cmd == MIPC_EIF_UP || cmd == MIPC_EIF_HO_ADDR || cmd == MIPC_EIF_CHANGE) {
			char v4[48] = "", v6[48] = "";
			int mtu = 0;

			struct mipc_tlv t;
			size_t off = 0;

			while (mipc_tlv_next(msg->payload, msg->len, &off, &t) == 1)
				md_log(m, 1, "mipc: eif tlv 0x%04x len %zu", t.raw_id, t.len);
			/* the record as a plain TLV or as an array element */
			rec = mipc_tlv_find_rec(msg->payload, msg->len, MIPC_T_EIF_V4_RECORDS, &vlen);
			if (rec && vlen >= 4 && (rec[0] | rec[1] | rec[2] | rec[3]))
				snprintf(v4, sizeof(v4), "%u.%u.%u.%u", rec[0], rec[1], rec[2], rec[3]);
			rec = mipc_tlv_find_rec(msg->payload, msg->len, MIPC_T_EIF_V6_RECORDS, &vlen);
			if (rec && vlen >= 16)
				bin_to_v6(rec, v6, sizeof(v6));
			if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_EIF_MTU, &v) && v >= 68 &&
			    v <= 9000)
				mtu = (int)v;
			md_event(m, "data if %d %s %s %s", tid, cmd == MIPC_EIF_UP ? "ifup" : "ifchg",
				 v4[0] ? v4 : "-", v6[0] ? v6 : "-");
			if (pdn_activating) {
				/* our request is pending: decided when its confirm is in */
				if (act_neif < ACT_EIF_MAX) {
					struct act_eif *a = &act_eif[act_neif++];

					a->cmd = cmd;
					a->tid = tid;
					a->mtu = mtu;
					str_copy(a->v4, sizeof(a->v4), v4);
					str_copy(a->v6, sizeof(a->v6), v6);
				}
				if (pdn_activating->eif_tid < 0) {
					pdn_activating->eif_tid = tid;
					str_copy(pdn_activating->eif_v4, sizeof(pdn_activating->eif_v4), v4);
					str_copy(pdn_activating->eif_v6, sizeof(pdn_activating->eif_v6), v6);
				}
			} else if (d->up && tid == d->tid) {
				d->eif_tid = tid;
				str_copy(d->eif_v4, sizeof(d->eif_v4), v4);
				str_copy(d->eif_v6, sizeof(d->eif_v6), v6);
			} else {
				/* an interface we did not request: the IMS/emergency PDN */
				ims_eif_up(m, cmd, tid, v4, v6, mtu);
			}
		} else if (cmd == MIPC_EIF_DOWN) {
			md_event(m, "data if %d ifdown", tid);
			ims_eif_down(m, tid);
			if (d->up && d->via_mipc && tid == d->tid) {
				md_log(m, 0, "data: interface %d down indication: taking the link down", tid);
				link_down(m, d);
				md_event(m, "data down eif ifdown");
				clear_state(d);
			}
		} else if (cmd == MIPC_EIF_MTU) {
			if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_EIF_MTU, &v))
				md_event(m, "data if %d mtu %u", tid, v);
		} else {
			md_event(m, "data if %d cmd %u", tid, cmd);
		}
		return;
	}
	if (msg->msg_id == MIPC_DATA_DEACT_CALL_IND) {
		int aid = -1;

		if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_IND_AID, &v))
			aid = (int)v;
		mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_IND_CAUSE, &cause);
		md_event(m, "data pdn deact aid %d cause %u", aid, cause);
		if (d->up && d->via_mipc && aid == d->aid) {
			link_down(m, d);
			md_event(m, "data down deact indication cause %u", cause);
			clear_state(d);
		}
		return;
	}
	if (msg->msg_id == MIPC_DATA_ACT_CALL_IND) {
		int aid = -1;

		if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_IND_AID, &v))
			aid = (int)v;
		md_event(m, "data pdn act aid %d (modem/network initiated)", aid);
	}
}

void data_mipc_lost(struct md *m)
{
	struct data_state *d = &m->data;

	ims_mipc_lost(m);

	if (d->up && d->via_mipc) {
		link_down(m, d);
		md_event(m, "data down mipc lost");
		clear_state(d);
	}
}

/* --- the link --- */

static void write_resolv(struct md *m)
{
	struct data_state *d = &m->data;
	char tmp[300];
	FILE *f;
	int fd;

	if (!m->cfg.resolv_file[0])
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", m->cfg.resolv_file);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		md_log(m, 0, "%s: %s", tmp, strerror(errno));
		return;
	}
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		return;
	}
	fprintf(f, "# generated by t2-modemd: aid %d, apn %s, %s\n", d->aid, d->apn, d->ifname);
	for (int i = 0; i < 2; i++)
		if (d->dns[i][0])
			fprintf(f, "nameserver %s\n", d->dns[i]);
	fclose(f);
	if (rename(tmp, m->cfg.resolv_file)) {
		md_log(m, 0, "rename %s: %s", m->cfg.resolv_file, strerror(errno));
		unlink(tmp);
		return;
	}
	if (m->cfg.manage_resolv && m->cfg.resolv_target[0]) {
		/* a copy, not a symlink: /etc may be on another file system */
		char buf[1024];
		ssize_t n;
		int in = open(m->cfg.resolv_file, O_RDONLY | O_CLOEXEC);
		int o;

		if (in < 0)
			return;
		snprintf(tmp, sizeof(tmp), "%s.t2-modemd", m->cfg.resolv_target);
		o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (o < 0) {
			md_log(m, 0, "%s: %s", tmp, strerror(errno));
			close(in);
			return;
		}
		while ((n = read(in, buf, sizeof(buf))) > 0)
			if (write(o, buf, (size_t)n) != n)
				break;
		close(in);
		close(o);
		if (rename(tmp, m->cfg.resolv_target))
			md_log(m, 0, "rename %s: %s", m->cfg.resolv_target, strerror(errno));
	}
}

/* the WWAN link of one PDN; d->no_route (IMS): no default route, no DNS */
static int link_up(struct md *m, struct data_state *d, char *out, size_t cap)
{
	char linkid[16], metric[16], mtu[16], v4[64], v6[64];
	int rc;

	snprintf(linkid, sizeof(linkid), "%d", d->linkid);
	snprintf(metric, sizeof(metric), "%d", m->cfg.route_metric);
	snprintf(d->ifname, sizeof(d->ifname), "%s-%d", m->cfg.wwan_dev, d->linkid);
	if (!link_exists(d->ifname)) {
		rc = ip(m, "link", "add", "dev", d->ifname, "parentdev", m->cfg.wwan_dev,
			"type", "wwan", "linkid", linkid, NULL);
		if (rc) {
			str_append(out, cap, "error: ip link add %s failed (%d)\n", d->ifname, rc);
			return -1;
		}
	} else {
		str_append(out, cap, "link %s exists, reusing\n", d->ifname);
	}
	IP4(m, "addr", "flush", "dev", d->ifname);
	if (d->v4[0]) {
		snprintf(v4, sizeof(v4), "%s/32", d->v4);
		rc = IP6(m, "addr", "add", v4, "dev", d->ifname, NULL);
		if (rc)
			str_append(out, cap, "warning: ip addr add %s failed (%d)\n", v4, rc);
	}
	if (d->v6[0] && strncasecmp(d->v6, "fe80:", 5)) {
		snprintf(v6, sizeof(v6), "%s/64", d->v6);
		rc = IP6(m, "-6", "addr", "add", v6, "dev", d->ifname);
		if (rc)
			str_append(out, cap, "warning: ip -6 addr add %s failed (%d)\n", v6, rc);
	}
	if (d->mtu > 0) {
		snprintf(mtu, sizeof(mtu), "%d", d->mtu);
		rc = IP6(m, "link", "set", "dev", d->ifname, "mtu", mtu);
		if (rc)
			str_append(out, cap, "warning: mtu %s refused (%d)\n", mtu, rc);
	}
	rc = IP6(m, "link", "set", "dev", d->ifname, "up", NULL);
	if (rc) {
		str_append(out, cap, "error: ip link set %s up failed (%d)\n", d->ifname, rc);
		return -1;
	}
	d->up = 1;
	if (d->no_route)
		return 0;
	if (d->v4[0]) {
		rc = IP8(m, "route", "replace", "default", "dev", d->ifname, "metric", metric, NULL);
		if (rc)
			str_append(out, cap, "warning: default route failed (%d)\n", rc);
	}
	if (d->v6[0]) {
		rc = IP8(m, "-6", "route", "replace", "default", "dev", d->ifname, "metric", metric);
		if (rc)
			str_append(out, cap, "warning: IPv6 default route failed (%d)\n", rc);
	}
	write_resolv(m);
	return 0;
}

static void link_down(struct md *m, struct data_state *d)
{
	char metric[16];

	if (!d->ifname[0])
		return;
	if (d->no_route) {
		IP4(m, "link", "del", "dev", d->ifname);
		d->up = 0;
		return;
	}
	snprintf(metric, sizeof(metric), "%d", m->cfg.route_metric);
	IP8(m, "route", "del", "default", "dev", d->ifname, "metric", metric, NULL);
	if (d->v6[0])
		IP8(m, "-6", "route", "del", "default", "dev", d->ifname, "metric", metric);
	IP4(m, "link", "del", "dev", d->ifname);
	if (m->cfg.resolv_file[0])
		unlink(m->cfg.resolv_file);
	/* /etc/resolv.conf is left as it is: we do not know what was there */
	d->up = 0;
}

static void read_counter(const char *ifname, const char *name, char *out, size_t cap)
{
	char path[160];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, name);
	str_copy(out, cap, "-");
	f = fopen(path, "re");
	if (!f)
		return;
	if (fgets(out, (int)cap, f))
		out[strcspn(out, "\r\n")] = 0;
	fclose(f);
}

/* --- commands --- */

static void clear_state(struct data_state *d)
{
	d->up = 0;
	d->aid = -1;
	d->tid = -1;
	d->linkid = 0;
	d->mtu = 0;
	d->activated_by_us = 0;
	d->via_mipc = 0;
	d->remote_down = 0;
	d->epdn_err = 0;
	d->tpl_idx = 0;
	d->apn[0] = d->ifname[0] = d->v4[0] = d->v6[0] = d->gw[0] = 0;
	d->dns[0][0] = d->dns[1][0] = 0;
	d->bind_status[0] = 0;
}

void data_init(struct md *m)
{
	struct data_state *d = &m->data;

	memset(d, 0, sizeof(*d));
	clear_state(d);
	d->eif_tid = -1;
	d->last_cgev_cid = -1;
}

int data_up(struct md *m, const char *apn, int cid, char *out, size_t cap)
{
	struct data_state *d = &m->data;
	int rc, mipc = !strcmp(m->cfg.data_backend, "mipc");

	if (m->state != MD_READY) {
		str_append(out, cap, "error: modem not ready\n");
		return -1;
	}
	if (d->up) {
		str_append(out, cap, "error: data is up (aid %d on %s); 'data down' first\n",
			   d->aid, d->ifname);
		return -1;
	}
	if (m->eps.stat != 1 && m->eps.stat != 5 && m->gprs.stat != 1 && m->gprs.stat != 5)
		str_append(out, cap, "warning: not registered for packet service (eps %s)\n",
			   reg_stat_str(m->eps.stat));
	clear_state(d);
	d->linkid = m->cfg.data_linkid;
	d->mtu = m->cfg.data_mtu;
	if (!apn || !*apn)
		apn = m->cfg.data_apn[0] ? m->cfg.data_apn : NULL;
	if (cid < 0)
		cid = m->cfg.data_cid;
	if (apn && (strlen(apn) > 100 || strpbrk(apn, "\"\r\n;{}")))
		apn = NULL;
	str_append(out, cap, "backend: %s\n", m->cfg.data_backend);
	rc = mipc ? activate_mipc(m, apn, cid, out, cap) : activate_at(m, apn, cid, out, cap);
	if (rc) {
		clear_state(d);
		return -1;
	}
	if (!d->v4[0] && !d->v6[0]) {
		str_append(out, cap, "error: no address for aid %d\n", d->aid);
		if (!mipc && m->cfg.data_deact_cmds[0]) {
			int idx = 0;

			run_templates(m, m->cfg.data_deact_cmds, &idx, 0, out, cap);
		}
		clear_state(d);
		return -1;
	}
	str_append(out, cap, "apn: %s\n", d->apn);
	str_append(out, cap, "address: %s%s%s\n", d->v4[0] ? d->v4 : "",
		   d->v4[0] && d->v6[0] ? " " : "", d->v6[0] ? d->v6 : "");
	if (d->gw[0])
		str_append(out, cap, "gateway: %s\n", d->gw);
	str_append(out, cap, "dns: %s %s\n", d->dns[0][0] ? d->dns[0] : "-",
		   d->dns[1][0] ? d->dns[1] : "");
	if (link_up(m, d, out, cap)) {
		link_down(m, d);
		clear_state(d);
		return -1;
	}
	str_append(out, cap, "link: %s (linkid %d on %s), default route metric %d\n", d->ifname,
		   d->linkid, m->cfg.wwan_dev, m->cfg.route_metric);
	/* AT flow: the templates after @linkup (AT+EIF ... "ifst","up") */
	if (!mipc && m->cfg.data_bind_cmds[0]) {
		char post[REPLY_MAX / 4];

		post[0] = 0;
		if (run_templates(m, m->cfg.data_bind_cmds, &d->tpl_idx, 0, post, sizeof(post)))
			str_append(out, cap, "warning: post-link commands failed (advisory):\n%s", post);
		else if (post[0])
			str_append(out, cap, "%s", post);
	}
	str_append(out, cap, "path: UL %s -> %s linkid %d -> modem aid %d (transIntfId %d); "
		   "DL from modem data interface %d (counters: 'data status')\n", d->ifname,
		   m->cfg.wwan_dev, d->linkid, d->aid, d->tid, d->linkid);
	str_append(out, cap, "bind: %s\n", d->bind_status);
	md_event(m, "data up aid=%d link=%s addr=%s", d->aid, d->ifname, d->v4[0] ? d->v4 : d->v6);
	return 0;
}

static int deactivate(struct md *m, char *out, size_t cap)
{
	struct data_state *d = &m->data;
	struct at_resp r;
	char cmd[32];

	if (d->via_mipc)
		return deactivate_mipc(m, d);
	if (!d->activated_by_us || d->aid < 0 || m->at.fd < 0)
		return 0;
	if (m->cfg.data_bind_cmds[0]) {
		int idx = 0;

		if (!m->cfg.data_deact_cmds[0])
			return 0;
		return run_templates(m, m->cfg.data_deact_cmds, &idx, 0, out, cap);
	}
	/* 3GPP route: a context we did not activate (the LTE default bearer)
	 * is left alone, deactivating it would detach */
	snprintf(cmd, sizeof(cmd), "AT+CGACT=0,%d", d->aid);
	at_cmd_ex(m, cmd, NULL, 0, 30000, &r);
	return r.res == AT_OK ? 0 : -1;
}

int data_down(struct md *m, char *out, size_t cap)
{
	struct data_state *d = &m->data;

	if (!d->up && d->aid < 0) {
		str_append(out, cap, "data is not up\n");
		return 0;
	}
	link_down(m, d);
	if (deactivate(m, out, cap))
		str_append(out, cap, "warning: deactivation of aid %d failed\n", d->aid);
	str_append(out, cap, "link %s removed\n", d->ifname);
	md_event(m, "data down requested");
	clear_state(d);
	return 0;
}

void data_status(struct md *m, char *out, size_t cap)
{
	struct data_state *d = &m->data;
	char rxp[32], txp[32], rxb[32], txb[32];

	if (!d->up) {
		str_append(out, cap, "data: down (backend %s)\n", m->cfg.data_backend);
		if (d->eif_tid >= 0)
			str_append(out, cap, "data last if: %d %s %s\n", d->eif_tid,
				   d->eif_v4[0] ? d->eif_v4 : "-", d->eif_v6[0] ? d->eif_v6 : "-");
		return;
	}
	str_append(out, cap, "data: up aid %d apn %s via %s\n", d->aid, d->apn,
		   d->via_mipc ? "mipc" : "at");
	str_append(out, cap, "data link: %s linkid %d transIntfId %d metric %d mtu %d\n", d->ifname,
		   d->linkid, d->tid, m->cfg.route_metric, d->mtu);
	str_append(out, cap, "data address: %s%s%s\n", d->v4[0] ? d->v4 : "",
		   d->v4[0] && d->v6[0] ? " " : "", d->v6[0] ? d->v6 : "");
	str_append(out, cap, "data dns: %s %s\n", d->dns[0][0] ? d->dns[0] : "-",
		   d->dns[1][0] ? d->dns[1] : "");
	str_append(out, cap, "data bind: %s\n", d->bind_status);
	read_counter(d->ifname, "rx_packets", rxp, sizeof(rxp));
	read_counter(d->ifname, "tx_packets", txp, sizeof(txp));
	read_counter(d->ifname, "rx_bytes", rxb, sizeof(rxb));
	read_counter(d->ifname, "tx_bytes", txb, sizeof(txb));
	str_append(out, cap, "data path: DL rx %s packets %s bytes, UL tx %s packets %s bytes\n",
		   rxp, rxb, txp, txb);
}

/* an +EIF ifdown for our interface arrived as a URC: act from the tick */
void data_tick(struct md *m)
{
	struct data_state *d = &m->data;

	if (d->remote_down && d->up) {
		md_log(m, 0, "data: +EIF ifdown for %s: taking the link down", d->ifname);
		link_down(m, d);
		md_event(m, "data down eif ifdown");
		clear_state(d);
	}
	d->remote_down = 0;
}

void data_modem_lost(struct md *m)
{
	struct data_state *d = &m->data;

	if (d->up) {
		link_down(m, d);
		md_event(m, "data down modem lost");
	}
	clear_state(d);
	d->eif_tid = -1;
}

void data_shutdown(struct md *m)
{
	struct data_state *d = &m->data;
	char out[1024] = "";

	if (!d->up)
		return;
	if (m->cfg.keep_data_on_exit) {
		md_log(m, 0, "exit: data link %s kept (keep_data_on_exit)", d->ifname);
		return;
	}
	link_down(m, d);
	deactivate(m, out, sizeof(out));
	clear_state(d);
}

/* --- further PDNs (IMS): MIPC activation and a link without route/DNS --- */

void data_pdn_clear(struct data_state *d)
{
	clear_state(d);
	d->eif_tid = -1;
	d->eif_v4[0] = d->eif_v6[0] = 0;
	d->last_cgev_cid = -1;
}

/*
 * The link of a PDN the modem brought up by itself (the IMS PDN): the
 * interface indication's transIntfId and address, link up, MTU, no default
 * route, no DNS. Nothing is sent to the modem; m->data is not touched.
 */
int data_pdn_link(struct md *m, struct data_state *d, int tid, const char *v4, const char *v6,
		  int mtu, char *out, size_t cap)
{
	data_pdn_clear(d);
	d->no_route = 1;
	d->via_mipc = 1;
	d->tid = tid;
	d->linkid = tid % MIPC_TRANS_INTF_MOD;
	d->mtu = mtu > 0 ? mtu : m->cfg.data_mtu;
	take_addr(d, v4);
	take_addr(d, v6);
	if (m->data.up && m->data.linkid == d->linkid) {
		str_append(out, cap, "error: linkid %d is the internet link %s\n", d->linkid,
			   m->data.ifname);
		goto fail;
	}
	if (link_up(m, d, out, cap)) {
		link_down(m, d);
		goto fail;
	}
	return 0;
fail:
	data_pdn_clear(d);
	d->no_route = 1;
	return -1;
}

/* the link removed (nothing sent to the modem) */
void data_pdn_down(struct md *m, struct data_state *d)
{
	if (d->up || d->ifname[0])
		link_down(m, d);
	data_pdn_clear(d);
	d->no_route = 1;
}

/* --- the APN profile list (APN_ADD_PROFILE_LIST_REQ) --- */

static void apn_record(uint8_t *r, const char *apn, uint32_t type, int pdp, int auth)
{
	size_t l = strlen(apn);

	memset(r, 0, MIPC_APN_REC_SIZE);
	mipc_put(r + MIPC_APN_REC_ID_OFF, 4, 0xFFFFFFFFu);
	if (l > MIPC_APN_REC_APN_MAX - 1)
		l = MIPC_APN_REC_APN_MAX - 1;
	memcpy(r + MIPC_APN_REC_APN_OFF, apn, l);
	mipc_put(r + MIPC_APN_REC_BEARER_OFF, 4, MIPC_APN_REC_BEARER_ALL);
	mipc_put(r + MIPC_APN_REC_TYPE_OFF, 4, type);
	r[MIPC_APN_REC_PDP_OFF] = (uint8_t)pdp;
	r[MIPC_APN_REC_ROAM_PDP_OFF] = (uint8_t)pdp;
	r[MIPC_APN_REC_AUTH_OFF] = (uint8_t)auth;
	mipc_put(r + MIPC_APN_REC_ENABLED_OFF, 4, 1);
}

/*
 * The profiles the factory firmware's RIL installs per slot before its first data call:
 * internet (type 0x29, auth 1 as captured), mms (only with mms_apn), ims
 * (0x02, IPv4v6), sos (0x100, IPv4v6). The modem raises the IMS PDN only
 * once it holds a profile of type IMS. 0 = CNF result 0.
 */
int data_apn_profiles(struct md *m, char *out, size_t cap, int *with_internet)
{
	static uint8_t mem[MIPC_APN_MAX_PROFILES * 300 + 64];
	static struct mipc_cnf c;
	uint8_t rec[MIPC_APN_REC_SIZE];
	struct mipc_buf b;
	char internet[101] = "";
	struct {
		const char *apn;
		uint32_t type;
		int pdp, auth;
	} p[MIPC_APN_MAX_PROFILES];
	int n = 0;
	unsigned first;
	uint32_t res = 0;
	enum mipc_res mr;

	if (with_internet)
		*with_internet = 0;
	if (!mipc_ready(m)) {
		str_append(out, cap, "error: MIPC session not ready\n");
		return -1;
	}
	if (m->cfg.data_apn[0]) {
		str_copy(internet, sizeof(internet), m->cfg.data_apn);
	} else if (m->at.fd >= 0) {
		/* the attach context's APN, as data up uses it */
		struct ctx_info ctx[16];
		int nc = list_contexts(m, ctx, 16), low = lowest_cid(ctx, nc);

		for (int i = 0; i < nc; i++)
			if (ctx[i].cid == low && ctx[i].apn[0])
				str_copy(internet, sizeof(internet), ctx[i].apn);
	}
	if (internet[0]) {
		p[n].apn = internet;
		p[n].type = MIPC_APN_REC_TYPE_DEFAULT;
		p[n].pdp = pdp_code(m->cfg.data_ip_type);
		p[n++].auth = 1;
	} else {
		str_append(out, cap, "warning: no internet APN (data_apn or attach context): "
			   "profile left out\n");
	}
	if (m->cfg.mms_apn[0]) {
		p[n].apn = m->cfg.mms_apn;
		p[n].type = MIPC_APN_REC_TYPE_MMS;
		p[n].pdp = MIPC_PDP_IP;
		p[n++].auth = 1;
	}
	if (m->cfg.ims_apn[0]) {
		p[n].apn = m->cfg.ims_apn;
		p[n].type = MIPC_APN_REC_TYPE_IMS;
		p[n].pdp = MIPC_PDP_IPV4V6;
		p[n++].auth = 0;
	}
	if (m->cfg.sos_apn[0]) {
		p[n].apn = m->cfg.sos_apn;
		p[n].type = MIPC_APN_REC_TYPE_EMERGENCY;
		p[n].pdp = MIPC_PDP_IPV4V6;
		p[n++].auth = 0;
	}
	if (!n) {
		str_append(out, cap, "error: no APN profile to send\n");
		return -1;
	}
	first = MIPC_ARRAY_FIRST(MIPC_T_APNLIST_ARRAY, (unsigned)n);
	mipc_buf_init(&b, mem, sizeof(mem));
	mipc_tlv_add_u8(&b, MIPC_T_APNLIST_COUNT, (uint8_t)n);
	mipc_tlv_add_array(&b, MIPC_T_APNLIST_ARRAY, first, (unsigned)n);
	for (int i = 0; i < n; i++) {
		apn_record(rec, p[i].apn, p[i].type, p[i].pdp, p[i].auth);
		mipc_tlv_add(&b, first + (unsigned)i, rec, sizeof(rec));
		str_append(out, cap, "apn profile %d: \"%s\" type 0x%x pdp %d auth %d\n", i, p[i].apn,
			   p[i].type, p[i].pdp, p[i].auth);
	}
	if (b.overflow) {
		str_append(out, cap, "error: APN profile list too long\n");
		return -1;
	}
	mr = mipc_request(m, MIPC_APN_ADD_PROFILE_LIST_REQ, mipc_slot(m), b.data, b.len,
			  m->cfg.mipc_timeout_ms, &c);
	if (mr == MIPC_OK)
		mipc_tlv_result(c.payload, c.len, &res);
	if (mr != MIPC_OK || res) {
		if (mr != MIPC_OK)
			str_append(out, cap, "mipc: APN_ADD_PROFILE_LIST_REQ slot 0x%02x (%d profiles) -> %s\n",
				   mipc_slot(m), n, mipc_res_str(mr));
		else
			str_append(out, cap, "mipc: APN_ADD_PROFILE_LIST_REQ slot 0x%02x (%d profiles) -> "
				   "result 0x%x\n", mipc_slot(m), n, res);
		return -1;
	}
	str_append(out, cap, "mipc: APN_ADD_PROFILE_LIST_REQ slot 0x%02x (%d profiles, %zu bytes) -> OK\n",
		   mipc_slot(m), n, b.len);
	if (with_internet)
		*with_internet = internet[0] != 0;
	return 0;
}
