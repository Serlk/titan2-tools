// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: MIPC transport.
 * Port open/reopen; one message per write; reads reassemble header + len
 * from the byte stream (a WWAN port delivers whole messages, a pty in the
 * tests delivers fragments); request/confirm matched by the transaction id;
 * indications dispatched by message class; timeouts; TLV builder/parser;
 * the session start (TEST -> OPEN -> REGISTER_IND).
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modemd.h"
#include "mipc.h"

/* indications registered per slot (IMS_PDN_IND for ims.c), then
 * the modem->AP commands we answer (INTERNAL_EIPSPI_CMD) */
static const unsigned reg_inds[] = {
	MIPC_INTERNAL_EIF_IND, MIPC_DATA_ACT_CALL_IND, MIPC_DATA_DEACT_CALL_IND,
	MIPC_SYS_AT_IND, MIPC_IMS_PDN_IND,
	MIPC_INTERNAL_EIPSPI_CMD,
};
#define N_REG_INDS (sizeof(reg_inds) / sizeof(reg_inds[0]))
#define REG_IS_CMD(id) MIPC_IS_CMD(id)

/* --- bytes (little-endian) --- */

void mipc_put(uint8_t *p, size_t size, uint32_t v)
{
	for (size_t i = 0; i < size; i++)
		p[i] = (uint8_t)(i < 4 ? v >> (8 * i) : 0);
}

uint32_t mipc_get(const uint8_t *p, size_t size)
{
	uint32_t v = 0;

	for (size_t i = 0; i < size && i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}

/* --- TLV --- */

void mipc_buf_init(struct mipc_buf *b, uint8_t *mem, size_t cap)
{
	b->data = mem;
	b->cap = cap;
	b->len = 0;
	b->overflow = 0;
}

int mipc_tlv_add(struct mipc_buf *b, unsigned id, const void *val, size_t len)
{
	size_t need;

	if (len == 0 || len > 0xFFFF)	/* len 0 is an array descriptor: not built here */
		goto fail;
	need = MIPC_TLV_SIZE(len);
	if (b->overflow || need > b->cap - b->len)
		goto fail;
	memset(b->data + b->len, 0, need);
	mipc_put(b->data + b->len, 2, id);
	mipc_put(b->data + b->len + 2, 2, (uint32_t)len);
	memcpy(b->data + b->len + MIPC_TLV_HDR, val, len);
	b->len += need;
	return 0;
fail:
	b->overflow = 1;
	return -1;
}

int mipc_tlv_add_array(struct mipc_buf *b, unsigned id, unsigned first, unsigned count)
{
	if (b->overflow || MIPC_TLV_DESCR_SIZE > b->cap - b->len) {
		b->overflow = 1;
		return -1;
	}
	memset(b->data + b->len, 0, MIPC_TLV_DESCR_SIZE);
	mipc_put(b->data + b->len, 2, id);
	/* len 0; body: u32 0, u32 0, u16 first, u16 count */
	mipc_put(b->data + b->len + MIPC_TLV_HDR + 8, 2, first);
	mipc_put(b->data + b->len + MIPC_TLV_HDR + 10, 2, count);
	b->len += MIPC_TLV_DESCR_SIZE;
	return 0;
}

int mipc_tlv_add_u8(struct mipc_buf *b, unsigned id, uint8_t v)
{
	return mipc_tlv_add(b, id, &v, 1);
}

int mipc_tlv_add_u16(struct mipc_buf *b, unsigned id, uint16_t v)
{
	uint8_t t[2];

	mipc_put(t, 2, v);
	return mipc_tlv_add(b, id, t, 2);
}

int mipc_tlv_add_u32(struct mipc_buf *b, unsigned id, uint32_t v)
{
	uint8_t t[4];

	mipc_put(t, 4, v);
	return mipc_tlv_add(b, id, t, 4);
}

/* string with its terminating NUL (len includes it) */
int mipc_tlv_add_str(struct mipc_buf *b, unsigned id, const char *s)
{
	return mipc_tlv_add(b, id, s, strlen(s) + 1);
}

int mipc_tlv_next(const uint8_t *p, size_t len, size_t *off, struct mipc_tlv *t)
{
	size_t o = *off, vlen, size;

	if (o == len)
		return 0;
	if (o > len || len - o < MIPC_TLV_HDR)
		return -1;
	t->raw_id = mipc_get(p + o, 2);
	t->id = t->raw_id & MIPC_TLV_ID_MASK;
	vlen = mipc_get(p + o + 2, 2);
	size = MIPC_TLV_SIZE(vlen);
	if (size > len - o) {
		/* the last element may be unpadded */
		if (vlen && MIPC_TLV_HDR + vlen <= len - o)
			size = len - o;
		else
			return -1;
	}
	t->val = p + o + MIPC_TLV_HDR;
	t->len = vlen;
	*off = o + size;
	return 1;
}

const uint8_t *mipc_tlv_find(const uint8_t *p, size_t len, unsigned id, size_t *vlen)
{
	struct mipc_tlv t;
	size_t off = 0;

	id &= MIPC_TLV_ID_MASK;
	while (mipc_tlv_next(p, len, &off, &t) == 1) {
		if (t.id == id && t.len) {
			if (vlen)
				*vlen = t.len;
			return t.val;
		}
	}
	return NULL;
}

/* a record that is either a plain TLV or an array (descriptor with that id):
 * the first element of the array */
const uint8_t *mipc_tlv_find_rec(const uint8_t *p, size_t len, unsigned id, size_t *vlen)
{
	struct mipc_tlv t;
	size_t off = 0;
	const uint8_t *v = mipc_tlv_find(p, len, id, vlen);

	if (v)
		return v;
	id &= MIPC_TLV_ID_MASK;
	while (mipc_tlv_next(p, len, &off, &t) == 1)
		if (t.id == id && !t.len && off <= len) {
			unsigned first = mipc_get(t.val + 8, 2), count = mipc_get(t.val + 10, 2);

			if (count)
				return mipc_tlv_find(p, len, first, vlen);
			return NULL;
		}
	return NULL;
}

int mipc_tlv_find_uint(const uint8_t *p, size_t len, unsigned id, uint32_t *v)
{
	size_t vlen;
	const uint8_t *val = mipc_tlv_find(p, len, id, &vlen);

	if (!val || vlen > 4)
		return -1;
	*v = mipc_get(val, vlen);
	return 0;
}

int mipc_tlv_find_str(const uint8_t *p, size_t len, unsigned id, char *out, size_t cap)
{
	size_t vlen, l;
	const uint8_t *val = mipc_tlv_find(p, len, id, &vlen);

	if (!val)
		return -1;
	l = strnlen((const char *)val, vlen);
	if (l >= cap)
		l = cap - 1;
	memcpy(out, val, l);
	out[l] = 0;
	str_sanitize(out);
	return 0;
}

/* result TLV 0x0000; a CNF without it counts as result 2 */
int mipc_tlv_result(const uint8_t *p, size_t len, uint32_t *result)
{
	if (mipc_tlv_find_uint(p, len, MIPC_T_RESULT, result)) {
		*result = 2;
		return -1;
	}
	return 0;
}

/* --- framing --- */

size_t mipc_frame_build(uint8_t *out, size_t cap, unsigned msg_id, unsigned txid,
			unsigned slot, const uint8_t *payload, size_t len)
{
	if (cap < MIPC_HDR_SIZE || len > cap - MIPC_HDR_SIZE || len >= MIPC_PAYLOAD_MAX)
		return 0;
	memset(out, 0, MIPC_HDR_SIZE);
	mipc_put(out + MIPC_HDR_MAGIC_OFF, 4, MIPC_MAGIC);
	out[MIPC_HDR_SLOT_OFF] = (uint8_t)slot;
	mipc_put(out + MIPC_HDR_MSGID_OFF, 2, msg_id);
	mipc_put(out + MIPC_HDR_TXID_OFF, 2, txid);
	mipc_put(out + MIPC_HDR_LEN_OFF, 2, (uint32_t)len);
	if (len)
		memcpy(out + MIPC_HDR_SIZE, payload, len);
	return MIPC_HDR_SIZE + len;
}

int mipc_frame_parse(const uint8_t *buf, size_t len, struct mipc_msg *msg,
		     size_t *consumed)
{
	size_t plen;

	if (len < MIPC_HDR_SIZE)
		return 0;
	if (mipc_get(buf + MIPC_HDR_MAGIC_OFF, 4) != MIPC_MAGIC)
		return -1;
	plen = mipc_get(buf + MIPC_HDR_LEN_OFF, 2);
	if (plen > MIPC_MSG_MAX - MIPC_HDR_SIZE)
		return -1;
	if (len < MIPC_HDR_SIZE + plen)
		return 0;
	msg->slot = buf[MIPC_HDR_SLOT_OFF];
	msg->msg_id = mipc_get(buf + MIPC_HDR_MSGID_OFF, 2);
	msg->txid = mipc_get(buf + MIPC_HDR_TXID_OFF, 2);
	msg->payload = buf + MIPC_HDR_SIZE;
	msg->len = plen;
	*consumed = MIPC_HDR_SIZE + plen;
	return 1;
}

/* --- the transport --- */

const char *mipc_res_str(enum mipc_res r)
{
	switch (r) {
	case MIPC_PENDING: return "pending";
	case MIPC_OK: return "ok";
	case MIPC_TIMEOUT: return "timeout";
	case MIPC_LOST: return "port lost";
	case MIPC_REFUSED: return "refused";
	}
	return "?";
}

static const char *sess_str(enum mipc_sess s)
{
	switch (s) {
	case SESS_NONE: return "closed";
	case SESS_TEST: return "test";
	case SESS_OPEN: return "open";
	case SESS_REGISTER: return "register";
	case SESS_READY: return "ready";
	}
	return "?";
}

void mipc_init(struct md *m)
{
	struct mipc *p = &m->mipc;

	memset(p, 0, sizeof(*p));
	p->fd = -1;
	p->next_txid = 0;	/* starts at 0 */
	p->enabled = !strcmp(m->cfg.data_backend, "mipc") && m->cfg.mipc_port[0];
}

int mipc_ready(struct md *m)
{
	return m->mipc.fd >= 0 && m->mipc.sess == SESS_READY;
}

/* header slot bit for the selected SIM stack */
unsigned mipc_slot(struct md *m)
{
	return 1u << ((m->sim_sel > 0 ? m->sim_sel : 1) - 1);
}

static void complete(struct md *m, struct mipc_pending *pd, enum mipc_res res,
		     const struct mipc_msg *msg)
{
	struct mipc_cnf local, *c = pd->out ? pd->out : &local;
	mipc_cb cb = pd->cb;
	void *ctx = pd->ctx;

	c->res = res;
	c->txid = pd->txid;
	c->msg_id = msg ? msg->msg_id : 0;
	c->len = 0;
	if (msg && msg->len <= sizeof(c->payload)) {
		memcpy(c->payload, msg->payload, msg->len);
		c->len = msg->len;
	}
	pd->used = 0;
	md_log(m, 1, "mipc: req 0x%04x txid %u -> %s", pd->req_id, pd->txid, mipc_res_str(res));
	if (cb)
		cb(m, c, ctx);
}

void mipc_close(struct md *m, const char *why)
{
	struct mipc *p = &m->mipc;

	if (p->fd < 0)
		return;
	md_log(m, 0, "mipc: %s closed: %s", m->cfg.mipc_port, why);
	close(p->fd);
	p->fd = -1;
	p->rxlen = 0;
	p->sess = SESS_NONE;
	for (int i = 0; i < MIPC_PENDING_MAX; i++)
		if (p->pend[i].used)
			complete(m, &p->pend[i], MIPC_LOST, NULL);
	data_mipc_lost(m);
	p->reopen_at = mono_ms() + m->cfg.reopen_ms;
	md_event(m, "mipc lost %s", why);
}

static int write_msg(struct md *m, const uint8_t *buf, size_t len)
{
	struct mipc *p = &m->mipc;
	size_t off = 0;

	for (int i = 0; i < 50 && off < len; i++) {
		ssize_t n = write(p->fd, buf + off, len - off);

		if (n > 0) {
			off += (size_t)n;	/* partial writes are continued */
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN) {
			struct pollfd pf = { .fd = p->fd, .events = POLLOUT };

			poll(&pf, 1, 100);
			continue;
		}
		md_log(m, 0, "mipc: write: %s", n < 0 ? strerror(errno) : "nothing written");
		return -2;
	}
	return off == len ? 0 : -1;
}

int mipc_send(struct md *m, unsigned msg_id, unsigned slot, const uint8_t *payload,
	      size_t len, int timeout_ms, mipc_cb cb, void *ctx, struct mipc_cnf *out)
{
	struct mipc *p = &m->mipc;
	struct mipc_pending *pd = NULL;
	uint8_t frame[MIPC_MSG_MAX];
	size_t flen;
	unsigned txid;
	int ret;

	if (p->fd < 0)
		return -1;
	for (int i = 0; i < MIPC_PENDING_MAX; i++)
		if (!p->pend[i].used) {
			pd = &p->pend[i];
			break;
		}
	if (!pd) {
		md_log(m, 0, "mipc: too many pending requests");
		return -1;
	}
	txid = p->next_txid;
	flen = mipc_frame_build(frame, sizeof(frame), msg_id, txid, slot, payload, len);
	if (!flen)
		return -1;
	p->next_txid = (p->next_txid + 1) & 0xFFFF;
	if (p->next_txid == MIPC_TXID_NONE)
		p->next_txid = 0;
	memset(pd, 0, sizeof(*pd));
	pd->used = 1;
	pd->txid = txid;
	pd->req_id = msg_id;
	pd->deadline = mono_ms() + (timeout_ms > 0 ? timeout_ms : m->cfg.mipc_timeout_ms);
	pd->cb = cb;
	pd->ctx = ctx;
	pd->out = out;
	if (out)
		out->res = MIPC_PENDING;
	p->n_tx++;
	md_log(m, 1, "mipc: > msg 0x%04x slot 0x%02x txid %u, %zu payload bytes", msg_id, slot,
	       txid, len);
	ret = write_msg(m, frame, flen);
	if (ret == -2) {
		mipc_close(m, "write error");
		return -1;
	}
	if (ret) {
		complete(m, pd, MIPC_REFUSED, NULL);
		return -1;
	}
	return (int)txid;
}

/* the answer to a modem->AP command: RSP id, the command's txid and header
 * slot byte (echoing the slot byte is unverified) */
int mipc_send_rsp(struct md *m, unsigned msg_id, unsigned txid, unsigned slot,
		  const uint8_t *payload, size_t len)
{
	uint8_t frame[MIPC_MSG_MAX];
	size_t flen;
	int ret;

	if (m->mipc.fd < 0)
		return -1;
	flen = mipc_frame_build(frame, sizeof(frame), msg_id, txid, slot, payload, len);
	if (!flen)
		return -1;
	m->mipc.n_tx++;
	md_log(m, 1, "mipc: > rsp 0x%04x slot 0x%02x txid %u, %zu payload bytes", msg_id, slot,
	       txid, len);
	ret = write_msg(m, frame, flen);
	if (ret == -2)
		mipc_close(m, "write error");
	return ret ? -1 : 0;
}

/* SYS_AT_REQ 0x0003 on the stack's slot, TLV 0x8100 = the command without
 * CR/LF; CNF 0x0004: result 0x0000 and 0x8100 = the response text */
int mipc_at(struct md *m, const char *cmd, char *resp, size_t cap)
{
	static struct mipc_cnf c;
	struct mipc_buf b;
	uint8_t mem[AT_CMD_MAX + 16];
	uint32_t res = 0;

	resp[0] = 0;
	if (!mipc_ready(m) || strlen(cmd) >= AT_CMD_MAX)
		return -1;
	mipc_buf_init(&b, mem, sizeof(mem));
	mipc_tlv_add_str(&b, MIPC_T_AT_STRING, cmd);
	if (b.overflow)
		return -1;
	md_log(m, 1, "mipc: at tunnel > %s", cmd);
	if (mipc_request(m, MIPC_SYS_AT_REQ, mipc_slot(m), b.data, b.len, m->cfg.mipc_timeout_ms,
			 &c) != MIPC_OK)
		return -1;
	mipc_tlv_result(c.payload, c.len, &res);
	if (!mipc_tlv_find_str(c.payload, c.len, MIPC_T_AT_STRING, resp, cap))
		str_sanitize(resp);
	md_log(m, 1, "mipc: at tunnel < result %u: %s", res, resp);
	return (int)res;
}

static void at_tunnel_ind(struct md *m, const struct mipc_msg *msg)
{
	char line[AT_LINE_MAX];

	m->mipc.n_at_ind++;
	if (mipc_tlv_find_str(msg->payload, msg->len, MIPC_T_AT_STRING, line, sizeof(line)))
		return;
	line[strcspn(line, "\r\n")] = 0;
	md_log(m, 1, "mipc: at ind: %s", line);
	md_event(m, "urc-mipc %s", line);
	/* +EIF: carries the IMS PDN's address when the 0x4302 record has none */
	if (!strncmp(line, "+EIF:", 5))
		data_urc(m, line);
}

static void handle_msg(struct md *m, const struct mipc_msg *msg)
{
	struct mipc *p = &m->mipc;

	p->n_rx++;
	if (MIPC_CLASS(msg->msg_id) == MIPC_CLASS_REQCNF) {
		for (int i = 0; i < MIPC_PENDING_MAX; i++) {
			struct mipc_pending *pd = &p->pend[i];

			if (pd->used && pd->txid == msg->txid) {
				complete(m, pd, MIPC_OK, msg);
				return;
			}
		}
		md_log(m, 0, "mipc: confirm 0x%04x txid %u without a request", msg->msg_id,
		       msg->txid);
		return;
	}
	if (msg->msg_id == MIPC_INTERNAL_EIPSPI_CMD) {
		ims_eipspi_cmd(m, msg);
		return;
	}
	if (MIPC_IS_CMD(msg->msg_id)) {
		/* another modem -> AP command: not registered; log only */
		md_log(m, 0, "mipc: unexpected command 0x%04x txid %u", msg->msg_id, msg->txid);
		md_event(m, "mipc cmd 0x%04x", msg->msg_id);
		return;
	}
	p->n_ind++;
	md_log(m, 1, "mipc: ind 0x%04x slot 0x%02x, %zu bytes", msg->msg_id, msg->slot, msg->len);
	if (msg->msg_id == MIPC_SYS_AT_IND) {
		at_tunnel_ind(m, msg);
		return;
	}
	md_event(m, "mipc ind 0x%04x len %zu", msg->msg_id, msg->len);
	if (msg->msg_id == MIPC_IMS_PDN_IND) {
		ims_mipc_ind(m, msg);
		return;
	}
	data_mipc_ind(m, msg);
}

void mipc_read(struct md *m)
{
	struct mipc *p = &m->mipc;
	ssize_t n;

	if (p->fd < 0)
		return;
	if (p->rxlen >= sizeof(p->rx))
		p->rxlen = 0;
	n = read(p->fd, p->rx + p->rxlen, sizeof(p->rx) - p->rxlen);
	if (n == 0) {
		mipc_close(m, "EOF");
		return;
	}
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		md_log(m, 0, "mipc: read: %s", strerror(errno));
		mipc_close(m, "read error");
		return;
	}
	p->rxlen += (size_t)n;
	for (;;) {
		struct mipc_msg msg;
		size_t used = 0;
		int r = mipc_frame_parse(p->rx, p->rxlen, &msg, &used);

		if (r == 0)
			break;
		if (r < 0) {
			md_log(m, 0, "mipc: malformed frame, %zu bytes dropped", p->rxlen);
			p->n_bad++;
			p->rxlen = 0;
			break;
		}
		handle_msg(m, &msg);
		if (p->fd < 0)
			return;
		memmove(p->rx, p->rx + used, p->rxlen - used);
		p->rxlen -= used;
	}
}

/* --- session start --- */

static void sess_step(struct md *m);

static void cb_test(struct md *m, struct mipc_cnf *c, void *ctx)
{
	struct mipc *p = &m->mipc;

	(void)ctx;
	if (p->sess != SESS_TEST)
		return;
	if (c->res == MIPC_OK) {
		md_log(m, 1, "mipc: test answered after %d tries", p->sess_tests);
		p->sess = SESS_OPEN;
		p->sess_next = 0;
		sess_step(m);
	} else if (c->res == MIPC_TIMEOUT) {
		p->sess_next = mono_ms();	/* resend right away (100 ms passed) */
	}
}

static void cb_open(struct md *m, struct mipc_cnf *c, void *ctx)
{
	struct mipc *p = &m->mipc;
	uint32_t v, result;

	(void)ctx;
	if (p->sess != SESS_OPEN)
		return;
	if (c->res != MIPC_OK) {
		md_log(m, 0, "mipc: INTERNAL_OPEN: %s, retrying", mipc_res_str(c->res));
		p->sess = SESS_TEST;
		p->sess_next = mono_ms() + 1000;
		return;
	}
	mipc_tlv_result(c->payload, c->len, &result);
	if (!mipc_tlv_find_uint(c->payload, c->len, MIPC_T_OPEN_CNF_TIMEOUT, &v) && v)
		p->open_timeout_ms = v;
	if (!mipc_tlv_find_uint(c->payload, c->len, MIPC_T_OPEN_VERSION, &v))
		md_log(m, 0, "mipc: open: result %u, target version %u", result, v);
	else
		md_log(m, 0, "mipc: open: result %u", result);
	p->sess = SESS_REGISTER;
	p->reg_index = 0;
	p->sess_next = 0;
	sess_step(m);
}

static void cb_register(struct md *m, struct mipc_cnf *c, void *ctx)
{
	struct mipc *p = &m->mipc;
	uint32_t result = 0;

	(void)ctx;
	if (p->sess != SESS_REGISTER)
		return;
	if (c->res == MIPC_OK)
		mipc_tlv_result(c->payload, c->len, &result);
	if (c->res != MIPC_OK || result)
		md_log(m, 0, "mipc: register ind 0x%04x: %s result %u (continuing)",
		       reg_inds[p->reg_index], mipc_res_str(c->res), result);
	p->reg_index++;
	p->sess_next = 0;
	sess_step(m);
}

static void sess_step(struct md *m)
{
	struct mipc *p = &m->mipc;
	struct mipc_buf b;
	uint8_t mem[256];

	if (p->fd < 0)
		return;
	switch (p->sess) {
	case SESS_TEST:
		p->sess_tests++;
		p->sess_next = INT64_MAX;	/* the callback schedules the next one */
		if (mipc_send(m, MIPC_INTERNAL_TEST_REQ, MIPC_SLOT_ANY, NULL, 0,
			      MIPC_TEST_INTERVAL_MS, cb_test, NULL, NULL) < 0)
			p->sess_next = mono_ms() + MIPC_TEST_INTERVAL_MS;
		break;
	case SESS_OPEN:
		mipc_buf_init(&b, mem, sizeof(mem));
		mipc_tlv_add_u32(&b, MIPC_T_OPEN_VERSION, MIPC_OPEN_VERSION);
		mipc_tlv_add_str(&b, MIPC_T_OPEN_CLIENT_NAME, m->cfg.mipc_client_name);
		mipc_tlv_add_u8(&b, MIPC_T_OPEN_FLAG, 1);
		p->sess_next = INT64_MAX;
		if (mipc_send(m, MIPC_INTERNAL_OPEN_REQ, MIPC_SLOT_ANY, b.data, b.len, 0,
			      cb_open, NULL, NULL) < 0) {
			p->sess = SESS_TEST;
			p->sess_next = mono_ms() + 1000;
		}
		break;
	case SESS_REGISTER:
		if (p->reg_index >= (int)N_REG_INDS) {
			p->sess = SESS_READY;
			md_log(m, 0, "mipc: session ready on %s", m->cfg.mipc_port);
			md_event(m, "mipc ready");
			break;
		}
		mipc_buf_init(&b, mem, sizeof(mem));
		p->sess_next = INT64_MAX;
		if (REG_IS_CMD(reg_inds[p->reg_index])) {
			/* INTERNAL_REGISTER_CMD_REQ: slot 0xFF, TLV 0x0100 u16 = CMD id */
			mipc_tlv_add_u16(&b, MIPC_T_REGISTER_CMD_ID, (uint16_t)reg_inds[p->reg_index]);
			if (mipc_send(m, MIPC_INTERNAL_REGISTER_CMD_REQ, MIPC_SLOT_ANY, b.data, b.len,
				      0, cb_register, NULL, NULL) < 0) {
				p->reg_index++;
				p->sess_next = 0;
			}
			break;
		}
		mipc_tlv_add_u16(&b, MIPC_T_REGISTER_IND_ID, (uint16_t)reg_inds[p->reg_index]);
		if (mipc_send(m, MIPC_INTERNAL_REGISTER_IND_REQ, mipc_slot(m), b.data, b.len, 0,
			      cb_register, NULL, NULL) < 0) {
			p->reg_index++;
			p->sess_next = 0;
		}
		break;
	case SESS_NONE:
	case SESS_READY:
		break;
	}
}

static int open_port(struct md *m)
{
	struct stat st;
	int fd = open(m->cfg.mipc_port, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
		md_log(m, 0, "%s: not a character device, refusing", m->cfg.mipc_port);
		close(fd);
		return -2;
	}
	return fd;
}

void mipc_tick(struct md *m, int64_t now)
{
	struct mipc *p = &m->mipc;

	if (!p->enabled)
		return;
	if (p->fd < 0) {
		int fd;

		if (m->state != MD_READY || now < p->reopen_at)
			return;
		fd = open_port(m);
		if (fd < 0) {
			p->reopen_at = now + (fd == -2 ? 10 : 1) * m->cfg.reopen_ms;
			return;
		}
		p->fd = fd;
		p->rxlen = 0;
		p->sess = SESS_TEST;
		p->sess_tests = 0;
		p->sess_next = now;
		md_log(m, 0, "%s open", m->cfg.mipc_port);
		md_event(m, "mipc open");
	}
	for (int i = 0; i < MIPC_PENDING_MAX; i++) {
		struct mipc_pending *pd = &p->pend[i];

		if (pd->used && now >= pd->deadline) {
			p->n_timeouts++;
			if (pd->req_id != MIPC_INTERNAL_TEST_REQ)
				md_log(m, 0, "mipc: req 0x%04x txid %u: no confirm", pd->req_id,
				       pd->txid);
			complete(m, pd, MIPC_TIMEOUT, NULL);
			if (p->fd < 0)
				return;
		}
	}
	if (p->sess != SESS_READY && p->sess != SESS_NONE && now >= p->sess_next)
		sess_step(m);
}

int64_t mipc_next_deadline(struct md *m)
{
	struct mipc *p = &m->mipc;
	int64_t dl = INT64_MAX;

	if (!p->enabled)
		return dl;
	if (p->fd < 0)
		return m->state == MD_READY ? p->reopen_at : dl;
	for (int i = 0; i < MIPC_PENDING_MAX; i++)
		if (p->pend[i].used && p->pend[i].deadline < dl)
			dl = p->pend[i].deadline;
	if (p->sess != SESS_READY && p->sess_next < dl)
		dl = p->sess_next;
	return dl;
}

struct sync_ctx {
	int done;
};

static void sync_cb(struct md *m, struct mipc_cnf *c, void *ctx)
{
	(void)m;
	(void)c;
	((struct sync_ctx *)ctx)->done = 1;
}

/* synchronous request: runs the MIPC port loop until the confirm */
enum mipc_res mipc_request(struct md *m, unsigned msg_id, unsigned slot,
			   const uint8_t *payload, size_t len, int timeout_ms,
			   struct mipc_cnf *out)
{
	struct mipc *p = &m->mipc;
	struct sync_ctx sc = { 0 };
	static struct mipc_cnf scratch;

	if (!out)
		out = &scratch;
	out->res = MIPC_REFUSED;
	out->len = 0;
	if (p->fd < 0) {
		out->res = MIPC_LOST;
		return out->res;
	}
	if (mipc_send(m, msg_id, slot, payload, len, timeout_ms, sync_cb, &sc, out) < 0) {
		if (out->res == MIPC_PENDING)
			out->res = MIPC_REFUSED;
		return out->res;
	}
	while (!sc.done && p->fd >= 0) {
		struct pollfd pf = { .fd = p->fd, .events = POLLIN };
		int64_t now = mono_ms(), dl = mipc_next_deadline(m);
		int to = dl == INT64_MAX ? 1000 : dl <= now ? 0 : (int)(dl - now);

		if (to > 1000)
			to = 1000;
		if (poll(&pf, 1, to) > 0 && (pf.revents & (POLLIN | POLLHUP | POLLERR)))
			mipc_read(m);
		mipc_tick(m, mono_ms());
	}
	if (!sc.done)
		out->res = MIPC_LOST;
	return out->res;
}

void mipc_status(struct md *m, char *out, size_t cap)
{
	struct mipc *p = &m->mipc;

	if (!p->enabled) {
		str_append(out, cap, "mipc: not used (data_backend=%s)\n", m->cfg.data_backend);
		return;
	}
	str_append(out, cap, "mipc: %s %s, session %s, tx %lu rx %lu ind %lu (at %lu) timeouts %lu bad %lu\n",
		   m->cfg.mipc_port, p->fd >= 0 ? "open" : "closed", sess_str(p->sess), p->n_tx,
		   p->n_rx, p->n_ind, p->n_at_ind, p->n_timeouts, p->n_bad);
}

/* --- self-test (no modem): framing and TLV against the protocol description's example --- */

int mipc_selftest(void)
{
	uint8_t mem[512], frame[MIPC_MSG_MAX], big[2 * MIPC_MSG_MAX];
	struct mipc_buf b;
	struct mipc_msg msg;
	struct mipc_tlv t;
	size_t flen, used = 0, off = 0, vlen;
	uint32_t v;
	const uint8_t *val;
	char s[64];
	int fails = 0, n;
	/* example header: msgid 0x0201, txid 0x03CD, len 0x0318 */
	static const uint8_t hdr_ex[16] = { 0x84, 0x19, 0x54, 0x24, 0, 0, 0, 0, 0x01, 0x00,
					    0x01, 0x02, 0xCD, 0x03, 0x18, 0x03 };
	/* TLVs: 0x0108 u8 0x7F; 0x0101 APN (len 18 + 2 pad); 0x0109 u32 */
	static const uint8_t tlv_ex[] = {
		0x08, 0x01, 0x01, 0x00, 0x7F, 0, 0, 0,
		0x01, 0x01, 0x12, 0x00, 'i', 'n', 't', 'e', 'r', 'n', 'e', 't', '.', 'c', 'a',
		'r', 'r', 'i', 'e', 'r', '1', 0, 0, 0,
		0x09, 0x01, 0x04, 0x00, 0xFF, 0xFF, 0xFD, 0x7F,
		/* array descriptor 0x812F: u32 0, u32 0, first id 0xFFFD, count 3 */
		0x2F, 0x81, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0xFD, 0xFF, 0x03, 0x00,
		0xFD, 0xFF, 0x04, 0x00, 1, 0, 0, 0,
		0x00, 0x00, 0x04, 0x00, 0, 0, 0, 0,
	};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "mipc selftest: %s failed\n", #c); fails++; } } while (0)
	/* header of the protocol description's example */
	memcpy(big, hdr_ex, 16);
	CHECK(mipc_frame_parse(big, 16, &msg, &used) == 0);	/* needs 792 payload bytes */
	memset(big + 16, 0, 0x318);
	CHECK(mipc_frame_parse(big, 16 + 0x318, &msg, &used) == 1);
	CHECK(msg.msg_id == 0x0201 && msg.txid == 0x03CD && msg.len == 0x318 && msg.slot == 1);
	CHECK(used == 16 + 0x318);
	flen = mipc_frame_build(frame, sizeof(frame), 0x0201, 0x03CD, 1, big + 16, 0x318);
	CHECK(flen == 16 + 0x318 && !memcmp(frame, hdr_ex, 16));
	/* the TLVs of the worked example */
	CHECK(!mipc_tlv_find_uint(tlv_ex, sizeof(tlv_ex), MIPC_T_ACT_CONST_7F, &v) && v == 0x7F);
	CHECK(!mipc_tlv_find_str(tlv_ex, sizeof(tlv_ex), MIPC_T_ACT_APN, s, sizeof(s)) &&
	      !strcmp(s, "internet.carrier1"));
	CHECK(!mipc_tlv_find_uint(tlv_ex, sizeof(tlv_ex), MIPC_T_ACT_BEARER_MASK, &v) &&
	      v == 0x7FFDFFFF);
	CHECK(!mipc_tlv_result(tlv_ex, sizeof(tlv_ex), &v) && v == 0);
	/* privacy bit ignored in lookups: 0x812F is found as 0x012F (array -> no value) */
	CHECK(!mipc_tlv_find(tlv_ex, sizeof(tlv_ex), 0x012F, &vlen));
	val = mipc_tlv_find(tlv_ex, sizeof(tlv_ex), 0xFFFD, &vlen);	/* array element */
	CHECK(val && vlen == 4 && mipc_get(val, 4) == 1);
	off = 0;
	n = 0;
	while (mipc_tlv_next(tlv_ex, sizeof(tlv_ex), &off, &t) == 1)
		n++;
	CHECK(n == 6 && off == sizeof(tlv_ex));
	/* builder: same bytes as the example for the three plain TLVs */
	mipc_buf_init(&b, mem, sizeof(mem));
	CHECK(!mipc_tlv_add_u8(&b, MIPC_T_ACT_CONST_7F, 0x7F));
	CHECK(!mipc_tlv_add_str(&b, MIPC_T_ACT_APN, "internet.carrier1"));
	CHECK(!mipc_tlv_add_u32(&b, MIPC_T_ACT_BEARER_MASK, 0x7FFDFFFF));
	CHECK(b.len == 8 + 24 + 8 && !memcmp(b.data, tlv_ex, b.len));
	/* missing result -> 2 */
	CHECK(mipc_tlv_result(b.data, b.len, &v) < 0 && v == 2);
	/* truncated payload is malformed */
	off = 0;
	while ((n = mipc_tlv_next(tlv_ex, sizeof(tlv_ex) - 10, &off, &t)) == 1)
		;
	CHECK(n == -1);
	/* bad magic */
	memcpy(big, hdr_ex, 16);
	big[0] = 0x85;
	CHECK(mipc_frame_parse(big, 16 + 0x318, &msg, &used) == -1);
	/* two frames back to back, read in one go */
	flen = mipc_frame_build(frame, sizeof(frame), MIPC_INTERNAL_TEST_REQ, 5, MIPC_SLOT_ANY,
				NULL, 0);
	CHECK(flen == 16);
	memcpy(big, frame, flen);
	memcpy(big + flen, frame, flen);
	CHECK(mipc_frame_parse(big, 2 * flen, &msg, &used) == 1 && used == 16 &&
	      msg.msg_id == MIPC_INTERNAL_TEST_REQ && msg.slot == 0xFF);
	/* class macros */
	CHECK(MIPC_IS_IND(MIPC_INTERNAL_EIF_IND) && !MIPC_IS_IND(MIPC_DATA_ACT_CALL_CNF));
	CHECK(MIPC_IS_CMD(0x8303) && !MIPC_IS_CMD(0x8304));
	CHECK(MIPC_CNF_OF(MIPC_DATA_ACT_CALL_REQ) == MIPC_DATA_ACT_CALL_CNF);
	/* builder overflow */
	mipc_buf_init(&b, mem, 8);
	CHECK(mipc_tlv_add(&b, 1, "0123456789", 10) == -1 && b.overflow);
	CHECK(mipc_frame_build(frame, 16, 1, 1, 1, mem, 100) == 0);
#undef CHECK
	if (!fails)
		printf("mipc selftest ok\n");
	return fails ? 1 : 0;
}
