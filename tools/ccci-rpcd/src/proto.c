// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-rpcd: fragments, reassembly and the argument stream.
 *
 * Everything that arrives here comes from the modem and is untrusted:
 * every length is checked against the bytes actually present.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <string.h>

#include "rpcd.h"

void rpcd_out_init(struct rpcd_out *o)
{
	o->len = 4;
	o->n = 0;
	o->overflow = 0;
	put_le32(o->buf, 0);
}

/* append one argument; the reply frame must stay within 0x800 bytes */
void rpcd_out_add(struct rpcd_out *o, const void *data, size_t len)
{
	size_t padded = (len + 3) & ~(size_t)3;

	if (o->overflow || o->n >= RPCD_MAX_ARGS ||
	    len > sizeof(o->buf) || 4 + padded > sizeof(o->buf) - o->len) {
		o->overflow = 1;
		return;
	}
	put_le32(o->buf + o->len, (uint32_t)len);
	o->len += 4;
	if (len)
		memcpy(o->buf + o->len, data, len);
	/* the vendor leaves stale bytes here; zeros are what we send */
	memset(o->buf + o->len + len, 0, padded - len);
	o->len += padded;
	o->n++;
	put_le32(o->buf, o->n);
}

void rpcd_out_u32(struct rpcd_out *o, uint32_t v)
{
	uint8_t b[4];

	put_le32(b, v);
	rpcd_out_add(o, b, 4);
}

/*
 * u32 num_args (<= 6), then per argument u32 len, len bytes, filler
 * to a multiple of 4. The whole stream, filler of the last argument
 * included, must fit in 0x800 bytes. Returns 0 or -1 (reject with -2).
 * Bytes after the last argument are ignored.
 */
int rpcd_parse_args(const uint8_t *s, size_t len, struct rpcd_args *args)
{
	size_t off = 4;
	uint32_t i;

	memset(args, 0, sizeof(*args));
	if (len < 4)
		return -1;
	args->n = get_le32(s);
	if (args->n > RPCD_MAX_ARGS)
		return -1;
	for (i = 0; i < args->n; i++) {
		uint32_t alen;

		if (len - off < 4)
			return -1;
		alen = get_le32(s + off);
		off += 4;
		if (alen > len - off)
			return -1;
		args->a[i].data = s + off;
		args->a[i].len = alen;
		/* the filler of the last argument may be missing */
		off += ((size_t)alen + 3) & ~(size_t)3;
		if (off > RPCD_STREAM_MAX)
			return -1;
		if (off > len)
			off = len;
	}
	return 0;
}

void rpcd_reset_slots(struct rpcd *r)
{
	for (unsigned int i = 0; i < RPCD_SLOTS; i++) {
		r->slot[i].busy = 0;
		r->slot[i].overflow = 0;
		r->slot[i].len = 0;
	}
}

/* every reply fits one fragment: the stream is capped at 0x800 - 0x14 */
static void send_reply(struct rpcd *r, unsigned int slot, uint32_t word0,
		       uint32_t op, const struct rpcd_out *o)
{
	uint8_t frag[RPCD_REPLY_MAX];
	size_t len = RPCD_HDR_LEN + o->len;

	/*
	 * word 0 of the last request fragment, bit 31 cleared. Bit 30
	 * is passed back unchanged, as the vendor server does.
	 */
	put_le32(frag + 0x00, word0 & ~RPCD_W0_MORE);
	put_le32(frag + 0x04, (uint32_t)len);
	put_le32(frag + 0x08, RPCD_CH_TX);
	put_le32(frag + 0x0C, slot);
	put_le32(frag + 0x10, op | RPCD_REPLY_OP);
	memcpy(frag + RPCD_HDR_LEN, o->buf, o->len);
	if (r->send(r, frag, len) < 0)
		rpcd_log(r, 0, "slot %u op 0x%x: reply not sent", slot, op);
}

static void process(struct rpcd *r, unsigned int slot, uint32_t word0)
{
	struct rpcd_slot *s = &r->slot[slot];
	struct rpcd_out *o = &r->out;
	struct rpcd_args args;

	rpcd_out_init(o);
	if (s->overflow) {
		rpcd_log(r, 0, "slot %u op 0x%x: request too long, rejected",
			 slot, s->op);
		rpcd_out_u32(o, (uint32_t)RPC_REJECT);
	} else if (rpcd_parse_args(s->buf + RPCD_HDR_LEN, s->len - RPCD_HDR_LEN,
				   &args)) {
		rpcd_log(r, 0, "slot %u op 0x%x: bad argument stream (%zu bytes), rejected",
			 slot, s->op, s->len - RPCD_HDR_LEN);
		rpcd_out_u32(o, (uint32_t)RPC_REJECT);
	} else {
		rpcd_dispatch(r, slot, s->op, &args, o);
	}
	if (o->overflow) {
		/* cannot happen with the replies in ops.c and drdi.c */
		rpcd_log(r, 0, "slot %u op 0x%x: reply too long", slot, s->op);
		rpcd_out_init(o);
		rpcd_out_u32(o, (uint32_t)RPC_REJECT);
	}
	send_reply(r, slot, word0, s->op, o);
}

/* one packet from the modem, CCCI header included */
void rpcd_feed(struct rpcd *r, const uint8_t *pkt, size_t len)
{
	uint32_t word0, word1, ch, slot, op;
	struct rpcd_slot *s;

	if (len < RPCD_HDR_LEN || len > RPCD_FRAG_MAX) {
		rpcd_log(r, 0, "drop: packet of %zu bytes", len);
		return;
	}
	word0 = get_le32(pkt);
	word1 = get_le32(pkt + 0x04);
	ch = get_le32(pkt + 0x08);
	slot = get_le32(pkt + 0x0C);
	op = get_le32(pkt + 0x10);
	if (word1 != len) {
		rpcd_log(r, 0, "drop: length field %u, packet %zu", word1, len);
		return;
	}
	if ((ch & 0xffff) != RPCD_CH_RX) {
		rpcd_log(r, 0, "drop: channel %u", ch & 0xffff);
		return;
	}
	if (slot >= RPCD_SLOTS) {
		/* the vendor server exits here; we keep serving */
		rpcd_log(r, 0, "drop: slot %u", slot);
		return;
	}
	s = &r->slot[slot];

	if (s->busy && s->op != op) {
		rpcd_log(r, 0, "slot %u: op 0x%x during 0x%x, partial request dropped",
			 slot, op, s->op);
		s->busy = 0;
	}
	if (!s->busy) {
		memcpy(s->buf, pkt, len);
		s->len = len;
		s->op = op;
		s->overflow = 0;
		s->busy = 1;
	} else if (!s->overflow) {
		size_t pl = len - RPCD_HDR_LEN;

		if (pl > sizeof(s->buf) - s->len)
			s->overflow = 1;
		else {
			memcpy(s->buf + s->len, pkt + RPCD_HDR_LEN, pl);
			s->len += pl;
		}
	}
	if (word0 & RPCD_W0_MORE)
		return;

	rpcd_log(r, 1, "slot %u: op 0x%x, %zu bytes", slot, s->op, s->len);
	process(r, slot, word0);
	s->busy = 0;
	s->overflow = 0;
	s->len = 0;
}
