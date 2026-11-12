/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * t2-speechd: voice-call speech control for the MT6878 modem over
 * /dev/ccci_aud, and the AP audio path in ALSA.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#ifndef T2_SPEECHD_H
#define T2_SPEECHD_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* --- wire format, proto.c: libc only --- */

#define SPH_HDR			16	/* CCCI header */
#define SPH_MTU			0xD80	/* packet limit, header included */
#define SPH_AP_PL_HDR		6	/* sync, type, size */
#define SPH_MD_PL_HDR		10	/* sync, type, size, index, total */
#define SPH_AP_DATA_MAX		0xD6A	/* N <= 3434 */
#define SPH_MD_DATA_MAX		0xD66	/* D < 0xD67 */
#define SPH_INFO_SIZE		0x80	/* speech info block */
#define SPH_CH_TX		5	/* CCCI_PCM_TX */
#define SPH_MAILBOX		0xFFFFFFFFu
#define SPH_SYNC_AP		0xA2A2
#define SPH_SYNC_MD		0x1234

#define SPH_TYPE_CODEC		0x14	/* MD network codec info */
#define SPH_TYPE_INFO		0x19	/* AP speech info block */
#define SPH_TYPE_ALIVE		0x1A	/* MD alive info */
#define SPH_CODEC_SIZE		0xB8
#define SPH_ALIVE_SIZE		0x20

/* AP -> MD */
#define SPH_DL_GAIN		0x2F00
#define SPH_UL_GAIN		0x2F01
#define SPH_UL_MUTE		0x2F02
#define SPH_DL_MUTE		0x2F03
#define SPH_SPEECH_ON		0x2F20
#define SPH_SPEECH_OFF		0x2F21
#define SPH_ROUTING		0x2F2B
#define SPH_EPOF_ACK		0x2F78
#define SPH_NEW_PARAM		0x2F7B
#define SPH_CODEC_ACK		0x2F90
#define SPH_ALIVE_ACK		0x2FA0
/* MD -> AP */
#define SPH_MD_IGNORED		0xAF70
#define SPH_MD_BAND		0xAF75
#define SPH_MD_EPOF		0xAF78
#define SPH_MD_CODEC		0xAF90
#define SPH_MD_ALIVE		0xAFA0
#define SPH_MD_LOOPBACK		0xAFE4

#define SPH_ACK(id)		((uint16_t)((id) | 0x8000))

enum sph_class {
	SPH_UNSUPPORTED,
	SPH_A0,		/* AP control, no ack */
	SPH_A1,		/* AP control, needs ack */
	SPH_A2,		/* AP answer to an MD request */
	SPH_A3,		/* AP data */
	SPH_M0,		/* MD notification */
	SPH_M1,		/* MD request, needs an answer */
	SPH_M2,		/* MD ack */
	SPH_M3,		/* MD data */
};

/* one decoded modem packet */
struct sph_rx {
	int mailbox;
	uint16_t id;
	uint16_t p16;		/* mailbox */
	uint32_t p32;		/* mailbox */
	uint16_t type;		/* payload */
	uint16_t size;
	uint16_t frag_idx;
	uint16_t frag_total;
	const uint8_t *data;	/* payload data, size bytes */
};

/* what goes into the speech info block */
struct sph_info {
	uint8_t app;		/* 0 phone call */
	uint8_t rate_enum;
	uint16_t ext_dev;
	uint8_t mute;		/* bit0 UL, bit1 DL, bit3 UL source, bit5 dyn DL */
	uint16_t enh_ctrl;
	uint16_t common[12];
	uint8_t smartpa;
};

#define SPH_MUTE_UL		0x01
#define SPH_MUTE_DL		0x02

enum sph_class sph_class(uint16_t id);
int sph_block_ms(uint16_t id);
int sph_ack_pairs(uint16_t req, uint16_t ack);
size_t sph_put_mailbox(uint8_t out[SPH_HDR], uint16_t id, uint16_t p16,
		       uint32_t p32);
size_t sph_put_payload(uint8_t *out, size_t cap, uint16_t id, uint16_t type,
		       const uint8_t *data, size_t n);
/* 0 ok; -1 not decodable (reason in *why) */
int sph_decode(const uint8_t *buf, size_t len, struct sph_rx *m,
	       const char **why, const char **warn);
int sph_parse_md_header(const uint8_t *rec, size_t len, struct sph_rx *m,
			const char **why);
void sph_build_info(uint8_t blk[SPH_INFO_SIZE], const struct sph_info *in);
int sph_rate_enum(unsigned rate);
void sph_band_decode(uint16_t p16, int *band, int *net_rate);

static inline uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* --- /dev/ccci_raw_audio rings, shm.c: libc only --- */

#define SHM_SIZE		0xD000
#define SHM_AP_FLAG		0x20
#define SHM_MD_FLAG		0x24
#define SHM_DESC_PARAM		0x28
#define SHM_DESC_AP		0x38
#define SHM_DESC_MD		0x48

struct sd;

void shm_format(uint8_t *shm);
int shm_reset_indices(struct sd *s);
/* indirect modem data: record of len bytes at ring index idx */
int shm_read_record(struct sd *s, uint32_t idx, uint16_t len,
		    uint8_t *out, size_t cap, struct sph_rx *m);

/* --- the daemon state, engine.c: libc only --- */

enum sd_device { SD_EARPIECE, SD_SPEAKER };

#define SD_QLEN			32
#define SD_VOL_MAX		6
#define SD_ACK_TIMEOUT_MS	60000	/* queue worker */
#define SD_READY_WAIT_MS	480	/* call start */
#define SD_READY_POLL_MS	20
#define SD_MONITOR_MS		200
#define SD_RECOVER_WAIT_MS	1000
#define SD_NO_ALIVE_MS		10000	/* lost: ready this long, no 0xAFA0 */
#define SD_NEVER		INT64_MAX

struct sd_msg {
	uint16_t id;
	uint16_t p16;
	uint32_t seq;
	int has_info;
	uint8_t info[SPH_INFO_SIZE];
};

/* transport: the real port or the replay script */
struct sd_io {
	/* write one packet: 0, or -errno */
	int (*write)(struct sd *s, const uint8_t *buf, size_t len);
	/* modem state: 1 ready, 0 not ready, -1 unknown (no state source) */
	int (*md_state)(struct sd *s);
	/*
	 * wait for at most one modem event (handled through sd_rx) until
	 * deadline; 1 event, 0 deadline reached, -1 end of input (replay)
	 */
	int (*wait)(struct sd *s, int64_t deadline);
	int64_t (*now)(struct sd *s);
};

/* AP audio: alsa-lib (audio_alsa.c) or the replay fake */
struct sd_audio_ops {
	int (*ctl)(void *ctx, const char *name, const char *value);
	int (*pcm_open)(void *ctx, unsigned rate);
	void (*pcm_close)(void *ctx);
};

struct sd {
	/* configuration */
	int verbose;
	int assume_ready;		/* -a: ready once the port is open */
	unsigned ack_timeout_ms;
	const char *persist_path;	/* state across restarts */
	const char *shm_marker;		/* "formatted this boot" marker */
	const char *handset_vol;	/* "Handset Volume" value, "" = keep */
	const char *speaker_vol;	/* "Speaker Volume" value */
	int codec_routes;		/* -r: 1 = set the codec controls too (old
					 * behaviour); 0 = only the PCM 1 switches,
					 * the codec belongs to UCM/PipeWire */
	uint8_t smartpa;		/* info block 0x54 */
	int zero_common;		/* -C: no factory common parameters */

	const struct sd_io *io;
	void *io_ctx;
	const struct sd_audio_ops *au;
	void *au_ctx;

	/* modem */
	int port_open;
	int alive_seen;
	int dead;			/* lost during speech / write ENODEV */
	int epof;
	int recovering;			/* start-up recovery */
	uint32_t md_status;		/* bits */
	uint16_t md_head_ver, md_ver;
	int band, net_rate;
	char codec_info[2][0x5C + 1];
	unsigned long n_rx, n_tx, n_drop, n_synth;

	/* A0/A1 FIFO and the one outstanding A1 */
	struct sd_msg q[SD_QLEN];
	unsigned q_head, q_len;
	struct sd_msg out;
	int out_busy;
	int64_t out_deadline;
	uint32_t seq_next, seq_done;
	uint8_t seq_real[64];		/* real ack for seq & 63 */
	int in_kick;

	/* the call (AP side) */
	int call_active;
	int pcm_open;
	int refresh_pending;
	unsigned rate;
	enum sd_device device;
	int volume;
	int ul_mute;
	int dl_mute;			/* transient, during a device switch */
	int ul_hold;			/* transient UL mute, device switch */
	int dl_sent;			/* last 0x2F00 value, -1 none */
	unsigned depth;			/* nested waits */
	int64_t tick_next;		/* status monitor */
	int64_t ready_since;		/* lost, but the state source is ready */

	/* shared memory */
	uint8_t *shm;
	int shm_formatted;
};

void sd_log(struct sd *s, int debug, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

void sd_init(struct sd *s);
int sd_md_ready(struct sd *s);
void sd_rx(struct sd *s, const uint8_t *buf, size_t len);
void sd_run_timers(struct sd *s);
int64_t sd_next_deadline(struct sd *s);
void sd_deferred(struct sd *s);
void sd_md_lost(struct sd *s, const char *why);
void sd_recover(struct sd *s);
/* run one control command; the reply goes to out (lines, last "ok"/"error") */
void sd_command(struct sd *s, const char *line, char *out, size_t cap);

/* audio.c: path tables over sd_audio_ops (libc only) */
int audio_path_on(struct sd *s, enum sd_device dev);
void audio_path_off(struct sd *s, enum sd_device dev);
int audio_switch_output(struct sd *s, enum sd_device from, enum sd_device to);
int audio_pcm_start(struct sd *s, unsigned rate);
void audio_pcm_stop(struct sd *s);
const char *audio_device_name(enum sd_device dev);

/* audio_alsa.c: the alsa-lib backend */
void *alsa_backend_new(const char *card, const char *pcm);
void alsa_backend_free(void *ctx);
extern const struct sd_audio_ops alsa_audio_ops;

/* replay.c */
int replay_run(struct sd *s, const char *script, const char *out_path,
	       int state_source);

#endif
