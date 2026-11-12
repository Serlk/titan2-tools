// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-fsd: command line, the /dev/ccci_fs loop and offline replay
 *.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fsd.h"

/* larger than any packet: a read() that fills it was an oversized one */
#define RX_BUF		0x8000
#define TX_TIMEOUT_MS	5000

static volatile sig_atomic_t stop;
static int modem_down;
static struct fsd fsd_state;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: t2-ccci-fsd [-v] -r ROOT -d /dev/ccci_fs [-p]\n"
		"       t2-ccci-fsd [-v] -r ROOT -R requests.bin -o replies.bin [-p]\n"
		"  -r ROOT  sandbox root (ro/, rw/, journal.log)\n"
		"  -d DEV   serve the CCCI file server port\n"
		"  -R, -o   offline replay: u32 LE length + fragment, in and out\n"
		"  -p       refuse writes to X: and Y: (protect_f, protect_s)\n"
		"  -v       verbose\n");
}

/* write one fragment to the port; the kernel sets length, channel, seq */
static int port_send(struct fsd *fsd, const uint8_t *frag, size_t len)
{
	int fd = *(int *)fsd->send_ctx;
	long waited = 0;

	for (;;) {
		ssize_t n = write(fd, frag, len);

		if (n == (ssize_t)len)
			return 0;
		if (n >= 0) {
			fsd_log(fsd, 0, "short write %zd of %zu", n, len);
			return -1;
		}
		switch (errno) {
		case EINTR:
			if (stop)
				return -1;
			continue;
		case EAGAIN:
			/*
			 * POLLOUT only says that writes are accepted, not that
			 * the queue has room: wait in small steps, bounded.
			 */
			if (stop || waited >= TX_TIMEOUT_MS) {
				fsd_log(fsd, 0, "port full for %ld ms", waited);
				return -1;
			}
			msleep(2);
			waited += 2;
			continue;
		case ENODEV:
			fsd_log(fsd, 0, "modem not running, reply dropped");
			modem_down = 1;
			return -1;
		default:
			fsd_log(fsd, 0, "write: %s", strerror(errno));
			return -1;
		}
	}
}

static int open_port(struct fsd *fsd, const char *dev)
{
	struct stat st1, st2;
	int fd;

	/* O_PATH does not call the driver: check before really opening */
	fd = open(dev, O_PATH | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st1)) {
		fsd_log(fsd, 0, "%s: %s", dev, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	if (!S_ISCHR(st1.st_mode)) {
		fsd_log(fsd, 0, "%s: not a character device, refusing", dev);
		return -1;
	}
	fd = open(dev, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		fsd_log(fsd, 0, "%s: %s", dev, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st2) || !S_ISCHR(st2.st_mode) ||
	    st1.st_rdev != st2.st_rdev || st1.st_ino != st2.st_ino) {
		fsd_log(fsd, 0, "%s: changed while opening, refusing", dev);
		close(fd);
		return -1;
	}
	return fd;
}

static int serve(struct fsd *fsd, const char *dev)
{
	static uint8_t buf[RX_BUF];
	int fd, errors = 0;

	fd = open_port(fsd, dev);
	if (fd < 0)
		return 1;
	fsd->send = port_send;
	fsd->send_ctx = &fd;
	fsd_journal(fsd, "START port=%s%s", dev,
		    fsd->refuse_protect ? " protect=refuse" : "");

	while (!stop) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			fsd_log(fsd, 0, "poll: %s", strerror(errno));
			break;
		}
		if (p.revents & POLLNVAL)
			break;
		if (!(p.revents & POLLIN)) {
			msleep(100);
			continue;
		}
		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			fsd_log(fsd, 0, "read: %s", strerror(errno));
			if (++errors > 100)
				break;
			msleep(100);
			continue;
		}
		if (!n) {
			/* not something /dev/ccci_fs does; do not spin */
			msleep(100);
			continue;
		}
		errors = 0;
		if (n == (ssize_t)sizeof(buf)) {
			/* the rest of an oversized packet comes with the next reads */
			while (read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf))
				;
			fsd_log(fsd, 0, "drop: oversized packet");
			continue;
		}
		fsd_feed(fsd, buf, (size_t)n);
		if (modem_down) {
			/* the modem restarts: its requests and handles are void */
			modem_down = 0;
			fsd_reset_slots(fsd);
			fsd_close_all(fsd);
			fsd_journal(fsd, "MODEM-RESET handles closed");
		}
	}
	fsd_journal(fsd, "STOP");
	close(fd);
	return stop ? 0 : 1;
}

static int replay_send(struct fsd *fsd, const uint8_t *frag, size_t len)
{
	FILE *out = fsd->send_ctx;
	uint8_t hdr[4];
	uint8_t copy[FSD_FRAG_MAX];

	/* what the kernel would fill in: length and the TX channel */
	memcpy(copy, frag, len);
	put_le32(copy + 4, (uint32_t)len);
	put_le32(copy + 8, FSD_CH_TX);
	put_le32(hdr, (uint32_t)len);
	if (fwrite(hdr, 1, 4, out) != 4 || fwrite(copy, 1, len, out) != len)
		return -1;
	return 0;
}

static int replay(struct fsd *fsd, const char *in_path, const char *out_path)
{
	static uint8_t buf[RX_BUF];
	FILE *in, *out;
	int ret = 0;

	in = fopen(in_path, "rbe");
	if (!in) {
		fsd_log(fsd, 0, "%s: %s", in_path, strerror(errno));
		return 1;
	}
	out = fopen(out_path, "wbe");
	if (!out) {
		fsd_log(fsd, 0, "%s: %s", out_path, strerror(errno));
		fclose(in);
		return 1;
	}
	fsd->send = replay_send;
	fsd->send_ctx = out;

	for (;;) {
		uint8_t lb[4];
		size_t n = fread(lb, 1, 4, in);
		uint32_t len;

		if (!n)
			break;
		if (n != 4) {
			fsd_log(fsd, 0, "replay: truncated length");
			break;
		}
		len = get_le32(lb);
		if (len > sizeof(buf)) {
			fsd_log(fsd, 0, "drop: replay record of %u bytes", len);
			if (fseek(in, (long)len, SEEK_CUR))
				break;
			continue;
		}
		if (fread(buf, 1, len, in) != len) {
			fsd_log(fsd, 0, "replay: truncated record");
			break;
		}
		fsd_feed(fsd, buf, len);
	}
	if (ferror(in))
		ret = 1;
	if (fclose(out))
		ret = 1;
	fclose(in);
	return ret;
}

int main(int argc, char **argv)
{
	const char *root = NULL, *dev = NULL, *rin = NULL, *rout = NULL;
	struct fsd *fsd = &fsd_state;
	struct sigaction sa;
	int c, ret;

	memset(fsd, 0, sizeof(*fsd));
	fsd->root_fd = -1;
	fsd->journal_fd = -1;
	for (int i = 0; i < FSD_HANDLES; i++)
		fsd->h[i].fd = -1;

	while ((c = getopt(argc, argv, "vr:d:pR:o:h")) != -1) {
		switch (c) {
		case 'v':
			fsd->verbose = 1;
			break;
		case 'r':
			root = optarg;
			break;
		case 'd':
			dev = optarg;
			break;
		case 'p':
			fsd->refuse_protect = 1;
			break;
		case 'R':
			rin = optarg;
			break;
		case 'o':
			rout = optarg;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind != argc || !root || (!dev == !rin) || (!rin != !rout)) {
		usage();
		return 2;
	}

	umask(077);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (dev && geteuid() == 0)
		fsd_log(fsd, 0, "warning: running as root; use a dedicated user");

	if (sb_init(fsd, root)) {
		sb_fini(fsd);
		return 1;
	}
	if (fsd_journal_open(fsd)) {
		fsd_log(fsd, 0, "%s/journal.log: %s", root, strerror(errno));
		sb_fini(fsd);
		return 1;
	}

	ret = dev ? serve(fsd, dev) : replay(fsd, rin, rout);

	fsd_close_all(fsd);
	sb_fini(fsd);
	close(fsd->journal_fd);
	return ret;
}
