// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-sensors-cali: calibration records of the Titan 2 sensor hub.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 *
 * The SCP sensor hub keeps per-sensor calibration words only while it runs:
 * offsets measured once for this unit and values it learns at run time (the
 * gyroscope bias, the magnetometer hard-iron offset). The Android install of
 * the phone keeps them as small JSON files in sensor/ of its nvcfg partition.
 * This system never mounts or writes that partition: it imports the records
 * read-only (debugfs, no mount) into a local directory of its own, sends them
 * to the hub from there and writes what the hub learns back to that local
 * copy only.
 *
 *   import  copy the records from nvcfg into the local directory: a record is
 *           taken when the local copy is missing or when the nvcfg file
 *           differs from what the last import took from it (the other system
 *           wrote it since); otherwise the local copy, which may hold newer
 *           values learned here, stays. -F takes every usable nvcfg record.
 *   load    once at boot: read the local records, send each sensor its
 *           complete word list through the hub_cali attribute of its IIO
 *           device (mtk-hf-iio). A missing or unusable record counts as zeros.
 *   watch   long-running: listen on /dev/hf_manager for the BIAS, TEMP and
 *           CALI results the hub reports and rewrite the local records,
 *           rate-limited.
 *   event   test aid: print one binary hub event to stdout (for watch -e).
 *
 * The words are the hub's own integers: nothing is converted, ever.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEF_DIR		"/var/lib/titan2-sensors/cali"	/* the records used here */
#define DEF_SNAP	"/var/lib/titan2-sensors/nvcfg"	/* what the last import took */
#define DEF_DEBUGFS	"debugfs"
#define NVCFG_DIR	"/sensor"			/* on the nvcfg file system */
#define DEF_SYSFS	"/sys"
#define DEF_DEV		"/dev/hf_manager"
#define DEF_INTERVAL	600		/* s between rewrites of a streaming record */
#define MAX_FILE	65536		/* a record file larger than this is unusable */
#define MAX_DEPTH	32		/* JSON nesting */
#define MAX_WORDS	12		/* one CONFIG_CALI command: 48 bytes */
#define REC_MODE	0644

/* ---- the hub's UAPI (hf_manager character device, drivers/soc/mediatek/sensorhub/core) ---- */

enum { ACT_DATA, ACT_FLUSH, ACT_BIAS, ACT_CALI, ACT_TEMP, ACT_TEST, ACT_RAW };

struct hf_event {
	int64_t timestamp;
	uint8_t sensor_type;
	uint8_t status;		/* "accurancy" in the kernel header */
	uint8_t action;
	uint8_t padding;
	int32_t word[16];
} __attribute__((packed, aligned(4)));

struct hf_packet {
	uint8_t sensor_type;
	uint8_t padding[3];
	union {
		uint8_t status;	/* bool */
		int8_t byte[64];
	};
} __attribute__((packed, aligned(4)));

_Static_assert(sizeof(struct hf_event) == 76, "hf_manager_event layout");
_Static_assert(sizeof(struct hf_packet) == 68, "ioctl_packet layout");

#define HF_REQ_REGISTER_STATUS	_IOWR('a', 1, struct hf_packet)
#define HF_REQ_BIAS_DATA	_IOW('a', 2, struct hf_packet)
#define HF_REQ_CALI_DATA	_IOW('a', 3, struct hf_packet)
#define HF_REQ_TEMP_DATA	_IOW('a', 4, struct hf_packet)

enum { T_ACCEL = 1, T_MAGN = 2, T_GYRO = 4, T_LIGHT = 5, T_PROX = 8 };

/* ---- records ---- */

enum rec_id { R_ACC_BIAS, R_ACC_CALI, R_GYRO_BIAS, R_GYRO_CALI, R_GYRO_TEMP,
	      R_MAG_BIAS, R_MAG_CALI, R_ALS_CALI, R_PS_CALI, R_COUNT };

static const struct { const char *name; int n; } recs[R_COUNT] = {
	[R_ACC_BIAS]  = { "acc_bias", 3 },
	[R_ACC_CALI]  = { "acc_cali", 3 },
	[R_GYRO_BIAS] = { "gyro_bias", 3 },
	[R_GYRO_CALI] = { "gyro_cali", 3 },
	[R_GYRO_TEMP] = { "gyro_temp", 6 },
	[R_MAG_BIAS]  = { "mag_bias", 3 },
	[R_MAG_CALI]  = { "mag_cali", 6 },
	[R_ALS_CALI]  = { "als_cali", 1 },
	[R_PS_CALI]   = { "ps_cali", 2 },
};

/* the word list of each sensor, in the order sensors are sent at boot */
static const struct sensor {
	const char *iio;
	uint8_t type;
	int nrec;
	enum rec_id rec[3];
} sensors[] = {
	{ "hub-accel", T_ACCEL, 2, { R_ACC_BIAS, R_ACC_CALI } },
	{ "hub-gyro",  T_GYRO,  3, { R_GYRO_BIAS, R_GYRO_CALI, R_GYRO_TEMP } },
	{ "hub-magn",  T_MAGN,  2, { R_MAG_BIAS, R_MAG_CALI } },
	{ "hub-light", T_LIGHT, 1, { R_ALS_CALI } },
	{ "hub-prox",  T_PROX,  1, { R_PS_CALI } },
};
#define NSENSORS ((int)(sizeof(sensors) / sizeof(sensors[0])))

enum rec_state { RS_OK, RS_MISSING, RS_INVALID, RS_ERROR };
static const char *const rs_name[] = { "file", "missing", "unusable", "unreadable" };

/* ---- logging ---- */

static bool verbose;

static void logmsg(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logmsg(int prio, const char *fmt, ...)
{
	va_list ap;

	if (prio == LOG_DEBUG && !verbose)
		return;
	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
}

/* ---- a small strict JSON reader ---- */

struct jp {
	const char *p, *end;
};

static void j_ws(struct jp *j)
{
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
		j->p++;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* a string; its ASCII value goes to out (if not NULL); *ascii=false when it holds anything else */
static bool j_string(struct jp *j, char *out, size_t outsz, bool *ascii)
{
	size_t n = 0;

	if (ascii)
		*ascii = true;
	if (j->p >= j->end || *j->p != '"')
		return false;
	j->p++;
	while (j->p < j->end) {
		unsigned char c = (unsigned char)*j->p++;
		int v = -1;

		if (c == '"') {
			if (out)
				out[n < outsz ? n : outsz - 1] = 0;
			return true;
		}
		if (c < 0x20)
			return false;
		if (c == '\\') {
			if (j->p >= j->end)
				return false;
			c = (unsigned char)*j->p++;
			switch (c) {
			case '"': case '\\': case '/': v = c; break;
			case 'b': v = '\b'; break;
			case 'f': v = '\f'; break;
			case 'n': v = '\n'; break;
			case 'r': v = '\r'; break;
			case 't': v = '\t'; break;
			case 'u': {
				int i, h;

				if (j->end - j->p < 4)
					return false;
				for (v = 0, i = 0; i < 4; i++) {
					h = hexval((unsigned char)j->p[i]);
					if (h < 0)
						return false;
					v = v * 16 + h;
				}
				j->p += 4;
				break;
			}
			default:
				return false;
			}
		} else {
			v = c;
		}
		if (v <= 0 || v > 0x7e) {
			if (ascii)
				*ascii = false;
			continue;
		}
		if (out && n + 1 < outsz)
			out[n] = (char)v;
		else if (ascii)
			*ascii = false;	/* too long to be one of our keys */
		n++;
	}
	return false;
}

/* a number; *is_int and *val when it is an integer within int32 */
static bool j_number(struct jp *j, bool *is_int, int32_t *val)
{
	const char *s = j->p;
	bool frac = false;
	char buf[32];
	long long v;
	char *e;

	if (j->p < j->end && *j->p == '-')
		j->p++;
	if (j->p >= j->end || !isdigit((unsigned char)*j->p))
		return false;
	if (*j->p == '0')
		j->p++;
	else
		while (j->p < j->end && isdigit((unsigned char)*j->p))
			j->p++;
	if (j->p < j->end && *j->p == '.') {
		j->p++;
		frac = true;
		if (j->p >= j->end || !isdigit((unsigned char)*j->p))
			return false;
		while (j->p < j->end && isdigit((unsigned char)*j->p))
			j->p++;
	}
	if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
		j->p++;
		frac = true;
		if (j->p < j->end && (*j->p == '+' || *j->p == '-'))
			j->p++;
		if (j->p >= j->end || !isdigit((unsigned char)*j->p))
			return false;
		while (j->p < j->end && isdigit((unsigned char)*j->p))
			j->p++;
	}
	*is_int = false;
	if (frac || (size_t)(j->p - s) >= sizeof(buf))
		return true;
	memcpy(buf, s, (size_t)(j->p - s));
	buf[j->p - s] = 0;
	errno = 0;
	v = strtoll(buf, &e, 10);
	if (errno || *e || v < INT32_MIN || v > INT32_MAX)
		return true;
	*is_int = true;
	*val = (int32_t)v;
	return true;
}

static bool j_lit(struct jp *j, const char *lit)
{
	size_t n = strlen(lit);

	if ((size_t)(j->end - j->p) < n || memcmp(j->p, lit, n))
		return false;
	j->p += n;
	return true;
}

static bool j_value(struct jp *j, int depth);

/*
 * an array; with want > 0 the first want elements must be integers (int32),
 * they go to out and *ok says whether all of them were there
 */
static bool j_array(struct jp *j, int depth, int want, int32_t *out, bool *ok)
{
	int i = 0;
	bool good = true;

	if (depth > MAX_DEPTH || j->p >= j->end || *j->p != '[')
		return false;
	j->p++;
	j_ws(j);
	if (j->p < j->end && *j->p == ']') {
		j->p++;
		goto done;
	}
	for (;;) {
		j_ws(j);
		if (i < want) {
			bool is_int = false;
			int32_t v = 0;

			if (j->p < j->end && (*j->p == '-' || isdigit((unsigned char)*j->p))) {
				if (!j_number(j, &is_int, &v))
					return false;
			} else if (!j_value(j, depth + 1)) {
				return false;
			}
			if (is_int)
				out[i] = v;
			else
				good = false;
		} else if (!j_value(j, depth + 1)) {
			return false;
		}
		i++;
		j_ws(j);
		if (j->p < j->end && *j->p == ',') {
			j->p++;
			continue;
		}
		if (j->p < j->end && *j->p == ']') {
			j->p++;
			break;
		}
		return false;
	}
done:
	if (ok)
		*ok = good && i >= want;
	return true;
}

/*
 * an object; with key != NULL (top level) the value of that key is looked
 * for: *found = it was there (the last one counts), *ok = it was an array
 * whose first want elements are integers
 */
static bool j_object(struct jp *j, int depth, const char *key, int want, int32_t *out,
		     bool *found, bool *ok)
{
	if (depth > MAX_DEPTH || j->p >= j->end || *j->p != '{')
		return false;
	j->p++;
	j_ws(j);
	if (j->p < j->end && *j->p == '}') {
		j->p++;
		return true;
	}
	for (;;) {
		char name[32];
		bool ascii;

		j_ws(j);
		if (!j_string(j, name, sizeof(name), &ascii))
			return false;
		j_ws(j);
		if (j->p >= j->end || *j->p != ':')
			return false;
		j->p++;
		j_ws(j);
		if (key && ascii && !strcmp(name, key)) {
			int32_t tmp[MAX_WORDS];
			bool good = false;

			*found = true;
			if (j->p < j->end && *j->p == '[') {
				if (!j_array(j, depth + 1, want, tmp, &good))
					return false;
			} else if (!j_value(j, depth + 1)) {
				return false;
			}
			*ok = good;
			if (good)
				memcpy(out, tmp, (size_t)want * sizeof(*out));
		} else if (!j_value(j, depth + 1)) {
			return false;
		}
		j_ws(j);
		if (j->p < j->end && *j->p == ',') {
			j->p++;
			continue;
		}
		if (j->p < j->end && *j->p == '}') {
			j->p++;
			return true;
		}
		return false;
	}
}

static bool j_value(struct jp *j, int depth)
{
	bool is_int;
	int32_t v;

	if (depth > MAX_DEPTH)
		return false;
	j_ws(j);
	if (j->p >= j->end)
		return false;
	switch (*j->p) {
	case '{': return j_object(j, depth, NULL, 0, NULL, NULL, NULL);
	case '[': return j_array(j, depth, 0, NULL, NULL);
	case '"': return j_string(j, NULL, 0, NULL);
	case 't': return j_lit(j, "true");
	case 'f': return j_lit(j, "false");
	case 'n': return j_lit(j, "null");
	default: return j_number(j, &is_int, &v);
	}
}

/*
 * Parse one record: the whole text must be one JSON object; its key <name>
 * must hold an array whose first n elements are int32 integers. Anything
 * else (bad JSON, other key, short array, a fraction, an out-of-range
 * number) leaves the record unusable. Extra elements are ignored.
 */
static enum rec_state parse_record(const char *text, size_t len, const char *name, int n,
				   int32_t *out)
{
	struct jp j = { text, text + len };
	bool found = false, ok = false;

	j_ws(&j);
	if (!j_object(&j, 0, name, n, out, &found, &ok))
		return RS_INVALID;
	j_ws(&j);
	if (j.p != j.end)
		return RS_INVALID;
	return found && ok ? RS_OK : RS_INVALID;
}

/* read a whole small regular file below dirfd into buf (MAX_FILE + 1 bytes) */
static enum rec_state read_file(int dirfd, const char *path, char *buf, size_t *lenp)
{
	struct stat st;
	ssize_t got;
	size_t len = 0;
	int fd;

	*lenp = 0;
	if (dirfd < 0)
		return RS_MISSING;
	fd = openat(dirfd, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT)
			return RS_MISSING;
		logmsg(LOG_WARNING, "%s: %m", path);
		return RS_ERROR;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > MAX_FILE) {
		logmsg(LOG_WARNING, "%s: not a regular file of at most %d bytes", path, MAX_FILE);
		close(fd);
		return RS_INVALID;
	}
	while (len <= MAX_FILE && (got = read(fd, buf + len, MAX_FILE + 1 - len)) != 0) {
		if (got < 0) {
			if (errno == EINTR)
				continue;
			logmsg(LOG_WARNING, "%s: %m", path);
			close(fd);
			return RS_ERROR;
		}
		len += (size_t)got;
	}
	close(fd);
	if (len > MAX_FILE)
		return RS_INVALID;
	*lenp = len;
	return RS_OK;
}

/* read and parse <name>.json below dirfd; out = zeros unless RS_OK */
static enum rec_state read_record(int dirfd, enum rec_id r, int32_t *out)
{
	int32_t tmp[MAX_WORDS];
	enum rec_state rs;
	char path[64], *buf;
	size_t len;

	memset(out, 0, (size_t)recs[r].n * sizeof(*out));
	if (dirfd < 0)
		return RS_MISSING;
	buf = malloc(MAX_FILE + 1);
	if (!buf)
		return RS_ERROR;
	snprintf(path, sizeof(path), "%s.json", recs[r].name);
	rs = read_file(dirfd, path, buf, &len);
	if (rs == RS_OK) {
		rs = parse_record(buf, len, recs[r].name, recs[r].n, tmp);
		if (rs == RS_OK)
			memcpy(out, tmp, (size_t)recs[r].n * sizeof(*out));
	}
	free(buf);
	return rs;
}

/* replace name below dirfd with text: temporary file, fsync, rename */
static int write_file_atomic(int dirfd, const char *name, const char *text, size_t len,
			     bool fsync_it)
{
	char tmp[96];
	size_t off = 0;
	int fd, err = 0;
	ssize_t w;

	if ((size_t)snprintf(tmp, sizeof(tmp), ".%s.tmp", name) >= sizeof(tmp))
		return -ENAMETOOLONG;
	fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, REC_MODE);
	if (fd < 0)
		return -errno;
	if (fchmod(fd, REC_MODE))
		err = -errno;
	while (!err && off < len) {
		w = write(fd, text + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			break;
		}
		off += (size_t)w;
	}
	if (!err && fsync_it && fsync(fd))
		err = -errno;
	if (close(fd) && !err)
		err = -errno;
	if (!err && renameat(dirfd, tmp, dirfd, name))
		err = -errno;
	if (err) {
		unlinkat(dirfd, tmp, 0);
		return err;
	}
	if (fsync_it)
		fsync(dirfd);
	return 0;
}

/* the exact text of a record file: 2-space key, 6-space values, 4-space bracket, no final newline */
static int format_record(char *buf, size_t sz, enum rec_id r, const int32_t *v)
{
	size_t n;
	int i, k;

	k = snprintf(buf, sz, "{\n  \"%s\": [\n", recs[r].name);
	if (k < 0 || (size_t)k >= sz)
		return -1;
	n = (size_t)k;
	for (i = 0; i < recs[r].n; i++) {
		k = snprintf(buf + n, sz - n, "      %d%s\n", v[i], i + 1 < recs[r].n ? "," : "");
		if (k < 0 || (size_t)k >= sz - n)
			return -1;
		n += (size_t)k;
	}
	k = snprintf(buf + n, sz - n, "    ]\n}");
	if (k < 0 || (size_t)k >= sz - n)
		return -1;
	return (int)(n + (size_t)k);
}

static void fmt_words(char *buf, size_t sz, const int32_t *w, int n, const char *sep)
{
	size_t len = 0;
	int i, k;

	buf[0] = 0;
	for (i = 0; i < n && len < sz; i++) {
		k = snprintf(buf + len, sz - len, "%s%d", i ? sep : "", w[i]);
		if (k < 0)
			break;
		len += (size_t)k;
	}
}

static double now_mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

/* ---- load ---- */

/* the IIO device directory of every hub sensor that exists */
static int find_iio(const char *sysfs, char paths[][PATH_MAX])
{
	char dir[PATH_MAX], file[PATH_MAX + 64], name[64];
	struct dirent *de;
	int found = 0, i;
	DIR *d;

	for (i = 0; i < NSENSORS; i++)
		paths[i][0] = 0;
	snprintf(dir, sizeof(dir), "%s/bus/iio/devices", sysfs);
	d = opendir(dir);
	if (!d)
		return 0;
	while ((de = readdir(d))) {
		FILE *f;

		if (de->d_name[0] == '.')
			continue;
		if ((size_t)snprintf(file, sizeof(file), "%s/%s/name", dir, de->d_name) >= sizeof(file))
			continue;
		f = fopen(file, "re");
		if (!f)
			continue;
		if (!fgets(name, sizeof(name), f))
			name[0] = 0;
		fclose(f);
		name[strcspn(name, "\n")] = 0;
		for (i = 0; i < NSENSORS; i++)
			if (!strcmp(name, sensors[i].iio) && !paths[i][0]) {
				if ((size_t)snprintf(paths[i], PATH_MAX, "%s/%s", dir, de->d_name) < PATH_MAX)
					found++;
				else
					paths[i][0] = 0;
			}
	}
	closedir(d);
	return found;
}

static int write_attr(const char *path, const char *text)
{
	size_t len = strlen(text);
	ssize_t w;
	int fd, err = 0;

	fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do
		w = write(fd, text, len);
	while (w < 0 && errno == EINTR);
	if (w < 0)
		err = -errno;
	else if ((size_t)w != len)
		err = -EIO;
	if (close(fd) && !err)
		err = -errno;
	return err;
}

static int open_dir(const char *dir, bool quiet)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0 && !quiet)
		logmsg(errno == ENOENT ? LOG_WARNING : LOG_ERR, "%s: %m", dir);
	return fd;
}

static int cmd_load(const char *dir, const char *sysfs, int wait_s, bool dry)
{
	static char paths[NSENSORS][PATH_MAX];
	double start = now_mono(), first = 0;
	int found = 0, i, failed = 0, sent = 0, dirfd;

	/* the bridge lists the hub's sensors once the SCP firmware is up */
	for (;;) {
		if (dry)
			break;
		found = find_iio(sysfs, paths);
		if (found == NSENSORS)
			break;
		if (found && !first)
			first = now_mono();
		if (found && now_mono() - first >= 2.0)
			break;
		if (now_mono() - start >= wait_s)
			break;
		sleep_ms(200);
	}
	if (!found && !dry) {
		logmsg(LOG_ERR, "no hub sensor IIO device in %s/bus/iio/devices after %d s", sysfs, wait_s);
		return 2;
	}

	dirfd = open_dir(dir, false);
	if (dirfd < 0)
		logmsg(LOG_WARNING, "no records: every sensor gets zeros");

	for (i = 0; i < NSENSORS; i++) {
		const struct sensor *s = &sensors[i];
		int32_t w[MAX_WORDS];
		char text[MAX_WORDS * 12 + 2], from[160], attr[PATH_MAX + 16];
		size_t fl = 0;
		int n = 0, k, err;

		from[0] = 0;
		for (k = 0; k < s->nrec; k++) {
			enum rec_id r = s->rec[k];
			enum rec_state rs = read_record(dirfd, r, w + n);
			int m;

			n += recs[r].n;
			m = snprintf(from + fl, sizeof(from) - fl, "%s%s %s", k ? ", " : "",
				     recs[r].name, rs_name[rs]);
			if (m > 0 && (size_t)m < sizeof(from) - fl)
				fl += (size_t)m;
		}
		fmt_words(text, sizeof(text), w, n, " ");
		if (dry) {
			printf("%s %s\n", s->iio, text);
			continue;
		}
		if (!paths[i][0]) {
			logmsg(LOG_WARNING, "%s: no such IIO device, not sent", s->iio);
			continue;
		}
		strcat(text, "\n");
		if ((size_t)snprintf(attr, sizeof(attr), "%s/hub_cali", paths[i]) >= sizeof(attr))
			err = -ENAMETOOLONG;
		else
			err = write_attr(attr, text);
		text[strlen(text) - 1] = 0;
		if (err) {
			logmsg(LOG_ERR, "%s: hub_cali refused %d words [%s]: %s", s->iio, n, text,
			       strerror(-err));
			failed++;
			continue;
		}
		logmsg(LOG_INFO, "%s: %d word%s sent [%s] (%s)", s->iio, n, n == 1 ? "" : "s", text, from);
		sent++;
	}
	if (dirfd >= 0)
		close(dirfd);
	if (!dry)
		logmsg(LOG_INFO, "calibration sent to %d of %d hub sensors%s", sent, NSENSORS,
		       failed ? " (errors above)" : "");
	return failed ? 1 : 0;
}

/* ---- import ---- */

static int mkdir_p(const char *path, mode_t mode)
{
	char buf[PATH_MAX], *q;

	if ((size_t)snprintf(buf, sizeof(buf), "%s", path) >= sizeof(buf))
		return -ENAMETOOLONG;
	for (q = buf + 1; *q; q++) {
		if (*q != '/')
			continue;
		*q = 0;
		if (mkdir(buf, mode) && errno != EEXIST)
			return -errno;
		*q = '/';
	}
	if (mkdir(buf, mode) && errno != EEXIST)
		return -errno;
	return 0;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	if (remove(path))
		logmsg(LOG_WARNING, "%s: %m", path);
	return 0;
}

/*
 * Copy sensor/ of the nvcfg file system on dev into tmp/sensor with debugfs:
 * it reads the block device itself (read-only, nothing is mounted).
 */
static int run_debugfs(const char *debugfs, const char *dev, const char *tmp)
{
	char req[PATH_MAX + 32], logp[PATH_MAX + 16], line[256], sub[PATH_MAX + 16];
	struct stat st;
	int status, fd;
	pid_t pid;
	FILE *f;

	if ((size_t)snprintf(req, sizeof(req), "rdump %s %s", NVCFG_DIR, tmp) >= sizeof(req) ||
	    (size_t)snprintf(logp, sizeof(logp), "%s/debugfs.log", tmp) >= sizeof(logp) ||
	    (size_t)snprintf(sub, sizeof(sub), "%s%s", tmp, NVCFG_DIR) >= sizeof(sub))
		return -ENAMETOOLONG;
	pid = fork();
	if (pid < 0)
		return -errno;
	if (!pid) {
		fd = open(logp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}
		execlp(debugfs, debugfs, "-R", req, dev, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -errno;
	if (!stat(sub, &st) && S_ISDIR(st.st_mode))
		return 0;
	/* no sensor/ came out: say why */
	logmsg(LOG_ERR, "%s on %s gave no %s (exit %d)", debugfs, dev, NVCFG_DIR,
	       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	f = fopen(logp, "re");
	if (f) {
		int n = 0;

		while (n++ < 8 && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			logmsg(LOG_ERR, "  %s", line);
		}
		fclose(f);
	}
	return -ENOENT;
}

static bool same_bytes(const char *a, size_t alen, const char *b, size_t blen)
{
	return alen == blen && !memcmp(a, b, alen);
}

/*
 * The import rule, per record present and usable on nvcfg:
 *   - no local copy                          -> take it
 *   - nvcfg differs from the last import     -> take it (the other system wrote it)
 *   - otherwise                              -> keep the local copy
 *   - -F                                     -> take it
 * "The last import" is a byte copy of what was taken (snap/), so neither clock
 * nor timestamps matter. A record that is absent or unusable on nvcfg never
 * replaces anything.
 */
static int cmd_import(const char *dir, const char *snap, const char *dev, const char *srcdir,
		      const char *debugfs, bool force)
{
	char tmp[PATH_MAX], path[64], *nv, *old;
	int dirfd = -1, snapfd = -1, srcfd = -1, r, err, ret = 1, taken = 0, kept = 0;
	const char *src = srcdir;
	bool have_tmp = false;

	nv = malloc(MAX_FILE + 1);
	old = malloc(MAX_FILE + 1);
	if (!nv || !old)
		goto out;
	if ((err = mkdir_p(dir, 0755)) || (err = mkdir_p(snap, 0755))) {
		logmsg(LOG_ERR, "cannot create %s / %s: %s", dir, snap, strerror(-err));
		goto out;
	}
	if (dev) {
		char sub[PATH_MAX + 16];

		/* next to the local records: the same file system, never a shared /tmp */
		if ((size_t)snprintf(tmp, sizeof(tmp), "%s/.import-XXXXXX", dir) >= sizeof(tmp) ||
		    !mkdtemp(tmp)) {
			logmsg(LOG_ERR, "%s: cannot make a temporary directory: %m", dir);
			goto out;
		}
		have_tmp = true;
		err = run_debugfs(debugfs, dev, tmp);
		if (err)
			goto out;
		snprintf(sub, sizeof(sub), "%s%s", tmp, NVCFG_DIR);
		srcfd = open_dir(sub, false);
		src = dev;
	} else {
		srcfd = open_dir(srcdir, false);
	}
	dirfd = open_dir(dir, false);
	snapfd = open_dir(snap, false);
	if (srcfd < 0 || dirfd < 0 || snapfd < 0)
		goto out;

	ret = 0;
	for (r = 0; r < R_COUNT; r++) {
		int32_t w[MAX_WORDS];
		size_t nlen, olen;
		enum rec_state rs, ls;
		const char *why;

		snprintf(path, sizeof(path), "%s.json", recs[r].name);
		rs = read_file(srcfd, path, nv, &nlen);
		if (rs == RS_MISSING) {
			logmsg(LOG_DEBUG, "%s: not on nvcfg", path);
			continue;
		}
		if (rs == RS_OK)
			rs = parse_record(nv, nlen, recs[r].name, recs[r].n, w);
		if (rs != RS_OK) {
			logmsg(LOG_WARNING, "%s on nvcfg is %s: not imported", path, rs_name[rs]);
			continue;
		}
		ls = read_file(snapfd, path, old, &olen);
		if (force)
			why = "forced";
		else if (faccessat(dirfd, path, F_OK, AT_SYMLINK_NOFOLLOW))
			why = "no local copy";
		else if (ls != RS_OK)
			why = "first import";
		else if (!same_bytes(nv, nlen, old, olen))
			why = "changed on nvcfg since the last import";
		else {
			logmsg(LOG_DEBUG, "%s: unchanged on nvcfg, local copy kept", path);
			kept++;
			continue;
		}
		err = write_file_atomic(dirfd, path, nv, nlen, true);
		if (!err)
			err = write_file_atomic(snapfd, path, nv, nlen, true);
		if (err) {
			logmsg(LOG_ERR, "%s: import failed: %s", path, strerror(-err));
			ret = 1;
			continue;
		}
		logmsg(LOG_INFO, "%s imported from nvcfg (%s)", path, why);
		taken++;
	}
	logmsg(LOG_INFO, "import from %s: %d record(s) taken, %d local kept", src, taken, kept);
out:
	if (srcfd >= 0)
		close(srcfd);
	if (dirfd >= 0)
		close(dirfd);
	if (snapfd >= 0)
		close(snapfd);
	if (have_tmp)
		nftw(tmp, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
	free(nv);
	free(old);
	return ret;
}

/* ---- watch ---- */

struct pending {
	bool dirty;
	bool immediate;		/* a one-off result: no rate limit */
	int32_t v[MAX_WORDS];	/* what the hub reported last */
	int32_t cur[MAX_WORDS];	/* what the file holds */
	bool cur_ok;
	double last;		/* monotonic time of the last write attempt */
};

static struct pending pend[R_COUNT];
static volatile sig_atomic_t sig_stop, sig_flush;

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		sig_flush = 1;
	else
		sig_stop = 1;
}

/* rewrite a local record (byte format of the record files, see format_record) */
static int store_record(int dirfd, enum rec_id r, const int32_t *v, bool fsync_it)
{
	char path[64], text[512];
	bool existed;
	int len, err;

	len = format_record(text, sizeof(text), r, v);
	if (len < 0)
		return -EOVERFLOW;
	snprintf(path, sizeof(path), "%s.json", recs[r].name);
	existed = !faccessat(dirfd, path, F_OK, AT_SYMLINK_NOFOLLOW);
	err = write_file_atomic(dirfd, path, text, (size_t)len, fsync_it);
	if (!err && !existed)
		logmsg(LOG_INFO, "%s created", path);
	return err;
}

static void flush(int dirfd, double interval, bool all, bool fsync_it)
{
	double t = now_mono();
	int r;

	for (r = 0; r < R_COUNT; r++) {
		struct pending *p = &pend[r];
		char words[MAX_WORDS * 13];
		int err;

		if (!p->dirty)
			continue;
		if (!all && !p->immediate && t - p->last < interval)
			continue;
		p->last = t;
		fmt_words(words, sizeof(words), p->v, recs[r].n, ", ");
		err = store_record(dirfd, (enum rec_id)r, p->v, fsync_it);
		if (err) {
			logmsg(LOG_ERR, "%s.json: not written [%s]: %s", recs[r].name, words,
			       strerror(-err));
			/* tried again after the interval (or at the next flush request) */
			p->immediate = false;
			continue;
		}
		logmsg(LOG_INFO, "[%s] ==> %s.json", words, recs[r].name);
		memcpy(p->cur, p->v, sizeof(p->cur));
		p->cur_ok = true;
		p->dirty = false;
		p->immediate = false;
	}
}

/* milliseconds until the next record is due, -1: none */
static int next_due(double interval)
{
	double t = now_mono(), best = -1;
	int r;

	for (r = 0; r < R_COUNT; r++) {
		double d;

		if (!pend[r].dirty)
			continue;
		d = pend[r].immediate ? 0 : pend[r].last + interval - t;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}
	return best < 0 ? -1 : (int)(best * 1000) + 1;
}

static void note(enum rec_id r, const int32_t *w, bool immediate)
{
	struct pending *p = &pend[r];
	int n = recs[r].n;

	memcpy(p->v, w, (size_t)n * sizeof(*w));
	if (p->cur_ok && !memcmp(p->cur, w, (size_t)n * sizeof(*w))) {
		p->dirty = false;	/* back to what the file has */
		return;
	}
	if (!p->dirty)
		logmsg(LOG_DEBUG, "%s: new value from the hub", recs[r].name);
	p->dirty = true;
	p->immediate |= immediate;
}

/* what the hub reported -> which record; status matters for the one-off results of accel/gyro */
static void handle_event(const struct hf_event *e)
{
	char words[16 * 12];

	switch (e->action) {
	case ACT_BIAS:
		switch (e->sensor_type) {
		case T_ACCEL: note(R_ACC_BIAS, e->word, false); return;
		case T_GYRO: note(R_GYRO_BIAS, e->word, false); return;
		case T_MAGN: note(R_MAG_BIAS, e->word, false); return;
		}
		break;
	case ACT_TEMP:
		if (e->sensor_type == T_GYRO) {
			note(R_GYRO_TEMP, e->word, false);
			return;
		}
		break;
	case ACT_CALI:
		switch (e->sensor_type) {
		case T_ACCEL:
		case T_GYRO:
			fmt_words(words, sizeof(words), e->word, 3, ", ");
			if (e->status) {
				logmsg(LOG_WARNING, "%s calibration failed (status %u) [%s]: not stored",
				       e->sensor_type == T_ACCEL ? "accelerometer" : "gyroscope",
				       e->status, words);
				return;
			}
			note(e->sensor_type == T_ACCEL ? R_ACC_CALI : R_GYRO_CALI, e->word, true);
			return;
		case T_MAGN: note(R_MAG_CALI, e->word, false); return;
		case T_LIGHT: note(R_ALS_CALI, e->word, true); return;
		case T_PROX: note(R_PS_CALI, e->word, true); return;
		}
		break;
	}
	logmsg(LOG_DEBUG, "event type %u action %u ignored", e->sensor_type, e->action);
}

static int subscribe(int fd)
{
	static const struct { unsigned long req; uint8_t type; } subs[] = {
		{ HF_REQ_BIAS_DATA, T_ACCEL }, { HF_REQ_BIAS_DATA, T_GYRO }, { HF_REQ_BIAS_DATA, T_MAGN },
		{ HF_REQ_TEMP_DATA, T_GYRO },
		{ HF_REQ_CALI_DATA, T_ACCEL }, { HF_REQ_CALI_DATA, T_GYRO }, { HF_REQ_CALI_DATA, T_MAGN },
		{ HF_REQ_CALI_DATA, T_LIGHT }, { HF_REQ_CALI_DATA, T_PROX },
	};
	size_t i;
	int n = 0;

	for (i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
		struct hf_packet pk;

		memset(&pk, 0, sizeof(pk));
		pk.sensor_type = subs[i].type;
		if (ioctl(fd, HF_REQ_REGISTER_STATUS, &pk) || !pk.status) {
			logmsg(LOG_DEBUG, "hub sensor %u not listed", subs[i].type);
			continue;
		}
		memset(&pk, 0, sizeof(pk));
		pk.sensor_type = subs[i].type;
		pk.status = 1;
		if (ioctl(fd, subs[i].req, &pk)) {
			logmsg(LOG_ERR, "subscribe %u/%lx: %m", subs[i].type, subs[i].req);
			continue;
		}
		n++;
	}
	return n;
}

static int cmd_watch(const char *dir, const char *dev, const char *evfile, int interval,
		     bool fsync_it)
{
	struct hf_event ev[16];
	struct sigaction sa;
	sigset_t block, empty;
	int dirfd, fd, r, n;
	double t0 = now_mono();

	dirfd = open_dir(dir, false);
	if (dirfd < 0)
		return 1;
	for (r = 0; r < R_COUNT; r++) {
		enum rec_state rs = read_record(dirfd, (enum rec_id)r, pend[r].cur);

		pend[r].cur_ok = rs == RS_OK;
		pend[r].last = t0;
	}

	if (evfile) {
		fd = open(evfile, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			logmsg(LOG_ERR, "%s: %m", evfile);
			return 1;
		}
	} else {
		fd = open(dev, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			logmsg(LOG_ERR, "%s: %m", dev);
			return 1;
		}
		n = subscribe(fd);
		if (!n) {
			logmsg(LOG_ERR, "no hub sensor to listen to");
			return 1;
		}
		logmsg(LOG_INFO, "listening for %d kinds of hub results, records in %s, every %d s at most",
		       n, dir, interval);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGUSR1);
	sigprocmask(SIG_BLOCK, &block, NULL);
	sigemptyset(&empty);

	while (!sig_stop) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int due = next_due(interval), pr;
		struct timespec ts, *tsp = NULL;
		ssize_t got;

		if (due >= 0) {
			ts.tv_sec = due / 1000;
			ts.tv_nsec = (long)(due % 1000) * 1000000L;
			tsp = &ts;
		}
		pr = ppoll(&pfd, 1, tsp, &empty);
		if (pr < 0 && errno != EINTR) {
			logmsg(LOG_ERR, "poll: %m");
			break;
		}
		if (sig_flush) {
			sig_flush = 0;
			flush(dirfd, interval, true, fsync_it);
		}
		if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
			got = read(fd, ev, sizeof(ev));
			if (got < 0 && errno != EINTR && errno != EAGAIN) {
				logmsg(LOG_ERR, "read: %m");
				break;
			}
			if (got == 0 && evfile)
				break;		/* end of the test stream */
			for (n = 0; got > 0 && n < (int)(got / (ssize_t)sizeof(ev[0])); n++)
				handle_event(&ev[n]);
		}
		flush(dirfd, interval, false, fsync_it);
	}
	/* shutdown, or the end of a test stream: nothing the hub reported is lost */
	flush(dirfd, interval, true, fsync_it);
	close(fd);
	close(dirfd);
	return 0;
}

/* ---- event (test aid) ---- */

static int cmd_event(int argc, char **argv)
{
	struct hf_event e;
	int i;

	if (argc < 3 || argc > 3 + 16) {
		fprintf(stderr, "usage: t2-sensors-cali event TYPE ACTION STATUS [WORD...]\n"
			"  ACTION: bias, cali, temp or a number\n");
		return 2;
	}
	memset(&e, 0, sizeof(e));
	e.sensor_type = (uint8_t)atoi(argv[0]);
	if (!strcmp(argv[1], "bias"))
		e.action = ACT_BIAS;
	else if (!strcmp(argv[1], "cali"))
		e.action = ACT_CALI;
	else if (!strcmp(argv[1], "temp"))
		e.action = ACT_TEMP;
	else
		e.action = (uint8_t)atoi(argv[1]);
	e.status = (uint8_t)atoi(argv[2]);
	for (i = 3; i < argc; i++)
		e.word[i - 3] = (int32_t)strtol(argv[i], NULL, 10);
	return fwrite(&e, sizeof(e), 1, stdout) == 1 ? 0 : 1;
}

/* ---- main ---- */

static void usage(void)
{
	fprintf(stderr,
		"usage: t2-sensors-cali import (-p NVCFG-DEVICE | -f NVCFG-SENSOR-DIR) [-d DIR] [-N SNAPDIR] [-F] [-X DEBUGFS] [-v]\n"
		"       t2-sensors-cali load  [-d DIR] [-s SYSFS] [-w SECONDS] [-n] [-v]\n"
		"       t2-sensors-cali watch [-d DIR] [-D DEVICE] [-i SECONDS] [-e EVENTFILE] [-S] [-v]\n"
		"       t2-sensors-cali event TYPE ACTION STATUS [WORD...]\n"
		"  -d  local record directory (default " DEF_DIR ")\n"
		"  -p  import: the nvcfg partition, read with debugfs (nothing is mounted)\n"
		"  -f  import: a directory holding the nvcfg sensor/ files instead\n"
		"  -N  import: copies of what the last import took (default " DEF_SNAP ")\n"
		"  -F  import: take every usable nvcfg record, whatever the local copy holds\n"
		"  -X  import: the debugfs program (default " DEF_DEBUGFS ")\n"
		"  -s  sysfs root (default " DEF_SYSFS ")\n"
		"  -w  load: seconds to wait for the hub's IIO devices (default 60)\n"
		"  -n  load: print the word lists, send nothing\n"
		"  -D  watch: hub device (default " DEF_DEV ")\n"
		"  -i  watch: least seconds between rewrites of a streaming record (default %d)\n"
		"  -e  watch: read binary events from this file instead of the hub (tests)\n"
		"  -S  watch: no fsync after a rewrite\n"
		"  -v  debug messages\n"
		"  watch: SIGUSR1 writes every pending record now, SIGTERM writes them and exits\n",
		DEF_INTERVAL);
}

int main(int argc, char **argv)
{
	const char *dir = DEF_DIR, *sysfs = DEF_SYSFS, *dev = DEF_DEV, *evfile = NULL, *cmd;
	const char *snap = DEF_SNAP, *part = NULL, *srcdir = NULL, *debugfs = DEF_DEBUGFS;
	int wait_s = 60, interval = DEF_INTERVAL, c;
	bool dry = false, fsync_it = true, force = false;

	if (argc < 2) {
		usage();
		return 2;
	}
	cmd = argv[1];
	if (!strcmp(cmd, "event"))
		return cmd_event(argc - 2, argv + 2);
	optind = 2;
	while ((c = getopt(argc, argv, "d:s:w:nD:i:e:SvN:p:f:FX:")) != -1) {
		switch (c) {
		case 'd': dir = optarg; break;
		case 's': sysfs = optarg; break;
		case 'w': wait_s = atoi(optarg); break;
		case 'n': dry = true; break;
		case 'D': dev = optarg; break;
		case 'i': interval = atoi(optarg); break;
		case 'e': evfile = optarg; break;
		case 'S': fsync_it = false; break;
		case 'v': verbose = true; break;
		case 'N': snap = optarg; break;
		case 'p': part = optarg; break;
		case 'f': srcdir = optarg; break;
		case 'F': force = true; break;
		case 'X': debugfs = optarg; break;
		default: usage(); return 2;
		}
	}
	if (optind != argc) {
		usage();
		return 2;
	}
	if (interval < 1)
		interval = 1;
	if (wait_s < 0)
		wait_s = 0;
	openlog("t2-sensors-cali", LOG_PERROR | LOG_PID, LOG_DAEMON);
	if (!strcmp(cmd, "import")) {
		if (!part == !srcdir) {
			usage();
			return 2;
		}
		return cmd_import(dir, snap, part, srcdir, debugfs, force);
	}
	if (!strcmp(cmd, "load"))
		return cmd_load(dir, sysfs, wait_s, dry);
	if (!strcmp(cmd, "watch"))
		return cmd_watch(dir, dev, evfile, interval, fsync_it);
	usage();
	return 2;
}
