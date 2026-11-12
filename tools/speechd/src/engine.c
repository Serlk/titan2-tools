// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-speechd: the speech state machine: the A0/A1
 * message FIFO with one outstanding ack, synthesised acks, the modem's
 * notifications, modem loss and the call refresh, and the control commands.
 * libc only; the AP audio side goes through audio.c.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speechd.h"

/* receiver dl_gain attenuation (dB) per volume index, ul_gain (dB) */
static const int dl_atten[SD_VOL_MAX + 1] = { 22, 19, 16, 13, 10, 7, 4 };
#define UL_GAIN_DB	23

/* 0x1C: factory "Common" speech_common_para */
static const uint16_t stock_common[12] = {
	0x0000, 0xDABD, 0x7918, 0x2A00, 0x8001,
};

#define ENH_CTRL_DEFAULT	0x0003	/* bit1 always, bit0 enhancement on */

/* modem-side state bits: set by the ack of on, cleared by off */
static const struct {
	uint32_t bit;
	uint16_t on, off;
} md_bits[] = {
	/* order of the "off" messages */
	{ 0x200, 0x2F3C, 0x2F3D },	/* raw record */
	{ 0x080, 0x2F3A, 0x2F3B },	/* voice-memo record */
	{ 0x004, 0x2F43, 0x2F44 },	/* BGS */
	{ 0x400, 0x2F3E, 0x2F3F },	/* VoIP RX */
	{ 0x800, 0x2F48, 0x2F49 },	/* telephony TX */
	{ 0x010, 0x2F40, 0x2F41 },	/* TTY */
	{ 0x008, 0x2F30, 0x2F31 },	/* PCM 2-way */
	{ 0x001, 0x2F20, 0x2F21 },	/* speech */
};

#define N_MD_BITS	(sizeof(md_bits) / sizeof(md_bits[0]))

static int64_t now(struct sd *s)
{
	return s->io->now(s);
}

void sd_init(struct sd *s)
{
	memset(s, 0, sizeof(*s));
	s->ack_timeout_ms = SD_ACK_TIMEOUT_MS;
	s->handset_vol = "0";
	s->speaker_vol = "512";
	s->codec_routes = 0;		/* the codec belongs to UCM/PipeWire */
	s->smartpa = 1;
	s->rate = 48000;
	s->device = SD_EARPIECE;
	s->volume = 4;
	s->dl_sent = -1;
	s->band = -1;
	s->net_rate = -1;
	s->seq_next = 1;
}

/* --- persisted state (factory keeps it in Speech_* controls) --- */

static void persist(struct sd *s)
{
	char tmp[4096];
	FILE *f;

	if (!s->persist_path)
		return;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", s->persist_path) >=
	    (int)sizeof(tmp))
		return;
	f = fopen(tmp, "we");
	if (!f) {
		sd_log(s, 1, "%s: %s", tmp, strerror(errno));
		return;
	}
	fprintf(f, "pending=%#x\nmd_status=%#x\ncall=%d\ndevice=%s\nepof=%d\n",
		s->out_busy ? s->out.id : 0, s->md_status, s->call_active,
		audio_device_name(s->device), s->epof);
	if (fclose(f) || rename(tmp, s->persist_path)) {
		sd_log(s, 0, "%s: %s", s->persist_path, strerror(errno));
		remove(tmp);
	}
}

/* --- readiness and send gating --- */

int sd_md_ready(struct sd *s)
{
	int st;

	if (!s->port_open)
		return 0;
	st = s->io->md_state(s);
	if (st >= 0)
		return st;
	/* no state source: the modem's alive notification, or -a */
	return s->alive_seen || s->assume_ready;
}

/* NULL when id may be written now; answers do not wait for "ready" */
static const char *gate(struct sd *s, uint16_t id, int answer)
{
	if (!s->port_open)
		return "port not open";
	if (id == SPH_ALIVE_ACK)
		return NULL;
	if (s->epof)
		return "modem in EPOF";
	if (s->dead)
		return "modem lost";
	if (!answer && !sd_md_ready(s))
		return "modem not ready";
	return NULL;
}

static int tx(struct sd *s, const uint8_t *buf, size_t len, uint16_t id)
{
	int ret = s->io->write(s, buf, len);

	if (!ret) {
		s->n_tx++;
		sd_log(s, 1, "tx %#06x (%zu bytes)", id, len);
		return 0;
	}
	sd_log(s, 0, "write %#06x: %s", id, strerror(-ret));
	if (ret == -ENODEV)
		sd_md_lost(s, "write: modem not running");
	return -1;
}

static int send_msg(struct sd *s, const struct sd_msg *m)
{
	uint8_t buf[SPH_HDR + SPH_AP_PL_HDR + SPH_INFO_SIZE];
	size_t len;

	if (m->has_info)
		len = sph_put_payload(buf, sizeof(buf), m->id, SPH_TYPE_INFO,
				      m->info, SPH_INFO_SIZE);
	else
		len = sph_put_mailbox(buf, m->id, m->p16, 0);
	return tx(s, buf, len, m->id);
}

static void answer(struct sd *s, uint16_t id)
{
	const char *why = gate(s, id, 1);
	uint8_t buf[SPH_HDR];

	if (why) {
		s->n_drop++;
		sd_log(s, 0, "drop answer %#06x: %s", id, why);
		return;
	}
	tx(s, buf, sph_put_mailbox(buf, id, 0, 0), id);
}

/* --- FIFO, outstanding ack, completion --- */

static void effects(struct sd *s, uint16_t id)
{
	for (size_t i = 0; i < N_MD_BITS; i++) {
		if (id == md_bits[i].on)
			s->md_status |= md_bits[i].bit;
		if (id == md_bits[i].off)
			s->md_status &= ~md_bits[i].bit;
	}
	if (id == SPH_SPEECH_OFF)
		shm_reset_indices(s);
}

static void finish(struct sd *s, const struct sd_msg *m, int real)
{
	if (sph_class(m->id) == SPH_A1) {
		if (!real) {
			s->n_synth++;
			sd_log(s, 0, "%#06x: no ack, synthesised", m->id);
		}
		effects(s, m->id);
	}
	if (m->seq) {
		s->seq_done = m->seq;
		s->seq_real[m->seq & 63] = (uint8_t)real;
	}
}

static void q_kick(struct sd *s)
{
	if (s->in_kick)
		return;
	s->in_kick = 1;
	while (!s->out_busy && s->q_len) {
		struct sd_msg m = s->q[s->q_head];
		const char *why;

		s->q_head = (s->q_head + 1) % SD_QLEN;
		s->q_len--;
		why = gate(s, m.id, 0);
		if (why) {
			s->n_drop++;
			sd_log(s, 0, "drop %#06x: %s", m.id, why);
			finish(s, &m, 0);
			continue;
		}
		if (send_msg(s, &m)) {
			finish(s, &m, 0);
			continue;
		}
		if (sph_class(m.id) != SPH_A1) {
			finish(s, &m, 1);
			continue;
		}
		s->out = m;
		s->out_busy = 1;
		s->out_deadline = now(s) + s->ack_timeout_ms;
	}
	s->in_kick = 0;
	persist(s);
}

static uint32_t enqueue(struct sd *s, uint16_t id, uint16_t p16,
			const uint8_t *info)
{
	struct sd_msg *m;

	if (s->q_len == SD_QLEN) {
		struct sd_msg lost = { .id = id };

		s->n_drop++;
		sd_log(s, 0, "drop %#06x: queue full", id);
		finish(s, &lost, 0);
		return 0;
	}
	m = &s->q[(s->q_head + s->q_len) % SD_QLEN];
	memset(m, 0, sizeof(*m));
	m->id = id;
	m->p16 = p16;
	m->seq = s->seq_next++;
	if (!s->seq_next)
		s->seq_next = 1;
	if (info) {
		m->has_info = 1;
		memcpy(m->info, info, SPH_INFO_SIZE);
	}
	s->q_len++;
	q_kick(s);
	return m->seq;
}

static int seq_pending(struct sd *s, uint32_t seq)
{
	return (int32_t)(seq - s->seq_done) > 0;
}

/*
 * Block the caller for at most ms (the per-id time of) while modem
 * packets are handled: 1 acked, 0 completed without an ack (dropped,
 * write error, synthesised), -1 still waiting for the ack.
 */
static int wait_seq(struct sd *s, uint32_t seq, int ms)
{
	int64_t dl = now(s) + ms;

	if (!seq)
		return 0;
	s->depth++;
	while (seq_pending(s, seq) && now(s) < dl) {
		int64_t w = sd_next_deadline(s);

		if (s->io->wait(s, w < dl ? w : dl) < 0)
			break;
		sd_run_timers(s);
	}
	s->depth--;
	if (seq_pending(s, seq))
		return -1;
	return s->seq_real[seq & 63];
}

static int send_wait(struct sd *s, uint16_t id, uint16_t p16)
{
	return wait_seq(s, enqueue(s, id, p16, NULL), sph_block_ms(id));
}

static void build_info(struct sd *s, uint8_t blk[SPH_INFO_SIZE])
{
	struct sph_info in;

	memset(&in, 0, sizeof(in));
	in.app = 0;
	in.rate_enum = (uint8_t)sph_rate_enum(s->rate);
	in.ext_dev = 0;
	in.mute = (uint8_t)(((s->ul_mute || s->ul_hold) ? SPH_MUTE_UL : 0) |
			    (s->dl_mute ? SPH_MUTE_DL : 0));
	in.enh_ctrl = ENH_CTRL_DEFAULT;
	if (!s->zero_common)
		memcpy(in.common, stock_common, sizeof(in.common));
	in.smartpa = s->smartpa;
	sph_build_info(blk, &in);
}

static int send_info_wait(struct sd *s, uint16_t id)
{
	uint8_t blk[SPH_INFO_SIZE];

	build_info(s, blk);
	return wait_seq(s, enqueue(s, id, 0, blk), sph_block_ms(id));
}

static void send_gains(struct sd *s, int force)
{
	int dl = -dl_atten[s->volume];

	if (force || dl != s->dl_sent) {
		enqueue(s, SPH_DL_GAIN, (uint16_t)dl, NULL);
		s->dl_sent = dl;
	}
	if (force)
		enqueue(s, SPH_UL_GAIN, (uint16_t)(UL_GAIN_DB * 4), NULL);
}

/* --- timers, modem loss --- */

static int monitoring(struct sd *s)
{
	return (s->call_active || (s->md_status & 1)) && !s->dead && !s->epof;
}

int64_t sd_next_deadline(struct sd *s)
{
	int64_t d = SD_NEVER;

	if (s->out_busy)
		d = s->out_deadline;
	if ((monitoring(s) || s->dead) && s->tick_next < d)
		d = s->tick_next;
	return d;
}

/* the modem came back (alive, or ready without alive for a while) */
static void md_back(struct sd *s)
{
	s->dead = 0;
	s->epof = 0;
	s->ready_since = 0;
	/* its speech task restarted: nothing is on in the modem */
	s->md_status = 0;
	if (s->call_active)
		s->refresh_pending = 1;
	persist(s);
}

void sd_run_timers(struct sd *s)
{
	int64_t t = now(s);

	if (s->out_busy && t >= s->out_deadline) {
		struct sd_msg m = s->out;

		sd_log(s, 0, "%#06x: no ack in %u ms", m.id, s->ack_timeout_ms);
		s->out_busy = 0;
		finish(s, &m, 0);
		q_kick(s);
	}
	if (!(monitoring(s) || s->dead) || t < s->tick_next)
		return;
	s->tick_next = t + SD_MONITOR_MS;
	if (monitoring(s)) {
		if (!sd_md_ready(s))
			sd_md_lost(s, "modem not ready during speech");
		return;
	}
	/* dead: wait for 0xAFA0, but not forever once the modem is ready */
	if (s->io->md_state(s) == 1 && s->port_open) {
		if (!s->ready_since) {
			s->ready_since = t ? t : 1;
		} else if (t - s->ready_since >= SD_NO_ALIVE_MS) {
			sd_log(s, 0, "modem ready for %d ms without an alive notification, resuming",
			       SD_NO_ALIVE_MS);
			md_back(s);
		}
	} else {
		s->ready_since = 0;
	}
}

static void cancel_outstanding(struct sd *s)
{
	if (s->out_busy) {
		struct sd_msg m = s->out;

		s->out_busy = 0;
		finish(s, &m, 0);
	}
}

static void call_suspend(struct sd *s)
{
	if (!s->call_active)
		return;
	/* contract: close Voice Call, redo the call when it is back */
	if (s->pcm_open)
		audio_pcm_stop(s);
	s->refresh_pending = 1;
}

void sd_md_lost(struct sd *s, const char *why)
{
	if (s->dead)
		return;
	sd_log(s, 0, "modem lost: %s", why);
	s->dead = 1;
	s->alive_seen = 0;
	s->ready_since = 0;
	s->tick_next = now(s) + SD_MONITOR_MS;
	cancel_outstanding(s);
	call_suspend(s);
	q_kick(s);
}

static void refresh(struct sd *s)
{
	int r;

	s->refresh_pending = 0;
	sd_log(s, 0, "modem back: re-running speech on (rate %u, %s)",
	       s->rate, audio_device_name(s->device));
	if (!s->pcm_open && audio_pcm_start(s, s->rate))
		sd_log(s, 0, "refresh: Voice Call PCM did not start");
	r = send_info_wait(s, SPH_SPEECH_ON);
	if (r != 1)
		sd_log(s, 0, "refresh: speech on %s",
		       r < 0 ? "not acked yet" : "not acked");
	send_gains(s, 1);
	if (s->ul_mute)
		send_wait(s, SPH_UL_MUTE, 1);
}

/* top level only: work that blocks or must not run inside a handler */
void sd_deferred(struct sd *s)
{
	if (s->depth)
		return;
	if (s->shm && !s->shm_formatted && sd_md_ready(s)) {
		FILE *f = s->shm_marker ? fopen(s->shm_marker, "re") : NULL;

		if (f) {
			fclose(f);
			sd_log(s, 1, "shm: already formatted this boot");
		} else {
			shm_format(s->shm);
			sd_log(s, 0, "shm: formatted");
			if (s->shm_marker) {
				f = fopen(s->shm_marker, "we");
				if (!f || fclose(f))
					sd_log(s, 0, "%s: %s", s->shm_marker,
					       strerror(errno));
			}
		}
		s->shm_formatted = 1;
	}
	if (s->refresh_pending && s->call_active && !s->dead && !s->epof &&
	    sd_md_ready(s))
		refresh(s);
}

/* --- modem packets --- */

static void on_ack(struct sd *s, uint16_t id)
{
	struct sd_msg m;

	if (!s->out_busy) {
		sd_log(s, 0, "ack %#06x with nothing outstanding, ignored", id);
		return;
	}
	if (!sph_ack_pairs(s->out.id, id)) {
		sd_log(s, 0, "ack %#06x not in pair with %#06x, ignored", id,
		       s->out.id);
		return;
	}
	sd_log(s, 1, "ack %#06x", id);
	m = s->out;
	s->out_busy = 0;
	finish(s, &m, 1);
	q_kick(s);
}

/* the data of an MD message: direct payload or a ring record */
static int md_data(struct sd *s, const struct sph_rx *in, struct sph_rx *m,
		   uint8_t *buf, size_t cap)
{
	if (!in->mailbox) {
		*m = *in;
		return 0;
	}
	memset(m, 0, sizeof(*m));
	m->id = in->id;
	return shm_read_record(s, in->p32, in->p16, buf, cap, m);
}

static void copy_text(char *dst, size_t cap, const uint8_t *src, size_t n)
{
	size_t i;

	for (i = 0; i < n && i + 1 < cap && src[i]; i++)
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
	dst[i] = 0;
}

static void on_alive(struct sd *s, const struct sph_rx *in)
{
	uint8_t buf[SPH_MD_PL_HDR + SPH_MD_DATA_MAX];
	struct sph_rx m;

	if (md_data(s, in, &m, buf, sizeof(buf)))
		return;
	if (m.type != SPH_TYPE_ALIVE || m.size != SPH_ALIVE_SIZE ||
	    m.frag_idx != m.frag_total) {
		sd_log(s, 0, "alive: type %#x size %#x fragment %u/%u, not answered",
		       m.type, m.size, m.frag_idx, m.frag_total);
		return;
	}
	s->md_head_ver = get_le16(m.data);
	s->md_ver = get_le16(m.data + 2);
	sd_log(s, 0, "modem alive: header version %#x, speech version %#x",
	       s->md_head_ver, s->md_ver);
	s->alive_seen = 1;
	md_back(s);
	answer(s, SPH_ALIVE_ACK);
}

static void on_epof(struct sd *s)
{
	answer(s, SPH_EPOF_ACK);
	sd_log(s, 0, "modem EPOF: messages dropped until it is alive again");
	s->epof = 1;
	s->alive_seen = 0;
	cancel_outstanding(s);
	call_suspend(s);
	q_kick(s);
}

static void on_codec(struct sd *s, const struct sph_rx *in)
{
	uint8_t buf[SPH_MD_PL_HDR + SPH_MD_DATA_MAX];
	struct sph_rx m;

	if (s->recovering) {
		sd_log(s, 0, "codec info dropped during start-up recovery");
		return;
	}
	if (!md_data(s, in, &m, buf, sizeof(buf))) {
		if (m.type == SPH_TYPE_CODEC && m.size == SPH_CODEC_SIZE &&
		    m.frag_idx == m.frag_total) {
			copy_text(s->codec_info[0], sizeof(s->codec_info[0]),
				  m.data, 0x5C);
			copy_text(s->codec_info[1], sizeof(s->codec_info[1]),
				  m.data + 0x5C, SPH_CODEC_SIZE - 0x5C);
			sd_log(s, 0, "codec info: \"%s\" / \"%s\"",
			       s->codec_info[0], s->codec_info[1]);
		} else {
			sd_log(s, 0, "codec info: type %#x size %#x fragment %u/%u",
			       m.type, m.size, m.frag_idx, m.frag_total);
		}
	}
	answer(s, SPH_CODEC_ACK);
}

static void on_band(struct sd *s, uint16_t p16)
{
	static const char *const names[] = { "nb", "wb", "swb", "fb" };
	int band, rate;

	sph_band_decode(p16, &band, &rate);
	if (band != s->band || rate != s->net_rate)
		sd_log(s, 0, "network codec %#06x: band %s, rate %d", p16,
		       names[band], rate);
	s->band = band;
	s->net_rate = rate;
	/* the volume tables do not depend on the band here: nothing to resend */
	if (s->call_active)
		send_gains(s, 0);
}

static void on_data(struct sd *s, const struct sph_rx *in)
{
	uint8_t buf[SPH_MD_PL_HDR + SPH_MD_DATA_MAX];
	struct sph_rx m;

	switch (in->id) {
	case 0xAF60:
	case 0xAF62:
	case 0xAF64:
	case 0xAF66:
		/* a feature we never enable: consume the record and answer */
		if (in->mailbox && in->p16)
			md_data(s, in, &m, buf, sizeof(buf));
		sd_log(s, 0, "unexpected data message %#06x, answered", in->id);
		answer(s, (uint16_t)(in->id & 0x7FFF));
		break;
	default:
		sd_log(s, 1, "data message %#06x ignored", in->id);
		break;
	}
}

void sd_rx(struct sd *s, const uint8_t *buf, size_t len)
{
	const char *why = NULL, *warn = NULL;
	struct sph_rx m;

	s->n_rx++;
	if (sph_decode(buf, len, &m, &why, &warn)) {
		sd_log(s, 0, "drop: packet of %zu bytes: %s", len, why);
		return;
	}
	if (warn)
		sd_log(s, 0, "%#06x: %s", m.id, warn);
	sd_log(s, 1, "rx %#06x %s p16 %#x p32 %#x", m.id,
	       m.mailbox ? "mailbox" : "payload", m.p16, m.p32);
	if ((s->dead || s->epof) && m.id != SPH_MD_ALIVE) {
		sd_log(s, 0, "%#06x ignored: modem %s", m.id,
		       s->dead ? "lost" : "in EPOF");
		return;
	}
	switch (sph_class(m.id)) {
	case SPH_M2:
		on_ack(s, m.id);
		break;
	case SPH_M0:
		if (m.id == SPH_MD_BAND)
			on_band(s, m.p16);
		else if (m.id == SPH_MD_LOOPBACK)
			sd_log(s, 0, "IPC loopback request %#x ignored", m.p16);
		break;
	case SPH_M1:
		if (m.id == SPH_MD_ALIVE) {
			on_alive(s, &m);
		} else if (m.id == SPH_MD_EPOF) {
			on_epof(s);
		} else if (m.id == SPH_MD_CODEC) {
			on_codec(s, &m);
		} else {
			sd_log(s, 0, "request %#06x answered", m.id);
			answer(s, (uint16_t)(m.id & 0x7FFF));
		}
		break;
	case SPH_M3:
		on_data(s, &m);
		break;
	default:
		s->n_drop++;
		sd_log(s, 0, "drop: %#06x not supported", m.id);
		break;
	}
}

/* --- start-up recovery --- */

void sd_recover(struct sd *s)
{
	unsigned pending = 0, status = 0;
	int call = 0, epof = 0;
	enum sd_device dev = SD_EARPIECE;
	char line[128], val[32];
	FILE *f;

	if (!s->persist_path)
		return;
	f = fopen(s->persist_path, "re");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "pending=%x", &pending) == 1 ||
		    sscanf(line, "md_status=%x", &status) == 1 ||
		    sscanf(line, "call=%d", &call) == 1 ||
		    sscanf(line, "epof=%d", &epof) == 1)
			continue;
		if (sscanf(line, "device=%31s", val) == 1)
			dev = strcmp(val, "speaker") ? SD_EARPIECE : SD_SPEAKER;
	}
	fclose(f);
	if (!pending && !status && !call)
		return;
	sd_log(s, 0, "previous instance left pending %#x, modem status %#x, call %d",
	       pending, status, call);
	s->recovering = 1;
	s->md_status = status;
	s->epof = epof != 0;
	if (pending && sph_class((uint16_t)pending) == SPH_A1) {
		/* wait for that ack as if it had just been sent */
		struct sd_msg m = { .id = (uint16_t)pending };

		m.seq = s->seq_next++;
		s->out = m;
		s->out_busy = 1;
		s->out_deadline = now(s) + SD_RECOVER_WAIT_MS;
		wait_seq(s, m.seq, SD_RECOVER_WAIT_MS);
	}
	if (call)
		s->md_status |= 1;
	for (size_t i = 0; i < N_MD_BITS; i++)
		if (s->md_status & md_bits[i].bit)
			send_wait(s, md_bits[i].off, 0);
	if (call)
		audio_path_off(s, dev);
	s->recovering = 0;
	persist(s);
}

/* --- control commands --- */

struct reply {
	char *buf;
	size_t cap, len;
};

static void say(struct reply *r, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void say(struct reply *r, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (r->len >= r->cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(r->buf + r->len, r->cap - r->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	r->len += (size_t)n;
	if (r->len >= r->cap)
		r->len = r->cap - 1;
}

static const char *ack_word(int r)
{
	return r > 0 ? "acked" : r < 0 ? "pending" : "not acked";
}

static int parse_device(const char *v, enum sd_device *d)
{
	if (!strcmp(v, "earpiece") || !strcmp(v, "handset"))
		*d = SD_EARPIECE;
	else if (!strcmp(v, "speaker"))
		*d = SD_SPEAKER;
	else
		return -1;
	return 0;
}

static int parse_uint(const char *v, unsigned long max, unsigned long *out)
{
	char *end;

	if (!*v || *v == '-' || *v == '+')
		return -1;
	errno = 0;
	*out = strtoul(v, &end, 10);
	return (errno || *end || *out > max) ? -1 : 0;
}

static void cmd_call_start(struct sd *s, char **av, int ac, struct reply *r)
{
	enum sd_device dev = s->device;
	unsigned rate = s->rate;
	int64_t dl;
	int ready, ret;

	for (int i = 1; i < ac; i++) {
		unsigned long v;

		if (!strcmp(av[i], "--rate") && i + 1 < ac) {
			if (parse_uint(av[++i], 48000, &v) ||
			    sph_rate_enum((unsigned)v) < 0) {
				say(r, "error: rate must be 8000, 16000, 32000 or 48000\n");
				return;
			}
			rate = (unsigned)v;
		} else if (!strcmp(av[i], "--device") && i + 1 < ac) {
			if (parse_device(av[++i], &dev)) {
				say(r, "error: device must be earpiece or speaker\n");
				return;
			}
		} else {
			say(r, "error: call-start [--rate 48000|32000|16000|8000] [--device earpiece|speaker]\n");
			return;
		}
	}
	if (s->call_active) {
		say(r, "error: a call is active (call-stop first, or use device)\n");
		return;
	}
	if (dev == SD_SPEAKER && rate < 16000) {
		say(r, "error: the speaker needs at least 16000 Hz\n");
		return;
	}
	/* wait a little for the modem, then go on anyway */
	dl = now(s) + SD_READY_WAIT_MS;
	s->depth++;
	while (!sd_md_ready(s) && now(s) < dl) {
		int64_t w = now(s) + SD_READY_POLL_MS;

		if (s->io->wait(s, w < dl ? w : dl) < 0)
			break;
		sd_run_timers(s);
	}
	s->depth--;
	ready = sd_md_ready(s) && !s->dead && !s->epof;
	if (!ready)
		say(r, "warning: modem not ready\n");

	/* AP paths and PCM links before speech on */
	s->rate = rate;
	s->device = dev;
	if (audio_path_on(s, dev)) {
		audio_path_off(s, dev);
		say(r, "error: audio path could not be set (see the log)\n");
		return;
	}
	if (audio_pcm_start(s, rate)) {
		audio_path_off(s, dev);
		say(r, "error: the Voice Call PCM did not start\n");
		return;
	}
	s->call_active = 1;
	s->dl_mute = 0;
	s->ul_hold = 0;
	s->refresh_pending = 0;
	s->tick_next = 0;
	persist(s);


	ret = send_info_wait(s, SPH_SPEECH_ON);
	send_gains(s, 1);
	say(r, "speech-on: %s\n", ack_word(ret));
	say(r, "ok\n");
}

static void cmd_call_stop(struct sd *s, struct reply *r)
{
	int was = s->call_active, prev = s->ul_mute, ret;

	if (!was)
		say(r, "warning: no call was active\n");

	send_wait(s, SPH_UL_MUTE, 1);
	if (s->pcm_open)
		audio_pcm_stop(s);
	if (was) {
		audio_path_off(s, s->device);
	} else {
		audio_path_off(s, SD_EARPIECE);
		audio_path_off(s, SD_SPEAKER);
	}
	s->call_active = 0;
	s->refresh_pending = 0;
	s->dl_mute = 0;
	s->ul_hold = 0;
	persist(s);
	ret = send_wait(s, SPH_SPEECH_OFF, 0);
	/* the "lost during speech" state ends with speech off */
	if (s->dead) {
		s->dead = 0;
		s->ready_since = 0;
	}
	s->dl_sent = -1;
	send_wait(s, SPH_UL_MUTE, (uint16_t)prev);
	say(r, "speech-off: %s\n", ack_word(ret));
	say(r, "ok\n");
}

static void cmd_device(struct sd *s, enum sd_device dev, struct reply *r)
{
	enum sd_device old = s->device;
	int ret, sw;

	if (!s->call_active) {
		s->device = dev;
		persist(s);
		say(r, "ok\n");
		return;
	}
	if (dev == old) {
		say(r, "ok\n");
		return;
	}
	if (dev == SD_SPEAKER && s->rate < 16000) {
		say(r, "error: the speaker needs at least 16000 Hz\n");
		return;
	}
	/* mute DL/UL around the switch */
	s->dl_mute = 1;
	send_wait(s, SPH_DL_MUTE, 1);
	s->ul_hold = 1;
	send_wait(s, SPH_UL_MUTE, 1);
	sw = audio_switch_output(s, old, dev);
	if (sw) {
		audio_switch_output(s, dev, old);
	} else {
		s->device = dev;
		persist(s);
		ret = send_info_wait(s, SPH_ROUTING);
		say(r, "routing: %s\n", ack_word(ret));
	}
	s->dl_mute = 0;
	send_wait(s, SPH_DL_MUTE, 0);
	s->ul_hold = 0;
	send_wait(s, SPH_UL_MUTE, (uint16_t)s->ul_mute);
	if (sw)
		say(r, "error: output path could not be switched (see the log)\n");
	else
		say(r, "ok\n");
}

static void cmd_status(struct sd *s, struct reply *r)
{
	static const char *const bands[] = { "nb", "wb", "swb", "fb" };

	say(r, "modem=%s port=%s lost=%d epof=%d alive_seen=%d\n",
	    sd_md_ready(s) ? "ready" : "not-ready",
	    s->port_open ? "open" : "closed", s->dead, s->epof, s->alive_seen);
	say(r, "call=%s device=%s rate=%u volume=%d mute=%s pcm=%s\n",
	    s->call_active ? "on" : "off", audio_device_name(s->device),
	    s->rate, s->volume, s->ul_mute ? "on" : "off",
	    s->pcm_open ? "open" : "closed");
	say(r, "md_status=0x%x pending=0x%04x queued=%u refresh=%d codec_routes=%d\n",
	    s->md_status, s->out_busy ? s->out.id : 0, s->q_len,
	    s->refresh_pending, s->codec_routes);
	say(r, "band=%s net_rate=%d md_version=0x%x/0x%x\n",
	    s->band >= 0 ? bands[s->band] : "unknown", s->net_rate,
	    s->md_head_ver, s->md_ver);
	if (s->codec_info[0][0] || s->codec_info[1][0])
		say(r, "codec_info=%s hd_voice=%s\n", s->codec_info[0],
		    s->codec_info[1]);
	say(r, "rx=%lu tx=%lu dropped=%lu synthesised=%lu\n", s->n_rx,
	    s->n_tx, s->n_drop, s->n_synth);
	say(r, "ok\n");
}

void sd_command(struct sd *s, const char *line, char *out, size_t cap)
{
	struct reply r = { out, cap, 0 };
	char buf[256], *av[8], *save = NULL, *t;
	int ac = 0;

	out[0] = 0;
	if (strlen(line) >= sizeof(buf)) {
		say(&r, "error: command too long\n");
		return;
	}
	strcpy(buf, line);
	for (t = strtok_r(buf, " \t\r\n", &save); t && ac < 8;
	     t = strtok_r(NULL, " \t\r\n", &save))
		av[ac++] = t;
	if (t) {
		say(&r, "error: too many arguments\n");
		return;
	}
	if (!ac) {
		say(&r, "error: empty command\n");
		return;
	}
	sd_log(s, 0, "command: %s", line);
	if (!strcmp(av[0], "call-start")) {
		cmd_call_start(s, av, ac, &r);
	} else if (!strcmp(av[0], "call-stop") && ac == 1) {
		cmd_call_stop(s, &r);
	} else if (!strcmp(av[0], "device") && ac == 2) {
		enum sd_device d;

		if (parse_device(av[1], &d))
			say(&r, "error: device must be earpiece or speaker\n");
		else
			cmd_device(s, d, &r);
	} else if (!strcmp(av[0], "volume") && ac == 2) {
		unsigned long v;

		if (parse_uint(av[1], SD_VOL_MAX, &v)) {
			say(&r, "error: volume must be 0..%d\n", SD_VOL_MAX);
		} else {
			s->volume = (int)v;
			if (s->call_active)
				send_gains(s, 0);
			say(&r, "ok\n");
		}
	} else if (!strcmp(av[0], "mute") && ac == 2 &&
		   (!strcmp(av[1], "on") || !strcmp(av[1], "off"))) {
		s->ul_mute = !strcmp(av[1], "on");
		if (s->call_active) {
			int ret = send_wait(s, SPH_UL_MUTE, (uint16_t)s->ul_mute);

			say(&r, "mute: %s\n", ack_word(ret));
		}
		say(&r, "ok\n");
	} else if (!strcmp(av[0], "status") && ac == 1) {
		cmd_status(s, &r);
	} else {
		say(&r, "error: unknown command (call-start, call-stop, device, volume, mute, status)\n");
	}
	sd_deferred(s);
}
