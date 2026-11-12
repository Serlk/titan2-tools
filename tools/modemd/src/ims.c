// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: IMS / VoLTE registration (what the phone was
 * observed to do).
 * The IMS stack (SIP, registration, media) runs inside the modem, and on
 * this phone the modem also raises the IMS PDN by itself. The AP:
 *
 *  start-up (ims=1, modem ready, radio on): AT+CIREG=2,
 *      AT+EIMSPDN= "onoff",1, AT+EIMSCFG=<ims_cfg>, AT+EIMS? (AT+EIMS=1 only
 *      when it says 0);
 *  INTERNAL_EIF_IND (0x4302) up for an interface we did not request: the
 *      IMS (or emergency) PDN came up: link wwan0-<transIntfId mod 100> with
 *      the indication's address, up, MTU, no default route, no DNS (the
 *      traffic is the modem's own); EIF_IND down removes it;
 *  IMS_PDN_IND (0x4A03): short form = bookkeeping reset, full form labels
 *      the PDN (aid, type 2 = ims, interface id) and action 0 removes it;
 *  INTERNAL_EIPSPI_CMD (0x8305): answered with INTERNAL_EIPSPI_RSP (0x8306),
 *      an unused random SPI in the requested range;
 *  nothing else is sent for the PDN: no DATA_ACT_CALL_REQ, no confirm.
 *
 * AT fallback (unverified: never observed): a +EIMSPDN: "notify" on the AT
 * port is answered AT+EIMSPDN="confirm",<aid>,0 from the tick, nothing else.
 * +CIREGU (modem.c) gives the registration; the services are decoded here.
 * The internet PDN (m->data) is never touched here.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "modemd.h"

#define IMS_SETUP_RETRY_MS	5000	/* start-up sequence after a timeout */
#define IMS_MIPC_POLL_MS	200	/* start-up waiting for the MIPC session */
#define IMS_MIPC_WAIT_MS	20000	/* then it runs without the MIPC part */
#define IMS_APN_RETRY_MS	5000	/* full APN list again once the attach APN is known */
#define IMS_APN_TRIES		60
#define IMS_RCS_CMD		"AT+EIMSRCS=0,0"	/* factory, through the AT tunnel */
#define IMS_CONFIRM_OK		0	/* 0 = success */

static const char *type_str(int type, char *buf, size_t cap)
{
	if (type == MIPC_IMSPDN_TYPE_IMS)
		return "ims";
	if (type < 0)
		return "-";
	snprintf(buf, cap, "%d", type);	/* emergency's value is not known */
	return buf;
}

/* 27.007 §8.71 <ext_info> bits: 1 RTP voice, 2 RTP text, 4 SMS over IMS,
 * 8 RTP video (the vendor description names bit 2 "video": unverified) */
void ims_services_str(int ext, char *out, size_t cap)
{
	out[0] = 0;
	if (ext & 1)
		str_append(out, cap, "%svoice", out[0] ? "," : "");
	if (ext & 2)
		str_append(out, cap, "%stext", out[0] ? "," : "");
	if (ext & 4)
		str_append(out, cap, "%ssms", out[0] ? "," : "");
	if (ext & 8)
		str_append(out, cap, "%svideo", out[0] ? "," : "");
	if (ext & ~15)
		str_append(out, cap, "%sother", out[0] ? "," : "");
	if (!out[0])
		str_copy(out, cap, "none");
}

void ims_set_ext(struct md *m, int ext)
{
	char sv[64];

	if (ext == m->ims.ext)
		return;
	m->ims.ext = ext;
	ims_services_str(ext, sv, sizeof(sv));
	md_event(m, "ims services %s ext=0x%x", sv, ext);
}

/* --- bookkeeping --- */

static struct ims_pdn *find_tid(struct md *m, int tid)
{
	for (int i = 0; i < IMS_PDN_MAX; i++)
		if (m->ims.pdn[i].st != IMS_PDN_FREE && m->ims.pdn[i].tid == tid)
			return &m->ims.pdn[i];
	return NULL;
}

static struct ims_pdn *find_aid(struct md *m, int aid)
{
	for (int i = 0; i < IMS_PDN_MAX; i++)
		if (m->ims.pdn[i].st != IMS_PDN_FREE && m->ims.pdn[i].aid == aid)
			return &m->ims.pdn[i];
	return NULL;
}

static struct ims_pdn *alloc_pdn(struct md *m, int tid)
{
	for (int i = 0; i < IMS_PDN_MAX; i++) {
		struct ims_pdn *p = &m->ims.pdn[i];

		if (p->st == IMS_PDN_FREE) {
			p->tid = tid;
			p->aid = p->type = -1;
			return p;
		}
	}
	return NULL;
}

static void free_pdn(struct ims_pdn *p)
{
	p->st = IMS_PDN_FREE;
	p->tid = p->aid = p->type = -1;
	data_pdn_clear(&p->d);
	p->d.no_route = 1;
}

/* the link removed (nothing is sent to the modem) and the slot freed */
static void pdn_down(struct md *m, struct ims_pdn *p, const char *why)
{
	char tb[16];

	if (p->st == IMS_PDN_UP) {
		char ifname[sizeof(p->d.ifname)];

		str_copy(ifname, sizeof(ifname), p->d.ifname);
		md_log(m, 0, "ims: %s down (transIntfId %d): %s", ifname, p->tid, why);
		data_pdn_down(m, &p->d);
		md_event(m, "ims pdn down link=%s aid=%d type=%s %s", ifname, p->aid,
			 type_str(p->type, tb, sizeof(tb)), why);
	}
	free_pdn(p);
}

static void all_down(struct md *m, const char *why)
{
	for (int i = 0; i < IMS_PDN_MAX; i++)
		if (m->ims.pdn[i].st != IMS_PDN_FREE)
			pdn_down(m, &m->ims.pdn[i], why);
}

void ims_init(struct md *m)
{
	struct ims_state *s = &m->ims;

	memset(s, 0, sizeof(*s));
	s->on = m->cfg.ims;
	s->eims = s->eimcflag = -1;
	s->ext = -1;
	s->urc_tid = -1;
	for (int i = 0; i < IMS_PDN_MAX; i++)
		free_pdn(&s->pdn[i]);
	str_copy(s->setup_status, sizeof(s->setup_status), s->on ? "pending" : "off");
}

/* --- the start-up sequence ("once per boot (per stack)") --- */

/* a command whose answer may carry IMS lines: +EIMSPDN: / +EIMS: lines
 * that the AT engine took for the response (same prefix as the pending
 * command) are handled as the URCs they are */
static enum at_result ims_cmd(struct md *m, const char *cmd, struct at_resp *r, char *out,
			      size_t cap)
{
	const char *q;
	char line[AT_LINE_MAX];

	at_cmd(m, cmd, r);
	if (r->res == AT_OK) {
		str_append(out, cap, "%s -> OK\n", cmd);
	} else {
		char e[96];

		at_resp_error(r, e, sizeof(e));
		str_append(out, cap, "%s -> %s\n", cmd, e);
	}
	q = r->buf;
	while (at_next_line(&q, line, sizeof(line)))
		if (!strncmp(line, "+EIMSPDN:", 9) ||
		    ((!strncmp(line, "+EIMS:", 6) || !strncmp(line, "+EIMCFLAG:", 10)) &&
		     strcmp(cmd, "AT+EIMS?")))
			ims_urc(m, line);
	return r->res;
}


/* ims_cfg "1,0,0,0,1,1" -> six bytes (checked at start) */
static void cfg_flags(const char *cfg, uint8_t *f)
{
	const char *p = cfg;

	for (int i = 0; i < MIPC_IMSCFG_FLAGS; i++) {
		f[i] = (uint8_t)strtoul(p, NULL, 10);
		p = strchr(p, ',');
		p = p ? p + 1 : "0";
	}
}

/*
 * IMS_SET_CONFIG_REQ 0x0A00 on the stack's slot: the
 * key/value form (mtk_ct_volte_support = 0x32) or the feature-switch form
 * (the six ims_cfg flags). 0 = CNF result 0, -1 otherwise.
 */
static int ims_mipc_cfg(struct md *m, int keyform, char *out, size_t cap)
{
	static struct mipc_cnf c;
	struct mipc_buf b;
	uint8_t mem[128], f[MIPC_IMSCFG_FLAGS];
	uint32_t res = 0;
	enum mipc_res mr;
	char what[64];

	mipc_buf_init(&b, mem, sizeof(mem));
	if (keyform) {
		mipc_tlv_add_u32(&b, MIPC_T_IMSCFG_FORM, MIPC_IMSCFG_FORM_KEY);
		mipc_tlv_add_str(&b, MIPC_T_IMSCFG_KEY, MIPC_IMSCFG_KEY_VOLTE);
		mipc_tlv_add_u8(&b, MIPC_T_IMSCFG_VALUE, MIPC_IMSCFG_KEY_VOLTE_VALUE);
		snprintf(what, sizeof(what), "%s=0x%02x", MIPC_IMSCFG_KEY_VOLTE,
			 MIPC_IMSCFG_KEY_VOLTE_VALUE);
	} else {
		cfg_flags(m->cfg.ims_cfg, f);
		mipc_tlv_add_u32(&b, MIPC_T_IMSCFG_FORM, MIPC_IMSCFG_FORM_FLAGS);
		mipc_tlv_add(&b, MIPC_T_IMSCFG_VALUE, f, sizeof(f));
		snprintf(what, sizeof(what), "flags %02x %02x %02x %02x %02x %02x", f[0], f[1], f[2],
			 f[3], f[4], f[5]);
	}
	mr = mipc_request(m, MIPC_IMS_SET_CONFIG_REQ, mipc_slot(m), b.data, b.len,
			  m->cfg.mipc_timeout_ms, &c);
	if (mr == MIPC_OK)
		mipc_tlv_result(c.payload, c.len, &res);
	if (mr != MIPC_OK || res) {
		if (mr != MIPC_OK)
			str_append(out, cap, "mipc: IMS_SET_CONFIG_REQ slot 0x%02x %s -> %s\n",
				   mipc_slot(m), what, mipc_res_str(mr));
		else
			str_append(out, cap, "mipc: IMS_SET_CONFIG_REQ slot 0x%02x %s -> result %u\n",
				   mipc_slot(m), what, res);
		return -1;
	}
	str_append(out, cap, "mipc: IMS_SET_CONFIG_REQ slot 0x%02x %s -> OK\n", mipc_slot(m), what);
	return 0;
}

/* 0 done (possibly with refusals noted), -1 failed, 1 retry later,
 * 2 waiting for the MIPC session */
static int ims_setup(struct md *m, char *out, size_t cap)
{
	struct ims_state *s = &m->ims;
	struct at_resp r;
	char cmd[AT_CMD_MAX], note[160] = "";
	enum at_result res;
	int want_eims = 0, use_mipc = 0;

	if (m->mipc.enabled) {
		/* the IMS configuration goes over MIPC (the AT port alone did
		 * not make the modem raise the IMS PDN) */
		if (mipc_ready(m)) {
			use_mipc = 1;
		} else {
			int64_t now = mono_ms();

			if (!s->mipc_wait_since)
				s->mipc_wait_since = now;
			if (now - s->mipc_wait_since < IMS_MIPC_WAIT_MS)
				return 2;
			str_append(note, sizeof(note), "MIPC session not ready: no MIPC set-up; ");
		}
	} else if (s->on) {
		str_append(out, cap, "warning: the IMS configuration and the IMS PDN are on MIPC; "
			   "with data_backend=%s neither is done\n", m->cfg.data_backend);
	}
	s->mipc_wait_since = 0;
	/* everything below belongs before the radio goes on */
	if (m->cfg.radio_off_before_setup && !s->radio_started && m->cfun == 1 &&
	    !vendor_radio_cmd(m, 0, NULL, cmd, sizeof(cmd))) {
		at_cmd_ex(m, cmd, NULL, 0, 15000, &r);
		str_append(out, cap, "%s -> %s (radio off for the set-up)\n", cmd, at_result_str(r.res));
		if (r.res == AT_LOST || r.res == AT_TIMEOUT)
			return 1;
		if (r.res == AT_OK)
			m->cfun = 4;
	}
	/* 1. IMS_SET_CONFIG: the key/value form, then the feature switches */
	if (s->on && use_mipc) {
		if (m->cfg.ims_mipc_key && ims_mipc_cfg(m, 1, out, cap))
			str_append(note, sizeof(note), "IMS_SET_CONFIG key refused; ");
		if (ims_mipc_cfg(m, 0, out, cap))
			str_append(note, sizeof(note), "IMS_SET_CONFIG flags refused; ");
	}
	/* 2. the APN profile list (internet, mms, ims, sos): the modem raises
	 * the IMS PDN only with a profile of type IMS */
	if (use_mipc && m->cfg.apn_profiles && mipc_ready(m)) {
		int with_internet = 0;

		if (data_apn_profiles(m, out, cap, &with_internet)) {
			str_append(note, sizeof(note), "APN profile list refused; ");
		} else if (!with_internet) {
			/* sent again with the attach APN once registered */
			s->apn_need_internet = 1;
			s->apn_tries = 0;
			s->apn_retry_at = mono_ms() + IMS_APN_RETRY_MS;
			str_append(note, sizeof(note), "APN list without internet, re-sent later; ");
		} else {
			s->apn_need_internet = 0;
		}
	}
	if (!s->on) {
		if (note[0])
			note[strlen(note) - 2] = 0;
		snprintf(s->setup_status, sizeof(s->setup_status), "done (ims off%s%s)",
			 note[0] ? "; " : "", note);
		return 0;
	}
	/* 3. the AT part: registration reports (+CIREGU) */
	res = ims_cmd(m, "AT+CIREG=2", &r, out, cap);
	if (res == AT_LOST || res == AT_TIMEOUT)
		return 1;
	/* the modem brings the IMS PDN up: the form the factory firmware's RIL sent first */
	vendor_ims_onoff_cmd(1, 1, cmd, sizeof(cmd));
	res = ims_cmd(m, cmd, &r, out, cap);
	if (res == AT_LOST || res == AT_TIMEOUT)
		return 1;
	if (res != AT_OK) {
		/* the plain form (equivalence unverified) */
		vendor_ims_onoff_cmd(1, 0, cmd, sizeof(cmd));
		res = ims_cmd(m, cmd, &r, out, cap);
		if (res == AT_LOST || res == AT_TIMEOUT)
			return 1;
		if (res != AT_OK) {
			snprintf(s->setup_status, sizeof(s->setup_status),
				 "failed: AT+EIMSPDN \"onoff\",1 refused");
			return -1;
		}
	}
	/* VoLTE, SMS over IMS, emergency IMS (the values observed for SIM1):
	 * AT+EIMSCFG too (harmless; the MIPC form above is the one that counts) */
	vendor_ims_cfg_cmd(m->cfg.ims_cfg, cmd, sizeof(cmd));
	res = ims_cmd(m, cmd, &r, out, cap);
	if (res == AT_LOST || res == AT_TIMEOUT)
		return 1;
	if (res != AT_OK)
		str_append(note, sizeof(note), "AT+EIMSCFG refused; ");
	/* AT+EIMSRCS=0,0 through the AT tunnel: the plain AT port does not know
	 * it; success = result 0 and no ERROR in the text (unverified) */
	if (use_mipc && mipc_ready(m)) {
		char resp[256];
		int rc = mipc_at(m, IMS_RCS_CMD, resp, sizeof(resp));

		str_append(out, cap, "mipc at: %s -> result %d%s%s\n", IMS_RCS_CMD, rc,
			   resp[0] ? ": " : "", resp);
		if (rc || strstr(resp, "ERROR"))
			str_append(note, sizeof(note), "%s refused; ", IMS_RCS_CMD);
	}
	/* AT+EIMS=1 only if needed (try AT+EIMS? first) */
	if (!strcmp(m->cfg.ims_eims, "always")) {
		want_eims = 1;
	} else if (!strcmp(m->cfg.ims_eims, "query")) {
		const char *p;
		int v;

		res = ims_cmd(m, "AT+EIMS?", &r, out, cap);
		if (res == AT_LOST || res == AT_TIMEOUT)
			return 1;
		if (res == AT_OK && (p = at_find(&r, "+EIMS:")) && !parse_int(p, 0, 9, &v)) {
			s->eims = v;
			want_eims = v == 0;
		} else {
			/* cannot query (this phone answers +CME ERROR: unknown): the
			 * factory phone came up without AT+EIMS=1 (md_auto_setup_ims), so
			 * it is not sent unless a +EIMS: 0 URC said the stack is off
			 * (unverified) */
			want_eims = s->eims == 0;
			str_append(note, sizeof(note), "AT+EIMS? cannot query; ");
		}
	}
	if (want_eims) {
		vendor_ims_eims_cmd(1, cmd, sizeof(cmd));
		res = ims_cmd(m, cmd, &r, out, cap);
		if (res == AT_LOST || res == AT_TIMEOUT)
			return 1;
		if (res == AT_OK)
			s->eims_by_us = 1;
		else
			str_append(note, sizeof(note), "AT+EIMS=1 refused; ");
	} else {
		str_append(note, sizeof(note), "AT+EIMS=1 not sent; ");
	}
	if (note[0])
		note[strlen(note) - 2] = 0;
	snprintf(s->setup_status, sizeof(s->setup_status), "done%s%s%s", note[0] ? " (" : "", note,
		 note[0] ? ")" : "");
	return 0;
}

/*
 * radio_on_at_start: after the start-up of a (re)connected modem the radio
 * goes on as "radio on" does (radio_mask or 1), once per connection, unless
 * "radio off" was given in this daemon run. Without it a modem restarted by
 * the kernel stays at +CFUN: 4 and never registers.
 */
static void radio_start(struct md *m, char *out, size_t cap)
{
	struct ims_state *s = &m->ims;
	char err[160];

	if (s->radio_started || !m->cfg.radio_on_at_start || m->state != MD_READY)
		return;
	s->radio_started = 1;
	if (m->radio_user_off) {
		str_append(out, cap, "radio: left off (radio off was requested)\n");
		md_log(m, 0, "start-up: radio left off (radio off was requested)");
		return;
	}
	if (m->cfun == 1 && !m->cfg.radio_mask[0]) {
		str_append(out, cap, "radio: already on\n");
		return;
	}
	if (modem_set_radio(m, 1, NULL, err, sizeof(err))) {
		str_append(out, cap, "radio: on failed: %s\n", err);
		md_log(m, 0, "start-up: radio on failed: %s", err);
		md_event(m, "radio start failed %s", err);
		return;
	}
	str_append(out, cap, "radio: on\n");
	md_log(m, 0, "start-up: radio on (after the IMS/APN set-up)");
	md_event(m, "radio start on");
}

static void run_setup(struct md *m, char *out, size_t cap)
{
	struct ims_state *s = &m->ims;
	unsigned gen = s->gen;
	int rc = ims_setup(m, out, cap);

	if (gen != s->gen || m->state != MD_READY)
		return;		/* modem lost meanwhile: starts again when ready */
	if (rc == 2) {
		s->setup = 0;
		s->next_setup = mono_ms() + IMS_MIPC_POLL_MS;
		snprintf(s->setup_status, sizeof(s->setup_status), "pending (waiting for the MIPC session)");
		return;
	}
	if (rc > 0) {
		s->setup = 0;
		s->next_setup = mono_ms() + IMS_SETUP_RETRY_MS;
		snprintf(s->setup_status, sizeof(s->setup_status), "pending (no answer, retrying)");
		return;
	}
	s->setup = rc ? -1 : 1;
	if (s->on && !rc)
		s->next_cireg = mono_ms() + m->cfg.ims_reg_idle_poll_ms;
	md_log(m, 0, "ims: setup %s", s->setup_status);
	if (rc)
		md_event(m, "ims setup failed %s", s->setup_status);
	else
		md_event(m, "ims setup ok");
	radio_start(m, out, cap);
}

/* --- the IMS PDN raised by the modem --- */

static void link_pdn(struct md *m, struct ims_pdn *p, const char *v4, const char *v6, int mtu,
		     int change)
{
	static char out[2048];
	char tb[16];
	const char *q;
	char line[AT_LINE_MAX];

	out[0] = 0;
	if (data_pdn_link(m, &p->d, p->tid, v4, v6, mtu, out, sizeof(out))) {
		q = out;
		while (at_next_line(&q, line, sizeof(line)))
			md_log(m, 0, "ims: transIntfId %d: %s", p->tid, line);
		md_event(m, "ims pdn failed transIntfId=%d", p->tid);
		free_pdn(p);
		return;
	}
	p->st = IMS_PDN_UP;
	md_log(m, 0, "ims: %s %s: transIntfId %d, %s%s%s, type %s, aid %d (no route, no DNS)",
	       change ? "address change on" : "PDN up on", p->d.ifname, p->tid, p->d.v4,
	       p->d.v4[0] && p->d.v6[0] ? " " : "", p->d.v6, type_str(p->type, tb, sizeof(tb)),
	       p->aid);
	if (!change)
		m->ims.next_cireg = mono_ms() + m->cfg.ims_reg_poll_ms;	/* until registered */
	md_event(m, "ims pdn %s link=%s transIntfId=%d addr=%s", change ? "change" : "up",
		 p->d.ifname, p->tid, p->d.v4[0] ? p->d.v4 : p->d.v6[0] ? p->d.v6 : "-");
}

/* from data.c: INTERNAL_EIF_IND up / address change for an interface the
 * daemon did not request */
void ims_eif_up(struct md *m, unsigned cmd, int tid, const char *v4, const char *v6, int mtu)
{
	struct ims_state *s = &m->ims;
	struct ims_pdn *p;

	s->n_eif++;
	if (tid < 0)
		return;
	if (!s->on) {
		md_log(m, 0, "ims: interface %d up (not requested) while ims is off: no link", tid);
		return;
	}
	p = find_tid(m, tid);
	if (!v4[0] && !v6[0] && s->urc_tid == tid) {
		/* no address record: the +EIF URC's address (seen first) */
		v4 = s->urc_v4;
		v6 = s->urc_v6;
	}
	if (p && p->st == IMS_PDN_UP) {
		char o4[48], o6[48];

		/* repeated up or address change: the link is configured again,
		 * keeping its address when the indication has none */
		str_copy(o4, sizeof(o4), p->d.v4);
		str_copy(o6, sizeof(o6), p->d.v6);
		if (!v4[0] && !v6[0]) {
			v4 = o4;
			v6 = o6;
		}
		link_pdn(m, p, v4, v6, mtu, 1);
		return;
	}
	if (cmd != MIPC_EIF_UP && !p) {
		md_log(m, 0, "ims: interface %d change without an up: ignored", tid);
		return;
	}
	if (!p)
		p = alloc_pdn(m, tid);
	if (!p) {
		md_log(m, 0, "ims: no free slot for interface %d", tid);
		return;
	}
	link_pdn(m, p, v4, v6, mtu, 0);
}

/* from data.c: +EIF: <tid>, ifup, <type>, <ipv4>, <x> (AT port or the AT
 * tunnel; layout observed once): the address of the IMS PDN when the 0x4302
 * record had none */
void ims_eif_urc(struct md *m, int tid, const char *v4, const char *v6)
{
	struct ims_state *s = &m->ims;
	struct ims_pdn *p;

	if (tid < 0 || (!v4[0] && !v6[0]))
		return;
	s->urc_tid = tid;
	str_copy(s->urc_v4, sizeof(s->urc_v4), v4);
	str_copy(s->urc_v6, sizeof(s->urc_v6), v6);
	p = find_tid(m, tid);
	if (!p || p->st != IMS_PDN_UP || !s->on)
		return;
	if (!strcmp(p->d.v4, v4) && (!v6[0] || !strcmp(p->d.v6, v6)))
		return;
	{
		char a4[48], a6[48];

		str_copy(a4, sizeof(a4), v4);
		str_copy(a6, sizeof(a6), v6[0] ? v6 : p->d.v6);
		md_log(m, 0, "ims: address of %s from the +EIF URC: %s %s", p->d.ifname, a4, a6);
		link_pdn(m, p, a4, a6, p->d.mtu, 1);
	}
}

void ims_eif_down(struct md *m, int tid)
{
	struct ims_pdn *p = find_tid(m, tid);

	if (p)
		pdn_down(m, p, "eif ifdown");
}

/* IMS_PDN_IND 0x4A03, both forms */
void ims_mipc_ind(struct md *m, const struct mipc_msg *msg)
{
	struct ims_state *s = &m->ims;
	uint32_t action, v;
	int aid = -1, type = -1, iface = -1;
	struct ims_pdn *p;
	char tb[16];

	s->n_ind++;
	if (mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_IMSPDN_ACTION, &action)) {
		/* short form: the bookkeeping reset (= +EIMSPDN: "init"); the links
		 * stay until their EIF_IND down (unverified), the labels go */
		md_log(m, 0, "ims: IMS_PDN_IND reset");
		md_event(m, "ims pdn init");
		for (int i = 0; i < IMS_PDN_MAX; i++) {
			p = &s->pdn[i];
			if (p->st == IMS_PDN_LABELLED)
				free_pdn(p);
			else if (p->st == IMS_PDN_UP)
				p->aid = p->type = -1;
		}
		return;
	}
	if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_IMSPDN_AID, &v))
		aid = (int)v;
	if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_IMSPDN_TYPE, &v) && v < 0x10000)
		type = (int)v;
	if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_IMSPDN_IFACE, &v) && v < 100000)
		iface = (int)v;
	md_log(m, 0, "ims: IMS_PDN_IND aid %d action %u type %d interface %d", aid, action, type,
	       iface);
	md_event(m, "ims pdn ind aid=%d action=%u type=%s iface=%d", aid, action,
		 type_str(type, tb, sizeof(tb)), iface);
	p = iface >= 0 ? find_tid(m, iface) : NULL;
	if (!p && aid >= 0)
		p = find_aid(m, aid);
	if (action == 0) {
		if (p)
			pdn_down(m, p, "ims pdn ind action 0");
		return;
	}
	if (action != 1)
		return;
	if (!p && iface >= 0 && s->on) {
		/* before its EIF_IND: remembered, the link comes with the EIF_IND */
		p = alloc_pdn(m, iface);
		if (p)
			p->st = IMS_PDN_LABELLED;
	}
	if (p) {
		p->aid = aid;
		p->type = type;
	}
}

/* --- INTERNAL_EIPSPI_CMD 0x8305 -> INTERNAL_EIPSPI_RSP 0x8306 --- */

static uint32_t rand32(void)
{
	static int fd = -2;
	static uint64_t x;
	uint32_t v;

	if (fd == -2)
		fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0 && read(fd, &v, sizeof(v)) == (ssize_t)sizeof(v))
		return v;
	/* no urandom: xorshift seeded from the clock (still unique-checked) */
	if (!x)
		x = (uint64_t)mono_ms() * 0x9E3779B97F4A7C15ull | 1;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return (uint32_t)(x >> 16);
}

/* uniform in [lo, hi] (rejection sampling) */
static uint32_t rand_range(uint32_t lo, uint32_t hi)
{
	uint64_t range = (uint64_t)hi - lo + 1, limit;
	uint32_t r;

	if (range > 0xFFFFFFFFull)
		return rand32();
	limit = (0x100000000ull / range) * range;
	do
		r = rand32();
	while ((uint64_t)r >= limit);
	return lo + (uint32_t)(r % range);
}

static int spi_used(struct ims_state *s, uint32_t spi)
{
	for (int i = 0; i < s->nspi; i++)
		if (s->spi[i] == spi)
			return 1;
	return 0;
}

static void log_addr(struct md *m, const struct mipc_msg *msg, unsigned id, const char *name)
{
	const uint8_t *a;
	size_t len;
	char buf[64];

	a = mipc_tlv_find(msg->payload, msg->len, id, &len);
	if (!a || len < 8)
		return;
	if (mipc_get(a, 4) == 4)
		snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a[4], a[5], a[6], a[7]);
	else if (len >= 20)
		snprintf(buf, sizeof(buf), "%x:%x:%x:%x:%x:%x:%x:%x", a[4] << 8 | a[5],
			 a[6] << 8 | a[7], a[8] << 8 | a[9], a[10] << 8 | a[11], a[12] << 8 | a[13],
			 a[14] << 8 | a[15], a[16] << 8 | a[17], a[18] << 8 | a[19]);
	else
		snprintf(buf, sizeof(buf), "family %u", mipc_get(a, 4));
	md_log(m, 1, "ims: eipspi %s %s", name, buf);
}

void ims_eipspi_cmd(struct md *m, const struct mipc_msg *msg)
{
	struct ims_state *s = &m->ims;
	uint32_t index = 0, lo = MIPC_SPI_DEFAULT_MIN, hi = MIPC_SPI_DEFAULT_MAX, v, spi = 0;
	struct mipc_buf b;
	uint8_t mem[64];
	int result = 0;

	s->n_spi++;
	mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_SPI_INDEX, &index);
	if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_SPI_MIN, &v))
		lo = v;
	if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_SPI_MAX, &v))
		hi = v;
	log_addr(m, msg, MIPC_T_SPI_ADDR1, "address 1 (0x8102)");
	log_addr(m, msg, MIPC_T_SPI_ADDR2, "address 2 (0x8103)");
	if (!mipc_tlv_find_uint(msg->payload, msg->len, MIPC_T_SPI_PROTO, &v))
		md_log(m, 1, "ims: eipspi protocol %u", v);
	if (lo > hi) {
		result = 1;	/* failure value unverified */
	} else {
		uint64_t range = (uint64_t)hi - lo + 1;
		int tries = 0;

		if (range <= IMS_SPI_MAX) {
			/* small range: uniform among the values not handed out yet */
			uint32_t free_v[IMS_SPI_MAX];
			int nfree = 0;

			for (uint64_t x = lo; x <= hi; x++)
				if (!spi_used(s, (uint32_t)x))
					free_v[nfree++] = (uint32_t)x;
			spi = nfree ? free_v[rand_range(0, (uint32_t)nfree - 1)] : 0;
			if (!nfree)
				result = 1;
		} else {
			do
				spi = rand_range(lo, hi);
			while (spi_used(s, spi) && ++tries < 1000);
			if (spi_used(s, spi))
				result = 1;
		}
		if (result) {
			spi = 0;	/* every value of the range was handed out */
		} else {
			/* the table keeps the last IMS_SPI_MAX values of this session */
			s->spi[s->spi_pos] = spi;
			s->spi_pos = (s->spi_pos + 1) % IMS_SPI_MAX;
			if (s->nspi < IMS_SPI_MAX)
				s->nspi++;
		}
	}
	mipc_buf_init(&b, mem, sizeof(mem));
	mipc_tlv_add_u32(&b, MIPC_T_SPI_RSP_INDEX, index);
	mipc_tlv_add_u8(&b, MIPC_T_SPI_RSP_RESULT, (uint8_t)result);
	mipc_tlv_add_u32(&b, MIPC_T_SPI_RSP_SPI, spi);
	md_log(m, 0, "ims: EIPSPI txid %u index %u range 0x%x..0x%x -> %s 0x%08x", msg->txid, index,
	       lo, hi, result ? "refused" : "spi", spi);
	if (result)
		md_event(m, "ims spi index=%u refused", index);
	else
		md_event(m, "ims spi index=%u spi=0x%08x", index, spi);
	if (mipc_send_rsp(m, MIPC_INTERNAL_EIPSPI_RSP, msg->txid, msg->slot, b.data, b.len))
		md_log(m, 0, "ims: EIPSPI answer not sent");
}

/* --- AT URCs --- */

static void push_ev(struct md *m, int aid, int action)
{
	struct ims_state *s = &m->ims;

	if (s->nev >= IMS_EV_MAX) {
		md_log(m, 0, "ims: confirm queue full, oldest dropped");
		memmove(&s->ev[0], &s->ev[1], sizeof(s->ev[0]) * (IMS_EV_MAX - 1));
		s->nev--;
	}
	s->ev[s->nev].aid = aid;
	s->ev[s->nev].action = action;
	s->nev++;
}

int ims_urc(struct md *m, const char *line)
{
	struct ims_state *s = &m->ims;
	struct eimspdn e;
	int v;

	if (!strncmp(line, "+EIMSPDN:", 9)) {
		if (vendor_parse_eimspdn(line + 9, &e)) {
			md_log(m, 0, "ims: malformed %s", line);
			md_event(m, "ims %s", line);
			return 1;
		}
		if (!strcmp(e.kind, "init")) {
			md_event(m, "ims pdn init");
		} else if (!strcmp(e.kind, "notify")) {
			/* AT fallback: confirm only, the modem raises the PDN itself
			 * (unverified: never observed on this phone) */
			s->n_at_notify++;
			md_log(m, 0, "ims: AT notify aid %d action %d type %s: confirm only", e.aid,
			       e.action, e.type);
			md_event(m, "ims pdn notify aid=%d action=%d type=%s", e.aid, e.action, e.type);
			if (e.action == 0 || e.action == 1)
				push_ev(m, e.aid, e.action);
		} else if (!strcmp(e.kind, "reject")) {
			s->n_reject++;
			snprintf(s->last_reject, sizeof(s->last_reject), "cause %d type %s", e.cause,
				 e.type[0] ? e.type : "-");
			md_log(m, 0, "ims: the network rejected the IMS PDN: %s", s->last_reject);
			md_event(m, "ims pdn reject cause=%d type=%s", e.cause, e.type[0] ? e.type : "-");
		} else {
			md_event(m, "ims %s", line);
		}
		return 1;
	}
	if (!strncmp(line, "+EIMSPDNOOS:", 12)) {
		const char *a = line + 12;

		s->n_oos++;
		while (*a == ' ')
			a++;
		md_log(m, 0, "ims: IMS PDN out of service: %s", a);
		md_event(m, "ims pdn oos %s", a);
		return 1;
	}
	if (!strncmp(line, "+EIMS:", 6)) {
		if (!parse_int(line + 6, 0, 9, &v))
			s->eims = v;
		md_event(m, "ims %s", line);
		return 1;
	}
	if (!strncmp(line, "+EIMCFLAG:", 10)) {
		if (!parse_int(line + 10, 0, 9, &v))
			s->eimcflag = v;
		md_event(m, "ims %s", line);
		return 1;
	}
	return 0;
}

/* --- the tick --- */

void ims_tick(struct md *m, int64_t now)
{
	struct ims_state *s = &m->ims;

	if (m->state != MD_READY)
		return;
	if (!s->setup && now >= s->next_setup) {
		static char out[REPLY_MAX / 4];
		const char *q = out;
		char line[AT_LINE_MAX];

		out[0] = 0;
		run_setup(m, out, sizeof(out));
		while (at_next_line(&q, line, sizeof(line)))
			md_log(m, 1, "ims: %s", line);
		if (m->state != MD_READY)
			return;
	}
	if (s->on && s->next_cireg && now >= s->next_cireg && m->at.fd >= 0) {
		int fast = 0;

		for (int i = 0; i < IMS_PDN_MAX; i++)
			fast |= s->pdn[i].st == IMS_PDN_UP;
		modem_refresh_ims(m);
		if (m->state != MD_READY)
			return;
		s->next_cireg = mono_ms() + (fast && m->ims_reg != 1 ? m->cfg.ims_reg_poll_ms :
					     m->cfg.ims_reg_idle_poll_ms);
	}
	if (s->apn_need_internet && now >= s->apn_retry_at && mipc_ready(m)) {
		int reg = m->eps.stat == 1 || m->eps.stat == 5 || m->gprs.stat == 1 ||
			  m->gprs.stat == 5;

		s->apn_retry_at = now + IMS_APN_RETRY_MS;
		if (reg) {
			static char out[4096];
			int with_internet = 0;

			out[0] = 0;
			if (!data_apn_profiles(m, out, sizeof(out), &with_internet) && with_internet) {
				s->apn_need_internet = 0;
				md_log(m, 0, "ims: APN profile list re-sent with the internet profile");
				md_event(m, "apn profiles sent with internet");
			} else if (++s->apn_tries >= IMS_APN_TRIES) {
				s->apn_need_internet = 0;
				md_log(m, 0, "ims: no attach APN known: the internet profile stays out");
			}
			if (m->state != MD_READY)
				return;
		}
	}
	while (s->nev > 0 && m->state == MD_READY && m->at.fd >= 0) {
		struct ims_ev ev = s->ev[0];
		struct at_resp r;
		char cmd[AT_CMD_MAX], e[96];

		memmove(&s->ev[0], &s->ev[1], sizeof(s->ev[0]) * (size_t)(s->nev - 1));
		s->nev--;
		vendor_ims_confirm_cmd(ev.aid, IMS_CONFIRM_OK, cmd, sizeof(cmd));
		e[0] = 0;
		ims_cmd(m, cmd, &r, e, sizeof(e));
		if (r.res == AT_OK) {
			md_event(m, "ims pdn confirm aid=%d result=%d", ev.aid, IMS_CONFIRM_OK);
		} else {
			at_resp_error(&r, e, sizeof(e));
			md_event(m, "ims pdn confirm aid=%d result=%d refused %s", ev.aid,
				 IMS_CONFIRM_OK, e);
		}
	}
}

int64_t ims_next_deadline(struct md *m)
{
	struct ims_state *s = &m->ims;
	int64_t dl = INT64_MAX;

	if (m->state != MD_READY)
		return dl;
	if (!s->setup)
		dl = s->next_setup;
	if (s->on && s->next_cireg && s->next_cireg < dl)
		dl = s->next_cireg;
	if (s->apn_need_internet && s->apn_retry_at < dl)
		dl = s->apn_retry_at;
	if (s->nev > 0)
		dl = 0;
	return dl;
}

/* --- losses, stack change, exit --- */

void ims_mipc_lost(struct md *m)
{
	struct ims_state *s = &m->ims;

	all_down(m, "mipc lost");
	s->nspi = s->spi_pos = 0;	/* a new session may reuse SPIs */
	/* the IMS configuration is sent again in the next session */
	s->gen++;
	s->setup = 0;
	s->next_setup = 0;
	s->mipc_wait_since = 0;
	if (s->on)
		str_copy(s->setup_status, sizeof(s->setup_status), "pending");
}

void ims_modem_lost(struct md *m)
{
	struct ims_state *s = &m->ims;

	all_down(m, "modem lost");
	s->radio_started = 0;	/* a reconnected modem gets its radio back */
	s->next_cireg = 0;
	s->apn_need_internet = 0;
	s->urc_tid = -1;
	s->nev = 0;
	s->gen++;
	s->setup = 0;
	s->next_setup = 0;
	s->mipc_wait_since = 0;
	s->eims = s->eimcflag = -1;
	s->eims_by_us = 0;
	s->ext = -1;
	if (s->on)
		str_copy(s->setup_status, sizeof(s->setup_status), "pending");
}

void ims_stack_change(struct md *m)
{
	struct ims_state *s = &m->ims;

	all_down(m, "stack change");
	s->nev = 0;
	s->gen++;
	s->setup = 0;
	s->next_setup = 0;
	s->mipc_wait_since = 0;
	s->eims = s->eimcflag = -1;
	s->eims_by_us = 0;
	if (s->on)
		str_copy(s->setup_status, sizeof(s->setup_status), "pending");
}

void ims_shutdown(struct md *m)
{
	if (m->cfg.keep_data_on_exit) {
		for (int i = 0; i < IMS_PDN_MAX; i++)
			if (m->ims.pdn[i].st == IMS_PDN_UP)
				md_log(m, 0, "exit: IMS link %s kept (keep_data_on_exit)",
				       m->ims.pdn[i].d.ifname);
		return;
	}
	all_down(m, "exit");
}

/* --- command, status --- */

void ims_status(struct md *m, char *out, size_t cap)
{
	struct ims_state *s = &m->ims;
	char tb[16];

	str_append(out, cap, "ims: %s, setup %s\n", s->on ? "on" : "off",
		   s->setup == 0 && m->state != MD_READY ? "pending (modem not ready)" : s->setup_status);
	str_append(out, cap, "ims stack: +EIMS %d +EIMCFLAG %d%s\n", s->eims, s->eimcflag,
		   s->eims_by_us ? " (AT+EIMS=1 sent)" : "");
	for (int i = 0; i < IMS_PDN_MAX; i++) {
		const struct ims_pdn *p = &s->pdn[i];

		if (p->st == IMS_PDN_UP)
			str_append(out, cap, "ims pdn: type %s aid %d up, link %s, addr %s%s%s, "
				   "transIntfId %d, mtu %d (raised by the modem)\n",
				   type_str(p->type, tb, sizeof(tb)), p->aid, p->d.ifname, p->d.v4,
				   p->d.v4[0] && p->d.v6[0] ? " " : "",
				   p->d.v6[0] ? p->d.v6 : p->d.v4[0] ? "" : "-", p->tid, p->d.mtu);
		else if (p->st == IMS_PDN_LABELLED)
			str_append(out, cap, "ims pdn: type %s aid %d announced for transIntfId %d, "
				   "no interface indication yet\n", type_str(p->type, tb, sizeof(tb)),
				   p->aid, p->tid);
	}
	str_append(out, cap, "ims counters: eif %lu ind %lu spi %lu (in use %d) at-notify %lu "
		   "reject %lu oos %lu%s%s\n", s->n_eif, s->n_ind, s->n_spi, s->nspi, s->n_at_notify,
		   s->n_reject, s->n_oos, s->last_reject[0] ? ", last reject " : "", s->last_reject);
}

void ims_snapshot(struct md *m, char *out, size_t cap)
{
	char tb[16];

	str_append(out, cap, "ims %s\n", m->ims.on ? "on" : "off");
	for (int i = 0; i < IMS_PDN_MAX; i++) {
		const struct ims_pdn *p = &m->ims.pdn[i];

		if (p->st == IMS_PDN_UP)
			str_append(out, cap, "ims pdn up link=%s transIntfId=%d addr=%s type=%s aid=%d\n",
				   p->d.ifname, p->tid, p->d.v4[0] ? p->d.v4 : p->d.v6[0] ? p->d.v6 : "-",
				   type_str(p->type, tb, sizeof(tb)), p->aid);
	}
}

static int ims_off(struct md *m, char *out, size_t cap)
{
	struct ims_state *s = &m->ims;
	struct at_resp r;
	char cmd[AT_CMD_MAX];

	s->on = 0;	/* the start-up state stays: ims on runs the IMS part again */
	if (m->state == MD_READY) {
		/* the observed form first, the plain one on a refusal */
		vendor_ims_onoff_cmd(0, 1, cmd, sizeof(cmd));
		if (ims_cmd(m, cmd, &r, out, cap) != AT_OK && r.res != AT_LOST &&
		    r.res != AT_TIMEOUT) {
			vendor_ims_onoff_cmd(0, 0, cmd, sizeof(cmd));
			ims_cmd(m, cmd, &r, out, cap);
		}
	}
	/* our links go; nothing is sent over MIPC (the modem owns the PDN) */
	for (int i = 0; i < IMS_PDN_MAX; i++)
		if (s->pdn[i].st == IMS_PDN_UP)
			str_append(out, cap, "link %s (transIntfId %d) removed\n", s->pdn[i].d.ifname,
				   s->pdn[i].tid);
	all_down(m, "ims off");
	if (s->eims_by_us && m->state == MD_READY) {
		vendor_ims_eims_cmd(0, cmd, sizeof(cmd));
		ims_cmd(m, cmd, &r, out, cap);
		s->eims_by_us = 0;
	}
	md_event(m, "ims off");
	return 0;
}

/* apn profiles: the APN profile list again */
int ims_apn_command(struct md *m, char *out, size_t cap)
{
	if (m->state != MD_READY || !mipc_ready(m)) {
		str_append(out, cap, "error: MIPC session not ready\n");
		return -1;
	}
	if (data_apn_profiles(m, out, cap, NULL)) {
		str_append(out, cap, "error: APN_ADD_PROFILE_LIST_REQ failed\n");
		return -1;
	}
	md_event(m, "apn profiles sent");
	return 0;
}

int ims_command(struct md *m, int argc, char **argv, char *out, size_t cap)
{
	struct ims_state *s = &m->ims;

	if (argc >= 1 && !strcmp(argv[0], "status")) {
		ims_status(m, out, cap);
		return 0;
	}
	if (argc >= 1 && !strcmp(argv[0], "off"))
		return ims_off(m, out, cap);
	if (argc >= 1 && !strcmp(argv[0], "config")) {
		/* the feature-switch form again (factory re-sent it right before the
		 * modem raised the IMS PDN) */
		if (m->state != MD_READY || !mipc_ready(m)) {
			str_append(out, cap, "error: MIPC session not ready\n");
			return -1;
		}
		if (ims_mipc_cfg(m, 0, out, cap)) {
			str_append(out, cap, "error: IMS_SET_CONFIG_REQ failed\n");
			return -1;
		}
		md_event(m, "ims config sent");
		return 0;
	}
	if (argc >= 1 && !strcmp(argv[0], "on")) {
		int was_on = s->on;

		s->on = 1;
		md_event(m, "ims on");
		if (was_on && s->setup == 1) {
			str_append(out, cap, "ims: already on, setup %s\n", s->setup_status);
			return 0;
		}
		s->setup = 0;
		s->next_setup = 0;
		str_copy(s->setup_status, sizeof(s->setup_status), "pending");
		if (m->state != MD_READY) {
			str_append(out, cap, "ims: on, setup when the modem is ready\n");
			return 0;
		}
		run_setup(m, out, cap);
		if (s->setup < 0) {
			str_append(out, cap, "error: ims setup %s\n", s->setup_status);
			return -1;
		}
		str_append(out, cap, "ims: on, setup %s\n", s->setup_status);
		return 0;
	}
	str_append(out, cap, "error: ims on|off|config|status\n");
	return -1;
}
