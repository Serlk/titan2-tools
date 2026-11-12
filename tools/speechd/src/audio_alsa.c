// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-speechd: alsa-lib backend. Mixer controls through the control API (by
 * name, values parsed as amixer cset does), and the hostless Voice Call PCM
 * held open in both directions with stop_threshold = boundary:
 * no data ever moves, the position
 * does not advance, and the stream must not be stopped as an xrun.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "speechd.h"

#define PERIOD_FRAMES	1024
#define PERIODS		2

struct alsa_be {
	char card[64];		/* e.g. hw:Titan2 */
	char pcm[64];		/* e.g. hw:Titan2,4 */
	snd_ctl_t *ctl;
	snd_pcm_t *pb, *cap;
};

void *alsa_backend_new(const char *card, const char *pcm)
{
	struct alsa_be *b = calloc(1, sizeof(*b));

	if (!b)
		return NULL;
	snprintf(b->card, sizeof(b->card), "%s", card);
	snprintf(b->pcm, sizeof(b->pcm), "%s", pcm);
	return b;
}

static void pcm_close_all(struct alsa_be *b)
{
	if (b->cap) {
		snd_pcm_drop(b->cap);
		snd_pcm_close(b->cap);
		b->cap = NULL;
	}
	if (b->pb) {
		snd_pcm_drop(b->pb);
		snd_pcm_close(b->pb);
		b->pb = NULL;
	}
}

void alsa_backend_free(void *ctx)
{
	struct alsa_be *b = ctx;

	if (!b)
		return;
	pcm_close_all(b);
	if (b->ctl)
		snd_ctl_close(b->ctl);
	free(b);
}

static int alsa_ctl(void *ctx, const char *name, const char *val)
{
	struct alsa_be *b = ctx;
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *v;
	int err;

	if (!b->ctl) {
		err = snd_ctl_open(&b->ctl, b->card, 0);
		if (err < 0) {
			fprintf(stderr, "t2-speechd: %s: %s\n", b->card,
				snd_strerror(err));
			b->ctl = NULL;
			return -1;
		}
	}
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_info_set_id(info, id);
	err = snd_ctl_elem_info(b->ctl, info);
	if (err < 0) {
		fprintf(stderr, "t2-speechd: control '%s': %s\n", name,
			snd_strerror(err));
		return -1;
	}
	snd_ctl_elem_info_get_id(info, id);
	snd_ctl_elem_value_set_id(v, id);
	err = snd_ctl_elem_read(b->ctl, v);
	if (err >= 0)
		err = snd_ctl_ascii_value_parse(b->ctl, v, info, val);
	if (err >= 0)
		err = snd_ctl_elem_write(b->ctl, v);
	if (err < 0) {
		fprintf(stderr, "t2-speechd: control '%s' = '%s': %s\n", name,
			val, snd_strerror(err));
		return -1;
	}
	return 0;
}

static int pcm_up(struct alsa_be *b, snd_pcm_t **pp, snd_pcm_stream_t dir,
		  unsigned rate)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t boundary, period = PERIOD_FRAMES;
	snd_pcm_uframes_t buffer = PERIOD_FRAMES * PERIODS;
	const char *what;
	snd_pcm_t *p;
	int err;

	err = snd_pcm_open(&p, b->pcm, dir, 0);
	if (err < 0) {
		what = "open";
		goto out;
	}
	*pp = p;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	what = "hw_params";
	if ((err = snd_pcm_hw_params_any(p, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(p, hw,
				SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(p, hw,
				SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(p, hw, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(p, hw, rate, 0)) < 0)
		goto out;
	/* factory geometry; the hostless FE takes what it gets */
	snd_pcm_hw_params_set_period_size_near(p, hw, &period, NULL);
	snd_pcm_hw_params_set_buffer_size_near(p, hw, &buffer);
	if ((err = snd_pcm_hw_params(p, hw)) < 0)
		goto out;
	what = "sw_params";
	if ((err = snd_pcm_sw_params_current(p, sw)) < 0 ||
	    (err = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
	    (err = snd_pcm_sw_params_set_stop_threshold(p, sw, boundary)) < 0 ||
	    (err = snd_pcm_sw_params(p, sw)) < 0)
		goto out;
	what = "start";
	if ((err = snd_pcm_prepare(p)) < 0 || (err = snd_pcm_start(p)) < 0)
		goto out;
	return 0;
out:
	fprintf(stderr, "t2-speechd: %s %s %s at %u Hz: %s\n", b->pcm,
		dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture", what,
		rate, snd_strerror(err));
	return -1;
}

/* factory starts playback, then capture */
static int alsa_pcm_open(void *ctx, unsigned rate)
{
	struct alsa_be *b = ctx;

	pcm_close_all(b);
	if (pcm_up(b, &b->pb, SND_PCM_STREAM_PLAYBACK, rate) ||
	    pcm_up(b, &b->cap, SND_PCM_STREAM_CAPTURE, rate))
		return -1;
	return 0;
}

static void alsa_pcm_close(void *ctx)
{
	pcm_close_all(ctx);
}

const struct sd_audio_ops alsa_audio_ops = {
	.ctl = alsa_ctl,
	.pcm_open = alsa_pcm_open,
	.pcm_close = alsa_pcm_close,
};
