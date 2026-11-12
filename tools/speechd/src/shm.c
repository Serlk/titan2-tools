// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-speechd: the speech rings in /dev/ccci_raw_audio: format
 * once per boot, index reset after speech off, and reading the modem data
 * records that a mailbox points at. libc only.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <string.h>

#include "speechd.h"

#define GUARD_LEN	32
#define GUARD_BYTE	0x0A
#define TAIL_GUARD	0xCFE0

#define PARAM_OFF	0x0080
#define PARAM_SIZE	0x3000
#define AP_OFF		0x3080
#define AP_SIZE		0x2000
#define MD_OFF		0x5080
#define MD_SIZE		0x7F60

#define AP_FORMATTED	0x1
#define AP_RESETTING	0x2
#define MD_READING	0x2

/* the mapping is write-combined: order the stores the modem looks at */
static void fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static uint32_t rd32(const uint8_t *shm, size_t off)
{
	return get_le32((const uint8_t *)shm + off);
}

static void wr32(uint8_t *shm, size_t off, uint32_t v)
{
	put_le32(shm + off, v);
}

static void desc(uint8_t *shm, size_t at, uint32_t off, uint32_t size)
{
	wr32(shm, at, off);
	wr32(shm, at + 4, size);
	wr32(shm, at + 8, 0);		/* read index */
	wr32(shm, at + 12, 0);		/* write index */
}

void shm_format(uint8_t *shm)
{
	wr32(shm, SHM_AP_FLAG, 0);
	fence();
	memset(shm, GUARD_BYTE, GUARD_LEN);
	wr32(shm, SHM_MD_FLAG, 0);
	desc(shm, SHM_DESC_PARAM, PARAM_OFF, PARAM_SIZE);
	desc(shm, SHM_DESC_AP, AP_OFF, AP_SIZE);
	desc(shm, SHM_DESC_MD, MD_OFF, MD_SIZE);
	memset(shm + 0x58, 0, 0x7C - 0x58);
	wr32(shm, 0x7C, 0x7C);
	memset(shm + PARAM_OFF, 0, TAIL_GUARD - PARAM_OFF);
	memset(shm + TAIL_GUARD, GUARD_BYTE, GUARD_LEN);
	fence();
	wr32(shm, SHM_AP_FLAG, AP_FORMATTED);
	fence();
}

/* index reset; 0 done, -1 the modem is reading */
int shm_reset_indices(struct sd *s)
{
	uint8_t *shm = s->shm;
	uint32_t ap;
	int ret = 0;

	if (!shm || !s->shm_formatted)
		return 0;
	ap = rd32(shm, SHM_AP_FLAG);
	wr32(shm, SHM_AP_FLAG, ap | AP_RESETTING);
	fence();
	if (rd32(shm, SHM_MD_FLAG) & MD_READING) {
		sd_log(s, 0, "shm: modem is reading, ring indices not reset");
		ret = -1;
	} else {
		static const size_t d[] = { SHM_DESC_PARAM, SHM_DESC_AP,
					    SHM_DESC_MD };

		for (size_t i = 0; i < 3; i++) {
			wr32(shm, d[i] + 8, 0);
			wr32(shm, d[i] + 12, 0);
		}
		fence();
	}
	wr32(shm, SHM_AP_FLAG, rd32(shm, SHM_AP_FLAG) & ~(uint32_t)AP_RESETTING);
	fence();
	return ret;
}

/*
 * A mailbox pointed at an md_data record: len bytes (10-byte header
 * included) at ring index idx. The record is copied out (it may wrap),
 * checked like a direct payload, and the ring read index is advanced.
 */
int shm_read_record(struct sd *s, uint32_t idx, uint16_t len,
		    uint8_t *out, size_t cap, struct sph_rx *m)
{
	const char *why = NULL;
	uint8_t *shm = s->shm;
	uint32_t widx, avail;

	if (!shm || !s->shm_formatted) {
		sd_log(s, 0, "drop: %#06x points into shared memory, which is not set up",
		       m->id);
		return -1;
	}
	if (idx >= MD_SIZE || len < SPH_MD_PL_HDR || len > MD_SIZE ||
	    len > cap) {
		sd_log(s, 0, "drop: %#06x ring record idx %#x len %u out of range",
		       m->id, idx, len);
		return -1;
	}
	if (rd32(shm, SHM_DESC_MD + 8) != idx)
		sd_log(s, 1, "shm: record at %#x, read index is %#x", idx,
		       rd32(shm, SHM_DESC_MD + 8));
	widx = rd32(shm, SHM_DESC_MD + 12);
	avail = widx >= idx ? widx - idx : MD_SIZE - idx + widx;
	if (widx < MD_SIZE && avail < len)
		sd_log(s, 0, "shm: record of %u bytes, only %u written", len,
		       avail);
	for (uint32_t i = 0; i < len; i++)
		out[i] = shm[MD_OFF + (idx + i) % MD_SIZE];
	wr32(shm, SHM_DESC_MD + 8, (idx + len) % MD_SIZE);
	fence();
	if (sph_parse_md_header(out, len, m, &why)) {
		sd_log(s, 0, "drop: %#06x ring record: %s", m->id, why);
		return -1;
	}
	if ((size_t)m->size + SPH_MD_PL_HDR != len) {
		sd_log(s, 0, "drop: %#06x ring record size %u, length %u",
		       m->id, m->size, len);
		return -1;
	}
	return 0;
}
