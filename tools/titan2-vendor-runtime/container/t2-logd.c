// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-logd: the writer side of Android's logd for the vendor container.
 *
 * Bionic processes log through liblog, which sends one datagram per message to
 * the unix socket /dev/socket/logdw and drops the message when nobody listens.
 * This daemon binds that socket and prints the messages as logcat would
 * ("MM-DD HH:MM:SS.mmm PID TID P/tag: text"), to stdout or to a file.
 * Wire format (liblog, public AOSP headers): a header { uint8 log_id;
 * uint16 tid; uint32 sec; uint32 nsec } followed, for the text logs, by the
 * priority byte, the NUL-terminated tag and the NUL-terminated message; the
 * events log (id 2) carries a binary payload and is only counted here.
 *
 *   t2-logd [-s PATH] [-o FILE]     default socket /dev/socket/logdw, stdout
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct __attribute__((packed)) log_header {
	uint8_t id;
	uint16_t tid;
	uint32_t sec;
	uint32_t nsec;
};

static const char prio_char[] = "??VDIWEFS";

int main(int argc, char **argv)
{
	const char *path = "/dev/socket/logdw";
	FILE *out = stdout;
	int opt, fd;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char buf[65536];

	while ((opt = getopt(argc, argv, "s:o:")) != -1) {
		if (opt == 's')
			path = optarg;
		else if (opt == 'o') {
			out = fopen(optarg, "a");
			if (!out) {
				perror(optarg);
				return 1;
			}
		} else {
			fprintf(stderr, "usage: t2-logd [-s socket] [-o file]\n");
			return 2;
		}
	}
	setvbuf(out, NULL, _IOLBF, 0);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		perror(path);
		return 1;
	}
	chmod(path, 0666);
	for (;;) {
		struct sockaddr_un from;
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
		struct log_header *h = (struct log_header *)buf;
		const char *tag, *msg, *end;
		struct tm tm;
		time_t t;
		char ts[32];

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recvfrom");
			return 1;
		}
		if (n < (ssize_t)sizeof(*h))
			continue;
		buf[n] = 0;
		end = buf + n;
		t = h->sec;
		localtime_r(&t, &tm);
		strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);
		if (h->id == 2) { /* events log: binary (tag id + typed values); show the printable part */
			const unsigned char *p = (const unsigned char *)buf + sizeof(*h);
			ssize_t i, len = n - (ssize_t)sizeof(*h);
			uint32_t tagid = len >= 4 ? p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24 : 0;

			fprintf(out, "%s.%03u %5u E/events: tag %u, %zd bytes: ", ts, h->nsec / 1000000, h->tid, tagid, len);
			for (i = 4; i < len; i++)
				fputc(p[i] >= 32 && p[i] < 127 ? p[i] : '.', out);
			fputc('\n', out);
			continue;
		}
		if (n < (ssize_t)sizeof(*h) + 3)
			continue;
		tag = buf + sizeof(*h) + 1;
		msg = tag + strnlen(tag, end - tag) + 1;
		if (msg > end)
			msg = end;
		fprintf(out, "%s.%03u %5u %c/%s: %s\n", ts, h->nsec / 1000000, h->tid,
			prio_char[(uint8_t)buf[sizeof(*h)] < sizeof(prio_char) - 1 ? (uint8_t)buf[sizeof(*h)] : 0], tag, msg);
	}
}
