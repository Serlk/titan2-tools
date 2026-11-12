// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-speechd: command line, the /dev/ccci_aud loop, the control socket and
 * the client mode.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "speechd.h"

#define DEF_PORT	"/dev/ccci_aud"
#define DEF_RAW		"/dev/ccci_raw_audio"
#define DEF_STATE	"/sys/kernel/debug/d180000.mddriver/fsm"
#define DEF_SOCK	"/run/t2-speechd.sock"
#define DEF_PERSIST	"/run/t2-speechd.state"
#define DEF_MARKER	"/run/t2-speechd.shm-init"
#define DEF_CARD	"hw:Titan2"
#define DEF_PCM		"hw:Titan2,4"

#define RX_BUF		4096	/* larger than CCCI_MTU: a full read is oversized */
#define OPEN_TRIES	200	/* every 10 ms */
#define REOPEN_MS	2000
#define WRITE_TRIES	20	/* every 2 ms */
#define CLIENT_MS	1000
#define REPLY_MAX	8192

static volatile sig_atomic_t stop;
static struct sd sd_state;

struct live {
	const char *port_path;
	const char *state_file;
	int fd;
	int64_t reopen_at;
};

void sd_log(struct sd *s, int debug, const char *fmt, ...)
{
	va_list ap;

	if (debug && !s->verbose)
		return;
	va_start(ap, fmt);
	fputs("t2-speechd: ", stderr);
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

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: t2-speechd [options] -d " DEF_PORT "        daemon\n"
		"       t2-speechd [options] -R script -o out.txt      offline replay\n"
		"       t2-speechd [-s SOCKET] COMMAND [ARGS]          client\n"
		"commands: call-start [--rate 48000|32000|16000|8000] [--device earpiece|speaker]\n"
		"          call-stop | device earpiece|speaker | volume 0..6 | mute on|off | status\n"
		"options:\n"
		"  -d DEV   speech control port (" DEF_PORT ")\n"
		"  -m DEV   raw audio shared memory (" DEF_RAW "), none = not used\n"
		"  -S FILE  modem state file (" DEF_STATE "),\n"
		"           none = ready after the modem's alive notification (or -a)\n"
		"  -a       with no state source: ready as soon as the port is open\n"
		"  -s SOCK  control socket (" DEF_SOCK ")\n"
		"  -p FILE  state kept across restarts (" DEF_PERSIST "), none = off\n"
		"  -I FILE  'shared memory formatted' marker (" DEF_MARKER ")\n"
		"  -c CARD  ALSA control device (" DEF_CARD ")\n"
		"  -P PCM   Voice Call PCM (" DEF_PCM ")\n"
		"  -f RATE  default call rate (48000)\n"
		"  -r 0|1   codec routes: 0 = only the PCM 1 switches, the codec is set by\n"
		"           the 'Voice Call' UCM verb (default); 1 = set the codec too\n"
		"  -H VAL   'Handset Volume' at call start (0 = -10 dB), '' = leave (-r 1)\n"
		"  -K VAL   'Speaker Volume' at call start (512), '' = leave (-r 1)\n"
		"  -A N     info block smart-PA byte 0x54 (1)\n"
		"  -C       info block common parameters all 0 (default: factory values)\n"
		"  -T MS    ack timeout of the message queue (60000)\n"
		"  -R, -o   offline replay script and output\n"
		"  -v       verbose\n");
}

/* --- the port --- */

static int live_write(struct sd *s, const uint8_t *buf, size_t len)
{
	struct live *lv = s->io_ctx;

	for (int i = 0;; i++) {
		ssize_t n = write(lv->fd, buf, len);

		if (n == (ssize_t)len)
			return 0;
		if (n >= 0)
			return -EIO;
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN || i >= WRITE_TRIES || !sd_md_ready(s))
			return -errno;
		msleep(2);
	}
}

static int live_md_state(struct sd *s)
{
	struct live *lv = s->io_ctx;
	char line[64];
	FILE *f;
	int ret = -1;

	if (!lv->state_file)
		return -1;
	f = fopen(lv->state_file, "re");
	if (!f)
		return -1;
	if (fgets(line, sizeof(line), f) && !strncmp(line, "state: ", 7))
		ret = !strcmp(line + 7, "ready\n") || !strcmp(line + 7, "ready");
	fclose(f);
	return ret;
}

static int64_t live_now(struct sd *s)
{
	(void)s;
	return mono_ms();
}

static void read_packet(struct sd *s, int fd)
{
	static uint8_t buf[RX_BUF];
	ssize_t n = read(fd, buf, sizeof(buf));

	if (n < 0) {
		if (errno != EAGAIN && errno != EINTR) {
			sd_log(s, 0, "read: %s", strerror(errno));
			msleep(100);
		}
		return;
	}
	if (n == (ssize_t)sizeof(buf)) {
		while (read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf))
			;
		sd_log(s, 0, "drop: oversized packet");
		return;
	}
	if (n < SPH_HDR) {
		sd_log(s, 0, "read of %zd bytes: %s", n,
		       sd_md_ready(s) ? "transient error" : "modem invalid");
		msleep(100);
		return;
	}
	sd_rx(s, buf, (size_t)n);
}

/* nested wait (a command waits for an ack): the port only */
static int live_wait(struct sd *s, int64_t deadline)
{
	struct live *lv = s->io_ctx;
	int64_t t = mono_ms();
	int timeout = deadline == SD_NEVER ? -1 :
		      deadline <= t ? 0 :
		      deadline - t > 60000 ? 60000 : (int)(deadline - t);
	struct pollfd p = { .fd = lv->fd, .events = POLLIN };

	if (lv->fd < 0) {
		if (timeout > 0)
			msleep(timeout);
		return 0;
	}
	if (poll(&p, 1, timeout) <= 0)
		return 0;
	if (p.revents & POLLIN) {
		read_packet(s, lv->fd);
		return 1;
	}
	return 0;
}

static const struct sd_io live_io = {
	.write = live_write,
	.md_state = live_md_state,
	.wait = live_wait,
	.now = live_now,
};

static int open_chr(struct sd *s, const char *dev, int loud)
{
	struct stat st;
	int fd = open(dev, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);

	if (fd < 0) {
		if (loud)
			sd_log(s, 0, "%s: %s", dev, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
		sd_log(s, 0, "%s: not a character device, refusing", dev);
		close(fd);
		return -2;
	}
	return fd;
}

/* retry every 10 ms, 200 times */
static int open_port(struct sd *s, struct live *lv, int tries)
{
	for (int i = 0; i < tries && !stop; i++) {
		int fd = open_chr(s, lv->port_path, i == 0 && tries > 1);

		if (fd >= 0) {
			lv->fd = fd;
			s->port_open = 1;
			sd_log(s, 0, "%s open", lv->port_path);
			return 0;
		}
		if (fd == -2)
			return -2;
		msleep(10);
	}
	lv->reopen_at = mono_ms() + REOPEN_MS;
	return -1;
}

static void map_shm(struct sd *s, const char *dev)
{
	int fd = open_chr(s, dev, 1);
	void *p;

	if (fd < 0)
		return;
	p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		sd_log(s, 0, "%s: mmap of %#x bytes: %s", dev, SHM_SIZE,
		       strerror(errno));
		return;
	}
	s->shm = p;
}

/* --- control socket --- */

static int listen_sock(struct sd *s, const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	struct stat st;
	mode_t old;
	int fd;

	if (strlen(path) >= sizeof(a.sun_path)) {
		sd_log(s, 0, "%s: path too long", path);
		return -1;
	}
	strcpy(a.sun_path, path);
	if (!lstat(path, &st)) {
		if (!S_ISSOCK(st.st_mode)) {
			sd_log(s, 0, "%s exists and is not a socket", path);
			return -1;
		}
		unlink(path);
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	old = umask(0117);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 4)) {
		umask(old);
		sd_log(s, 0, "%s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	umask(old);
	return fd;
}

static void serve_client(struct sd *s, int lfd)
{
	static char reply[REPLY_MAX];
	char line[256];
	size_t len = 0;
	int64_t dl = mono_ms() + CLIENT_MS;
	int c = accept(lfd, NULL, NULL);

	if (c < 0)
		return;
	fcntl(c, F_SETFD, FD_CLOEXEC);
	fcntl(c, F_SETFL, O_NONBLOCK);
	while (len < sizeof(line) - 1) {
		struct pollfd p = { .fd = c, .events = POLLIN };
		int64_t t = mono_ms();
		ssize_t n;

		if (t >= dl || poll(&p, 1, (int)(dl - t)) <= 0)
			break;
		n = read(c, line + len, sizeof(line) - 1 - len);
		if (n <= 0)
			break;
		len += (size_t)n;
		if (memchr(line, '\n', len))
			break;
	}
	line[len] = 0;
	line[strcspn(line, "\n")] = 0;
	sd_command(s, line, reply, sizeof(reply));
	for (size_t off = 0, n = strlen(reply); off < n;) {
		struct pollfd p = { .fd = c, .events = POLLOUT };
		ssize_t w;

		if (poll(&p, 1, CLIENT_MS) <= 0)
			break;
		w = send(c, reply + off, n - off, MSG_NOSIGNAL);
		if (w <= 0)
			break;
		off += (size_t)w;
	}
	close(c);
}

static int serve(struct sd *s, struct live *lv, const char *raw,
		 const char *sock)
{
	int lfd;

	s->io = &live_io;
	s->io_ctx = lv;
	lv->fd = -1;
	if (open_port(s, lv, OPEN_TRIES) == -2)
		return 1;
	if (raw)
		map_shm(s, raw);
	sd_recover(s);
	lfd = listen_sock(s, sock);
	if (lfd < 0)
		return 1;
	sd_log(s, 0, "listening on %s", sock);

	while (!stop) {
		struct pollfd p[2];
		int64_t dl, t;
		int n = 0, timeout, pi = -1;

		sd_deferred(s);
		dl = sd_next_deadline(s);
		if (lv->fd < 0 && lv->reopen_at < dl)
			dl = lv->reopen_at;
		t = mono_ms();
		timeout = dl == SD_NEVER ? -1 : dl <= t ? 0 :
			  dl - t > 60000 ? 60000 : (int)(dl - t);
		if (lv->fd >= 0) {
			pi = n;
			p[n++] = (struct pollfd){ .fd = lv->fd, .events = POLLIN };
		}
		p[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		if (poll(p, (nfds_t)n, timeout) < 0 && errno != EINTR) {
			sd_log(s, 0, "poll: %s", strerror(errno));
			break;
		}
		if (pi >= 0 && (p[pi].revents & POLLIN))
			read_packet(s, lv->fd);
		if (p[n - 1].revents & POLLIN)
			serve_client(s, lfd);
		if (lv->fd < 0 && mono_ms() >= lv->reopen_at &&
		    open_port(s, lv, 1) == -2)
			break;
		sd_run_timers(s);
	}
	if (s->call_active) {
		static char reply[REPLY_MAX];

		sd_log(s, 0, "stopping: ending the call");
		sd_command(s, "call-stop", reply, sizeof(reply));
	}
	close(lfd);
	unlink(sock);
	if (lv->fd >= 0)
		close(lv->fd);
	if (s->shm)
		munmap(s->shm, SHM_SIZE);
	s->shm = NULL;
	return 0;
}

/* --- client --- */

static int client(const char *sock, int argc, char **argv)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	static char resp[REPLY_MAX];
	char line[256], *last;
	struct timeval tv = { 10, 0 };
	size_t len = 0;
	ssize_t n;
	int fd;

	for (int i = 0; i < argc; i++) {
		int w = snprintf(line + len, sizeof(line) - len, "%s%s",
				 i ? " " : "", argv[i]);

		if (w < 0 || (size_t)w >= sizeof(line) - len - 1) {
			fprintf(stderr, "t2-speechd: command too long\n");
			return 2;
		}
		len += (size_t)w;
	}
	line[len++] = '\n';
	if (strlen(sock) >= sizeof(a.sun_path))
		return 2;
	strcpy(a.sun_path, sock);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		fprintf(stderr, "t2-speechd: %s: %s\n", sock, strerror(errno));
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (send(fd, line, len, MSG_NOSIGNAL) != (ssize_t)len) {
		fprintf(stderr, "t2-speechd: send: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	len = 0;
	shutdown(fd, SHUT_WR);
	while (len < sizeof(resp) - 1 &&
	       (n = read(fd, resp + len, sizeof(resp) - 1 - len)) > 0)
		len += (size_t)n;
	close(fd);
	resp[len] = 0;
	fputs(resp, stdout);
	/* the last line says ok or error */
	while (len && resp[len - 1] == '\n')
		resp[--len] = 0;
	last = strrchr(resp, '\n');
	last = last ? last + 1 : resp;
	return strncmp(last, "ok", 2) ? 1 : 0;
}

static const char *opt_none(const char *v)
{
	return strcmp(v, "none") ? v : NULL;
}

int main(int argc, char **argv)
{
	const char *raw = DEF_RAW, *sock = DEF_SOCK;
	const char *card = DEF_CARD, *pcm = DEF_PCM;
	const char *rin = NULL, *rout = NULL;
	struct live lv = { .state_file = DEF_STATE };
	struct sd *s = &sd_state;
	struct sigaction sa;
	int c, ret, persist_set = 0, marker_set = 0;
	unsigned long v;
	char *end;

	sd_init(s);
	s->persist_path = DEF_PERSIST;
	s->shm_marker = DEF_MARKER;

	while ((c = getopt(argc, argv, "+d:m:S:as:p:I:c:P:f:r:H:K:A:CT:R:o:vh")) != -1) {
		switch (c) {
		case 'd':
			lv.port_path = optarg;
			break;
		case 'm':
			raw = opt_none(optarg);
			break;
		case 'S':
			lv.state_file = opt_none(optarg);
			break;
		case 'a':
			s->assume_ready = 1;
			break;
		case 's':
			sock = optarg;
			break;
		case 'p':
			s->persist_path = opt_none(optarg);
			persist_set = 1;
			break;
		case 'I':
			s->shm_marker = opt_none(optarg);
			marker_set = 1;
			break;
		case 'c':
			card = optarg;
			break;
		case 'P':
			pcm = optarg;
			break;
		case 'f':
			errno = 0;
			v = strtoul(optarg, &end, 10);
			if (errno || *end || sph_rate_enum((unsigned)v) < 0 ||
			    v > 48000) {
				fprintf(stderr, "t2-speechd: bad rate '%s'\n", optarg);
				return 2;
			}
			s->rate = (unsigned)v;
			break;
		case 'r':
			if (strcmp(optarg, "0") && strcmp(optarg, "1")) {
				fprintf(stderr, "t2-speechd: -r takes 0 or 1\n");
				return 2;
			}
			s->codec_routes = optarg[0] == '1';
			break;
		case 'H':
			s->handset_vol = optarg;
			break;
		case 'K':
			s->speaker_vol = optarg;
			break;
		case 'A':
			errno = 0;
			v = strtoul(optarg, &end, 0);
			if (errno || *end || v > 2) {
				fprintf(stderr, "t2-speechd: bad smart-PA value '%s'\n",
					optarg);
				return 2;
			}
			s->smartpa = (uint8_t)v;
			break;
		case 'C':
			s->zero_common = 1;
			break;
		case 'T':
			errno = 0;
			v = strtoul(optarg, &end, 10);
			if (errno || *end || !v || v > 600000) {
				fprintf(stderr, "t2-speechd: bad timeout '%s'\n", optarg);
				return 2;
			}
			s->ack_timeout_ms = (unsigned)v;
			break;
		case 'R':
			rin = optarg;
			break;
		case 'o':
			rout = optarg;
			break;
		case 'v':
			s->verbose = 1;
			break;
		default:
			usage();
			return 2;
		}
	}

	if (!lv.port_path && !rin) {
		if (optind == argc) {
			usage();
			return 2;
		}
		return client(sock, argc - optind, argv + optind);
	}
	if (optind != argc || (lv.port_path && rin) || (!rin != !rout)) {
		usage();
		return 2;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (rin) {
		/* replay: no files in /run unless asked for */
		if (!persist_set)
			s->persist_path = NULL;
		if (!marker_set)
			s->shm_marker = NULL;
		return replay_run(s, rin, rout, lv.state_file != NULL);
	}

	s->au = &alsa_audio_ops;
	s->au_ctx = alsa_backend_new(card, pcm);
	if (!s->au_ctx)
		return 1;
	ret = serve(s, &lv, raw, sock);
	alsa_backend_free(s->au_ctx);
	return ret;
}
