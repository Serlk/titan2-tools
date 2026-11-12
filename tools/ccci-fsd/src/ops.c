// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-fsd: the file operations and their return codes.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fsd.h"

#define OP_OPEN			0x1001
#define OP_SEEK			0x1002
#define OP_READ			0x1003
#define OP_WRITE		0x1004
#define OP_CLOSE		0x1005
#define OP_CLOSE_ALL		0x1006
#define OP_CREATE_DIR		0x1007
#define OP_REMOVE_DIR		0x1008
#define OP_GET_FILE_SIZE	0x1009
#define OP_GET_FOLDER_SIZE	0x100A
#define OP_RENAME		0x100B
#define OP_MOVE			0x100C
#define OP_COUNT		0x100D
#define OP_GET_DISK_INFO	0x100E
#define OP_DELETE		0x100F
#define OP_GET_ATTRIBUTES	0x1010
#define OP_OPEN_VARIANT		0x1011
#define OP_FIND_FIRST		0x1012
#define OP_FIND_NEXT		0x1013
#define OP_FIND_CLOSE		0x1014
#define OP_NOP_15		0x1015
#define OP_NOP_16		0x1016
#define OP_SHUTDOWN		0x1017
#define OP_XDELETE		0x1018
#define OP_NOP_19		0x1019
#define OP_GET_DRIVE		0x101A
#define OP_GET_CLUSTER_SIZE	0x101B
#define OP_NOP_1C		0x101C
#define OP_OTP_WRITE		0x101D
#define OP_OTP_LOCK		0x1020
#define OP_RESTORE		0x1021
#define OP_COMBINED_READ	0x1022
#define OP_BIN_REGION		0x1023
#define OP_COMBINED_WRITE	0x1024
#define OP_GET_FILE_DETAIL	0x1025
#define OP_COMBINED_READ_EX	0x1026

/* the most data a Read reply (ret, count, data) can carry */
#define FSD_READ_MAX	0x40AC

/* compound operations: step mask bits */
#define CX_OPEN		0x01
#define CX_SIZE		0x02
#define CX_SEEK		0x04
#define CX_READ		0x08
#define CX_CLOSE	0x10
#define CX_WRITE	0x20

/* data limit of a 0x1022 / 0x1026 reply */
#define CX_READ_CAP	0x40A0
#define CX_READ_EX_CAP	0x4098

/* control block bytes we read: 0x1022 words 0-9, 0x1024 0-8, 0x1026 0-10 */
#define CX_BLOCK_READ	40
#define CX_BLOCK_WRITE	36
#define CX_BLOCK_READ_EX 44

/* Move flags; -1 is what the factory server sends for a short copy */
#define MV_COPY		0x00001
#define MV_KILL		0x00002
#define MV_OVERWRITE	0x10000
#define MV_SHORT	(-1)

/* open flags that make Close sync the file */
#define FS_O_SYNC_MASK	(FS_O_CREATE | FS_O_TRUNC | FS_O_COMMIT)

static const char *op_name(uint32_t op)
{
	static const char *const names[] = {
		"Open", "Seek", "Read", "Write", "Close", "CloseAll",
		"CreateDir", "RemoveDir", "GetFileSize", "GetFolderSize",
		"Rename", "Move", "Count", "GetDiskInfo", "Delete",
		"GetAttributes", "OpenVariant", "FindFirst", "FindNext",
		"FindClose", "Nop15", "Nop16", "Shutdown", "XDelete", "Nop19",
		"GetDrive", "GetClusterSize", "Nop1C", "OTPWrite", "OTPRead",
		"OTPQueryLength", "OTPLock", "Restore", "CombinedRead",
		"BinaryRegion", "CombinedWrite", "GetFileDetail",
		"CombinedReadEx",
	};

	if (op >= OP_OPEN && op <= OP_COMBINED_READ_EX)
		return names[op - OP_OPEN];
	return "unknown";
}

static void arg_lens(const struct fsd_args *a, char *buf, size_t size)
{
	size_t n = 0;

	buf[0] = 0;
	for (uint32_t i = 0; i < a->n && n < size; i++) {
		int r = snprintf(buf + n, size - n, "%s%u", i ? "," : "",
				 a->a[i].len);

		if (r < 0)
			break;
		n += (size_t)r;
	}
}

/* a modem supplied path, printable for the journal */
static void path_str(const struct fsd_arg *a, char *buf, size_t size)
{
	size_t n = 0;

	for (uint32_t i = 0; i + 1 < a->len && n + 1 < size; i += 2) {
		unsigned int u = a->data[i] | a->data[i + 1] << 8;

		if (!u)
			break;
		buf[n++] = (u >= 0x20 && u < 0x7f && u != '"') ? (char)u : '?';
	}
	buf[n] = 0;
}

static int bad_args(struct fsd *fsd, unsigned int slot, uint32_t op,
		    const struct fsd_args *a)
{
	char lens[96];

	arg_lens(a, lens, sizeof(lens));
	fsd_journal(fsd, "BADPARAM op=0x%x (%s) slot=%u nargs=%u lens=%s",
		    op, op_name(op), slot, a->n, lens);
	return FS_PARAM;
}

static int arg_u32(const struct fsd_args *a, uint32_t i, uint32_t *v)
{
	/* at least 4 bytes, only the first 4 count */
	if (i >= a->n || a->a[i].len < 4)
		return -1;
	*v = get_le32(a->a[i].data);
	return 0;
}

static int path_arg(struct fsd *fsd, uint32_t op, const struct fsd_arg *a,
		    struct fsd_path *p)
{
	int r = sb_parse_path(fsd, a, p);

	if (r) {
		char s[96];

		path_str(a, s, sizeof(s));
		fsd_journal(fsd, "REFUSED name op=0x%x (%s) path=\"%s\" ret=%d",
			    op, op_name(op), s, r);
	}
	return r;
}

static const char *ptag(const struct fsd_drive *d)
{
	return d->protect ? "PROTECT " : "";
}

/* files on this drive show the read-only attribute */
static int drive_ro(const struct fsd *fsd, const struct fsd_drive *d)
{
	return !d->writable || (d->protect && fsd->refuse_protect);
}

/* may the modem change something on this drive? */
static int may_modify(struct fsd *fsd, const struct fsd_path *p,
		      const char *what)
{
	const struct fsd_drive *d = p->drv;

	if (!d->writable || d->rw_fd < 0) {
		fsd_journal(fsd, "REFUSED write-protected %s %c:/%s", what,
			    d->letter, p->rel);
		return FS_WRITE_PROT;
	}
	if (d->protect && fsd->refuse_protect) {
		fsd_journal(fsd, "REFUSED PROTECT %s %c:/%s", what, d->letter,
			    p->rel);
		return FS_WRITE_PROT;
	}
	return FS_OK;
}

/*
 * argument 0: a handle; -2 if it is missing, -10 if it is not open or
 * (except for Close) a directory handle
 */
static int handle_arg(struct fsd *fsd, const struct fsd_args *a,
		      struct fsd_handle **h, int allow_dir)
{
	uint32_t v;

	if (arg_u32(a, 0, &v))
		return FS_PARAM;
	if (v < 1 || v > FSD_HANDLES || !fsd->h[v - 1].used)
		return FS_BAD_HANDLE;
	if (fsd->h[v - 1].is_dir && !allow_dir)
		return FS_BAD_HANDLE;
	*h = &fsd->h[v - 1];
	return FS_OK;
}

static void find_free(struct fsd_find *f);
static int drive_ro(const struct fsd *fsd, const struct fsd_drive *d);

static struct fsd_handle *handle_get(struct fsd *fsd, int32_t v)
{
	if (v < 1 || v > FSD_HANDLES || !fsd->h[v - 1].used)
		return NULL;
	return &fsd->h[v - 1];
}

/*
 * Close (and the close step of the compound operations) syncs a
 * file opened with create, truncate or commit; a failed sync is logged
 * and ignored. CloseAll, Shutdown and a modem reset never sync.
 */
static void handle_close(struct fsd *fsd, struct fsd_handle *h, int sync)
{
	if (!h->used)
		return;
	if (sync && !h->is_dir && (h->flags & FS_O_SYNC_MASK)) {
		if (fsync(h->fd))
			fsd_log(fsd, 0, "fsync %c:/%s: %s (ignored)", h->drv->letter,
				h->rel, strerror(errno));
		else
			fsd_log(fsd, 1, "sync %c:/%s", h->drv->letter, h->rel);
	}
	if (h->fd >= 0)
		close(h->fd);
	find_free(h->find);
	memset(h, 0, sizeof(*h));
	h->fd = -1;
}

void fsd_close_all(struct fsd *fsd)
{
	for (int i = 0; i < FSD_HANDLES; i++)
		handle_close(fsd, &fsd->h[i], 0);
}

/*
 * The entry behind these handles is gone or replaced: stop writing
 * through them (an ro/ handle could otherwise copy an old file up).
 */
static void handles_stale(struct fsd *fsd, const struct fsd_path *p,
			  int only_ro, int tree)
{
	size_t n = strlen(p->rel);

	for (int i = 0; i < FSD_HANDLES; i++) {
		struct fsd_handle *h = &fsd->h[i];
		int match = !strcmp(h->rel, p->rel) ||
			    (tree && !strncmp(h->rel, p->rel, n) && h->rel[n] == '/');

		if (h->used && !h->is_dir && h->drv == p->drv && match &&
		    (!only_ro || h->layer == LAYER_RO))
			h->stale = 1;
	}
}

static int free_handle(struct fsd *fsd)
{
	for (int i = 0; i < FSD_HANDLES; i++)
		if (!fsd->h[i].used)
			return i;
	return -1;
}

/*
 * path errors of Open, GetAttributes, GetFileDetail, the
 * compound operations and Move: a bad drive letter is -19, a ".."
 * component -16; other name errors stay -3.
 */
static int has_dotdot(const struct fsd_arg *a)
{
	unsigned int run = 0;
	int dots = 1;

	for (uint32_t i = 0;; i += 2) {
		unsigned int u = i + 1 < a->len ?
				 a->data[i] | a->data[i + 1] << 8 : 0;

		if (!u || u == '\\' || u == '/') {
			if (dots && run == 2)
				return 1;
			if (!u)
				return 0;
			run = 0;
			dots = 1;
			continue;
		}
		run++;
		if (u != '.')
			dots = 0;
	}
}

static int cx_path_err(int r, const struct fsd_arg *a)
{
	if (r == FS_BAD_NAME && has_dotdot(a))
		return FS_DENIED;
	return r;
}

/*
 * The host error mapping of the same operations: a missing directory is
 * -9 like a missing file (never -19), other host errors (EEXIST,
 * ENOSPC, EFBIG, our file size limit) are -18. The codes of our own
 * rules (-2, -5, -10, -16, -30, -45, -49) stay.
 */
static int cx_err(int r)
{
	switch (r) {
	case FS_PATH_NOT_FOUND:
		return FS_NOT_FOUND;
	case FS_EXISTS:
	case FS_DISK_FULL:
		return FS_GENERAL;
	default:
		return r;
	}
}

/* the open operation without the handle: an fd and the layer it is in */
static int open_fd(struct fsd *fsd, const struct fsd_path *pp, uint32_t flags,
		   int *fdp, enum fsd_layer *layerp)
{
	const struct fsd_path p = *pp;
	struct fsd_lookup lk;
	int fd, r, write;
	enum fsd_layer layer;

	r = sb_lookup(fsd, &p, &lk);
	if (r)
		return r;
	/* create and truncate open read/write even with 0x100 */
	write = !(flags & FS_O_RDONLY) || (flags & (FS_O_CREATE | FS_O_TRUNC));

	if (flags & FS_O_DIR) {
		if (lk.layer == LAYER_NONE || !lk.is_dir)
			return FS_PATH_NOT_FOUND;
		fd = sb_open_dir(fsd, &p, lk.layer);
		if (fd < 0)
			return sb_errno_to_fs(fsd, fd, &p, FS_PATH_NOT_FOUND);
		layer = lk.layer;
		goto done;
	}
	if (lk.layer != LAYER_NONE && lk.is_dir)
		return FS_DENIED;

	if (lk.layer == LAYER_NONE) {
		if (!(flags & (FS_O_CREATE | FS_O_TRUNC)))
			return FS_NOT_FOUND;
		r = may_modify(fsd, &p, "create");
		if (r)
			return r;
		fd = sb_open_file(fsd, &p, LAYER_RW, write, 1, 0);
		if (fd < 0)
			return sb_errno_to_fs(fsd, fd, &p, FS_PATH_NOT_FOUND);
		if (lk.whiteout)
			sb_whiteout(fsd, &p, 0);
		fsd_journal(fsd, "%sCREATE %c:/%s", ptag(p.drv), p.drv->letter,
			    p.rel);
		layer = LAYER_RW;
	} else if (flags & FS_O_TRUNC) {
		r = may_modify(fsd, &p, "truncate");
		if (r)
			return r;
		/* over an ro/ file: a new empty file in rw/, nothing to copy */
		fd = sb_open_file(fsd, &p, LAYER_RW, write,
				  lk.layer == LAYER_RO, lk.layer == LAYER_RW);
		if (fd < 0)
			return sb_errno_to_fs(fsd, fd, &p, FS_NOT_FOUND);
		handles_stale(fsd, &p, 1, 0);
		fsd_journal(fsd, "%sTRUNCATE %c:/%s", ptag(p.drv), p.drv->letter,
			    p.rel);
		layer = LAYER_RW;
	} else if (lk.layer == LAYER_RW) {
		fd = sb_open_file(fsd, &p, LAYER_RW, write && p.drv->writable,
				  0, 0);
		if (fd < 0)
			return sb_errno_to_fs(fsd, fd, &p, FS_NOT_FOUND);
		layer = LAYER_RW;
	} else {
		/* copied up on the first write */
		fd = sb_open_file(fsd, &p, LAYER_RO, 0, 0, 0);
		if (fd < 0)
			return sb_errno_to_fs(fsd, fd, &p, FS_NOT_FOUND);
		layer = LAYER_RO;
	}
done:
	*fdp = fd;
	*layerp = layer;
	return FS_OK;
}

/* Open into a new handle: returns the handle (>= 1) or an error */
static int open_handle(struct fsd *fsd, const struct fsd_path *p,
		       uint32_t flags)
{
	struct fsd_handle *h;
	enum fsd_layer layer;
	int idx, fd, r;

	idx = free_handle(fsd);
	if (idx < 0)
		return FS_NO_HANDLE;
	r = open_fd(fsd, p, flags, &fd, &layer);
	if (r)
		return r;
	h = &fsd->h[idx];
	memset(h, 0, sizeof(*h));
	h->used = 1;
	h->fd = fd;
	h->layer = layer;
	h->is_dir = !!(flags & FS_O_DIR);
	h->want_write = !(flags & FS_O_RDONLY) ||
			(flags & (FS_O_CREATE | FS_O_TRUNC));
	h->flags = flags;
	h->drv = p->drv;
	memcpy(h->rel, p->rel, sizeof(h->rel));
	fsd_log(fsd, 1, "open %c:/%s flags 0x%x -> %d", p->drv->letter, p->rel,
		flags, idx + 1);
	return idx + 1;
}

static int op_open(struct fsd *fsd, const struct fsd_args *a, uint32_t op)
{
	struct fsd_path p;
	uint32_t flags;
	int r;

	if (a->n < 2 || arg_u32(a, 1, &flags))
		return FS_PARAM;
	r = path_arg(fsd, op, &a->a[0], &p);
	if (r)
		return cx_path_err(r, &a->a[0]);
	r = open_handle(fsd, &p, flags);
	return r < 0 ? cx_err(r) : r;
}

/* move the file of h to rw/ and point every ro/ handle of it there */
static int handle_copy_up(struct fsd *fsd, struct fsd_handle *h)
{
	struct fsd_path p;
	int r;

	p.drv = h->drv;
	memcpy(p.rel, h->rel, sizeof(p.rel));
	r = sb_copy_up(fsd, &p, h->fd);
	if (r)
		return sb_errno_to_fs(fsd, r, &p, FS_NOT_FOUND);
	for (int i = 0; i < FSD_HANDLES; i++) {
		struct fsd_handle *g = &fsd->h[i];
		int fd;

		if (!g->used || g->is_dir || g->stale || g->layer != LAYER_RO ||
		    g->drv != h->drv || strcmp(g->rel, h->rel))
			continue;
		fd = sb_open_file(fsd, &p, LAYER_RW, g->want_write, 0, 0);
		if (fd < 0) {
			g->stale = 1;
			if (g == h)
				return sb_errno_to_fs(fsd, fd, &p, FS_NOT_FOUND);
			continue;
		}
		close(g->fd);
		g->fd = fd;
		g->layer = LAYER_RW;
	}
	return FS_OK;
}

static int file_size(struct fsd_handle *h, uint64_t *size)
{
	struct stat st;

	if (fstat(h->fd, &st))
		return FS_GENERAL;
	*size = (uint64_t)st.st_size;
	return FS_OK;
}

/*
 * whence 0/1/2, others -2; beyond the end is allowed, a
 * negative position is -18
 */
static int seek_to(struct fsd_handle *h, int32_t off, uint32_t whence)
{
	uint64_t size;
	int64_t base, pos;
	int r;

	if (h->is_dir)
		return FS_BAD_HANDLE;
	r = file_size(h, &size);
	if (r)
		return r;
	switch (whence) {
	case 0:
		base = 0;
		break;
	case 1:
		base = (int64_t)h->pos;
		break;
	case 2:
		base = (int64_t)size;
		break;
	default:
		return FS_PARAM;
	}
	pos = base + off;
	if (pos < 0)
		return FS_GENERAL;
	h->pos = (uint64_t)pos;
	return FS_OK;
}

/* the reply is the new position (low 32 bits) or an error */
static int op_seek(struct fsd *fsd, const struct fsd_args *a)
{
	struct fsd_handle *h;
	uint32_t off, whence;
	int r;

	if (a->n < 3 || arg_u32(a, 1, &off) || arg_u32(a, 2, &whence))
		return FS_PARAM;
	r = handle_arg(fsd, a, &h, 0);
	if (r)
		return r;
	r = seek_to(h, (int32_t)off, whence);
	if (r)
		return cx_err(r);
	return (int)(uint32_t)h->pos;
}

/* read up to len bytes at the position; short reads and EOF are no error */
static int handle_read(struct fsd *fsd, struct fsd_handle *h, uint8_t *buf,
		       size_t len, size_t *got)
{
	*got = 0;
	if (h->is_dir)
		return FS_BAD_HANDLE;
	while (*got < len) {
		ssize_t n = pread(h->fd, buf + *got, len - *got,
				  (off_t)(h->pos + *got));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			*got = 0;
			return sb_errno_to_fs(fsd, errno, NULL, FS_GENERAL);
		}
		if (!n)
			break;
		*got += (size_t)n;
	}
	h->pos += *got;
	return FS_OK;
}

static void op_read(struct fsd *fsd, const struct fsd_args *a,
		    struct fsd_out *o)
{
	static uint8_t buf[FSD_READ_MAX];
	struct fsd_handle *h;
	uint32_t len;
	size_t got = 0;
	int r = FS_OK;

	if (a->n < 2 || arg_u32(a, 1, &len)) {
		r = FS_PARAM;
		goto out;
	}
	r = handle_arg(fsd, a, &h, 0);
	if (r)
		goto out;
	if (len > FSD_READ_MAX)
		len = FSD_READ_MAX;	/* never more than a reply can carry */
	r = handle_read(fsd, h, buf, len, &got);
	r = cx_err(r);
out:
	/* ret, count, data */
	fsd_out_u32(o, (uint32_t)r);
	fsd_out_u32(o, r ? 0 : (uint32_t)got);
	fsd_out_add(o, buf, r ? 0 : got);
}

/*
 * Write len bytes at the position: the drive rules, copy-up and the
 * journal. A short write is no error; *done is what was written.
 */
static int handle_write(struct fsd *fsd, struct fsd_handle *h,
			const uint8_t *data, size_t len, size_t *done)
{
	struct fsd_path p;
	int r;

	*done = 0;
	p.drv = h->drv;
	memcpy(p.rel, h->rel, sizeof(p.rel));
	if (h->is_dir)
		return FS_BAD_HANDLE;
	if (!h->want_write)
		return FS_DENIED;	/* question 9 */
	r = may_modify(fsd, &p, "write");
	if (r)
		return r;
	if (h->stale) {
		fsd_journal(fsd, "REFUSED write to a deleted/replaced file %c:/%s",
			    p.drv->letter, p.rel);
		return FS_DENIED;
	}
	if (h->pos + len > FSD_FILE_MAX)
		return FS_DISK_FULL;
	if (h->layer == LAYER_RO) {
		r = handle_copy_up(fsd, h);
		if (r)
			return r;
	}
	while (*done < len) {
		ssize_t n = pwrite(h->fd, data + *done, len - *done,
				   (off_t)(h->pos + *done));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			r = n < 0 ? sb_errno_to_fs(fsd, errno, &p, FS_GENERAL) :
				    FS_DISK_FULL;
			break;
		}
		*done += (size_t)n;
	}
	fsd_journal(fsd, "%sWRITE %c:/%s off=%llu len=%zu crc32=%08x%s",
		    ptag(p.drv), p.drv->letter, p.rel,
		    (unsigned long long)h->pos, *done,
		    fsd_crc32(0, data, *done), r ? " (short)" : "");
	h->pos += *done;
	if (r && *done)
		r = FS_OK;	/* report what was written */
	return r;
}

static void op_write(struct fsd *fsd, const struct fsd_args *a,
		     struct fsd_out *o)
{
	struct fsd_handle *h;
	uint32_t len;
	size_t done = 0;
	int r = FS_OK;

	if (a->n < 3 || arg_u32(a, 2, &len) || len > a->a[1].len) {
		r = FS_PARAM;
		goto out;
	}
	r = handle_arg(fsd, a, &h, 0);
	if (r)
		goto out;
	r = cx_err(handle_write(fsd, h, a->a[1].data, len, &done));
out:
	/* a failed write is ret < 0 with count -1 */
	fsd_out_u32(o, (uint32_t)r);
	fsd_out_u32(o, r ? 0xFFFFFFFFu : (uint32_t)done);
}

static void op_file_size(struct fsd *fsd, const struct fsd_args *a,
			 struct fsd_out *o)
{
	struct fsd_handle *h = NULL;
	uint64_t size = 0;
	int r;

	r = handle_arg(fsd, a, &h, 0);	/* -2 without the handle */
	if (!r)
		r = file_size(h, &size);
	if (size > UINT32_MAX)
		size = UINT32_MAX;
	fsd_out_u32(o, (uint32_t)r);
	fsd_out_u32(o, r ? 0 : (uint32_t)size);
}

static int op_close(struct fsd *fsd, const struct fsd_args *a)
{
	struct fsd_handle *h;
	int r;

	r = handle_arg(fsd, a, &h, 1);
	if (r)
		return r;
	handle_close(fsd, h, 1);
	return FS_OK;
}

static int op_create_dir(struct fsd *fsd, const struct fsd_args *a,
			 uint32_t op)
{
	struct fsd_lookup lk;
	struct fsd_path p;
	int r;

	if (a->n < 1)
		return FS_PARAM;
	r = path_arg(fsd, op, &a->a[0], &p);
	if (r)
		return r;
	r = sb_lookup(fsd, &p, &lk);
	if (r)
		return r;
	if (!*p.rel || lk.layer != LAYER_NONE)
		return FS_EXISTS;
	r = may_modify(fsd, &p, "mkdir");
	if (r)
		return r;
	r = sb_mkdir(fsd, &p);
	if (r)
		return sb_errno_to_fs(fsd, r, &p, FS_PATH_NOT_FOUND);
	if (lk.whiteout)
		sb_whiteout(fsd, &p, 0);
	fsd_journal(fsd, "%sMKDIR %c:/%s", ptag(p.drv), p.drv->letter, p.rel);
	return FS_OK;
}

/*
 * Remove an existing entry of the merged view: a file as Delete does, a
 * directory (Move with overwrite) with everything below it.
 */
static int delete_entry(struct fsd *fsd, const struct fsd_path *p,
			const struct fsd_lookup *lk)
{
	int r;

	if (!*p->rel)
		return FS_DENIED;
	r = may_modify(fsd, p, "delete");
	if (r)
		return r;
	/* whiteout first: a failed unlink then still shows the rw/ entry */
	if (lk->in_ro) {
		r = sb_whiteout(fsd, p, 1);
		if (r)
			return sb_errno_to_fs(fsd, r, p, FS_GENERAL);
	}
	if (lk->layer == LAYER_RW) {
		r = lk->is_dir ? sb_rmtree(fsd, p) : sb_unlink(fsd, p);
		if (r)
			return sb_errno_to_fs(fsd, r, p, FS_NOT_FOUND);
	}
	handles_stale(fsd, p, 0, lk->is_dir);
	fsd_journal(fsd, "%sDELETE%s %c:/%s%s", ptag(p->drv),
		    lk->is_dir ? "-TREE" : "", p->drv->letter, p->rel,
		    lk->in_ro ? " (whiteout)" : "");
	return FS_OK;
}

static int op_delete(struct fsd *fsd, const struct fsd_args *a, uint32_t op)
{
	struct fsd_lookup lk;
	struct fsd_path p;
	int r;

	if (a->n < 1)
		return FS_PARAM;
	r = path_arg(fsd, op, &a->a[0], &p);
	if (r)
		return r;
	r = sb_lookup(fsd, &p, &lk);
	if (r)
		return r;
	if (lk.layer == LAYER_NONE)
		return FS_NOT_FOUND;
	if (!*p.rel || lk.is_dir)
		return FS_DENIED;
	return delete_entry(fsd, &p, &lk);
}

static int op_rename(struct fsd *fsd, const struct fsd_args *a, uint32_t op)
{
	struct fsd_lookup lo, ln;
	struct fsd_path po, pn;
	int r, fd;

	if (a->n < 2)
		return FS_PARAM;
	r = path_arg(fsd, op, &a->a[0], &po);
	if (r)
		return r;
	r = path_arg(fsd, op, &a->a[1], &pn);
	if (r)
		return r;
	if (po.drv != pn.drv)
		return FS_PARAM;
	if (!*po.rel || !*pn.rel)
		return FS_DENIED;
	r = sb_lookup(fsd, &po, &lo);
	if (r)
		return r;
	if (lo.layer == LAYER_NONE)
		return FS_NOT_FOUND;
	if (lo.is_dir)
		return FS_DENIED;	/* files only */
	r = sb_lookup(fsd, &pn, &ln);
	if (r)
		return r;
	if (ln.layer != LAYER_NONE)
		return FS_EXISTS;
	r = may_modify(fsd, &po, "rename");
	if (r)
		return r;

	if (lo.layer == LAYER_RW) {
		if (lo.in_ro) {
			r = sb_whiteout(fsd, &po, 1);
			if (r)
				return sb_errno_to_fs(fsd, r, &po, FS_GENERAL);
		}
		r = sb_rename_rw(fsd, &po, &pn);
		if (r)
			return sb_errno_to_fs(fsd, r, &pn, FS_PATH_NOT_FOUND);
	} else {
		fd = sb_open_file(fsd, &po, LAYER_RO, 0, 0, 0);
		if (fd < 0)
			return sb_errno_to_fs(fsd, fd, &po, FS_NOT_FOUND);
		r = sb_copy_up(fsd, &pn, fd);
		close(fd);
		if (r)
			return sb_errno_to_fs(fsd, r, &pn, FS_PATH_NOT_FOUND);
		r = sb_whiteout(fsd, &po, 1);
		if (r)
			return sb_errno_to_fs(fsd, r, &po, FS_GENERAL);
	}
	if (ln.whiteout)
		sb_whiteout(fsd, &pn, 0);

	/* rw/ handles follow the inode; ro/ ones would copy up the old name */
	for (int i = 0; i < FSD_HANDLES; i++) {
		struct fsd_handle *h = &fsd->h[i];

		if (!h->used || h->is_dir || h->drv != po.drv ||
		    strcmp(h->rel, po.rel))
			continue;
		if (h->layer == LAYER_RO)
			h->stale = 1;
		else
			memcpy(h->rel, pn.rel, sizeof(h->rel));
	}
	fsd_journal(fsd, "%sRENAME %c:/%s -> %c:/%s", ptag(po.drv),
		    po.drv->letter, po.rel, pn.drv->letter, pn.rel);
	return FS_OK;
}

static int op_attributes(struct fsd *fsd, const struct fsd_args *a,
			 uint32_t op)
{
	struct fsd_lookup lk;
	struct fsd_path p;
	int r, attr;

	if (a->n < 1)
		return FS_PARAM;
	r = path_arg(fsd, op, &a->a[0], &p);
	if (r)
		return cx_path_err(r, &a->a[0]);
	r = sb_lookup(fsd, &p, &lk);
	if (r)
		return cx_err(r);
	if (lk.layer == LAYER_NONE)
		return FS_NOT_FOUND;
	/* no archive bit; read-only: a file the modem cannot write */
	attr = lk.is_dir ? FS_ATTR_DIR : 0;
	if (!lk.is_dir && drive_ro(fsd, p.drv))
		attr |= FS_ATTR_RO;
	return attr;
}

static void put_le64(uint8_t *b, uint64_t v)
{
	put_le32(b, (uint32_t)v);
	put_le32(b + 4, (uint32_t)(v >> 32));
}

/* ret and 24 bytes (mtime, atime, ctime in seconds), never opened */
static void op_file_detail(struct fsd *fsd, const struct fsd_args *a,
			   uint32_t op, struct fsd_out *o)
{
	uint8_t blk[24] = { 0 };
	struct fsd_lookup lk;
	struct fsd_path p;
	struct stat st;
	char s[FSD_PATH_CHARS + 1];
	int r;

	if (a->n < 1) {
		r = FS_PARAM;
		goto out;
	}
	path_str(&a->a[0], s, sizeof(s));
	fsd_log(fsd, 1, "GetFileDetail \"%s\"", s);
	r = path_arg(fsd, op, &a->a[0], &p);
	if (r) {
		r = cx_path_err(r, &a->a[0]);
		goto out;
	}
	r = sb_lookup(fsd, &p, &lk);
	if (r) {
		r = cx_err(r);
		goto out;
	}
	if (lk.layer == LAYER_NONE) {
		r = FS_NOT_FOUND;
		goto out;
	}
	r = sb_stat(fsd, &p, lk.layer, &st);
	if (r) {
		r = cx_err(sb_errno_to_fs(fsd, r, &p, FS_NOT_FOUND));
		goto out;
	}
	put_le64(blk + 0x00, (uint64_t)st.st_mtime);
	put_le64(blk + 0x08, (uint64_t)st.st_atime);
	put_le64(blk + 0x10, (uint64_t)st.st_ctime);
out:
	fsd_out_u32(o, (uint32_t)r);
	fsd_out_add(o, blk, sizeof(blk));
}

struct cx {
	uint32_t op;
	uint32_t mask;		/* word 0 */
	uint32_t flags;		/* word 3 */
	int32_t off;
	uint32_t whence;
	uint32_t len;		/* read or write length */
	int open;		/* run the open step */
	int32_t handle;		/* in: the handle when !open; out: the one used */
	const struct fsd_arg *path;
	const uint8_t *data;	/* 0x1024 */
	uint32_t data_len;
	/* results */
	uint32_t done;
	int err;
	uint32_t size;		/* word 4, replaced by the size step */
	size_t got;		/* bytes read */
	int32_t written;	/* bytes written, -1 after a failed write */
};

/*
 * Run the steps in their fixed order; the first failure stops the
 * sequence and closes the handle (also one the modem passed in).
 */
static void cx_run(struct fsd *fsd, struct cx *c, uint8_t *buf, size_t cap)
{
	int is_read = c->op != OP_COMBINED_WRITE;
	struct fsd_handle *h;
	struct fsd_path p;
	int r;

	if (c->open) {
		r = path_arg(fsd, c->op, c->path, &p);
		if (r) {
			r = cx_path_err(r, c->path);
			goto fail_open;
		}
		r = open_handle(fsd, &p, c->flags);
		if (r < 0) {
			r = cx_err(r);
			goto fail_open;
		}
		c->handle = r;
		c->done |= CX_OPEN;
	}
	h = handle_get(fsd, c->handle);

	/* question 23: never write past the data argument */
	if (!is_read && (c->mask & CX_WRITE) && c->len > c->data_len) {
		r = FS_PARAM;
		goto fail;
	}
	if (is_read && (c->mask & CX_SIZE)) {
		uint64_t size = 0;

		r = !h || h->is_dir ? FS_BAD_HANDLE : file_size(h, &size);
		if (r)
			goto fail;
		c->size = (uint32_t)size;	/* low 32 bits */
		c->done |= CX_SIZE;
	}
	if (c->mask & CX_SEEK) {
		r = h ? seek_to(h, c->off, c->whence) : FS_BAD_HANDLE;
		if (r)
			goto fail;
		c->done |= CX_SEEK;
	}
	if (is_read && (c->mask & CX_READ)) {
		r = h ? handle_read(fsd, h, buf, c->len < cap ? c->len : cap,
				    &c->got) : FS_BAD_HANDLE;
		if (r)
			goto fail;
		c->done |= CX_READ;
	}
	if (!is_read && (c->mask & CX_WRITE)) {
		size_t n = 0;

		r = h ? handle_write(fsd, h, c->data, c->len, &n) : FS_BAD_HANDLE;
		if (r) {
			c->written = -1;
			goto fail;
		}
		c->written = (int32_t)n;
		c->done |= CX_WRITE;
	}
	if (c->mask & CX_CLOSE) {
		if (!h) {
			r = FS_BAD_HANDLE;
			goto fail;
		}
		handle_close(fsd, h, 1);
		c->done |= CX_CLOSE;
	} else if (c->op == OP_COMBINED_READ && h) {
		/* 0x1022 never returns its handle: closed anyway */
		handle_close(fsd, h, 1);
	}
	return;
fail:
	c->err = cx_err(r);
	if (h)
		handle_close(fsd, h, 1);
	return;
fail_open:
	c->err = r;
	c->handle = -1;
}

static void cx_status(struct fsd_out *o, const struct cx *c)
{
	uint8_t st[8];

	put_le32(st, c->done);
	put_le32(st + 4, (uint32_t)c->err);
	fsd_out_add(o, st, sizeof(st));
}

/* 0x1022 and 0x1026 */
static void op_combined_read(struct fsd *fsd, unsigned int slot, uint32_t op,
			     const struct fsd_args *a, struct fsd_out *o)
{
	static uint8_t buf[CX_READ_CAP];
	const struct fsd_arg *cb = &a->a[1];
	int ex = op == OP_COMBINED_READ_EX;
	struct cx c;
	char s[FSD_PATH_CHARS + 1];

	memset(&c, 0, sizeof(c));
	c.op = op;
	c.handle = -1;
	if (a->n < 2 || cb->len < (ex ? CX_BLOCK_READ_EX : CX_BLOCK_READ)) {
		bad_args(fsd, slot, op, a);
		c.err = FS_PARAM;
		goto out;
	}
	c.path = &a->a[0];
	c.mask = get_le32(cb->data + 0x00);
	c.flags = get_le32(cb->data + 0x0C);
	c.size = get_le32(cb->data + 0x10);
	c.off = (int32_t)get_le32(cb->data + 0x14);
	c.whence = get_le32(cb->data + 0x18);
	c.len = get_le32(cb->data + 0x20);
	path_str(c.path, s, sizeof(s));
	if (ex) {
		int32_t hin = (int32_t)get_le32(cb->data + 0x28);

		fsd_journal(fsd, "COMBINED op=0x%x (%s) slot=%u path=\"%s\" mask=0x%x flags=0x%x off=%d whence=%u len=%u handle=%d",
			    op, op_name(op), slot, s, c.mask, c.flags, c.off,
			    c.whence, c.len, hin);
		c.open = !!(c.mask & CX_OPEN);
		if (!c.open) {
			if (hin < 0) {
				c.err = FS_BAD_HANDLE;	/* our choice */
				goto out;
			}
			c.handle = hin;
		}
	} else {
		fsd_journal(fsd, "COMBINED op=0x%x (%s) slot=%u path=\"%s\" mask=0x%x flags=0x%x off=%d whence=%u len=%u",
			    op, op_name(op), slot, s, c.mask, c.flags, c.off,
			    c.whence, c.len);
		if (!(c.mask & CX_OPEN)) {
			c.err = FS_GENERAL;
			goto out;
		}
		c.open = 1;
	}
	cx_run(fsd, &c, buf, ex ? CX_READ_EX_CAP : CX_READ_CAP);
	if (ex && c.err)
		c.handle = -1;
out:
	fsd_log(fsd, 1, "%s -> done 0x%x error %d size %u read %zu handle %d",
		op_name(op), c.done, c.err, c.size, c.got, c.handle);
	cx_status(o, &c);
	fsd_out_u32(o, c.size);
	fsd_out_u32(o, (uint32_t)c.got);
	fsd_out_add(o, buf, c.got);
	if (ex)
		fsd_out_u32(o, (uint32_t)c.handle);
}

/* 0x1024 */
static void op_combined_write(struct fsd *fsd, unsigned int slot,
			      const struct fsd_args *a, struct fsd_out *o)
{
	const uint32_t op = OP_COMBINED_WRITE;
	const struct fsd_arg *cb = &a->a[1];
	struct cx c;
	char s[FSD_PATH_CHARS + 1];
	int32_t hin;

	memset(&c, 0, sizeof(c));
	c.op = op;
	c.handle = -1;
	if (a->n < 3 || cb->len < CX_BLOCK_WRITE) {
		bad_args(fsd, slot, op, a);
		c.err = FS_PARAM;
		goto out;
	}
	c.path = &a->a[0];
	c.mask = get_le32(cb->data + 0x00);
	c.flags = get_le32(cb->data + 0x0C);
	c.off = (int32_t)get_le32(cb->data + 0x10);
	c.whence = get_le32(cb->data + 0x14);
	c.len = get_le32(cb->data + 0x1C);
	hin = (int32_t)get_le32(cb->data + 0x20);
	c.data = a->a[2].data;
	c.data_len = a->a[2].len;
	path_str(c.path, s, sizeof(s));
	fsd_journal(fsd, "COMBINED op=0x%x (%s) slot=%u path=\"%s\" mask=0x%x flags=0x%x off=%d whence=%u len=%u handle=%d",
		    op, op_name(op), slot, s, c.mask, c.flags, c.off, c.whence,
		    c.len, hin);
	if (hin >= 0) {
		c.handle = hin;		/* no open, even with bit 0x01 */
	} else if (!(c.mask & CX_OPEN)) {
		c.err = FS_PARAM;
		goto out;
	} else {
		c.open = 1;
	}
	cx_run(fsd, &c, NULL, 0);
out:
	fsd_log(fsd, 1, "%s -> done 0x%x error %d handle %d written %d",
		op_name(op), c.done, c.err, c.handle, c.written);
	cx_status(o, &c);
	fsd_out_u32(o, (uint32_t)c.handle);
	fsd_out_u32(o, (uint32_t)c.written);
}

/* copy src over dst, both in the merged view */
static int move_copy(struct fsd *fsd, const struct fsd_path *ps,
		     const struct fsd_path *pd)
{
	enum fsd_layer layer;
	uint8_t *buf = NULL;
	size_t size, got = 0, done = 0;
	struct stat st;
	int sfd, dfd, r;

	r = open_fd(fsd, ps, FS_O_RDONLY, &sfd, &layer);
	if (r)
		return cx_err(r);
	if (fstat(sfd, &st)) {
		r = FS_GENERAL;
		goto out;
	}
	if (st.st_size > FSD_FILE_MAX) {
		r = cx_err(FS_DISK_FULL);
		goto out;
	}
	size = (size_t)st.st_size;
	buf = malloc(size ? size : 1);
	if (!buf) {
		r = FS_GENERAL;
		goto out;
	}
	while (got < size) {
		ssize_t n = pread(sfd, buf + got, size - got, (off_t)got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	if (got < size) {
		r = MV_SHORT;
		goto out;
	}
	/* created or truncated even without the overwrite flag */
	r = open_fd(fsd, pd, FS_O_CREATE | FS_O_TRUNC, &dfd, &layer);
	if (r) {
		r = cx_err(r);
		goto out;
	}
	while (done < size) {
		ssize_t n = pwrite(dfd, buf + done, size - done, (off_t)done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += (size_t)n;
	}
	close(dfd);
	fsd_journal(fsd, "%sWRITE %c:/%s off=0 len=%zu crc32=%08x (copy of %c:/%s)%s",
		    ptag(pd->drv), pd->drv->letter, pd->rel, done,
		    fsd_crc32(0, buf, done), ps->drv->letter, ps->rel,
		    done < size ? " (short)" : "");
	r = done < size ? MV_SHORT : FS_OK;
out:
	free(buf);
	close(sfd);
	return r;
}

static int op_move(struct fsd *fsd, unsigned int slot,
		   const struct fsd_args *a)
{
	const uint32_t op = OP_MOVE;
	struct fsd_lookup lk;
	struct fsd_path ps, pd;
	char s1[FSD_PATH_CHARS + 1], s2[FSD_PATH_CHARS + 1];
	uint32_t flags;
	int r, interim;

	if (a->n < 3 || arg_u32(a, 2, &flags))
		return bad_args(fsd, slot, op, a);
	path_str(&a->a[0], s1, sizeof(s1));
	path_str(&a->a[1], s2, sizeof(s2));
	fsd_journal(fsd, "MOVE slot=%u src=\"%s\" dst=\"%s\" flags=0x%x", slot,
		    s1, s2, flags);

	/* 1. both paths, then the same name */
	r = path_arg(fsd, op, &a->a[0], &ps);
	if (r)
		return cx_path_err(r, &a->a[0]);
	r = path_arg(fsd, op, &a->a[1], &pd);
	if (r)
		return cx_path_err(r, &a->a[1]);
	if (ps.drv == pd.drv && !strcmp(ps.rel, pd.rel))
		return FS_PARAM;
	/* 2. flags */
	if (!(flags & (MV_COPY | MV_KILL)) ||
	    (flags & ~(uint32_t)(MV_COPY | MV_KILL | MV_OVERWRITE)))
		return FS_PARAM;
	/* 3. kill: a read-only source; a missing one is no error here */
	if ((flags & MV_KILL) && !sb_lookup(fsd, &ps, &lk) &&
	    lk.layer != LAYER_NONE && drive_ro(fsd, ps.drv))
		return FS_READ_ONLY;
	/* 4. overwrite: remove the destination */
	interim = FS_GENERAL;
	if (flags & MV_OVERWRITE) {
		r = sb_lookup(fsd, &pd, &lk);
		if (r && r != FS_PATH_NOT_FOUND)
			return cx_err(r);
		if (!r && lk.layer != LAYER_NONE) {
			if (drive_ro(fsd, pd.drv))
				return FS_READ_ONLY;
			r = delete_entry(fsd, &pd, &lk);
			if (r)
				return cx_err(r);
			interim = FS_OK;
		}
	}
	/* 5. kill has no further effect; 6. without copy: the interim result */
	if (!(flags & MV_COPY))
		return interim;
	/* 7. the copy */
	return move_copy(fsd, &ps, &pd);
}

/*
 * FindFirst / FindNext / FindClose.
 *
 * FindFirst takes a snapshot of the merged directory (question 31): the
 * rw/ names in host order, then the ro/ names that rw/ neither has nor
 * hides with a whiteout, in host order; the server's internal names are
 * never listed. Each entry is examined in the layer it came from when it
 * is returned, so attributes and times are those of the visible copy.
 */
#define FIND_PAT_MAX	255
#define FIND_ENTRIES	4096		/* our limit per directory */
#define FIND_REC	0x34

struct find_ent {
	uint32_t off;			/* into names */
	uint8_t layer;
};

struct namelist {
	struct find_ent *ent;
	uint32_t n, ncap;
	char *names;
	size_t len, cap;
	int full;
};

struct fsd_find {
	struct fsd_drive *drv;
	char dir[FSD_REL_MAX];
	char pat[FIND_PAT_MAX + 1];
	uint8_t req, excl;
	uint32_t pos;
	struct namelist l;
};

static void names_free(struct namelist *l)
{
	free(l->ent);
	free(l->names);
	memset(l, 0, sizeof(*l));
}

static void find_free(struct fsd_find *f)
{
	if (!f)
		return;
	names_free(&f->l);
	free(f);
}

static const char *names_at(const struct namelist *l, uint32_t i)
{
	return l->names + l->ent[i].off;
}

static int names_has(const struct namelist *l, const char *name)
{
	for (uint32_t i = 0; i < l->n; i++)
		if (!strcmp(names_at(l, i), name))
			return 1;
	return 0;
}

static void names_add(struct namelist *l, const char *name, uint8_t layer)
{
	size_t len = strlen(name) + 1;

	if (l->full)
		return;
	if (l->n == FIND_ENTRIES) {
		l->full = 1;
		return;
	}
	if (l->n == l->ncap) {
		uint32_t nc = l->ncap ? l->ncap * 2 : 32;
		struct find_ent *e = realloc(l->ent, nc * sizeof(*e));

		if (!e) {
			l->full = 1;
			return;
		}
		l->ent = e;
		l->ncap = nc;
	}
	if (l->len + len > l->cap) {
		size_t nc = l->cap ? l->cap * 2 : 1024;
		char *b;

		while (nc < l->len + len)
			nc *= 2;
		b = realloc(l->names, nc);
		if (!b) {
			l->full = 1;
			return;
		}
		l->names = b;
		l->cap = nc;
	}
	memcpy(l->names + l->len, name, len);
	l->ent[l->n].off = (uint32_t)l->len;
	l->ent[l->n].layer = layer;
	l->n++;
	l->len += len;
}

struct find_collect {
	struct namelist *l;
	struct namelist *wh;	/* names whited out in rw/ */
	uint32_t rw_n;		/* entries that came from rw/ */
	uint8_t layer;
};

static void find_collect(void *ctx, const char *name)
{
	struct find_collect *c = ctx;
	size_t ip = strlen(FSD_INTERNAL_PREFIX);

	if (!strncmp(name, FSD_WHITEOUT_PREFIX, strlen(FSD_WHITEOUT_PREFIX))) {
		if (c->layer == LAYER_RW)
			names_add(c->wh, name + strlen(FSD_WHITEOUT_PREFIX), 0);
		return;
	}
	if (!strncmp(name, FSD_INTERNAL_PREFIX, ip))
		return;
	if (c->layer == LAYER_RO) {
		struct namelist rw = *c->l;

		rw.n = c->rw_n;	/* only the rw/ names hide ro/ ones */
		if (names_has(&rw, name) || names_has(c->wh, name))
			return;
	}
	names_add(c->l, name, c->layer);
}

/* '?' one character, '*' any run, everything else itself, case-sensitive */
static int glob_match(const char *p, const char *s)
{
	const char *star = NULL, *ss = NULL;

	while (*s) {
		if (*p == '*') {
			star = p++;
			ss = s;
		} else if (*p == '?' || *p == *s) {
			p++;
			s++;
		} else if (star) {
			p = star + 1;
			s = ++ss;
		} else {
			return 0;
		}
	}
	while (*p == '*')
		p++;
	return !*p;
}

static int is_dots(const char *n)
{
	return !strcmp(n, ".") || !strcmp(n, "..");
}

/* stat entry i where it is visible; -1 to skip it */
static int find_stat(struct fsd *fsd, const struct fsd_find *f, uint32_t i,
		     struct stat *st, uint8_t *attr)
{
	const char *name = names_at(&f->l, i);
	struct fsd_path p;
	int r = -1;

	p.drv = f->drv;
	if (is_dots(name)) {
		char parent[FSD_REL_MAX];
		const char *slash = strrchr(f->dir, '/');

		/* "..": the parent; of the drive root, the root itself */
		if (!strcmp(name, ".") || !*f->dir) {
			memcpy(p.rel, f->dir, sizeof(p.rel));
		} else {
			size_t n = slash ? (size_t)(slash - f->dir) : 0;

			memcpy(parent, f->dir, n);
			parent[n] = 0;
			memcpy(p.rel, parent, n + 1);
		}
		if (f->drv->rw_fd >= 0)
			r = sb_stat(fsd, &p, LAYER_RW, st);
		if (r)
			r = sb_stat(fsd, &p, LAYER_RO, st);
	} else {
		int n = snprintf(p.rel, sizeof(p.rel), "%s%s%s", f->dir,
				 *f->dir ? "/" : "", name);

		if (n < 0 || (size_t)n >= sizeof(p.rel))
			return -1;
		r = sb_stat(fsd, &p, (enum fsd_layer)f->l.ent[i].layer, st);
	}
	if (r || (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)))
		return -1;	/* dangling or special: skipped */
	*attr = S_ISDIR(st->st_mode) ? FS_ATTR_DIR :
		drive_ro(fsd, f->drv) ? FS_ATTR_RO : 0;
	return 0;
}

/* the next entry that matches; FS_NO_MORE at the end */
static int find_next(struct fsd *fsd, struct fsd_find *f, int skip_dots,
		     struct stat *st, uint8_t *attr, const char **name)
{
	while (f->pos < f->l.n) {
		uint32_t i = f->pos++;
		const char *n = names_at(&f->l, i);

		if (skip_dots && is_dots(n))
			continue;
		if (!glob_match(f->pat, n))
			continue;
		if (find_stat(fsd, f, i, st, attr))
			continue;
		if ((*attr & f->req) != f->req || (*attr & f->excl))
			continue;
		*name = n;
		return FS_OK;
	}
	return FS_NO_MORE;
}

/*
 * DOS date-time with the factory quirks, from local time: raw
 * seconds in bits 0-4, the day of month again in the month bits.
 */
static uint32_t dos_time(time_t t)
{
	struct tm tm;

	if (!localtime_r(&t, &tm))
		memset(&tm, 0, sizeof(tm));
	return ((uint32_t)tm.tm_sec & 0x1F) | (uint32_t)tm.tm_min << 5 |
	       (uint32_t)tm.tm_hour << 11 | (uint32_t)tm.tm_mday << 16 |
	       ((uint32_t)tm.tm_mday & 0xF) << 21 |
	       ((uint32_t)(tm.tm_year - 80) & 0x7F) << 25;
}

/* ret, the 0x34-byte record, the name; without an entry: zeros, empty */
static void find_reply(struct fsd_out *o, int r, const char *name,
		       uint8_t attr, const struct stat *st, uint32_t maxlen)
{
	uint8_t rec[FIND_REC] = { 0 };
	uint8_t nm[2 * (FSD_NAME_MAX + 16) + 2];
	size_t units = 0, nl = 0;

	fsd_out_u32(o, (uint32_t)r);
	if (name) {
		size_t len = strlen(name);

		rec[0x0B] = attr;
		rec[0x0C] = len < maxlen;
		put_le32(rec + 0x0E, dos_time(st->st_atime));
		put_le32(rec + 0x16, dos_time(st->st_mtime));
		put_le32(rec + 0x1C, (uint32_t)st->st_size);
		if (maxlen) {
			units = len < maxlen - 1 ? len : maxlen - 1;
			if (units > sizeof(nm) / 2 - 1)
				units = sizeof(nm) / 2 - 1;
			for (size_t i = 0; i < units; i++) {
				uint8_t c = (uint8_t)name[i];

				nm[2 * i] = c == '/' ? '\\' : c;
				nm[2 * i + 1] = 0;
			}
			nm[2 * units] = nm[2 * units + 1] = 0;
			nl = 2 * units + 2;
		}
	}
	fsd_out_add(o, rec, sizeof(rec));
	fsd_out_add(o, nm, nl);
}

/*
 * Split "X:\dir\pattern" at its last separator: the directory as a path
 * argument of its own (with a terminator) and the pattern.
 */
static int find_split(const struct fsd_arg *a, uint8_t *dbuf,
		      struct fsd_arg *dir, char *pat)
{
	uint32_t units = 0, sep = 0, np = 0;
	int have_sep = 0;

	if (a->len & 1)
		return FS_BAD_NAME;
	while (units < a->len / 2) {
		unsigned int u = a->data[2 * units] | a->data[2 * units + 1] << 8;

		if (!u)
			break;
		if (u == '\\' || u == '/') {
			sep = units;
			have_sep = 1;
		}
		units++;
	}
	if (units > FSD_PATH_CHARS)
		return FS_PATH_LONG;
	if (!have_sep || sep < 2)
		return FS_PATH_NOT_FOUND;	/* no separator after the drive */
	for (uint32_t i = sep + 1; i < units; i++) {
		unsigned int u = a->data[2 * i] | a->data[2 * i + 1] << 8;

		if (np >= FIND_PAT_MAX)
			return FS_PATH_LONG;
		if (u < 0x20 || u > 0x7e)
			return FS_BAD_NAME;	/* ours: names are ASCII */
		pat[np++] = (char)u;
	}
	pat[np] = 0;
	memcpy(dbuf, a->data, 2 * sep);
	dbuf[2 * sep] = dbuf[2 * sep + 1] = 0;
	dir->data = dbuf;
	dir->len = 2 * sep + 2;
	return FS_OK;
}

static void op_find_first(struct fsd *fsd, unsigned int slot,
			  const struct fsd_args *a, struct fsd_out *o)
{
	const uint32_t op = OP_FIND_FIRST;
	uint8_t dbuf[2 * FSD_PATH_CHARS + 2];
	struct find_collect c;
	struct namelist wh;
	struct fsd_lookup lk;
	struct fsd_find *f = NULL;
	struct fsd_arg dir;
	struct fsd_path p;
	struct fsd_handle *h;
	const char *name = NULL;
	struct stat st;
	uint8_t attr = 0;
	uint32_t maxlen = 0;
	char s[FSD_PATH_CHARS + 1];
	int r, idx;

	memset(&wh, 0, sizeof(wh));
	if (a->n < 4 || a->a[1].len < 1 || a->a[2].len < 1 ||
	    arg_u32(a, 3, &maxlen)) {
		bad_args(fsd, slot, op, a);
		r = FS_PARAM;
		goto out;
	}
	f = calloc(1, sizeof(*f));
	if (!f) {
		r = FS_GENERAL;
		goto out;
	}
	f->req = a->a[1].data[0];
	f->excl = a->a[2].data[0];
	r = find_split(&a->a[0], dbuf, &dir, f->pat);
	if (!r) {
		r = path_arg(fsd, op, &dir, &p);
		if (r)
			r = cx_path_err(r, &dir);
	}
	if (r)
		goto out;
	r = sb_lookup(fsd, &p, &lk);
	if (r) {
		r = cx_err(r);
		goto out;
	}
	if (lk.layer == LAYER_NONE) {
		r = FS_NOT_FOUND;
		goto out;
	}
	if (!lk.is_dir) {
		r = FS_GENERAL;		/* ENOTDIR: "other" */
		goto out;
	}
	f->drv = p.drv;
	memcpy(f->dir, p.rel, sizeof(f->dir));

	c.l = &f->l;
	c.wh = &wh;
	c.rw_n = 0;
	if (lk.layer == LAYER_RW) {
		c.layer = LAYER_RW;
		r = sb_readdir(fsd, &p, LAYER_RW, find_collect, &c);
		if (r) {
			r = cx_err(sb_errno_to_fs(fsd, r, &p, FS_NOT_FOUND));
			goto out;
		}
		c.rw_n = f->l.n;
	}
	if (lk.layer == LAYER_RO || lk.in_ro) {
		c.layer = LAYER_RO;
		r = sb_readdir(fsd, &p, LAYER_RO, find_collect, &c);
		/* under an rw/ directory a missing or plain ro/ entry is fine */
		if (r && lk.layer == LAYER_RO) {
			r = cx_err(sb_errno_to_fs(fsd, r, &p, FS_NOT_FOUND));
			goto out;
		}
	}
	if (f->l.full)
		fsd_log(fsd, 0, "find %c:/%s: more than %u entries, rest ignored",
			p.drv->letter, p.rel, FIND_ENTRIES);

	r = find_next(fsd, f, 1, &st, &attr, &name);
	if (r)
		goto out;	/* -6: no handle, the snapshot is dropped */
	idx = free_handle(fsd);
	if (idx < 0) {
		name = NULL;
		r = FS_NO_HANDLE;
		goto out;
	}
	h = &fsd->h[idx];
	memset(h, 0, sizeof(*h));
	h->used = 1;
	h->fd = -1;
	h->is_dir = 1;
	h->layer = lk.layer;
	h->drv = p.drv;
	memcpy(h->rel, p.rel, sizeof(h->rel));
	h->find = f;
	f = NULL;
	r = idx + 1;
out:
	path_str(&a->a[0], s, sizeof(s));
	fsd_journal(fsd, "FIND slot=%u path=\"%s\" attr=0x%x/0x%x max=%u -> %d%s%s%s",
		    slot, s, a->n > 1 && a->a[1].len ? a->a[1].data[0] : 0,
		    a->n > 2 && a->a[2].len ? a->a[2].data[0] : 0, maxlen, r,
		    name ? " \"" : "", name ? name : "", name ? "\"" : "");
	find_reply(o, r, name, attr, &st, maxlen);
	names_free(&wh);
	find_free(f);
}

static void op_find_next(struct fsd *fsd, unsigned int slot,
			 const struct fsd_args *a, struct fsd_out *o)
{
	struct fsd_handle *h;
	const char *name = NULL;
	struct stat st;
	uint8_t attr = 0;
	uint32_t v, maxlen = 0;
	int r;

	if (a->n < 2 || arg_u32(a, 0, &v) || arg_u32(a, 1, &maxlen)) {
		bad_args(fsd, slot, OP_FIND_NEXT, a);
		r = FS_PARAM;
	} else if (!(h = handle_get(fsd, (int32_t)v)) || !h->find) {
		r = FS_BAD_HANDLE;	/* also a file handle */
	} else {
		r = find_next(fsd, h->find, 0, &st, &attr, &name);
	}
	fsd_log(fsd, 1, "FindNext -> %d%s%s", r, name ? " " : "", name ? name : "");
	find_reply(o, r, name, attr, &st, maxlen);
}

/* FindClose: a search handle or any other handle */
static int op_find_close(struct fsd *fsd, const struct fsd_args *a)
{
	struct fsd_handle *h;
	int r;

	r = handle_arg(fsd, a, &h, 1);
	if (r)
		return r;
	handle_close(fsd, h, 1);
	return FS_OK;
}

/* only the internal system drive (type 0x04) has a letter */
static int op_get_drive(const struct fsd_args *a)
{
	uint32_t type, serial, alt;

	if (arg_u32(a, 0, &type) || arg_u32(a, 1, &serial) ||
	    arg_u32(a, 2, &alt))
		return FS_PARAM;
	if (!(type & 0x1C) || (type & ~0x1Cu))
		return FS_PARAM;
	if (!serial || (type == 0x10 ? serial != 1 : serial > 2))
		return FS_PARAM;
	if (((alt & 1) && alt != 1) || ((alt & 2) && alt != 2))
		return FS_PARAM;
	return type == 0x04 ? 'Z' : FS_PARAM;
}

static void log_unimpl(struct fsd *fsd, unsigned int slot, uint32_t op,
		       const struct fsd_args *a)
{
	char lens[96];

	arg_lens(a, lens, sizeof(lens));
	fsd_journal(fsd, "UNIMPL op=0x%x (%s) slot=%u nargs=%u lens=%s", op,
		    op_name(op), slot, a->n, lens);
}

/* the arguments an implemented op uses; more are ignored */
static uint32_t args_used(uint32_t op)
{
	switch (op) {
	case OP_CLOSE:
	case OP_CREATE_DIR:
	case OP_GET_FILE_SIZE:
	case OP_DELETE:
	case OP_GET_ATTRIBUTES:
	case OP_GET_FILE_DETAIL:
	case OP_FIND_CLOSE:
		return 1;
	case OP_OPEN:
	case OP_READ:
	case OP_RENAME:
	case OP_COMBINED_READ:
	case OP_COMBINED_READ_EX:
	case OP_FIND_NEXT:
		return 2;
	case OP_OPEN_VARIANT:
	case OP_SEEK:
	case OP_WRITE:
	case OP_MOVE:
	case OP_COMBINED_WRITE:
	case OP_GET_DRIVE:
		return 3;
	case OP_FIND_FIRST:
		return 4;
	default:
		return FSD_MAX_ARGS;	/* not checked */
	}
}

void fsd_dispatch(struct fsd *fsd, unsigned int slot, uint32_t op,
		  const struct fsd_args *a, struct fsd_out *o)
{
	uint8_t echo[8] = { 0 };
	int r;

	if (a->n > args_used(op)) {
		char lens[96];

		arg_lens(a, lens, sizeof(lens));
		fsd_journal(fsd, "EXTRAARGS op=0x%x (%s) slot=%u nargs=%u lens=%s (ignored)",
			    op, op_name(op), slot, a->n, lens);
	}
	switch (op) {
	case OP_OPEN:
	case OP_OPEN_VARIANT:
		r = op_open(fsd, a, op);
		if (r == FS_PARAM)
			bad_args(fsd, slot, op, a);
		fsd_out_u32(o, (uint32_t)r);
		if (op == OP_OPEN_VARIANT) {
			/* request argument 2 echoed; missing: zeros */
			if (a->n >= 3)
				memcpy(echo, a->a[2].data,
				       a->a[2].len < 8 ? a->a[2].len : 8);
			fsd_out_add(o, echo, sizeof(echo));
		}
		return;
	case OP_FIND_FIRST:
		op_find_first(fsd, slot, a, o);
		return;
	case OP_FIND_NEXT:
		op_find_next(fsd, slot, a, o);
		return;
	case OP_FIND_CLOSE:
		r = op_find_close(fsd, a);
		goto ret;
	case OP_GET_DRIVE:
		r = op_get_drive(a);
		if (r == FS_PARAM && (a->n < 3 || a->a[0].len < 4 ||
				      a->a[1].len < 4 || a->a[2].len < 4))
			bad_args(fsd, slot, op, a);	/* not for a refused value */
		fsd_out_u32(o, (uint32_t)r);
		return;
	case OP_GET_FILE_DETAIL:
		op_file_detail(fsd, a, op, o);
		if (get_le32(o->buf + 8) == (uint32_t)FS_PARAM)
			bad_args(fsd, slot, op, a);
		return;
	case OP_READ:
		op_read(fsd, a, o);
		break;
	case OP_WRITE:
		op_write(fsd, a, o);
		break;
	case OP_GET_FILE_SIZE:
		op_file_size(fsd, a, o);
		break;
	case OP_SEEK:
		r = op_seek(fsd, a);
		goto ret;
	case OP_CLOSE:
		r = op_close(fsd, a);
		goto ret;
	case OP_CLOSE_ALL:
	case OP_SHUTDOWN:
		fsd_close_all(fsd);
		r = FS_OK;
		goto ret;
	case OP_CREATE_DIR:
		r = op_create_dir(fsd, a, op);
		goto ret;
	case OP_DELETE:
		r = op_delete(fsd, a, op);
		goto ret;
	case OP_RENAME:
		r = op_rename(fsd, a, op);
		goto ret;
	case OP_GET_ATTRIBUTES:
		r = op_attributes(fsd, a, op);
		goto ret;
	case OP_MOVE:
		/* journals its own BADPARAM; -2 is also a flag/name result */
		fsd_out_u32(o, (uint32_t)op_move(fsd, slot, a));
		return;
	case OP_COMBINED_READ:
	case OP_COMBINED_READ_EX:
		op_combined_read(fsd, slot, op, a, o);
		return;
	case OP_COMBINED_WRITE:
		op_combined_write(fsd, slot, a, o);
		return;
	case OP_NOP_15:
	case OP_NOP_19:
	case OP_NOP_1C:
		r = 0;
		goto ret;
	case OP_NOP_16:
		r = 1;
		goto ret;
	case OP_OTP_WRITE:
	case OP_OTP_WRITE + 1:	/* OTP read */
	case OP_OTP_WRITE + 2:	/* OTP query length */
	case OP_OTP_LOCK:
		fsd_journal(fsd, "REFUSED OTP op=0x%x (%s) slot=%u", op,
			    op_name(op), slot);
		r = FS_DENIED;
		goto ret;
	case OP_RESTORE: {
		char s[96] = "";

		if (a->n >= 1)
			path_str(&a->a[0], s, sizeof(s));
		fsd_journal(fsd, "REFUSED Restore slot=%u path=\"%s\"", slot, s);
		r = FS_DENIED;
		goto ret;
	}
	case OP_BIN_REGION:
		fsd_journal(fsd, "REFUSED BinaryRegion slot=%u", slot);
		r = FS_PARAM;
		goto ret;
	default:
		/* RemoveDir, GetFolderSize, Count, GetDiskInfo, XDelete,
		 * GetClusterSize and every unknown op */
		log_unimpl(fsd, slot, op, a);
		r = FS_GENERAL;
		goto ret;
	}
	/* Read, Write, GetFileSize: ret is the first argument */
	if (o->n && get_le32(o->buf + 8) == (uint32_t)FS_PARAM)
		bad_args(fsd, slot, op, a);
	return;
ret:
	if (r == FS_PARAM && op != OP_BIN_REGION)
		bad_args(fsd, slot, op, a);
	fsd_out_u32(o, (uint32_t)r);
}
