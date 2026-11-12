// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-speechd: message framing, message classes, ack timing and the speech
 * info block. libc only.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <string.h>

#include "speechd.h"

static int in_list(uint16_t id, const uint16_t *l, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (l[i] == id)
			return 1;
	return 0;
}

#define IN(id, l)	in_list(id, l, sizeof(l) / sizeof(l[0]))

static const uint16_t cls_a0[] = {
	0x2F00, 0x2F01, 0x2F05, 0x2F09, 0x2F13, 0x2F23, 0x2F2C, 0x2F42,
	0x2F45, 0x2F46, 0x2F47, 0x2F4A, 0x2FB2, 0x2FB3,
};
static const uint16_t cls_a1[] = {
	0x2F02, 0x2F03, 0x2F08, 0x2F0A, 0x2F20, 0x2F21, 0x2F2B, 0x2F30,
	0x2F31, 0x2F3A, 0x2F3B, 0x2F3C, 0x2F3D, 0x2F3E, 0x2F3F, 0x2F40,
	0x2F41, 0x2F43, 0x2F44, 0x2F48, 0x2F49, 0x2F79, 0x2F7B, 0x2F80,
	0x2FB0, 0x2FB1, 0x2FB4, 0x2FB6, 0x2FB7,
};
static const uint16_t cls_a2[] = { 0x2F78, 0x2F90, 0x2FA0, 0x2FB5 };
static const uint16_t cls_a3[] = {
	0x2F50, 0x2F51, 0x2F57, 0x2F58, 0x2F59, 0x2F60, 0x2F62, 0x2F64,
	0x2F66, 0x2F67, 0x2FB8,
};
static const uint16_t cls_m0[] = { 0xAF70, 0xAF75, 0xAFE4 };
static const uint16_t cls_m1[] = { 0xAF78, 0xAF90, 0xAFA0, 0xAFB5 };
static const uint16_t cls_m3[] = {
	0xAF50, 0xAF51, 0xAF57, 0xAF58, 0xAF59, 0xAF60, 0xAF62, 0xAF64,
	0xAF66, 0xAF67, 0xAFB8,
};


enum sph_class sph_class(uint16_t id)
{
	if (IN(id, cls_a0))
		return SPH_A0;
	if (IN(id, cls_a1))
		return SPH_A1;
	if (IN(id, cls_a2))
		return SPH_A2;
	if (IN(id, cls_a3))
		return SPH_A3;
	if (IN(id, cls_m0))
		return SPH_M0;
	if (IN(id, cls_m1))
		return SPH_M1;
	if (IN(id, cls_m3))
		return SPH_M3;
	if (id == 0xAF81 || ((id & 0xFF00) == 0xAF00 &&
			     IN((uint16_t)(id & 0x7FFF), cls_a1)))
		return SPH_M2;
	return SPH_UNSUPPORTED;
}

/* how long the caller blocks for the ack, 0 = not at all */
int sph_block_ms(uint16_t id)
{
	static const uint16_t ms300[] = {
		0x2F2B, 0x2F3A, 0x2F3B, 0x2F79, 0x2F7B, 0x2F80,
	};
	static const uint16_t ms100[] = {
		0x2F3E, 0x2F3F, 0x2F43, 0x2F44, 0x2F48, 0x2F49,
	};
	static const uint16_t ms50[] = {
		0x2F02, 0x2F03, 0x2F08, 0x2F09, 0x2F0A, 0x2F13, 0x2F30, 0x2F31,
		0x2F3C, 0x2F3D, 0x2F40, 0x2F41, 0x2F42, 0x2F78,
	};

	if (id == SPH_SPEECH_ON || id == SPH_SPEECH_OFF)
		return 500;
	if (IN(id, ms300))
		return 300;
	if (IN(id, ms100))
		return 100;
	if (IN(id, ms50))
		return 50;
	return 0;
}

/* 0x2Fxx is acked by 0xAFxx with the same low byte */
int sph_ack_pairs(uint16_t req, uint16_t ack)
{
	return (req & 0xFF00) == 0x2F00 && (ack & 0xFF00) == 0xAF00 &&
	       (req & 0xFF) == (ack & 0xFF);
}

/* mailbox: exactly 16 bytes */
size_t sph_put_mailbox(uint8_t out[SPH_HDR], uint16_t id, uint16_t p16,
		       uint32_t p32)
{
	put_le32(out, SPH_MAILBOX);
	put_le16(out + 4, p16);
	put_le16(out + 6, id);
	put_le32(out + 8, SPH_CH_TX);
	put_le32(out + 12, p32);
	return SPH_HDR;
}

/* payload: 16 + 6 + n bytes, no padding; 0 if it does not fit */
size_t sph_put_payload(uint8_t *out, size_t cap, uint16_t id, uint16_t type,
		       const uint8_t *data, size_t n)
{
	size_t len = SPH_HDR + SPH_AP_PL_HDR + n;

	if (n > SPH_AP_DATA_MAX || len > cap)
		return 0;
	put_le32(out, 0);
	put_le32(out + 4, (uint32_t)(n + 6));	/* the kernel sets n + 22 */
	put_le32(out + 8, SPH_CH_TX);
	put_le16(out + 12, (uint16_t)(n + 6));
	put_le16(out + 14, id);
	put_le16(out + 16, SPH_SYNC_AP);
	put_le16(out + 18, type);
	put_le16(out + 20, (uint16_t)n);
	if (n)
		memcpy(out + 22, data, n);
	return len;
}

/* the 10-byte modem payload header; rec holds at least len bytes */
int sph_parse_md_header(const uint8_t *rec, size_t len, struct sph_rx *m,
			const char **why)
{
	if (len < SPH_MD_PL_HDR) {
		*why = "payload header cut short";
		return -1;
	}
	if (get_le16(rec) != SPH_SYNC_MD) {
		*why = "bad sync word";
		return -1;
	}
	m->type = get_le16(rec + 2);
	m->size = get_le16(rec + 4);
	m->frag_idx = get_le16(rec + 6);
	m->frag_total = get_le16(rec + 8);
	if (m->size > SPH_MD_DATA_MAX) {
		*why = "data size too large";
		return -1;
	}
	if ((size_t)m->size + SPH_MD_PL_HDR > len) {
		*why = "data size beyond the packet";
		return -1;
	}
	m->data = rec + SPH_MD_PL_HDR;
	return 0;
}

int sph_decode(const uint8_t *buf, size_t len, struct sph_rx *m,
	       const char **why, const char **warn)
{
	memset(m, 0, sizeof(*m));
	*warn = NULL;
	if (len < SPH_HDR) {
		*why = "shorter than a CCCI header";
		return -1;
	}
	if (get_le32(buf) == SPH_MAILBOX) {
		m->mailbox = 1;
		m->p16 = get_le16(buf + 4);
		m->id = get_le16(buf + 6);
		m->p32 = get_le32(buf + 12);
		return 0;
	}
	m->id = get_le16(buf + 14);
	if (sph_parse_md_header(buf + SPH_HDR, len - SPH_HDR, m, why))
		return -1;
	/* logged, not fatal */
	if (get_le32(buf + 4) != len)
		*warn = "length word does not match the read length";
	else if ((size_t)m->size + SPH_HDR + SPH_MD_PL_HDR != len)
		*warn = "read length is not data size + 26";
	return 0;
}

/* fields not set here stay 0 */
void sph_build_info(uint8_t blk[SPH_INFO_SIZE], const struct sph_info *in)
{
	memset(blk, 0, SPH_INFO_SIZE);
	blk[0x00] = in->app;
	blk[0x01] = 0;			/* BT info: not a BT call */
	blk[0x02] = in->rate_enum;
	blk[0x03] = 0;			/* enhancement in the modem, no AP DSP */
	blk[0x04] = 0;			/* no parameter path: none passed */
	blk[0x05] = 0;			/* parameter status: modem defaults */
	put_le16(blk + 0x0A, in->ext_dev);
	blk[0x11] = in->mute;
	put_le16(blk + 0x12, in->enh_ctrl);
	put_le32(blk + 0x14, 0);	/* parameter size */
	put_le32(blk + 0x18, 0);	/* parameter index */
	for (int i = 0; i < 12; i++)
		put_le16(blk + 0x1C + 2 * i, in->common[i]);
	blk[0x54] = in->smartpa;
}

/* field 0x02 */
int sph_rate_enum(unsigned rate)
{
	switch (rate) {
	case 8000:
		return 0;
	case 16000:
		return 1;
	case 32000:
		return 2;
	case 48000:
		return 3;
	default:
		return -1;
	}
}


void sph_band_decode(uint16_t p16, int *band, int *net_rate)
{
	int b;

	if (p16 & 0x8000)
		b = (p16 >> 4) & 3;
	else
		b = (p16 >> 3) & 7;
	if (b >= 4)
		b = 0;
	*band = b;
	*net_rate = p16 & 0xF;
}
