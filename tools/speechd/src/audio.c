// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-speechd: the AP audio path of a voice call: the PCM 1 interconnect switches (modem <-> AFE)
 * and the hostless "Voice Call" PCM, and, only with codec_routes (-r 1),
 * the codec controls. By default the codec (RCV/HP muxes, mic muxes and
 * PGAs, pin switches, volumes) belongs to the "Voice Call" UCM verb
 * (tools/ucm/Titan2/VoiceCall.conf), i.e. to PipeWire/WirePlumber, so that
 * a device change there and this daemon do not fight over the same
 * controls. Control names are those of our mt6878/mt6369/fs199x ASoC
 * drivers. The ALSA calls are behind sd_audio_ops (audio_alsa.c, or the
 * replay fake).
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <stddef.h>
#include <string.h>

#include "speechd.h"

#define VOL_HANDSET	"\x01"	/* placeholder: s->handset_vol */
#define VOL_SPEAKER	"\x02"	/* placeholder: s->speaker_vol */

struct ctl {
	const char *name;
	const char *on;		/* value at call start */
	const char *off;	/* value at call end, NULL = left alone */
	int required;		/* the call cannot work without it */
};

/* output codec part (codec_routes only) */
static const struct ctl out_earpiece[] = {
	{ "RCV Mux", "Voice Playback", "Open", 1 },
	{ "Handset Volume", VOL_HANDSET, NULL, 0 },
	{ "Earpiece Switch", "on", "off", 0 },
};

static const struct ctl out_speaker[] = {
	{ "Speaker Volume", VOL_SPEAKER, NULL, 0 },
	{ "Speaker Switch", "on", "off", 0 },
};

/* codec_routes only: main mic AIN0 -> UL1_CH1, second mic AIN2 -> UL1_CH2 */
static const struct ctl mic[] = {
	{ "UL_SRC_MUX", "AMIC", NULL, 0 },
	{ "MISO0_MUX", "UL1_CH1", NULL, 0 },
	{ "MISO1_MUX", "UL1_CH2", NULL, 0 },
	{ "PGA_L_Mux", "AIN0", NULL, 0 },
	{ "ADC_L_Mux", "Left Preamplifier", NULL, 0 },
	{ "PGA_R_Mux", "AIN2", NULL, 0 },
	{ "ADC_R_Mux", "Right Preamplifier", NULL, 0 },
	{ "PGA1 Volume", "2", NULL, 0 },
	{ "PGA2 Volume", "2", NULL, 0 },
	{ "Main Mic Switch", "on", "off", 0 },
	{ "Second Mic Switch", "on", "off", 0 },
};

/* AP -> modem (ADDA_UL_TO_MD1) */
static const struct ctl ul_md[] = {
	{ "PCM_1_PB_CH1 ADDA_UL_CH1", "1", "0", 1 },
	{ "PCM_1_PB_CH2 ADDA_UL_CH2", "1", "0", 1 },
};

/* modem -> AP: CH1 (mono) into both output channels */
static const struct ctl dl_earpiece[] = {
	{ "ADDA_DL_CH1 PCM_1_CAP_CH1", "1", "0", 1 },
	{ "ADDA_DL_CH2 PCM_1_CAP_CH1", "1", "0", 1 },
};

static const struct ctl dl_speaker[] = {
	{ "I2SOUT4_CH1 PCM_1_CAP_CH1", "1", "0", 1 },
	{ "I2SOUT4_CH2 PCM_1_CAP_CH1", "1", "0", 1 },
};

struct table {
	const struct ctl *c;
	size_t n;
};

#define T(x)	{ x, sizeof(x) / sizeof(x[0]) }

static const struct table out_tab[] = { T(out_earpiece), T(out_speaker) };
static const struct table dl_tab[] = { T(dl_earpiece), T(dl_speaker) };
static const struct table mic_tab = T(mic);
static const struct table ul_tab = T(ul_md);

const char *audio_device_name(enum sd_device dev)
{
	return dev == SD_SPEAKER ? "speaker" : "earpiece";
}

static const char *value(struct sd *s, const char *v)
{
	if (!strcmp(v, VOL_HANDSET))
		return s->handset_vol;
	if (!strcmp(v, VOL_SPEAKER))
		return s->speaker_vol;
	return v;
}

/* 0, or -1 when a required control failed */
static int apply(struct sd *s, const struct table *t, int on)
{
	int ret = 0;

	for (size_t i = 0; i < t->n; i++) {
		const struct ctl *c = on ? &t->c[i] : &t->c[t->n - 1 - i];
		const char *v = on ? c->on : c->off;

		if (!v)
			continue;
		v = value(s, v);
		if (!*v)
			continue;
		if (s->au->ctl(s->au_ctx, c->name, v)) {
			sd_log(s, 0, "audio: '%s' = '%s' failed%s", c->name, v,
			       c->required ? "" : " (not required)");
			if (c->required && on)
				ret = -1;
		}
	}
	return ret;
}

/*
 * order: output, mic, uplink to the modem, downlink from it. The
 * codec parts (output, mic) only with codec_routes; the PCM 1 switches
 * always: the Hostless Speech PCM cannot be opened without an enabled back
 * end, and the UCM verb sets the same switches to the same values.
 */
int audio_path_on(struct sd *s, enum sd_device dev)
{
	if (s->codec_routes &&
	    (apply(s, &out_tab[dev], 1) || apply(s, &mic_tab, 1)))
		return -1;
	if (apply(s, &ul_tab, 1) || apply(s, &dl_tab[dev], 1))
		return -1;
	return 0;
}

void audio_path_off(struct sd *s, enum sd_device dev)
{
	apply(s, &dl_tab[dev], 0);
	apply(s, &ul_tab, 0);
	if (s->codec_routes) {
		apply(s, &mic_tab, 0);
		apply(s, &out_tab[dev], 0);
	}
}

/* in a call: PCM 1 keeps running, only the output side moves */
int audio_switch_output(struct sd *s, enum sd_device from, enum sd_device to)
{
	apply(s, &dl_tab[from], 0);
	if (s->codec_routes) {
		apply(s, &out_tab[from], 0);
		if (apply(s, &out_tab[to], 1))
			return -1;
	}
	if (apply(s, &dl_tab[to], 1))
		return -1;
	return 0;
}

/* the hostless Voice Call PCM, both directions; one retry */
int audio_pcm_start(struct sd *s, unsigned rate)
{
	for (int i = 0; i < 2; i++) {
		if (!s->au->pcm_open(s->au_ctx, rate)) {
			s->pcm_open = 1;
			return 0;
		}
		s->au->pcm_close(s->au_ctx);
		sd_log(s, 0, "audio: Voice Call PCM at %u Hz did not start%s",
		       rate, i ? "" : ", retrying");
	}
	return -1;
}

void audio_pcm_stop(struct sd *s)
{
	s->au->pcm_close(s->au_ctx);
	s->pcm_open = 0;
}
