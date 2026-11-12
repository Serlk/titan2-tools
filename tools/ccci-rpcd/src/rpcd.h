/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * t2-ccci-rpcd: user-space RPC server for the MT6878 modem
 *.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#ifndef RPCD_H
#define RPCD_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* fragments */
#define RPCD_CCCI_HDR		16		/* kernel CCCI header */
#define RPCD_HDR_LEN		20		/* CCCI header + op word */
#define RPCD_FRAG_MAX		0xD94		/* one read()/write() */
#define RPCD_PAYLOAD_MAX	(RPCD_FRAG_MAX - RPCD_HDR_LEN)	/* 0xD80 */
/*
 * Per-slot reassembly buffer. The protocol description's slot is 0x814 bytes; ours holds
 * one whole fragment so that a single valid fragment is never cut. The
 * stream limit is checked separately.
 */
#define RPCD_REQ_MAX		RPCD_FRAG_MAX
#define RPCD_STREAM_MAX		0x800		/* request stream limit */
#define RPCD_REPLY_MAX		0x800		/* whole reply frame */
#define RPCD_SLOTS		5
#define RPCD_MAX_ARGS		6
#define RPCD_CH_RX		32
#define RPCD_CH_TX		33
#define RPCD_W0_MORE		0x80000000u
#define RPCD_REPLY_OP		0xFFFF0000u

/* operations served here */
#define OP_QUERY_AP_SYS_PROPERTY	0x400F
#define OP_SAR_TABLE_IDX_QUERY		0x4010
#define OP_AMMS_DRDI_CONTROL		0x4014
#define OP_SAVE_MD_CAPID		0x4015

#define RPC_REJECT		(-2)

/* DRDI */
#define DRDI_REQ_LEN		0xBC
#define DRDI_WINDOW_DEFAULT	0x10000
#define DRDI_LEN_MAX		0x10000		/* per-set len limit, */
#define DRDI_MAX_SETS		15
#define DRDI_VERSION		3
#define DRDI_CMD_INIT		1
#define DRDI_CMD_COPY		2
#define DRDI_SIZE_MAX		0x20000000u
#define DRDI_NAME		"md1drdi"

/* container */
#define IMG_HDR_SIZE		0x200
#define IMG_MAGIC		0x58881688u
#define IMG_MAX_SECTIONS	256

/* Android PROP_VALUE_MAX, NUL included */
#define PROP_VALUE_MAX		92
#define PROP_NAME_MAX		256

struct rpcd_arg {
	const uint8_t *data;
	uint32_t len;
};

struct rpcd_args {
	uint32_t n;
	struct rpcd_arg a[RPCD_MAX_ARGS];
};

/* reply argument stream: num_args, then len/data/zero filler */
struct rpcd_out {
	uint8_t buf[RPCD_REPLY_MAX - RPCD_HDR_LEN];
	size_t len;
	uint32_t n;
	int overflow;
};

struct rpcd_slot {
	int busy;
	int overflow;		/* too long: answer -2 once the last fragment is in */
	uint32_t op;
	size_t len;
	uint8_t buf[RPCD_REQ_MAX];
};

struct rpcd_prop {
	char *name;
	char *value;
};


struct rpcd_drdi {
	int prepared;		/* md1drdi found and window present */
	uint32_t nsets;		/* N from the last accepted INIT */
	int copy_state;		/* -1 not copied/failed, 0 copied, 1 final */
	const char *image;	/* container (or bare md1drdi with raw) */
	int raw;		/* image is the md1drdi data itself */
	uint64_t data_off;	/* md1drdi data offset in the image */
	uint32_t size;		/* md1drdi size */
	uint8_t *window;
	size_t window_size;
};

struct rpcd;
typedef int (*rpcd_send_fn)(struct rpcd *r, const uint8_t *frag, size_t len);

struct rpcd {
	int verbose;
	struct rpcd_slot slot[RPCD_SLOTS];
	rpcd_send_fn send;
	void *send_ctx;
	struct rpcd_out out;
	struct rpcd_drdi drdi;
	struct rpcd_prop *props;
	size_t nprops;
	int32_t capid_status;	/* left over from the last 0x4015 */
};

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

void rpcd_log(struct rpcd *r, int debug, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* proto.c */
void rpcd_feed(struct rpcd *r, const uint8_t *pkt, size_t len);
void rpcd_reset_slots(struct rpcd *r);
int rpcd_parse_args(const uint8_t *s, size_t len, struct rpcd_args *args);
void rpcd_out_init(struct rpcd_out *o);
void rpcd_out_add(struct rpcd_out *o, const void *data, size_t len);
void rpcd_out_u32(struct rpcd_out *o, uint32_t v);

/* ops.c */
void rpcd_dispatch(struct rpcd *r, unsigned int slot, uint32_t op,
		   const struct rpcd_args *args, struct rpcd_out *out);

/* props.c */
int rpcd_props_load(struct rpcd *r, const char *path);
void rpcd_props_free(struct rpcd *r);
const char *rpcd_prop_get(const struct rpcd *r, const char *name);

/* drdi.c */
int drdi_locate(struct rpcd *r);
void drdi_request(struct rpcd *r, const struct rpcd_arg *a0,
		  struct rpcd_out *out);

#endif
