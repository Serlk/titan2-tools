// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-rpcd: 0x4014 AMMS_DRDI_CONTROL.
 *
 * md1drdi is found once at start-up by walking the section headers of
 * the modem image container. COPY requests read pieces of it into the
 * DRDI window. The image is only ever opened O_RDONLY.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rpcd.h"

/* identity of the image seen at start-up; COPY reopens it */
static dev_t img_dev, img_rdev;
static ino_t img_ino;

static int open_image(struct rpcd *r, const char *path, uint64_t *size,
		      int check_same)
{
	struct stat st;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) {
		rpcd_log(r, 0, "%s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st)) {
		rpcd_log(r, 0, "%s: %s", path, strerror(errno));
		goto fail;
	}
	if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
		rpcd_log(r, 0, "%s: neither a block device nor a regular file", path);
		goto fail;
	}
	if (check_same) {
		if (st.st_dev != img_dev || st.st_ino != img_ino ||
		    st.st_rdev != img_rdev) {
			rpcd_log(r, 0, "%s: not the image seen at start-up, refusing",
				 path);
			goto fail;
		}
	} else {
		img_dev = st.st_dev;
		img_ino = st.st_ino;
		img_rdev = st.st_rdev;
	}
	if (size) {
		off_t end;

		if (S_ISREG(st.st_mode))
			end = st.st_size;
		else
			end = lseek(fd, 0, SEEK_END);
		if (end < 0) {
			rpcd_log(r, 0, "%s: size: %s", path, strerror(errno));
			goto fail;
		}
		*size = (uint64_t)end;
	}
	return fd;
fail:
	close(fd);
	return -1;
}

/* read up to len bytes at off; returns the count (short only at EOF) or -1 */
static ssize_t read_at(int fd, void *buf, size_t len, uint64_t off)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = pread(fd, (uint8_t *)buf + got, len - got,
				  (off_t)(off + got));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!n)
			break;
		got += (size_t)n;
	}
	return (ssize_t)got;
}

/* returns 0 and fills data_off/size, or -1 */
static int walk(struct rpcd *r, int fd, uint64_t *data_off, uint32_t *size)
{
	uint8_t h[IMG_HDR_SIZE];
	uint64_t off = 0;

	for (unsigned int i = 0; i < IMG_MAX_SECTIONS; i++) {
		uint32_t dsize, hsize, align;
		uint64_t step;
		char name[32];

		if (read_at(fd, h, sizeof(h), off) != (ssize_t)sizeof(h)) {
			rpcd_log(r, 0, "container: short header at 0x%llx",
				 (unsigned long long)off);
			return -1;
		}
		if (get_le32(h) != IMG_MAGIC) {
			/*
			 * Retry once after the padding the previous section
			 * may have: up to the next 4 KiB boundary, plus 0x100.
			 * The first section has no predecessor: no retry.
			 */
			if (i == 0) {
				rpcd_log(r, 0, "container: no section magic at 0");
				return -1;
			}
			off += ((off + 0xFFF) & ~(uint64_t)0xFFF) - off + 0x100;
			if (off > INT32_MAX ||
			    read_at(fd, h, sizeof(h), off) != (ssize_t)sizeof(h) ||
			    get_le32(h) != IMG_MAGIC) {
				rpcd_log(r, 0, "container: section %u has no magic", i);
				return -1;
			}
		}
		dsize = get_le32(h + 0x04);
		memcpy(name, h + 0x08, sizeof(name));
		name[31] = '\0';
		rpcd_log(r, 1, "container: section '%s' at 0x%llx, 0x%x bytes",
			 name, (unsigned long long)off, dsize);
		if (!strcmp(name, DRDI_NAME)) {
			/* the data always follows at 0x200, whatever 0x34 says */
			*data_off = off + IMG_HDR_SIZE;
			*size = dsize;
			return 0;
		}
		if (get_le32(h + 0x40)) {
			rpcd_log(r, 0, "container: no section '%s'", DRDI_NAME);
			return -1;
		}
		hsize = get_le32(h + 0x34);
		if (!hsize)
			hsize = IMG_HDR_SIZE;
		align = get_le32(h + 0x44);
		step = dsize;
		if (align)
			step = (step + align - 1) / align * align;
		off += hsize + step;
		/* offsets are signed 32-bit in the format */
		if (off > INT32_MAX) {
			rpcd_log(r, 0, "container: walk past 2 GiB");
			return -1;
		}
	}
	rpcd_log(r, 0, "container: more than %d sections", IMG_MAX_SECTIONS);
	return -1;
}

/*
 * Start-up: find md1drdi. Returns 0 when found and usable; the caller then
 * provides the window and sets prepared.
 */
int drdi_locate(struct rpcd *r)
{
	struct rpcd_drdi *d = &r->drdi;
	uint64_t img_size, off = 0;
	uint32_t size = 0;
	int fd, ret = -1;

	fd = open_image(r, d->image, &img_size, 0);
	if (fd < 0)
		return -1;
	if (d->raw) {
		if (img_size > DRDI_SIZE_MAX) {
			rpcd_log(r, 0, "%s: too big for md1drdi", d->image);
			goto out;
		}
		size = (uint32_t)img_size;
	} else if (walk(r, fd, &off, &size)) {
		goto out;
	}
	if (!size || size > DRDI_SIZE_MAX) {
		rpcd_log(r, 0, "md1drdi: bad size 0x%x", size);
		goto out;
	}
	if (off + size > img_size) {
		rpcd_log(r, 0, "md1drdi: 0x%x bytes at 0x%llx run past the end of %s",
			 size, (unsigned long long)off, d->image);
		goto out;
	}
	d->data_off = off;
	d->size = size;
	rpcd_log(r, 0, "md1drdi: data at 0x%llx, 0x%x bytes",
		 (unsigned long long)off, size);
	ret = 0;
out:
	close(fd);
	return ret;
}

/* INIT */
static int drdi_init(struct rpcd *r, const uint8_t *q, uint8_t *st)
{
	struct rpcd_drdi *d = &r->drdi;
	uint32_t n;

	if (!d->prepared) {
		rpcd_log(r, 0, "DRDI INIT: md1drdi not prepared");
		goto fail_a;		/* seq not echoed */
	}
	st[1] = q[1];
	if (q[4] != DRDI_VERSION) {
		rpcd_log(r, 0, "DRDI INIT: version %u", q[4]);
		goto fail_a;
	}
	st[4] = DRDI_VERSION;
	if (q[5] > DRDI_MAX_SETS) {
		rpcd_log(r, 0, "DRDI INIT: %u sets", q[5]);
		goto fail_a;
	}
	d->nsets = n = q[5];
	for (uint32_t i = 0; i < n; i++) {
		uint32_t start = get_le32(q + 0x08 + 8 * i);
		uint32_t len = get_le32(q + 0x0C + 8 * i);

		/* u32, wrapping, as specified: INIT reads nothing */
		if ((uint32_t)(start + len) > d->size) {
			rpcd_log(r, 0, "DRDI INIT: pair %u 0x%x+0x%x past 0x%x",
				 i, start, len, d->size);
			st[0] = 0xFE;
			st[6] = 0xFF;
			return -1;
		}
	}
	d->copy_state = -1;
	st[0] = 0x00;
	rpcd_log(r, 0, "DRDI INIT seq %u: %u sets", q[1], n);
	return 0;
fail_a:
	st[0] = 0xFF;
	st[4] = DRDI_VERSION;
	st[5] = 0x00;
	st[6] = 0xFF;
	return -1;
}

/* COPY */
static int drdi_copy(struct rpcd *r, const uint8_t *q, uint8_t *st)
{
	struct rpcd_drdi *d = &r->drdi;
	int final = q[4] == 1;
	uint32_t copied = 0;
	int fd;

	st[1] = q[1];
	for (uint32_t i = 0; i < d->nsets; i++) {
		const uint8_t *s = q + 0x08 + 12 * i;
		uint32_t src = get_le32(s), dst = get_le32(s + 4);
		uint32_t len = get_le32(s + 8);

		if (!len)
			continue;
		if (len > DRDI_LEN_MAX ||
		    (uint32_t)(src + len) > d->size ||		/* vendor check */
		    (uint64_t)src + len > d->size ||		/* no wrap */
		    (uint64_t)dst + len > d->window_size ||	/* window */
		    !d->window) {
			rpcd_log(r, 0, "DRDI COPY: set %u src 0x%x dst 0x%x len 0x%x out of range",
				 i, src, dst, len);
			goto fail_c;
		}
	}
	if (d->copy_state == 1) {
		rpcd_log(r, 0, "DRDI COPY seq %u: already done, nothing read", q[1]);
		st[0] = 0x00;
		return 0;
	}
	fd = open_image(r, d->image, NULL, 1);
	if (fd < 0)
		goto fail_c;
	st[4] = final ? 0x00 : 0xFF;
	for (uint32_t i = 0; i < d->nsets; i++) {
		const uint8_t *s = q + 0x08 + 12 * i;
		uint32_t src = get_le32(s), dst = get_le32(s + 4);
		uint32_t len = get_le32(s + 8);
		ssize_t n;

		if (!len)
			continue;
		n = read_at(fd, d->window + dst, len, d->data_off + src);
		if (n < 0) {
			rpcd_log(r, 0, "DRDI COPY: set %u: %s", i, strerror(errno));
			close(fd);
			goto fail_c;
		}
		if ((uint32_t)n != len)
			rpcd_log(r, 0, "DRDI COPY: set %u: short read %zd of 0x%x",
				 i, n, len);
		rpcd_log(r, 1, "DRDI COPY: set %u src 0x%x -> dst 0x%x, 0x%x bytes",
			 i, src, dst, len);
		copied += (uint32_t)n;
		d->copy_state = final ? 1 : 0;
	}
	close(fd);
	st[0] = d->copy_state == -1 ? 0xFF : 0x00;
	rpcd_log(r, 0, "DRDI COPY seq %u flag %u: %u bytes into the window",
		 q[1], q[4], copied);
	return 0;
fail_c:
	d->copy_state = -1;
	st[0] = 0xFF;
	return -1;
}

/* always two arguments, return code and the 8-byte status */
void drdi_request(struct rpcd *r, const struct rpcd_arg *a0,
		  struct rpcd_out *out)
{
	uint8_t st[8] = { 0 };
	int32_t ret;

	if (a0->len != DRDI_REQ_LEN) {
		rpcd_log(r, 0, "DRDI: request of %u bytes, want %u", a0->len,
			 DRDI_REQ_LEN);
		ret = -1;
	} else if (a0->data[0] == DRDI_CMD_INIT) {
		ret = drdi_init(r, a0->data, st);
	} else if (a0->data[0] == DRDI_CMD_COPY) {
		ret = drdi_copy(r, a0->data, st);
	} else {
		rpcd_log(r, 0, "DRDI: unknown cmd %u", a0->data[0]);
		st[0] = 0xFF;
		st[1] = a0->data[1];
		ret = -1;
	}
	rpcd_out_u32(out, (uint32_t)ret);
	rpcd_out_add(out, st, sizeof(st));
}
