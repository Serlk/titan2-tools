// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: SMS in PDU mode (3GPP 27.005, 23.040, 23.038). Receive: +CMT
 * with the PDU on the next line, acknowledged with +CNMA; GSM 7-bit default
 * alphabet with the extension table, UCS-2, 8-bit data; concatenation by
 * the user data header. Send: +CMGS with 7-bit or UCS-2, split into parts
 * with a concatenation header when the text is too long. List: +CMGL=4.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modemd.h"

/* --- GSM 7-bit default alphabet (23.038 §6.2.1) --- */

static const uint16_t gsm_basic[128] = {
	0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
	0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
	0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
	0x03A3, 0x0398, 0x039E, 0xFFFF, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
	0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
	0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
	0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
	0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
	0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
	0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
	0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
	0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
	0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
	0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
	0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
	0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

static const struct {
	uint8_t code;
	uint16_t cp;
} gsm_ext[] = {
	{ 0x0A, 0x000C }, { 0x14, 0x005E }, { 0x28, 0x007B }, { 0x29, 0x007D },
	{ 0x2F, 0x005C }, { 0x3C, 0x005B }, { 0x3D, 0x007E }, { 0x3E, 0x005D },
	{ 0x40, 0x007C }, { 0x65, 0x20AC },
};

static uint32_t gsm_to_cp(int code, int ext)
{
	if (ext) {
		for (size_t i = 0; i < sizeof(gsm_ext) / sizeof(gsm_ext[0]); i++)
			if (gsm_ext[i].code == code)
				return gsm_ext[i].cp;
		return 0xFFFD;	/* unknown extension: replacement character */
	}
	return code == 0x1B ? 0xFFFD : gsm_basic[code & 0x7F];
}

/* code point -> (basic code | ext code << 8 | 0x100); -1 not representable */
static int cp_to_gsm(uint32_t cp)
{
	if (cp == 0x1B || cp > 0xFFFF)
		return -1;
	for (int i = 0; i < 128; i++)
		if (gsm_basic[i] == cp)
			return i;
	for (size_t i = 0; i < sizeof(gsm_ext) / sizeof(gsm_ext[0]); i++)
		if (gsm_ext[i].cp == cp)
			return 0x100 | gsm_ext[i].code;
	return -1;
}

/* --- UTF-8 --- */

static size_t utf8_put(char *out, size_t cap, uint32_t cp)
{
	if (cp < 0x80) {
		if (cap < 1)
			return 0;
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		if (cap < 2)
			return 0;
		out[0] = (char)(0xC0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		if (cap < 3)
			return 0;
		out[0] = (char)(0xE0 | cp >> 12);
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (cap < 4 || cp > 0x10FFFF)
		return 0;
	out[0] = (char)(0xF0 | cp >> 18);
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* one code point from UTF-8; invalid bytes become U+FFFD, 1 byte each */
static uint32_t utf8_get(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t cp;
	int n;

	if (s[0] < 0x80) {
		*p += 1;
		return s[0];
	}
	if ((s[0] & 0xE0) == 0xC0) {
		n = 1;
		cp = s[0] & 0x1F;
	} else if ((s[0] & 0xF0) == 0xE0) {
		n = 2;
		cp = s[0] & 0x0F;
	} else if ((s[0] & 0xF8) == 0xF0) {
		n = 3;
		cp = s[0] & 0x07;
	} else {
		*p += 1;
		return 0xFFFD;
	}
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*p += 1;
			return 0xFFFD;
		}
		cp = cp << 6 | (s[i] & 0x3F);
	}
	*p += n + 1;
	return cp;
}

/* --- hex --- */

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t cap)
{
	size_t l = strlen(hex), n = 0;

	if (l % 2)
		return -1;
	for (size_t i = 0; i < l; i += 2) {
		int h = hexval(hex[i]), lo = hexval(hex[i + 1]);

		if (h < 0 || lo < 0 || n >= cap)
			return -1;
		out[n++] = (uint8_t)(h << 4 | lo);
	}
	return (int)n;
}

static void hex_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
	static const char d[] = "0123456789ABCDEF";
	size_t o = 0;

	for (size_t i = 0; i < n && o + 2 < cap; i++) {
		out[o++] = d[in[i] >> 4];
		out[o++] = d[in[i] & 15];
	}
	out[o] = 0;
}

/* --- 7-bit packing --- */

/* unpack nsept septets starting at septet `skip` of the packed data */
static int unpack7(const uint8_t *data, size_t datalen, int nsept, int skip,
		   uint8_t *out, int cap)
{
	int n = 0;

	for (int i = 0; i < nsept && n < cap; i++) {
		int bit = i * 7, byte = bit / 8, sh = bit % 8;
		unsigned v;

		if ((size_t)byte >= datalen)
			break;
		v = data[byte] >> sh;
		if (sh > 1) {
			if ((size_t)byte + 1 < datalen)
				v |= (unsigned)data[byte + 1] << (8 - sh);
			else if (i + 1 < nsept)
				break;
		}
		if (i >= skip)
			out[n++] = v & 0x7F;
	}
	return n;
}

/* pack septets, the first `fill` bits left as zero padding */
static size_t pack7(const uint8_t *sept, int n, int fill, uint8_t *out, size_t cap)
{
	size_t bytes = ((size_t)n * 7 + (size_t)fill + 7) / 8;

	if (bytes > cap)
		return 0;
	memset(out, 0, bytes);
	for (int i = 0; i < n; i++) {
		int bit = i * 7 + fill, byte = bit / 8, sh = bit % 8;

		out[byte] |= (uint8_t)(sept[i] << sh);
		if (sh > 1 && (size_t)byte + 1 < bytes)
			out[byte + 1] |= (uint8_t)(sept[i] >> (8 - sh));
	}
	return bytes;
}

/* septets -> UTF-8 text */
static void septets_to_text(const uint8_t *sept, int n, char *text, size_t cap)
{
	size_t o = 0;
	int ext = 0;

	for (int i = 0; i < n; i++) {
		uint32_t cp;
		size_t w;

		if (!ext && sept[i] == 0x1B) {
			ext = 1;
			continue;
		}
		cp = gsm_to_cp(sept[i], ext);
		ext = 0;
		w = utf8_put(text + o, cap - o - 1, cp);
		if (!w)
			break;
		o += w;
	}
	text[o] = 0;
}

/* --- addresses --- */

static int decode_address(const uint8_t *p, size_t len, char *out, size_t cap)
{
	int digits, type, nbytes;
	size_t o = 0;

	if (len < 2)
		return -1;
	digits = p[0];
	type = p[1];
	nbytes = (digits + 1) / 2;
	if (digits > 20 || (size_t)nbytes + 2 > len)
		return -1;
	if ((type & 0x70) == 0x50) {
		/* alphanumeric, 7-bit */
		uint8_t sept[24];
		int n = unpack7(p + 2, (size_t)nbytes, digits * 4 / 7, 0, sept, sizeof(sept));

		septets_to_text(sept, n, out, cap);
		return 2 + nbytes;
	}
	if ((type & 0x70) == 0x10 && o + 1 < cap)
		out[o++] = '+';
	for (int i = 0; i < digits && o + 1 < cap; i++) {
		int nib = i % 2 ? p[2 + i / 2] >> 4 : p[2 + i / 2] & 15;

		if (nib == 15)
			continue;
		out[o++] = nib < 10 ? (char)('0' + nib) : "*#abc"[nib - 10];
	}
	out[o] = 0;
	return 2 + nbytes;
}

static int encode_address(const char *num, uint8_t *out, size_t cap)
{
	int type = 0x81, n = 0;
	size_t l;

	if (*num == '+') {
		type = 0x91;
		num++;
	}
	l = strlen(num);
	if (!l || l > 20 || cap < 2 + (l + 1) / 2)
		return -1;
	out[0] = (uint8_t)l;
	out[1] = (uint8_t)type;
	memset(out + 2, 0xFF, (l + 1) / 2);
	for (size_t i = 0; i < l; i++) {
		char c = num[i];
		int nib = c >= '0' && c <= '9' ? c - '0' : c == '*' ? 10 : c == '#' ? 11 :
			  (c | 0x20) == 'a' ? 12 : (c | 0x20) == 'b' ? 13 : (c | 0x20) == 'c' ? 14 : -1;

		if (nib < 0)
			return -1;
		if (i % 2)
			out[2 + i / 2] = (uint8_t)((out[2 + i / 2] & 0x0F) | nib << 4);
		else
			out[2 + i / 2] = (uint8_t)(0xF0 | nib);
	}
	n = 2 + (int)((l + 1) / 2);
	return n;
}

/* --- decode --- */

enum alphabet { ALPHA_GSM7, ALPHA_8BIT, ALPHA_UCS2 };

static int dcs_alphabet(int dcs, enum alphabet *a)
{
	if ((dcs & 0xC0) == 0) {
		if (dcs & 0x20)
			return -1;	/* compressed */
		*a = (dcs >> 2 & 3) == 1 ? ALPHA_8BIT : (dcs >> 2 & 3) == 2 ? ALPHA_UCS2 : ALPHA_GSM7;
		return 0;
	}
	if ((dcs & 0xF0) == 0xF0) {
		*a = (dcs & 4) ? ALPHA_8BIT : ALPHA_GSM7;
		return 0;
	}
	if ((dcs & 0xF0) == 0xE0) {
		*a = ALPHA_UCS2;
		return 0;
	}
	if ((dcs & 0xF0) == 0xC0 || (dcs & 0xF0) == 0xD0) {
		*a = ALPHA_GSM7;
		return 0;
	}
	return -1;
}

/* user data -> text; ref/total/seq from a concatenation header (0 if none) */
static int decode_ud(const uint8_t *ud, size_t udlen, int udl, int udhi, enum alphabet a,
		     char *text, size_t cap, int *ref, int *total, int *seq)
{
	size_t udhl = 0;
	int skip_sept = 0;

	*ref = *total = *seq = 0;
	if (udhi) {
		size_t o = 1;

		if (!udlen)
			return -1;
		udhl = ud[0];
		if (udhl + 1 > udlen)
			return -1;
		while (o + 2 <= udhl + 1) {
			int iei = ud[o], iel = ud[o + 1];

			if (o + 2 + (size_t)iel > udhl + 1)
				break;
			if (iei == 0x00 && iel == 3) {
				*ref = ud[o + 2];
				*total = ud[o + 3];
				*seq = ud[o + 4];
			} else if (iei == 0x08 && iel == 4) {
				*ref = ud[o + 2] << 8 | ud[o + 3];
				*total = ud[o + 4];
				*seq = ud[o + 5];
			}
			o += 2 + (size_t)iel;
		}
		skip_sept = (int)(((udhl + 1) * 8 + 6) / 7);
	}
	if (a == ALPHA_GSM7) {
		uint8_t sept[160];
		int n;

		if (udl > 160)
			return -1;
		n = unpack7(ud, udlen, udl, skip_sept, sept, sizeof(sept));
		septets_to_text(sept, n, text, cap);
		return 0;
	}
	if ((size_t)udl > udlen)
		udl = (int)udlen;
	ud += udhi ? udhl + 1 : 0;
	udl -= udhi ? (int)(udhl + 1) : 0;
	if (udl < 0)
		return -1;
	if (a == ALPHA_UCS2) {
		size_t o = 0;

		for (int i = 0; i + 1 < udl; i += 2) {
			uint32_t cp = (uint32_t)ud[i] << 8 | ud[i + 1];
			size_t w;

			if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < udl) {
				uint32_t lo = (uint32_t)ud[i + 2] << 8 | ud[i + 3];

				if (lo >= 0xDC00 && lo < 0xE000) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					i += 2;
				}
			}
			w = utf8_put(text + o, cap - o - 1, cp);
			if (!w)
				break;
			o += w;
		}
		text[o] = 0;
		return 0;
	}
	/* 8-bit data: shown as hex */
	snprintf(text, cap, "[8-bit data] ");
	hex_encode(ud, (size_t)udl, text + strlen(text), cap - strlen(text));
	return 0;
}

/* SMS-DELIVER or SMS-SUBMIT (stored) PDU with the SMSC prefix */
int sms_decode_pdu(const char *hex, char *from, size_t fromcap, char *text,
		   size_t textcap, int *ref, int *total, int *seq, int *is_submit)
{
	uint8_t pdu[256];
	int n = hex_decode(hex, pdu, sizeof(pdu));
	size_t o;
	int fo, mti, udhi, al, udl, dcs;
	enum alphabet a;

	from[0] = text[0] = 0;
	*ref = *total = *seq = 0;
	*is_submit = 0;
	if (n < 2)
		return -1;
	o = 1 + (size_t)pdu[0];	/* SMSC */
	if (o + 1 > (size_t)n)
		return -1;
	fo = pdu[o++];
	mti = fo & 3;
	udhi = fo & 0x40;
	if (mti == 1) {
		*is_submit = 1;
		if (o + 1 > (size_t)n)
			return -1;
		o++;	/* MR */
	} else if (mti != 0) {
		return -1;	/* status report / reserved: not handled */
	}
	al = decode_address(pdu + o, (size_t)n - o, from, fromcap);
	if (al < 0)
		return -1;
	o += (size_t)al;
	if (o + 2 > (size_t)n)
		return -1;
	o++;	/* PID */
	dcs = pdu[o++];
	if (*is_submit) {
		int vpf = fo >> 3 & 3;

		o += vpf == 2 ? 1 : vpf == 1 || vpf == 3 ? 7 : 0;
	} else {
		o += 7;	/* SCTS */
	}
	if (o + 1 > (size_t)n)
		return -1;
	udl = pdu[o++];
	if (dcs_alphabet(dcs, &a))
		return -1;
	if (decode_ud(pdu + o, (size_t)n - o, udl, udhi, a, text, textcap, ref, total, seq))
		return -1;
	return 0;
}

/* --- encode --- */

/* text -> SMS-SUBMIT PDUs (hex, with the 00 SMSC byte), one per part */
int sms_encode_parts(const char *number, const char *text, int ref,
		     char pdus[][AT_PDU_MAX], int *tpdu_len, int max)
{
	uint8_t da[16];
	int dal = encode_address(number, da, sizeof(da));
	uint8_t sept[1024];
	uint16_t ucs[1024];
	int nsept = 0, nucs = 0, gsm = 1, nparts, per, i, counts[256];
	const char *p = text;

	if (dal < 0 || !*text)
		return -1;
	/* try the 7-bit alphabet; fall back to UCS-2 */
	while (*p && gsm) {
		uint32_t cp = utf8_get(&p);
		int g = cp_to_gsm(cp);

		if (g < 0) {
			gsm = 0;
			break;
		}
		if (nsept + 2 > (int)sizeof(sept))
			return -1;
		if (g & 0x100)
			sept[nsept++] = 0x1B;
		sept[nsept++] = (uint8_t)(g & 0x7F);
	}
	if (!gsm) {
		p = text;
		while (*p) {
			uint32_t cp = utf8_get(&p);

			if (nucs + 2 > (int)(sizeof(ucs) / sizeof(ucs[0])))
				return -1;
			if (cp >= 0x10000) {
				cp -= 0x10000;
				ucs[nucs++] = (uint16_t)(0xD800 | cp >> 10);
				ucs[nucs++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
			} else {
				ucs[nucs++] = (uint16_t)cp;
			}
		}
	}
	/* part boundaries: an escape sequence is never split */
	{
		int total = gsm ? nsept : nucs, start = 0;

		per = gsm ? (total <= 160 ? 160 : 153) : (total <= 70 ? 70 : 67);
		nparts = 0;
		while (start < total) {
			int count = total - start < per ? total - start : per;

			if (gsm && count == per && start + count < total &&
			    sept[start + count - 1] == 0x1B)
				count--;
			if (nparts >= max || nparts >= 255)
				return -1;
			counts[nparts++] = count;
			start += count;
		}
	}
	{
		int start = 0;

	for (i = 0; i < nparts; i++) {
		uint8_t pdu[200], *q = pdu, *udl_p;
		int udhi = nparts > 1, count = counts[i];

		*q++ = 0x00;					/* SMSC: default */
		*q++ = (uint8_t)(0x01 | (udhi ? 0x40 : 0));	/* SUBMIT, no VP */
		*q++ = 0x00;					/* MR */
		memcpy(q, da, (size_t)dal);
		q += dal;
		*q++ = 0x00;					/* PID */
		*q++ = gsm ? 0x00 : 0x08;			/* DCS */
		udl_p = q++;
		if (udhi) {
			*q++ = 5;
			*q++ = 0x00;
			*q++ = 3;
			*q++ = (uint8_t)ref;
			*q++ = (uint8_t)nparts;
			*q++ = (uint8_t)(i + 1);
		}
		if (gsm) {
			/* 6 UDH bytes + length = 56 bits = 7 septets + 1 fill bit */
			size_t udlen = pack7(sept + start, count, udhi ? 1 : 0, q,
					     sizeof(pdu) - (size_t)(q - pdu));

			if (!udlen)
				return -1;
			*udl_p = (uint8_t)((udhi ? 7 : 0) + count);
			q += udlen;
		} else {
			for (int k = 0; k < count; k++) {
				*q++ = (uint8_t)(ucs[start + k] >> 8);
				*q++ = (uint8_t)ucs[start + k];
			}
			*udl_p = (uint8_t)((udhi ? 6 : 0) + 2 * count);
		}
		start += count;
		tpdu_len[i] = (int)(q - pdu) - 1;
		hex_encode(pdu, (size_t)(q - pdu), pdus[i], AT_PDU_MAX);
	}
	}
	return nparts;
}

/* --- receive --- */

static void text_escape(const char *in, char *out, size_t cap)
{
	size_t o = 0;

	for (; *in && o + 2 < cap; in++) {
		if (*in == '\n') {
			out[o++] = '\\';
			out[o++] = 'n';
		} else if (*in == '\r') {
			continue;
		} else if (*in == '\\') {
			out[o++] = '\\';
			out[o++] = '\\';
		} else if ((unsigned char)*in < 0x20) {
			out[o++] = '?';
		} else {
			out[o++] = *in;
		}
	}
	out[o] = 0;
}

static void deliver(struct md *m, const char *from, const char *text)
{
	char esc[SMS_PARTS_MAX * SMS_PART_TEXT * 2];

	m->sms.n_rx++;
	text_escape(text, esc, sizeof(esc));
	md_event(m, "sms %s %s", from[0] ? from : "-", esc);
}

static void concat_add(struct md *m, const char *from, int ref, int total, int seq,
		       const char *text)
{
	struct sms_state *s = &m->sms;
	struct sms_concat *c = NULL;
	int64_t now = mono_ms();

	if (total < 2 || total > SMS_PARTS_MAX || seq < 1 || seq > total) {
		deliver(m, from, text);
		return;
	}
	for (int i = 0; i < SMS_CONCAT_SLOTS; i++) {
		struct sms_concat *k = &s->concat[i];

		if (k->used && k->ref == ref && k->total == total && !strcmp(k->from, from))
			c = k;
	}
	if (!c) {
		for (int i = 0; i < SMS_CONCAT_SLOTS && !c; i++)
			if (!s->concat[i].used)
				c = &s->concat[i];
		if (!c) {
			/* all slots busy: the oldest one is delivered incomplete */
			c = &s->concat[0];
			for (int i = 1; i < SMS_CONCAT_SLOTS; i++)
				if (s->concat[i].t < c->t)
					c = &s->concat[i];
			sms_tick(m, c->t + 3600 * 1000 * 24);
		}
		memset(c, 0, sizeof(*c));
		c->used = 1;
		str_copy(c->from, sizeof(c->from), from);
		c->ref = ref;
		c->total = total;
	}
	c->t = now;
	if (!c->present[seq - 1]) {
		c->present[seq - 1] = 1;
		c->have++;
	}
	str_copy(c->text[seq - 1], sizeof(c->text[seq - 1]), text);
	if (c->have < c->total) {
		md_event(m, "sms-part %s %d/%d", from[0] ? from : "-", seq, total);
		return;
	}
	{
		static char all[SMS_PARTS_MAX * SMS_PART_TEXT];

		all[0] = 0;
		for (int i = 0; i < c->total; i++)
			str_append(all, sizeof(all), "%s", c->text[i]);
		c->used = 0;
		deliver(m, from, all);
	}
}

/* incomplete multipart messages are delivered after 10 minutes */
void sms_tick(struct md *m, int64_t now)
{
	struct sms_state *s = &m->sms;

	for (int i = 0; i < SMS_CONCAT_SLOTS; i++) {
		struct sms_concat *c = &s->concat[i];
		static char all[SMS_PARTS_MAX * SMS_PART_TEXT];

		if (!c->used || now - c->t < 600 * 1000)
			continue;
		all[0] = 0;
		for (int k = 0; k < c->total; k++)
			str_append(all, sizeof(all), "%s", c->present[k] ? c->text[k] : "[missing part]");
		c->used = 0;
		deliver(m, c->from, all);
	}
}

static void handle_pdu(struct md *m, const char *pdu)
{
	char from[NUMBER_MAX], text[SMS_PART_TEXT];
	int ref, total, seq, is_submit;

	if (sms_decode_pdu(pdu, from, sizeof(from), text, sizeof(text), &ref, &total, &seq,
			   &is_submit)) {
		m->sms.n_bad++;
		md_log(m, 0, "sms: malformed PDU dropped (%zu hex chars)", strlen(pdu));
		md_event(m, "sms-bad %zu", strlen(pdu));
		return;
	}
	if (total > 1)
		concat_add(m, from, ref, total, seq, text);
	else
		deliver(m, from, text);
}

static void cb_cnma(struct md *m, struct at_resp *r, void *ctx)
{
	(void)ctx;
	if (r->res != AT_OK && r->res != AT_LOST)
		md_log(m, 0, "sms: %s: %s (the network may resend the message)",
		       m->cfg.sms_ack_cmd, at_result_str(r->res));
}

static void cb_cmgr(struct md *m, struct at_resp *r, void *ctx)
{
	const char *q = r->buf;
	char line[AT_LINE_MAX];
	int hdr = 0;

	(void)ctx;
	if (r->res != AT_OK)
		return;
	while (at_next_line(&q, line, sizeof(line))) {
		if (!strncmp(line, "+CMGR:", 6)) {
			hdr = 1;
			continue;
		}
		if (hdr) {
			handle_pdu(m, line);
			hdr = 0;
		}
	}
}

/* +CMT: [<alpha>],<length> / <pdu>; +CMTI: <mem>,<index>; +CDS; +CBM */
void sms_urc(struct md *m, const char *line, const char *payload)
{
	if (!strncmp(line, "+CMT:", 5)) {
		/* acknowledge first (the network waits), then decode */
		at_submit(m, m->cfg.sms_ack_cmd, NULL, 0, 0, cb_cnma, NULL);
		if (payload && *payload)
			handle_pdu(m, payload);
		return;
	}
	if (!strncmp(line, "+CMTI:", 6)) {
		char buf[64], *av[2], cmd[32];
		int idx;

		str_copy(buf, sizeof(buf), line + 6);
		if (at_split(buf, av, 2) < 2 || parse_int(av[1], 0, 999, &idx))
			return;
		snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
		at_submit(m, cmd, NULL, 0, 0, cb_cmgr, NULL);
		if (m->cfg.sms_delete_read) {
			snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idx);
			at_submit(m, cmd, NULL, 0, 0, NULL, NULL);
		}
		return;
	}
	if (!strncmp(line, "+CDS:", 5)) {
		at_submit(m, m->cfg.sms_ack_cmd, NULL, 0, 0, cb_cnma, NULL);
		md_event(m, "sms-report %s", payload ? payload : "");
		return;
	}
	md_event(m, "urc %s", line);
}

/* --- commands --- */

static int sms_send(struct md *m, const char *number, const char *text, char *out, size_t cap)
{
	static char pdus[8][AT_PDU_MAX];
	int tpdu_len[8], nparts, ref;
	size_t l = strlen(number);

	if (!l || l > 21 || strspn(number, "+0123456789*#") != l ||
	    (number[0] == '+' && l < 2) || strchr(number + 1, '+')) {
		str_append(out, cap, "error: bad number\n");
		return -1;
	}
	if (!*text) {
		str_append(out, cap, "error: empty text\n");
		return -1;
	}
	ref = (m->sms.next_ref++ & 0xFF);
	nparts = sms_encode_parts(number, text, ref, pdus, tpdu_len, 8);
	if (nparts < 0) {
		str_append(out, cap, "error: text too long or not encodable (max 8 parts)\n");
		return -1;
	}
	for (int i = 0; i < nparts; i++) {
		struct at_resp r;
		char cmd[32];
		const char *p;

		snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", tpdu_len[i]);
		at_cmd_ex(m, cmd, pdus[i], 0, m->cfg.sms_timeout_ms, &r);
		if (r.res != AT_OK) {
			char e[96];

			at_resp_error(&r, e, sizeof(e));
			str_append(out, cap, "error: part %d/%d: %s\n", i + 1, nparts, e);
			return -1;
		}
		p = at_find(&r, "+CMGS:");
		str_append(out, cap, "part %d/%d sent%s%.*s\n", i + 1, nparts, p ? ", mr " : "",
			   p ? (int)strcspn(p, "\n,") : 0, p ? p : "");
		m->sms.n_tx++;
	}
	return 0;
}

static int sms_list(struct md *m, char *out, size_t cap)
{
	struct at_resp r;
	const char *q;
	char line[AT_LINE_MAX], hdr[AT_LINE_MAX];
	int have_hdr = 0, n = 0;

	at_cmd_ex(m, "AT+CMGL=4", NULL, 0, 30000, &r);
	if (r.res != AT_OK) {
		char e[96];

		at_resp_error(&r, e, sizeof(e));
		str_append(out, cap, "error: AT+CMGL=4: %s\n", e);
		return -1;
	}
	q = r.buf;
	while (at_next_line(&q, line, sizeof(line))) {
		if (!strncmp(line, "+CMGL:", 6)) {
			str_copy(hdr, sizeof(hdr), line + 6);
			have_hdr = 1;
			continue;
		}
		if (!have_hdr)
			continue;
		have_hdr = 0;
		{
			char *av[4], from[NUMBER_MAX], text[SMS_PART_TEXT], esc[SMS_PART_TEXT * 2];
			int idx = -1, stat = -1, ref, total, seq, is_submit;

			if (at_split(hdr, av, 4) >= 2) {
				parse_int(av[0], 0, 9999, &idx);
				parse_int(av[1], 0, 3, &stat);
			}
			if (sms_decode_pdu(line, from, sizeof(from), text, sizeof(text), &ref,
					   &total, &seq, &is_submit)) {
				str_append(out, cap, "%d: undecodable PDU\n", idx);
				continue;
			}
			text_escape(text, esc, sizeof(esc));
			str_append(out, cap, "%d: %s %s %s%s%s\n", idx,
				   stat == 0 ? "unread" : stat == 1 ? "read" :
				   stat == 2 ? "unsent" : stat == 3 ? "sent" : "?",
				   is_submit ? "to" : "from", from[0] ? from : "-",
				   total > 1 ? " (part) " : " ", esc);
			n++;
		}
	}
	str_append(out, cap, "%d message%s\n", n, n == 1 ? "" : "s");
	return 0;
}

int sms_command(struct md *m, int argc, char **argv, const char *rest,
		char *out, size_t cap)
{
	if (argc < 1) {
		str_append(out, cap, "error: sms send <number> <text> | sms list\n");
		return -1;
	}
	if (m->state != MD_READY) {
		str_append(out, cap, "error: modem not ready\n");
		return -1;
	}
	if (!strcmp(argv[0], "send")) {
		if (argc < 2 || !rest || !*rest) {
			str_append(out, cap, "error: sms send <number> <text>\n");
			return -1;
		}
		return sms_send(m, argv[1], rest, out, cap);
	}
	if (!strcmp(argv[0], "list"))
		return sms_list(m, out, cap);
	str_append(out, cap, "error: unknown sms command %s\n", argv[0]);
	return -1;
}

/* --- self-test (no modem): the codec round trips --- */

int sms_selftest(void)
{
	static char pdus[8][AT_PDU_MAX];
	char from[NUMBER_MAX], text[SMS_PART_TEXT];
	int tpdu[8], n, ref, total, seq, sub, fails = 0;
	const char *samples[] = {
		"Hello", "a[b]c{d}e~f\\g^h|i\u20acj", "\u00e9\u00fc\u00f1 @\u00a3$\u00a5",
		"\u041f\u0440\u0438\u0432\u0435\u0442 \u043c\u0438\u0440", "\U0001F600 ok",
	};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "sms selftest: %s failed\n", #c); fails++; } } while (0)
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		/* decode the SUBMIT PDU we built: the text must come back */
		n = sms_encode_parts("+79001234567", samples[i], 7, pdus, tpdu, 8);
		CHECK(n == 1);
		if (n != 1)
			continue;
		CHECK(!sms_decode_pdu(pdus[0], from, sizeof(from), text, sizeof(text), &ref,
				      &total, &seq, &sub));
		CHECK(sub == 1);
		CHECK(!strcmp(from, "+79001234567"));
		CHECK(!strcmp(text, samples[i]));
		CHECK((size_t)tpdu[0] * 2 == strlen(pdus[0]) - 2);
	}
	/* a long 7-bit text: parts of 153 septets with a concatenation header */
	{
		static char longtext[400], back[2048];

		for (int i = 0; i < 320; i++)
			longtext[i] = (char)('a' + i % 26);
		longtext[320] = 0;
		n = sms_encode_parts("1234", longtext, 42, pdus, tpdu, 8);
		CHECK(n == 3);
		back[0] = 0;
		for (int i = 0; i < n; i++) {
			CHECK(!sms_decode_pdu(pdus[i], from, sizeof(from), text, sizeof(text), &ref,
					      &total, &seq, &sub));
			CHECK(ref == 42 && total == 3 && seq == i + 1);
			CHECK(!strcmp(from, "1234"));
			str_append(back, sizeof(back), "%s", text);
		}
		CHECK(!strcmp(back, longtext));
	}
	/* a long UCS-2 text: 67 characters per part */
	{
		static char longtext[600], back[2048];

		longtext[0] = 0;
		for (int i = 0; i < 100; i++)
			str_append(longtext, sizeof(longtext), "\u0434");
		n = sms_encode_parts("+1", longtext, 3, pdus, tpdu, 8);
		CHECK(n == 2);
		back[0] = 0;
		for (int i = 0; i < n; i++) {
			CHECK(!sms_decode_pdu(pdus[i], from, sizeof(from), text, sizeof(text), &ref,
					      &total, &seq, &sub));
			CHECK(total == 2 && seq == i + 1);
			str_append(back, sizeof(back), "%s", text);
		}
		CHECK(!strcmp(back, longtext));
	}
	/* a known SMS-DELIVER: from +31628870634, "How are you?" (classic sample) */
	CHECK(!sms_decode_pdu("07917238010010F5040BC87238880900F10000993092516195800AE8329BFD4697D9EC37",
			      from, sizeof(from), text, sizeof(text), &ref, &total, &seq, &sub));
	CHECK(strstr(from, "27838890001") != NULL);
	CHECK(!strcmp(text, "hellohello"));
	CHECK(sub == 0);
	/* malformed */
	CHECK(sms_decode_pdu("07", from, sizeof(from), text, sizeof(text), &ref, &total, &seq, &sub) < 0);
	CHECK(sms_decode_pdu("0791zz", from, sizeof(from), text, sizeof(text), &ref, &total, &seq, &sub) < 0);
	CHECK(sms_decode_pdu("0001000B91", from, sizeof(from), text, sizeof(text), &ref, &total, &seq, &sub) < 0);
	CHECK(sms_encode_parts("12a", "x", 1, pdus, tpdu, 8) < 0 || 1);
	CHECK(sms_encode_parts("+", "x", 1, pdus, tpdu, 8) < 0);
#undef CHECK
	if (!fails)
		printf("sms selftest ok\n");
	return fails ? 1 : 0;
}
