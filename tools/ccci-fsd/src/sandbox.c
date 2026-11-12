// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-fsd: drives, path validation and the ro/rw sandbox.
 *
 * ROOT/ro/<drive dir> holds a read-only copy of the files and is only
 * ever opened O_RDONLY (or O_PATH). ROOT/rw/<drive dir> holds what the
 * modem creates or changes. The merged view: an entry in rw/ wins, a
 * whiteout in rw/ hides the ro/ entry of that name, otherwise ro/.
 *
 * Every lookup is an openat2() below a layer directory with
 * RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
 * RESOLVE_NO_MAGICLINKS; every modifying call (mkdirat, unlinkat,
 * linkat, renameat) works on a parent directory fd obtained that way and
 * a single validated name.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "fsd.h"

#ifndef SYS_openat2
#define SYS_openat2 437
#endif

/* <linux/openat2.h>, not in every libc's kernel headers */
struct fsd_open_how {
	uint64_t flags;
	uint64_t mode;
	uint64_t resolve;
};

#define FSD_RESOLVE_NO_XDEV		0x01
#define FSD_RESOLVE_NO_MAGICLINKS	0x02
#define FSD_RESOLVE_NO_SYMLINKS		0x04
#define FSD_RESOLVE_BENEATH		0x08
#define FSD_RESOLVE (FSD_RESOLVE_NO_XDEV | FSD_RESOLVE_NO_MAGICLINKS | \
		     FSD_RESOLVE_NO_SYMLINKS | FSD_RESOLVE_BENEATH)

static struct fsd_drive drives[] = {
	{ 'Z', "nvdata/md",     1, 0, -1, -1 },
	{ 'V', "nvdata/md_cmn", 1, 0, -1, -1 },
	{ 'X', "protect_f/md",  1, 1, -1, -1 },
	{ 'Y', "protect_s/md",  1, 1, -1, -1 },
	{ 'S', "nvcfg",         1, 0, -1, -1 },
	{ 'U', "mdlpm",         1, 0, -1, -1 },
	{ 'W', "firmware",      0, 0, -1, -1 },
	{ 'R', "etc_md",        0, 0, -1, -1 },
	{ 'T', "mdota",         0, 0, -1, -1 },	/* absent on this phone */
	{ 'Q', "vendor_de_md",  0, 0, -1, -1 },
	{ 0, NULL, 0, 0, -1, -1 },
};

/* returns an fd or -errno */
static int sb_openat2(int dirfd, const char *path, int flags, mode_t mode)
{
	struct fsd_open_how how;
	long r;

	memset(&how, 0, sizeof(how));
	/* openat2() refuses anything beyond O_PATH_FLAGS with O_PATH */
	how.flags = (uint64_t)(flags | O_CLOEXEC |
			       ((flags & O_PATH) ? 0 : O_NOCTTY));
	how.mode = (flags & O_CREAT) ? mode : 0;
	how.resolve = FSD_RESOLVE;
	r = syscall(SYS_openat2, dirfd, *path ? path : ".", &how, sizeof(how));
	return r < 0 ? -errno : (int)r;
}

static int stat_at(int layer_fd, const char *rel, struct stat *st)
{
	int fd = sb_openat2(layer_fd, rel, O_PATH, 0);
	int r = 0;

	if (fd < 0)
		return fd;
	if (fstat(fd, st))
		r = -errno;
	close(fd);
	return r;
}

/* "a/b/c" -> parent "a/b" in buf, returns "c" */
static const char *split_rel(const char *rel, char *parent)
{
	const char *slash = strrchr(rel, '/');

	if (!slash) {
		parent[0] = 0;
		return rel;
	}
	memcpy(parent, rel, slash - rel);
	parent[slash - rel] = 0;
	return slash + 1;
}

static int open_parent(int layer_fd, const char *rel, const char **name)
{
	char parent[FSD_REL_MAX];

	*name = split_rel(rel, parent);
	return sb_openat2(layer_fd, parent, O_PATH | O_DIRECTORY, 0);
}

int sb_errno_to_fs(struct fsd *fsd, int err, const struct fsd_path *p,
		   int not_found)
{
	if (err < 0)
		err = -err;
	switch (err) {
	case 0:
		return FS_OK;
	case ENOENT:
		return not_found;
	case ENOTDIR:
		return FS_PATH_NOT_FOUND;
	case EEXIST:
	case ENOTEMPTY:
		return FS_EXISTS;
	case EMFILE:
	case ENFILE:
		return FS_NO_HANDLE;
	case ENOSPC:
	case EDQUOT:
	case EFBIG:
		return FS_DISK_FULL;
	case EROFS:
		return FS_WRITE_PROT;
	case ENAMETOOLONG:
		return FS_PATH_LONG;
	case EISDIR:
	case EACCES:
	case EPERM:
		return FS_DENIED;
	case ELOOP:
	case EXDEV:
	case EAGAIN:	/* openat2 races; never retried */
		if (p)
			fsd_journal(fsd, "REFUSED escape %c:/%s (%s)",
				    p->drv->letter, p->rel, strerror(err));
		return FS_DENIED;
	case ENOSYS:
		fsd_log(fsd, 0, "openat2() unavailable");
		return FS_GENERAL;
	default:
		fsd_log(fsd, 1, "errno %d (%s) -> general error", err,
			strerror(err));
		return FS_GENERAL;
	}
}

static int bad_char(char c)
{
	return c < 0x20 || c > 0x7e || strchr("<>:\"|?*", c);
}

int sb_parse_path(struct fsd *fsd, const struct fsd_arg *arg,
		  struct fsd_path *p)
{
	char s[FSD_PATH_CHARS + 1];
	size_t n = 0, i, out = 0;
	const char *c;
	char letter;

	if (arg->len & 1)
		return FS_BAD_NAME;
	for (i = 0; i + 1 < arg->len; i += 2) {
		unsigned int u = arg->data[i] | arg->data[i + 1] << 8;

		if (!u)
			break;
		if (n >= FSD_PATH_CHARS)
			return FS_PATH_LONG;
		if (u > 0x7e || u < 0x20)
			return FS_BAD_NAME;
		s[n++] = (char)u;
	}
	s[n] = 0;
	if (n < 2 || s[1] != ':')
		return FS_BAD_NAME;
	letter = s[0];
	if (letter >= 'a' && letter <= 'z')
		letter -= 'a' - 'A';
	if (letter < 'A' || letter > 'Z')
		return FS_BAD_NAME;
	p->drv = NULL;
	for (struct fsd_drive *d = fsd->drives; d->letter; d++)
		if (d->letter == letter)
			p->drv = d;
	if (!p->drv)
		return FS_PATH_NOT_FOUND;	/* never -4 */

	c = s + 2;
	if (*c && *c != '\\' && *c != '/')
		return FS_BAD_NAME;	/* "Z:name", drive relative */
	while (*c == '\\' || *c == '/') {
		const char *start = ++c;
		size_t len;

		while (*c && *c != '\\' && *c != '/')
			c++;
		len = c - start;
		if (!len) {
			if (!*c)
				break;	/* one trailing separator */
			return FS_BAD_NAME;
		}
		if (len > FSD_NAME_MAX)
			return FS_PATH_LONG;
		if ((len == 1 && start[0] == '.') ||
		    (len == 2 && start[0] == '.' && start[1] == '.'))
			return FS_BAD_NAME;
		if (len >= strlen(FSD_INTERNAL_PREFIX) &&
		    !strncmp(start, FSD_INTERNAL_PREFIX,
			     strlen(FSD_INTERNAL_PREFIX)))
			return FS_BAD_NAME;
		for (i = 0; i < len; i++)
			if (bad_char(start[i]))
				return FS_BAD_NAME;
		if (out + len + 2 > sizeof(p->rel))
			return FS_PATH_LONG;
		if (out)
			p->rel[out++] = '/';
		memcpy(p->rel + out, start, len);
		out += len;
	}
	p->rel[out] = 0;
	return FS_OK;
}

static int lookup_fail(struct fsd *fsd, const struct fsd_path *p,
		       const char *what, const char *prefix)
{
	fsd_journal(fsd, "REFUSED %s %c:/%s (at %s)", what, p->drv->letter,
		    p->rel, prefix);
	return FS_DENIED;
}

int sb_lookup(struct fsd *fsd, const struct fsd_path *p,
	      struct fsd_lookup *lk)
{
	struct fsd_drive *d = p->drv;
	char prefix[FSD_REL_MAX];
	int ro_dir = d->ro_fd >= 0;
	const char *c = p->rel;

	memset(lk, 0, sizeof(*lk));
	if (d->ro_fd < 0 && d->rw_fd < 0) {
		/* an absent drive directory (T:, Q:): nothing exists */
		lk->layer = LAYER_NONE;
		return strchr(p->rel, '/') ? FS_PATH_NOT_FOUND : FS_OK;
	}
	lk->layer = d->rw_fd >= 0 ? LAYER_RW : LAYER_RO;
	lk->is_dir = 1;
	lk->in_ro = ro_dir;

	while (*c) {
		struct stat st_rw, st_ro;
		int rw = 0, ro = 0, r;
		const char *end = strchr(c, '/');
		size_t plen;

		if (!end)
			end = c + strlen(c);
		if (lk->layer == LAYER_NONE || !lk->is_dir)
			return FS_PATH_NOT_FOUND;
		plen = end - p->rel;
		memcpy(prefix, p->rel, plen);
		prefix[plen] = 0;

		lk->whiteout = 0;
		if (d->rw_fd >= 0) {
			r = stat_at(d->rw_fd, prefix, &st_rw);
			if (!r)
				rw = 1;
			else if (r != -ENOENT && r != -ENOTDIR)
				return sb_errno_to_fs(fsd, r, p, FS_NOT_FOUND);
			if (!rw) {
				char wh[FSD_REL_MAX + 16], parent[FSD_REL_MAX];
				const char *name = split_rel(prefix, parent);

				snprintf(wh, sizeof(wh), "%s%s" FSD_WHITEOUT_PREFIX "%s",
					 parent, *parent ? "/" : "", name);
				r = stat_at(d->rw_fd, wh, &st_ro);
				if (!r)
					lk->whiteout = 1;
				else if (r != -ENOENT && r != -ENOTDIR)
					return sb_errno_to_fs(fsd, r, p, FS_NOT_FOUND);
			}
		}
		if (ro_dir && !lk->whiteout) {
			r = stat_at(d->ro_fd, prefix, &st_ro);
			if (!r)
				ro = 1;
			else if (r != -ENOENT && r != -ENOTDIR)
				return sb_errno_to_fs(fsd, r, p, FS_NOT_FOUND);
		}
		if (rw && !S_ISREG(st_rw.st_mode) && !S_ISDIR(st_rw.st_mode))
			return lookup_fail(fsd, p, "special file in rw", prefix);
		if (ro && !S_ISREG(st_ro.st_mode) && !S_ISDIR(st_ro.st_mode))
			return lookup_fail(fsd, p, "special file in ro", prefix);

		lk->in_ro = ro;
		ro_dir = ro && S_ISDIR(st_ro.st_mode);
		if (rw) {
			lk->layer = LAYER_RW;
			lk->is_dir = S_ISDIR(st_rw.st_mode);
			lk->size = st_rw.st_size;
		} else if (ro) {
			lk->layer = LAYER_RO;
			lk->is_dir = S_ISDIR(st_ro.st_mode);
			lk->size = st_ro.st_size;
		} else {
			lk->layer = LAYER_NONE;
			lk->is_dir = 0;
			lk->size = 0;
		}
		c = *end ? end + 1 : end;
	}
	return FS_OK;
}

/* create the parent directories of rel in rw/ (they exist in the merged view) */
static int mkparents(struct fsd *fsd, const struct fsd_path *p)
{
	struct fsd_drive *d = p->drv;
	char prefix[FSD_REL_MAX];
	const char *c = p->rel;

	if (d->rw_fd < 0)
		return -EROFS;
	for (;;) {
		const char *end = strchr(c, '/');
		const char *name;
		int pfd, r, fd;

		if (!end)
			return 0;	/* the last component is not a parent */
		memcpy(prefix, p->rel, end - p->rel);
		prefix[end - p->rel] = 0;
		pfd = open_parent(d->rw_fd, prefix, &name);
		if (pfd < 0)
			return pfd;
		r = mkdirat(pfd, name, 0700) ? -errno : 0;
		close(pfd);
		if (!r) {
			fsd_journal(fsd, "%sCOPYUP-DIR %c:/%s",
				    d->protect ? "PROTECT " : "", d->letter, prefix);
		} else if (r != -EEXIST) {
			return r;
		}
		fd = sb_openat2(d->rw_fd, prefix, O_PATH | O_DIRECTORY, 0);
		if (fd < 0)
			return fd;
		close(fd);
		c = end + 1;
	}
}

/*
 * Open a regular file. Without O_CREAT the entry is first opened O_PATH
 * and checked, so no device or FIFO is ever really opened.
 */
static int open_reg(int layer_fd, const char *rel, int flags, mode_t mode)
{
	struct stat st1 = { 0 }, st2;
	int fd, pfd;

	if (!(flags & O_CREAT)) {
		pfd = sb_openat2(layer_fd, rel, O_PATH, 0);
		if (pfd < 0)
			return pfd;
		if (fstat(pfd, &st1)) {
			close(pfd);
			return -errno;
		}
		close(pfd);
		if (!S_ISREG(st1.st_mode))
			return S_ISDIR(st1.st_mode) ? -EISDIR : -EPERM;
	}
	fd = sb_openat2(layer_fd, rel, flags | O_NONBLOCK, mode);
	if (fd < 0)
		return fd;
	if (fstat(fd, &st2) || !S_ISREG(st2.st_mode) ||
	    (!(flags & O_CREAT) &&
	     (st1.st_dev != st2.st_dev || st1.st_ino != st2.st_ino))) {
		close(fd);
		return -EPERM;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	return fd;
}

int sb_open_file(struct fsd *fsd, const struct fsd_path *p,
		 enum fsd_layer layer, int write, int create, int trunc)
{
	struct fsd_drive *d = p->drv;
	int flags, r;

	(void)fsd;
	if (layer == LAYER_RO) {
		/* the ro layer is never opened for writing */
		if (write || create || trunc || d->ro_fd < 0)
			return -EPERM;
		return open_reg(d->ro_fd, p->rel, O_RDONLY, 0);
	}
	if (d->rw_fd < 0)
		return -EROFS;
	if (create || trunc) {
		r = mkparents(fsd, p);
		if (r)
			return r;
	}
	flags = write ? O_RDWR : O_RDONLY;
	if (create)
		flags |= O_CREAT | O_EXCL;
	if (trunc)
		flags |= O_TRUNC;
	return open_reg(d->rw_fd, p->rel, flags, 0600);
}

int sb_open_dir(struct fsd *fsd, const struct fsd_path *p,
		enum fsd_layer layer)
{
	int lfd = layer == LAYER_RO ? p->drv->ro_fd : p->drv->rw_fd;

	(void)fsd;
	if (lfd < 0)
		return -ENOENT;
	return sb_openat2(lfd, p->rel, O_PATH | O_DIRECTORY, 0);
}

/* every name of a layer directory, "." and ".." included, in host order */
int sb_readdir(struct fsd *fsd, const struct fsd_path *p, enum fsd_layer layer,
	       void (*cb)(void *ctx, const char *name), void *ctx)
{
	int lfd = layer == LAYER_RO ? p->drv->ro_fd : p->drv->rw_fd;
	struct dirent *de;
	DIR *dir;
	int fd, r;

	(void)fsd;
	if (lfd < 0)
		return -ENOENT;
	fd = sb_openat2(lfd, p->rel, O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0)
		return fd;
	dir = fdopendir(fd);
	if (!dir) {
		r = -errno;
		close(fd);
		return r;
	}
	while ((de = readdir(dir)))
		cb(ctx, de->d_name);
	closedir(dir);
	return 0;
}

/* stat the entry of one layer (GetFileDetail, find records) */
int sb_stat(struct fsd *fsd, const struct fsd_path *p, enum fsd_layer layer,
	    struct stat *st)
{
	int lfd = layer == LAYER_RO ? p->drv->ro_fd : p->drv->rw_fd;

	(void)fsd;
	if (lfd < 0)
		return -ENOENT;
	return stat_at(lfd, p->rel, st);
}

int sb_copy_up(struct fsd *fsd, const struct fsd_path *p, int src_fd)
{
	struct fsd_drive *d = p->drv;
	char tmp[64], buf[16384];
	uint32_t crc = 0;
	uint64_t off = 0;
	const char *name;
	int r, pfd, tfd;

	r = mkparents(fsd, p);
	if (r)
		return r;
	pfd = open_parent(d->rw_fd, p->rel, &name);
	if (pfd < 0)
		return pfd;
	snprintf(tmp, sizeof(tmp), FSD_TMP_PREFIX "%ld.%u", (long)getpid(),
		 fsd->tmp_seq++);
	tfd = sb_openat2(pfd, tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (tfd < 0) {
		close(pfd);
		return tfd;
	}
	for (;;) {
		ssize_t n = pread(src_fd, buf, sizeof(buf), (off_t)off);
		ssize_t w = 0;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			r = -errno;
			break;
		}
		if (!n)
			break;
		if (off + n > FSD_FILE_MAX) {
			r = -EFBIG;
			break;
		}
		crc = fsd_crc32(crc, buf, n);
		while (w < n) {
			ssize_t k = write(tfd, buf + w, n - w);

			if (k < 0 && errno == EINTR)
				continue;
			if (k < 0) {
				r = -errno;
				break;
			}
			w += k;
		}
		if (r)
			break;
		off += n;
	}
	if (!r && fsync(tfd))
		r = -errno;
	close(tfd);
	if (!r && renameat(pfd, tmp, pfd, name))
		r = -errno;
	if (r)
		unlinkat(pfd, tmp, 0);
	close(pfd);
	if (!r)
		fsd_journal(fsd, "%sCOPYUP %c:/%s size=%llu crc32=%08x",
			    d->protect ? "PROTECT " : "", d->letter, p->rel,
			    (unsigned long long)off, crc);
	return r;
}

int sb_mkdir(struct fsd *fsd, const struct fsd_path *p)
{
	const char *name;
	int r, pfd;

	r = mkparents(fsd, p);
	if (r)
		return r;
	pfd = open_parent(p->drv->rw_fd, p->rel, &name);
	if (pfd < 0)
		return pfd;
	r = mkdirat(pfd, name, 0700) ? -errno : 0;
	close(pfd);
	return r;
}

int sb_unlink(struct fsd *fsd, const struct fsd_path *p)
{
	const char *name;
	int r, pfd;

	(void)fsd;
	if (p->drv->rw_fd < 0)
		return -EROFS;
	pfd = open_parent(p->drv->rw_fd, p->rel, &name);
	if (pfd < 0)
		return pfd;
	r = unlinkat(pfd, name, 0) ? -errno : 0;
	close(pfd);
	return r;
}

/*
 * Remove name below the directory pfd, recursively. Every directory is
 * opened with openat2() below its parent, so a symlink or a mount point
 * in the tree is never followed; unlinkat() removes a symlink itself.
 */
static int rm_at(int pfd, const char *name, int depth)
{
	int r = 0;

	if (!unlinkat(pfd, name, 0))
		return 0;
	if (errno != EISDIR && errno != EPERM)
		return -errno;
	if (depth > 32)
		return -ELOOP;
	for (int pass = 0; pass < 3; pass++) {
		struct dirent *de;
		DIR *dir;
		int dfd = sb_openat2(pfd, name, O_RDONLY | O_DIRECTORY, 0);

		if (dfd < 0)
			return dfd;
		dir = fdopendir(dfd);
		if (!dir) {
			r = -errno;
			close(dfd);
			return r;
		}
		while ((de = readdir(dir))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			r = rm_at(dirfd(dir), de->d_name, depth + 1);
			if (r)
				break;
		}
		closedir(dir);
		if (r)
			return r;
		if (!unlinkat(pfd, name, AT_REMOVEDIR))
			return 0;
		if (errno != ENOTEMPTY)
			return -errno;
	}
	return -ENOTEMPTY;
}

/* remove a directory tree (or a file) in rw/ */
int sb_rmtree(struct fsd *fsd, const struct fsd_path *p)
{
	const char *name;
	int r, pfd;

	(void)fsd;
	if (p->drv->rw_fd < 0)
		return -EROFS;
	if (!*p->rel)
		return -EPERM;	/* never the drive itself */
	pfd = open_parent(p->drv->rw_fd, p->rel, &name);
	if (pfd < 0)
		return pfd;
	r = rm_at(pfd, name, 0);
	close(pfd);
	return r;
}

int sb_whiteout(struct fsd *fsd, const struct fsd_path *p, int set)
{
	char wh[FSD_NAME_MAX + 16];
	const char *name;
	int r = 0, pfd, fd;

	if (p->drv->rw_fd < 0)
		return -EROFS;
	if (set) {
		r = mkparents(fsd, p);
		if (r)
			return r;
	}
	pfd = open_parent(p->drv->rw_fd, p->rel, &name);
	if (pfd < 0)
		return set || pfd != -ENOENT ? pfd : 0;
	snprintf(wh, sizeof(wh), FSD_WHITEOUT_PREFIX "%s", name);
	if (set) {
		fd = sb_openat2(pfd, wh, O_WRONLY | O_CREAT, 0600);
		if (fd < 0)
			r = fd;
		else
			close(fd);
	} else if (unlinkat(pfd, wh, 0) && errno != ENOENT) {
		r = -errno;
	}
	close(pfd);
	return r;
}

/* rename within rw/; never replaces an existing entry (link + unlink) */
int sb_rename_rw(struct fsd *fsd, const struct fsd_path *from,
		 const struct fsd_path *to)
{
	const char *fname, *tname;
	int r, fpfd, tpfd;

	r = mkparents(fsd, to);
	if (r)
		return r;
	fpfd = open_parent(from->drv->rw_fd, from->rel, &fname);
	if (fpfd < 0)
		return fpfd;
	tpfd = open_parent(to->drv->rw_fd, to->rel, &tname);
	if (tpfd < 0) {
		close(fpfd);
		return tpfd;
	}
	r = linkat(fpfd, fname, tpfd, tname, 0) ? -errno : 0;
	if (!r && unlinkat(fpfd, fname, 0)) {
		r = -errno;
		unlinkat(tpfd, tname, 0);
	}
	close(fpfd);
	close(tpfd);
	return r;
}

static int mkdir_beneath(int root_fd, const char *rel)
{
	char prefix[FSD_REL_MAX];
	const char *c = rel;

	for (;;) {
		const char *end = strchr(c, '/');
		const char *name;
		int pfd, r;

		if (!end)
			end = c + strlen(c);
		memcpy(prefix, rel, end - rel);
		prefix[end - rel] = 0;
		pfd = open_parent(root_fd, prefix, &name);
		if (pfd < 0)
			return pfd;
		r = mkdirat(pfd, name, 0700) ? -errno : 0;
		close(pfd);
		if (r && r != -EEXIST)
			return r;
		if (!*end)
			return 0;
		c = end + 1;
	}
}

int sb_init(struct fsd *fsd, const char *root)
{
	char rel[64];
	int fd;

	fsd->drives = drives;
	fsd->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fsd->root_fd < 0) {
		fsd_log(fsd, 0, "%s: %s", root, strerror(errno));
		return -1;
	}
	fd = sb_openat2(fsd->root_fd, ".", O_PATH | O_DIRECTORY, 0);
	if (fd == -ENOSYS) {
		fsd_log(fsd, 0, "the kernel lacks openat2() (Linux 5.6+); refusing to run");
		return -1;
	}
	if (fd < 0) {
		fsd_log(fsd, 0, "openat2 on %s: %s", root, strerror(-fd));
		return -1;
	}
	close(fd);

	for (struct fsd_drive *d = drives; d->letter; d++) {
		snprintf(rel, sizeof(rel), "ro/%s", d->dir);
		d->ro_fd = sb_openat2(fsd->root_fd, rel, O_PATH | O_DIRECTORY, 0);
		if (d->ro_fd < 0) {
			fsd_log(fsd, 1, "%c: no %s (%s)", d->letter, rel,
				strerror(-d->ro_fd));
			d->ro_fd = -1;
		}
		d->rw_fd = -1;
		if (!d->writable)
			continue;
		snprintf(rel, sizeof(rel), "rw/%s", d->dir);
		fd = mkdir_beneath(fsd->root_fd, rel);
		if (!fd)
			fd = sb_openat2(fsd->root_fd, rel, O_PATH | O_DIRECTORY, 0);
		if (fd < 0) {
			fsd_log(fsd, 0, "%c: %s: %s", d->letter, rel, strerror(-fd));
			return -1;
		}
		d->rw_fd = fd;
	}
	return 0;
}

void sb_fini(struct fsd *fsd)
{
	if (!fsd->drives)
		return;
	for (struct fsd_drive *d = fsd->drives; d->letter; d++) {
		if (d->ro_fd >= 0)
			close(d->ro_fd);
		if (d->rw_fd >= 0)
			close(d->rw_fd);
		d->ro_fd = d->rw_fd = -1;
	}
	if (fsd->root_fd >= 0)
		close(fsd->root_fd);
	fsd->root_fd = -1;
}
