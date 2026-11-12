// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-fsd: diagnostics, journal and CRC-32.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fsd.h"

void fsd_log(struct fsd *fsd, int debug, const char *fmt, ...)
{
	va_list ap;

	if (debug && !fsd->verbose)
		return;
	va_start(ap, fmt);
	fputs("t2-ccci-fsd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

int fsd_journal_open(struct fsd *fsd)
{
	fsd->journal_fd = openat(fsd->root_fd, "journal.log",
				 O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW |
				 O_CLOEXEC | O_NOCTTY, 0600);
	if (fsd->journal_fd < 0)
		return -errno;
	return 0;
}

/* one line per event; written with a single write() so lines never mix */
void fsd_journal(struct fsd *fsd, const char *fmt, ...)
{
	char line[1024];
	struct timespec ts;
	struct tm tm;
	va_list ap;
	size_t n;
	int r;

	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	n = strftime(line, sizeof(line), "%Y-%m-%dT%H:%M:%S", &tm);
	r = snprintf(line + n, sizeof(line) - n, ".%03ldZ ",
		     ts.tv_nsec / 1000000);
	if (r > 0)
		n += r;
	va_start(ap, fmt);
	r = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
	va_end(ap);
	if (r < 0)
		r = 0;
	n += (size_t)r < sizeof(line) - n - 1 ? (size_t)r : sizeof(line) - n - 2;
	line[n++] = '\n';

	if (fsd->verbose)
		fwrite(line, 1, n, stderr);
	if (fsd->journal_fd >= 0 && write(fsd->journal_fd, line, n) != (ssize_t)n)
		fsd_log(fsd, 0, "journal write failed: %s", strerror(errno));
}

/* CRC-32 (IEEE 802.3, reflected 0xEDB88320), as zlib's crc32() */
uint32_t fsd_crc32(uint32_t crc, const void *buf, size_t len)
{
	static uint32_t table[256];
	const uint8_t *p = buf;
	size_t i;

	if (!table[1]) {
		for (uint32_t n = 0; n < 256; n++) {
			uint32_t c = n;

			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[n] = c;
		}
	}
	crc = ~crc;
	for (i = 0; i < len; i++)
		crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}
