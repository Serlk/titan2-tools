// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-speechd: offline replay. A text script stands in for /dev/ccci_aud,
 * the modem state, the clock and ALSA; everything the daemon writes or does
 * goes to the output file, one line each:
 *
 *   T <ms> <hex>               packet written to the port
 *   A <ms> ctl <name>=<value>  mixer control set
 *   A <ms> pcm open <rate>     Voice Call PCM opened (both directions)
 *   A <ms> pcm close
 *   R <ms> <text>              reply line of a command (time it returned)
 *   S <hex>                    shm-dump
 *
 * Script lines (blank lines and # comments ignored):
 *
 *   cmd <command>       run a control command (as from the socket)
 *   rx <hex>            the modem sends this packet
 *   time <ms>           the clock moves on
 *   state ready|down    what the modem state file reads (-S none: no source)
 *   txfail <n>          the next n writes fail with ENODEV
 *   pcmfail <n>         the next n PCM opens fail
 *   ctlfail <name>      setting this control fails (name up to end of line)
 *   shm-put <off> <hex> write bytes into the raw-audio shared memory
 *   shm-dump <off> <n>  print n bytes of it
 *
 * While a command waits for an ack, the script continues with the next
 * line; a cmd line or the end of the script means "nothing happens", so
 * the wait runs into its timeout.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speechd.h"

#define LINE_MAX_LEN	16384
#define CTLFAIL_MAX	8

struct rp {
	FILE *in, *out;
	char line[LINE_MAX_LEN];
	int have;
	int lineno;
	int64_t now;
	int64_t time_left;	/* rest of a partly consumed time line */
	int time_partial;
	int state;		/* 1 ready, 0 down */
	int state_source;
	int txfail;
	int pcmfail;
	char ctlfail[CTLFAIL_MAX][64];
	int nctlfail;
	int bad;
};

static void put_hex(FILE *f, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		fprintf(f, "%02x", b[i]);
}

static int parse_hex(const char *h, uint8_t *out, size_t cap, size_t *n)
{
	size_t len = 0;

	while (*h && *h != '\n') {
		unsigned v;

		if (*h == ' ' || *h == '\t') {
			h++;
			continue;
		}
		if (len == cap || sscanf(h, "%2x", &v) != 1 ||
		    !h[1] || h[1] == ' ')
			return -1;
		out[len++] = (uint8_t)v;
		h += 2;
	}
	*n = len;
	return 0;
}

static char *peek(struct rp *rp)
{
	while (!rp->have) {
		char *p;

		if (!fgets(rp->line, sizeof(rp->line), rp->in))
			return NULL;
		rp->lineno++;
		p = strchr(rp->line, '\n');
		if (p)
			*p = 0;
		p = rp->line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p || *p == '#')
			continue;
		memmove(rp->line, p, strlen(p) + 1);
		rp->have = 1;
		rp->time_partial = 0;
	}
	return rp->line;
}

static void consume(struct rp *rp)
{
	rp->have = 0;
}

static int is_word(const char *l, const char *w)
{
	size_t n = strlen(w);

	return !strncmp(l, w, n) && (l[n] == ' ' || !l[n]);
}

static void directive(struct sd *s, struct rp *rp, const char *l)
{
	static uint8_t buf[LINE_MAX_LEN / 2];
	size_t n;

	if (is_word(l, "rx")) {
		if (parse_hex(l + 2, buf, sizeof(buf), &n)) {
			fprintf(stderr, "replay line %d: bad hex\n", rp->lineno);
			rp->bad = 1;
			return;
		}
		sd_rx(s, buf, n);
	} else if (is_word(l, "state")) {
		rp->state = !strcmp(l + 6, "ready");
		/* look at it now, not at the next 200 ms tick */
		s->tick_next = 0;
	} else if (is_word(l, "txfail")) {
		rp->txfail = atoi(l + 7);
	} else if (is_word(l, "pcmfail")) {
		rp->pcmfail = atoi(l + 8);
	} else if (is_word(l, "ctlfail")) {
		if (rp->nctlfail < CTLFAIL_MAX)
			snprintf(rp->ctlfail[rp->nctlfail++], 64, "%s", l + 8);
	} else if (is_word(l, "shm-put")) {
		char *end;
		unsigned long off = strtoul(l + 8, &end, 0);

		if (!s->shm || parse_hex(end, buf, sizeof(buf), &n) ||
		    off > SHM_SIZE || n > SHM_SIZE - off) {
			fprintf(stderr, "replay line %d: bad shm-put\n",
				rp->lineno);
			rp->bad = 1;
			return;
		}
		memcpy(s->shm + off, buf, n);
	} else if (is_word(l, "shm-dump")) {
		long off, len;

		if (!s->shm || sscanf(l + 9, "%li %li", &off, &len) != 2 ||
		    off < 0 || len < 0 || off > SHM_SIZE ||
		    len > SHM_SIZE - off) {
			fprintf(stderr, "replay line %d: bad shm-dump\n",
				rp->lineno);
			rp->bad = 1;
			return;
		}
		fputs("S ", rp->out);
		put_hex(rp->out, s->shm + off, len);
		fputc('\n', rp->out);
	} else {
		fprintf(stderr, "replay line %d: unknown '%s'\n", rp->lineno, l);
		rp->bad = 1;
	}
}

static int rp_wait(struct sd *s, int64_t deadline)
{
	struct rp *rp = s->io_ctx;
	char *l = peek(rp);

	if (!l || is_word(l, "cmd")) {
		/* nothing else happens before the deadline */
		if (deadline == SD_NEVER)
			return -1;
		if (rp->now < deadline)
			rp->now = deadline;
		return 0;
	}
	if (is_word(l, "time")) {
		if (!rp->time_partial) {
			rp->time_left = strtoll(l + 5, NULL, 0);
			rp->time_partial = 1;
			if (rp->time_left < 0)
				rp->time_left = 0;
		}
		if (deadline != SD_NEVER && rp->now + rp->time_left > deadline) {
			rp->time_left -= deadline - rp->now;
			rp->now = deadline;
			return 0;
		}
		rp->now += rp->time_left;
		consume(rp);
		return 1;
	}
	consume(rp);
	directive(s, rp, l);
	return 1;
}

static int rp_write(struct sd *s, const uint8_t *buf, size_t len)
{
	struct rp *rp = s->io_ctx;

	if (rp->txfail > 0) {
		rp->txfail--;
		return -ENODEV;
	}
	fprintf(rp->out, "T %" PRId64 " ", rp->now);
	put_hex(rp->out, buf, len);
	fputc('\n', rp->out);
	return 0;
}

static int rp_md_state(struct sd *s)
{
	struct rp *rp = s->io_ctx;

	return rp->state_source ? rp->state : -1;
}

static int64_t rp_now(struct sd *s)
{
	return ((struct rp *)s->io_ctx)->now;
}

static const struct sd_io replay_io = {
	.write = rp_write,
	.md_state = rp_md_state,
	.wait = rp_wait,
	.now = rp_now,
};

/* the fake ALSA backend */
static int fake_ctl(void *ctx, const char *name, const char *value)
{
	struct rp *rp = ctx;

	for (int i = 0; i < rp->nctlfail; i++)
		if (!strcmp(rp->ctlfail[i], name))
			return -1;
	fprintf(rp->out, "A %" PRId64 " ctl %s=%s\n", rp->now, name, value);
	return 0;
}

static int fake_pcm_open(void *ctx, unsigned rate)
{
	struct rp *rp = ctx;

	if (rp->pcmfail > 0) {
		rp->pcmfail--;
		return -1;
	}
	fprintf(rp->out, "A %" PRId64 " pcm open %u\n", rp->now, rate);
	return 0;
}

static void fake_pcm_close(void *ctx)
{
	struct rp *rp = ctx;

	fprintf(rp->out, "A %" PRId64 " pcm close\n", rp->now);
}

static const struct sd_audio_ops fake_audio = {
	.ctl = fake_ctl,
	.pcm_open = fake_pcm_open,
	.pcm_close = fake_pcm_close,
};

int replay_run(struct sd *s, const char *script, const char *out_path,
	       int state_source)
{
	static struct rp rp;
	static char reply[8192];
	int ret = 0;

	memset(&rp, 0, sizeof(rp));
	rp.state = 1;
	rp.state_source = state_source;
	rp.in = fopen(script, "re");
	if (!rp.in) {
		sd_log(s, 0, "%s: %s", script, strerror(errno));
		return 1;
	}
	rp.out = fopen(out_path, "we");
	if (!rp.out) {
		sd_log(s, 0, "%s: %s", out_path, strerror(errno));
		fclose(rp.in);
		return 1;
	}
	s->io = &replay_io;
	s->io_ctx = &rp;
	s->au = &fake_audio;
	s->au_ctx = &rp;
	s->port_open = 1;
	s->shm = calloc(1, SHM_SIZE);
	if (!s->shm) {
		ret = 1;
		goto out;
	}

	sd_recover(s);
	for (;;) {
		char *l;

		sd_deferred(s);
		l = peek(&rp);
		if (!l)
			break;
		if (is_word(l, "cmd")) {
			char cmd[256];

			snprintf(cmd, sizeof(cmd), "%s", l + 4);
			consume(&rp);
			sd_command(s, cmd, reply, sizeof(reply));
			for (char *p = reply, *e; *p; p = e + 1) {
				e = strchr(p, '\n');
				if (!e) {
					fprintf(rp.out, "R %" PRId64 " %s\n", rp.now, p);
					break;
				}
				fprintf(rp.out, "R %" PRId64 " %.*s\n", rp.now,
					(int)(e - p), p);
			}
			continue;
		}
		if (rp_wait(s, sd_next_deadline(s)) < 0)
			break;
		sd_run_timers(s);
	}
out:
	if (rp.bad)
		ret = 1;
	if (fclose(rp.out))
		ret = 1;
	fclose(rp.in);
	free(s->shm);
	s->shm = NULL;
	return ret;
}
