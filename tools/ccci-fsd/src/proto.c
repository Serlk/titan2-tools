// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-fsd: fragments, reassembly and the argument stream.
 *
 * Everything that arrives here comes from the modem and is untrusted:
 * every length is checked against the bytes actually present.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <string.h>

#include "fsd.h"

void fsd_out_init(struct fsd_out *o)
{
	o->len = 4;
	o->n = 0;
	o->overflow = 0;
	put_le32(o->buf, 0);
}

size_t fsd_out_room(const struct fsd_out *o)
{
	size_t left = sizeof(o->buf) - o->len;

	/* room for the data of one more argument */
	return left < 4 ? 0 : (left - 4) & ~(size_t)3;
}

void fsd_out_add(struct fsd_out *o, const void *data, size_t len)
{
	size_t padded = (len + 3) & ~(size_t)3;

	if (o->overflow || o->n >= FSD_MAX_ARGS || len > fsd_out_room(o)) {
		o->overflow = 1;
		return;
	}
	put_le32(o->buf + o->len, (uint32_t)len);
	o->len += 4;
	if (len)
		memcpy(o->buf + o->len, data, len);
	memset(o->buf + o->len + len, 0, padded - len);
	o->len += padded;
	o->n++;
	put_le32(o->buf, o->n);
}

void fsd_out_u32(struct fsd_out *o, uint32_t v)
{
	uint8_t b[4];

	put_le32(b, v);
	fsd_out_add(o, b, 4);
}

/*
 * u32 num_args (<= 6), then per argument u32 len, len bytes, padding to
 * a multiple of 4. Returns 0 or -1. Trailing bytes are ignored.
 */
int fsd_parse_args(const uint8_t *s, size_t len, struct fsd_args *args)
{
	size_t off = 4;
	uint32_t i;

	memset(args, 0, sizeof(*args));
	if (len < 4)
		return -1;
	args->n = get_le32(s);
	if (args->n > FSD_MAX_ARGS)
		return -1;
	for (i = 0; i < args->n; i++) {
		uint32_t alen;
		size_t pad;

		if (len - off < 4)
			return -1;
		alen = get_le32(s + off);
		off += 4;
		if (alen > len - off)
			return -1;
		args->a[i].data = s + off;
		args->a[i].len = alen;
		off += alen;
		pad = (4 - (alen & 3)) & 3;
		/* the padding of the last argument may be missing */
		off += pad < len - off ? pad : len - off;
	}
	return 0;
}

void fsd_reset_slots(struct fsd *fsd)
{
	for (unsigned int i = 0; i < FSD_SLOTS; i++) {
		fsd->slot[i].busy = 0;
		fsd->slot[i].discard = 0;
		fsd->slot[i].len = 0;
	}
}

static void send_reply(struct fsd *fsd, unsigned int slot, uint32_t word0,
		       uint32_t op, const struct fsd_out *o)
{
	uint8_t frag[FSD_FRAG_MAX];
	size_t off = 0;

	word0 &= ~(FSD_W0_MORE | FSD_W0_WAKE);
	do {
		size_t chunk = o->len - off;
		uint32_t w0 = word0;

		if (chunk > FSD_PAYLOAD_MAX) {
			chunk = FSD_PAYLOAD_MAX;
			w0 |= FSD_W0_MORE;
		}
		put_le32(frag + 0x00, w0);
		put_le32(frag + 0x04, (uint32_t)(FSD_HDR_LEN + chunk));
		put_le32(frag + 0x08, FSD_CH_TX);
		put_le32(frag + 0x0C, slot);
		put_le32(frag + 0x10, op | FSD_REPLY_OP);
		memcpy(frag + FSD_HDR_LEN, o->buf + off, chunk);
		off += chunk;
		if (fsd->send(fsd, frag, FSD_HDR_LEN + chunk) < 0) {
			fsd_log(fsd, 0, "slot %u op 0x%x: reply not sent", slot, op);
			return;
		}
	} while (off < o->len);
}

static void process(struct fsd *fsd, unsigned int slot)
{
	struct fsd_slot *s = &fsd->slot[slot];
	struct fsd_out *o = &fsd->out;
	struct fsd_args args;

	fsd_out_init(o);
	if (fsd_parse_args(s->buf + FSD_HDR_LEN, s->len - FSD_HDR_LEN, &args)) {
		fsd_journal(fsd, "BADARGS slot=%u op=0x%x len=%zu", slot, s->op,
			    s->len - FSD_HDR_LEN);
		fsd_out_u32(o, (uint32_t)FS_PARAM);
	} else {
		fsd_dispatch(fsd, slot, s->op, &args, o);
	}
	if (o->overflow) {
		/* cannot happen with the caps in ops.c; never send half a reply */
		fsd_log(fsd, 0, "slot %u op 0x%x: reply too long", slot, s->op);
		fsd_out_init(o);
		fsd_out_u32(o, (uint32_t)FS_GENERAL);
	}
	send_reply(fsd, slot, s->word0, s->op, o);
}

/* one packet from the modem, CCCI header included */
void fsd_feed(struct fsd *fsd, const uint8_t *pkt, size_t len)
{
	uint32_t word0, word1, ch, slot, op;
	struct fsd_slot *s;

	if (len < FSD_HDR_LEN || len > FSD_FRAG_MAX) {
		fsd_log(fsd, 0, "drop: packet of %zu bytes", len);
		return;
	}
	word0 = get_le32(pkt);
	word1 = get_le32(pkt + 0x04);
	ch = get_le32(pkt + 0x08);
	slot = get_le32(pkt + 0x0C);
	op = get_le32(pkt + 0x10);
	if (word1 != len) {
		fsd_log(fsd, 0, "drop: length field %u, packet %zu", word1, len);
		return;
	}
	if ((ch & 0xffff) != FSD_CH_RX) {
		fsd_log(fsd, 0, "drop: channel %u", ch & 0xffff);
		return;
	}
	if (slot >= FSD_SLOTS) {
		fsd_log(fsd, 0, "drop: slot %u", slot);
		return;
	}
	s = &fsd->slot[slot];

	if (s->discard) {
		if (!(word0 & FSD_W0_MORE))
			s->discard = 0;
		fsd_log(fsd, 1, "slot %u: dropping the rest of a bad request", slot);
		return;
	}
	if (s->busy && s->op != op) {
		fsd_log(fsd, 0, "slot %u: op 0x%x during 0x%x, partial request dropped",
			slot, op, s->op);
		s->busy = 0;
	}
	if (!s->busy) {
		memcpy(s->buf, pkt, len);
		s->len = len;
		s->word0 = word0;
		s->op = op;
		s->busy = 1;
	} else {
		size_t pl = len - FSD_HDR_LEN;

		if (pl > sizeof(s->buf) - s->len) {
			fsd_log(fsd, 0, "slot %u: request over %u bytes, dropped",
				slot, FSD_REQ_MAX);
			s->busy = 0;
			s->discard = !!(word0 & FSD_W0_MORE);
			return;
		}
		memcpy(s->buf + s->len, pkt + FSD_HDR_LEN, pl);
		s->len += pl;
	}
	if (word0 & FSD_W0_MORE)
		return;

	fsd_log(fsd, 1, "slot %u: op 0x%x, %zu bytes", slot, s->op, s->len);
	process(fsd, slot);
	s->busy = 0;
	s->len = 0;
}
