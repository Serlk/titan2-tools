// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-rpcd: command line, the /dev/ccci_rpc loop and offline replay
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rpcd.h"

/* larger than any packet: a read() that fills it was an oversized one */
#define RX_BUF		0x8000
#define TX_TIMEOUT_MS	5000
#define OPEN_TRIES	800		/* every 10 ms, about 8 s */
#define WINDOW_LIMIT	(16u << 20)
#define WAKE_NAME	"ccci_rpc"

static volatile sig_atomic_t stop;
static int modem_down;
static int wake_lock_fd = -1, wake_unlock_fd = -1;
static struct rpcd rpcd_state;

void rpcd_log(struct rpcd *r, int debug, const char *fmt, ...)
{
	va_list ap;

	if (debug && !r->verbose)
		return;
	va_start(ap, fmt);
	fputs("t2-ccci-rpcd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

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
		"usage: t2-ccci-rpcd [-v] [-m IMAGE [-b]] [-p PROPS] [-w SIZE] [-L] -d /dev/ccci_rpc\n"
		"       t2-ccci-rpcd [-v] [-m IMAGE [-b]] [-p PROPS] [-w SIZE] -R requests.bin -o replies.bin [-O window.bin]\n"
		"  -m IMAGE  modem image container, opened read-only\n"
		"            (e.g. /dev/disk/by-partlabel/modem_a, or an image file)\n"
		"  -b        IMAGE is the bare md1drdi data, not a container\n"
		"  -p PROPS  name=value table for property queries (0x400F)\n"
		"  -w SIZE   DRDI window size (default 0x10000)\n"
		"  -d DEV    serve the CCCI RPC port\n"
		"  -R, -o    offline replay: u32 LE length + fragment, in and out\n"
		"  -O FILE   with -R: write the final window contents to FILE\n"
		"  -L        do not take the ccci_rpc wake lock\n"
		"  -v        verbose\n");
}

static void wake(int fd)
{
	if (fd >= 0 && write(fd, WAKE_NAME, sizeof(WAKE_NAME) - 1) < 0)
		rpcd_log(&rpcd_state, 1, "wake lock: %s", strerror(errno));
}

static void wake_init(struct rpcd *r)
{
	wake_lock_fd = open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC);
	wake_unlock_fd = open("/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
	if (wake_lock_fd < 0 || wake_unlock_fd < 0) {
		rpcd_log(r, 0, "no wake lock: %s", strerror(errno));
		if (wake_lock_fd >= 0)
			close(wake_lock_fd);
		if (wake_unlock_fd >= 0)
			close(wake_unlock_fd);
		wake_lock_fd = wake_unlock_fd = -1;
	}
}

/* write one fragment to the port; the kernel sets length, channel, seq */
static int port_send(struct rpcd *r, const uint8_t *frag, size_t len)
{
	int fd = *(int *)r->send_ctx;
	long waited = 0;

	for (;;) {
		ssize_t n = write(fd, frag, len);

		if (n == (ssize_t)len)
			return 0;
		if (n >= 0) {
			rpcd_log(r, 0, "short write %zd of %zu", n, len);
			return -1;
		}
		switch (errno) {
		case EINTR:
			if (stop)
				return -1;
			continue;
		case EAGAIN:
			if (stop || waited >= TX_TIMEOUT_MS) {
				rpcd_log(r, 0, "port full for %ld ms", waited);
				return -1;
			}
			msleep(2);
			waited += 2;
			continue;
		case ENODEV:
			rpcd_log(r, 0, "modem not running, reply dropped");
			modem_down = 1;
			return -1;
		default:
			rpcd_log(r, 0, "write: %s", strerror(errno));
			return -1;
		}
	}
}

/* 1: retry later, 0: opened, -1: give up */
static int try_open_port(struct rpcd *r, const char *dev, int *fdp, int loud)
{
	struct stat st1, st2;
	int fd;

	/* O_PATH does not call the driver: check before really opening */
	fd = open(dev, O_PATH | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st1)) {
		if (loud)
			rpcd_log(r, 0, "%s: %s", dev, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	close(fd);
	if (!S_ISCHR(st1.st_mode)) {
		rpcd_log(r, 0, "%s: not a character device, refusing", dev);
		return -1;
	}
	fd = open(dev, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		if (loud)
			rpcd_log(r, 0, "%s: %s", dev, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st2) || !S_ISCHR(st2.st_mode) ||
	    st1.st_rdev != st2.st_rdev || st1.st_ino != st2.st_ino) {
		rpcd_log(r, 0, "%s: changed while opening, refusing", dev);
		close(fd);
		return -1;
	}
	*fdp = fd;
	return 0;
}

static int open_port(struct rpcd *r, const char *dev)
{
	int fd = -1;

	for (int i = 0; i < OPEN_TRIES && !stop; i++) {
		int ret = try_open_port(r, dev, &fd,
					i == 0 || i == 50 || i == 400 ||
					i == OPEN_TRIES - 1);

		if (ret <= 0)
			return ret ? -1 : fd;
		msleep(10);
	}
	rpcd_log(r, 0, "%s: giving up", dev);
	return -1;
}

static int serve(struct rpcd *r, const char *dev, int use_wake)
{
	static uint8_t buf[RX_BUF];
	struct rpcd_drdi *d = &r->drdi;
	int fd, errors = 0;

	fd = open_port(r, dev);
	if (fd < 0)
		return 1;
	r->send = port_send;
	r->send_ctx = &fd;

	/* the window is mapped only when md1drdi was found */
	if (d->image && !drdi_locate(r)) {
		void *w = mmap(NULL, d->window_size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);

		if (w == MAP_FAILED) {
			rpcd_log(r, 0, "mmap of %zu bytes: %s", d->window_size,
				 strerror(errno));
		} else {
			d->window = w;
			d->prepared = 1;
		}
	}
	if (!d->prepared)
		rpcd_log(r, 0, "DRDI not prepared: every 0x4014 INIT will fail");
	if (use_wake)
		wake_init(r);
	rpcd_log(r, 0, "serving %s", dev);

	while (!stop) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			rpcd_log(r, 0, "poll: %s", strerror(errno));
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
			rpcd_log(r, 0, "read: %s", strerror(errno));
			if (++errors > 100)
				break;
			msleep(100);
			continue;
		}
		if (!n) {
			msleep(100);
			continue;
		}
		errors = 0;
		if (n == (ssize_t)sizeof(buf)) {
			/* the rest of an oversized packet comes with the next reads */
			while (read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf))
				;
			rpcd_log(r, 0, "drop: oversized packet");
			continue;
		}
		wake(wake_lock_fd);
		rpcd_feed(r, buf, (size_t)n);
		wake(wake_unlock_fd);
		if (modem_down) {
			/* the modem restarts: its partial requests are void */
			modem_down = 0;
			rpcd_reset_slots(r);
		}
	}
	if (d->window)
		munmap(d->window, d->window_size);
	d->window = NULL;
	close(fd);
	return stop ? 0 : 1;
}

static int replay_send(struct rpcd *r, const uint8_t *frag, size_t len)
{
	FILE *out = r->send_ctx;
	uint8_t hdr[4];
	uint8_t copy[RPCD_FRAG_MAX];

	/* what the kernel would fill in: length and the TX channel */
	memcpy(copy, frag, len);
	put_le32(copy + 4, (uint32_t)len);
	put_le32(copy + 8, RPCD_CH_TX);
	put_le32(hdr, (uint32_t)len);
	if (fwrite(hdr, 1, 4, out) != 4 || fwrite(copy, 1, len, out) != len)
		return -1;
	return 0;
}

static int replay(struct rpcd *r, const char *in_path, const char *out_path,
		  const char *win_path)
{
	static uint8_t buf[RX_BUF];
	struct rpcd_drdi *d = &r->drdi;
	FILE *in, *out;
	int ret = 0;

	/* a plain buffer stands in for the mapped window */
	d->window = calloc(1, d->window_size);
	if (!d->window) {
		rpcd_log(r, 0, "out of memory");
		return 1;
	}
	if (d->image && !drdi_locate(r))
		d->prepared = 1;
	else
		rpcd_log(r, 0, "DRDI not prepared: every 0x4014 INIT will fail");

	in = fopen(in_path, "rbe");
	if (!in) {
		rpcd_log(r, 0, "%s: %s", in_path, strerror(errno));
		goto fail;
	}
	out = fopen(out_path, "wbe");
	if (!out) {
		rpcd_log(r, 0, "%s: %s", out_path, strerror(errno));
		fclose(in);
		goto fail;
	}
	r->send = replay_send;
	r->send_ctx = out;

	for (;;) {
		uint8_t lb[4];
		size_t n = fread(lb, 1, 4, in);
		uint32_t len;

		if (!n)
			break;
		if (n != 4) {
			rpcd_log(r, 0, "replay: truncated length");
			break;
		}
		len = get_le32(lb);
		if (len > sizeof(buf)) {
			rpcd_log(r, 0, "drop: replay record of %u bytes", len);
			if (fseek(in, (long)len, SEEK_CUR))
				break;
			continue;
		}
		if (fread(buf, 1, len, in) != len) {
			rpcd_log(r, 0, "replay: truncated record");
			break;
		}
		rpcd_feed(r, buf, len);
	}
	if (ferror(in))
		ret = 1;
	if (fclose(out))
		ret = 1;
	fclose(in);

	if (win_path) {
		FILE *w = fopen(win_path, "wbe");

		if (!w || fwrite(d->window, 1, d->window_size, w) != d->window_size) {
			rpcd_log(r, 0, "%s: %s", win_path, strerror(errno));
			ret = 1;
		}
		if (w && fclose(w))
			ret = 1;
	}
	free(d->window);
	d->window = NULL;
	return ret;
fail:
	free(d->window);
	d->window = NULL;
	return 1;
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *rin = NULL, *rout = NULL, *wout = NULL;
	const char *props = NULL;
	struct rpcd *r = &rpcd_state;
	struct sigaction sa;
	int c, ret, use_wake = 1;

	memset(r, 0, sizeof(*r));
	r->capid_status = -1;
	r->drdi.copy_state = -1;
	r->drdi.window_size = DRDI_WINDOW_DEFAULT;

	while ((c = getopt(argc, argv, "vm:bp:w:d:R:o:O:Lh")) != -1) {
		switch (c) {
		case 'v':
			r->verbose = 1;
			break;
		case 'm':
			r->drdi.image = optarg;
			break;
		case 'b':
			r->drdi.raw = 1;
			break;
		case 'p':
			props = optarg;
			break;
		case 'w': {
			char *end;
			unsigned long v;

			errno = 0;
			v = strtoul(optarg, &end, 0);
			if (errno || *end || !v || v > WINDOW_LIMIT) {
				fprintf(stderr, "t2-ccci-rpcd: bad window size '%s'\n",
					optarg);
				return 2;
			}
			r->drdi.window_size = v;
			break;
		}
		case 'd':
			dev = optarg;
			break;
		case 'R':
			rin = optarg;
			break;
		case 'o':
			rout = optarg;
			break;
		case 'O':
			wout = optarg;
			break;
		case 'L':
			use_wake = 0;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind != argc || (!dev == !rin) || (!rin != !rout) ||
	    (wout && !rin) || (r->drdi.raw && !r->drdi.image)) {
		usage();
		return 2;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (props && rpcd_props_load(r, props)) {
		rpcd_props_free(r);
		return 1;
	}

	ret = dev ? serve(r, dev, use_wake) : replay(r, rin, rout, wout);

	rpcd_props_free(r);
	if (wake_lock_fd >= 0)
		close(wake_lock_fd);
	if (wake_unlock_fd >= 0)
		close(wake_unlock_fd);
	return ret;
}
