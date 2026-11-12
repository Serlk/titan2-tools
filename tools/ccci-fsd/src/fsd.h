/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * t2-ccci-fsd: file server for the MT6878 modem.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#ifndef FSD_H
#define FSD_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* frames */
#define FSD_CCCI_HDR		16		/* kernel CCCI header */
#define FSD_HDR_LEN		20		/* CCCI header + op word */
#define FSD_FRAG_MAX		0xD94		/* one read()/write() */
#define FSD_PAYLOAD_MAX		(FSD_FRAG_MAX - FSD_HDR_LEN)	/* 0xD80 */
#define FSD_REQ_MAX		0x40DC		/* header + stream, per slot */
#define FSD_STREAM_MAX		(FSD_REQ_MAX - FSD_HDR_LEN)
#define FSD_SLOTS		5
#define FSD_MAX_ARGS		6
#define FSD_CH_RX		14
#define FSD_CH_TX		15
#define FSD_W0_MORE		0x80000000u
#define FSD_W0_WAKE		0x40000000u
#define FSD_REPLY_OP		0xFFFF0000u

/* return codes */
#define FS_OK			0
#define FS_PARAM		(-2)
#define FS_BAD_NAME		(-3)
#define FS_NO_DRIVE		(-4)
#define FS_NO_HANDLE		(-5)
#define FS_NO_MORE		(-6)	/* FindFirst/FindNext: no more files */
#define FS_NOT_FOUND		(-9)
#define FS_BAD_HANDLE		(-10)
#define FS_BAD_POS		(-15)
#define FS_DENIED		(-16)
#define FS_GENERAL		(-18)
#define FS_PATH_NOT_FOUND	(-19)
#define FS_DISK_FULL		(-22)
#define FS_WRITE_PROT		(-30)
#define FS_EXISTS		(-36)
#define FS_READ_ONLY		(-45)
#define FS_PATH_LONG		(-49)

/* open flags and attributes */
#define FS_O_RDONLY		0x00000100u
#define FS_O_SHARED		0x00000200u
#define FS_O_NODIR		0x00000400u
#define FS_O_DIR		0x00000800u
#define FS_O_CREATE		0x00010000u
#define FS_O_TRUNC		0x00020000u
#define FS_O_NONBLOCK		0x10000000u
#define FS_O_COMMIT		0x01000000u	/* sync on close */

#define FS_ATTR_RO		0x01
#define FS_ATTR_DIR		0x10

/* limits of our own */
#define FSD_PATH_CHARS		260	/* UTF-16 units, drive included */
#define FSD_NAME_MAX		240	/* one component, leaves room for prefixes */
#define FSD_REL_MAX		512
#define FSD_HANDLES		128	/* handles 1..128 */
#define FSD_FILE_MAX		(64u << 20)	/* no NVRAM file is that big */

/* internal names in rw/: refused as path components */
#define FSD_INTERNAL_PREFIX	".t2fsd."
#define FSD_WHITEOUT_PREFIX	".t2fsd.wh."
#define FSD_TMP_PREFIX		".t2fsd.tmp."

struct fsd_arg {
	const uint8_t *data;
	uint32_t len;
};

struct fsd_args {
	uint32_t n;
	struct fsd_arg a[FSD_MAX_ARGS];
};

struct fsd_out {
	uint8_t buf[FSD_STREAM_MAX];
	size_t len;
	uint32_t n;
	int overflow;
};

struct fsd_drive {
	char letter;
	const char *dir;	/* below ro/ and rw/ */
	int writable;
	int protect;		/* X:, Y: */
	int ro_fd;		/* O_PATH dir fd of ro/<dir>, or -1 */
	int rw_fd;		/* O_PATH dir fd of rw/<dir>, or -1 */
};

/* a validated path: drive plus '/'-separated relative path ("" = root) */
struct fsd_path {
	struct fsd_drive *drv;
	char rel[FSD_REL_MAX];
};

enum fsd_layer { LAYER_NONE, LAYER_RW, LAYER_RO };

struct fsd_lookup {
	enum fsd_layer layer;	/* where the merged entry comes from */
	int is_dir;
	int in_ro;		/* the ro layer has an entry of that name */
	int whiteout;		/* rw/ has a whiteout for it */
	off_t size;
};

struct fsd_find;

struct fsd_handle {
	int used;
	int fd;
	enum fsd_layer layer;
	int is_dir;
	int want_write;		/* opened read/write */
	uint32_t flags;		/* open flags, for the sync on close */
	struct fsd_find *find;	/* a search handle (FindFirst); is_dir is set */
	int stale;		/* deleted or replaced under the handle */
	uint64_t pos;
	struct fsd_drive *drv;
	char rel[FSD_REL_MAX];
};

struct fsd_slot {
	int busy;
	int discard;		/* drop until the last fragment */
	uint32_t word0;
	uint32_t op;
	size_t len;
	uint8_t buf[FSD_REQ_MAX];
};

struct fsd;
typedef int (*fsd_send_fn)(struct fsd *fsd, const uint8_t *frag, size_t len);

struct fsd {
	int verbose;
	int refuse_protect;	/* -p */
	int root_fd;
	int journal_fd;
	unsigned int tmp_seq;
	struct fsd_drive *drives;
	struct fsd_handle h[FSD_HANDLES];
	struct fsd_slot slot[FSD_SLOTS];
	fsd_send_fn send;
	void *send_ctx;
	struct fsd_out out;
};

/* util */
static inline uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

void fsd_log(struct fsd *fsd, int debug, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void fsd_journal(struct fsd *fsd, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
uint32_t fsd_crc32(uint32_t crc, const void *buf, size_t len);
int fsd_journal_open(struct fsd *fsd);

/* proto.c */
void fsd_feed(struct fsd *fsd, const uint8_t *pkt, size_t len);
void fsd_reset_slots(struct fsd *fsd);
int fsd_parse_args(const uint8_t *s, size_t len, struct fsd_args *args);
void fsd_out_init(struct fsd_out *o);
void fsd_out_add(struct fsd_out *o, const void *data, size_t len);
void fsd_out_u32(struct fsd_out *o, uint32_t v);
size_t fsd_out_room(const struct fsd_out *o);

/* ops.c */
void fsd_dispatch(struct fsd *fsd, unsigned int slot, uint32_t op,
		  const struct fsd_args *args, struct fsd_out *out);
void fsd_close_all(struct fsd *fsd);

/* sandbox.c */
int sb_init(struct fsd *fsd, const char *root);
void sb_fini(struct fsd *fsd);
int sb_parse_path(struct fsd *fsd, const struct fsd_arg *arg,
		  struct fsd_path *p);
int sb_lookup(struct fsd *fsd, const struct fsd_path *p,
	      struct fsd_lookup *lk);
int sb_open_file(struct fsd *fsd, const struct fsd_path *p,
		 enum fsd_layer layer, int write, int create, int trunc);
int sb_open_dir(struct fsd *fsd, const struct fsd_path *p,
		enum fsd_layer layer);
int sb_copy_up(struct fsd *fsd, const struct fsd_path *p, int src_fd);
int sb_mkdir(struct fsd *fsd, const struct fsd_path *p);
int sb_unlink(struct fsd *fsd, const struct fsd_path *p);
int sb_rmtree(struct fsd *fsd, const struct fsd_path *p);
int sb_readdir(struct fsd *fsd, const struct fsd_path *p, enum fsd_layer layer,
	       void (*cb)(void *ctx, const char *name), void *ctx);
struct stat;
int sb_stat(struct fsd *fsd, const struct fsd_path *p, enum fsd_layer layer,
	    struct stat *st);
int sb_whiteout(struct fsd *fsd, const struct fsd_path *p, int set);
int sb_rename_rw(struct fsd *fsd, const struct fsd_path *from,
		 const struct fsd_path *to);
int sb_errno_to_fs(struct fsd *fsd, int err, const struct fsd_path *p,
		   int not_found);

#endif
