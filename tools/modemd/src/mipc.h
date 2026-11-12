/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * t2-modemd: MIPC transport (the modem's binary request/confirm/indication
 * protocol on /dev/wwan0mipcN). The factory software activates mobile data
 * over it. Written from a clean-room behavioural description of the
 * protocol (no code of the factory firmware was used); items not confirmed
 * on the device are marked (unverified).
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#ifndef T2_MIPC_H
#define T2_MIPC_H

#include <stddef.h>
#include <stdint.h>

struct md;

/* --- header, little-endian --- */

#define MIPC_HDR_SIZE		16
#define MIPC_MAGIC		0x24541984u	/* bytes 84 19 54 24 */
#define MIPC_HDR_MAGIC_OFF	0	/* u32 */
#define MIPC_HDR_RES_OFF	4	/* u32, always 0 */
#define MIPC_HDR_SLOT_OFF	8	/* u8 slot bit mask: 0x01 SIM1, 0x02 SIM2, 0xFF any */
#define MIPC_HDR_UNUSED_OFF	9	/* u8 0 */
#define MIPC_HDR_MSGID_OFF	10	/* u16 */
#define MIPC_HDR_TXID_OFF	12	/* u16 */
#define MIPC_HDR_LEN_OFF	14	/* u16 payload length */
#define MIPC_SLOT_ANY		0xFF
#define MIPC_TXID_NONE		0xFFFF	/* indications and notifications */
#define MIPC_PAYLOAD_MAX	0xFFF0	/* the serializer refuses more */
#define MIPC_MSG_MAX		(MIPC_HDR_SIZE + 8192)	/* what we accept in one message */

/* message classes: bits 15..14 of the id */
#define MIPC_CLASS(id)		(((id) >> 14) & 3)
#define MIPC_CLASS_REQCNF	0
#define MIPC_CLASS_IND		1
#define MIPC_CLASS_CMDRSP	2
#define MIPC_CLASS_NTF		3
#define MIPC_IS_IND(id)		(MIPC_CLASS(id) == MIPC_CLASS_IND)
#define MIPC_IS_CMD(id)		(MIPC_CLASS(id) == MIPC_CLASS_CMDRSP && ((id) & 1))
#define MIPC_CNF_OF(req)	((req) + 1)

/* --- TLV: u16 id (bit 15 = privacy flag), u16 len, value,
 * padded to 8 bytes; len == 0 is a 16-byte array descriptor --- */

#define MIPC_TLV_HDR		4
#define MIPC_TLV_ALIGN		8
#define MIPC_TLV_ID_MASK	0x7FFF
#define MIPC_TLV_DESCR_SIZE	16
#define MIPC_TLV_SIZE(len)	((len) ? ((MIPC_TLV_HDR + (len) + 7) & ~(size_t)7) : MIPC_TLV_DESCR_SIZE)
#define MIPC_T_RESULT		0x0000	/* u32 in every CNF, 0 = success */

/* --- message ids --- */

/* INTERNAL group */
#define MIPC_INTERNAL_OPEN_REQ		0x0301
#define MIPC_INTERNAL_OPEN_CNF		0x0302
#define MIPC_INTERNAL_CLOSE_REQ		0x0303
#define MIPC_INTERNAL_CLOSE_CNF		0x0304
#define MIPC_INTERNAL_TEST_REQ		0x0305
#define MIPC_INTERNAL_TEST_CNF		0x0306
#define MIPC_INTERNAL_REGISTER_IND_REQ	0x0307
#define MIPC_INTERNAL_REGISTER_IND_CNF	0x0308
#define MIPC_INTERNAL_REGISTER_CMD_REQ	0x030B	/* modem->AP commands we answer */
#define MIPC_INTERNAL_REGISTER_CMD_CNF	0x030C
#define MIPC_T_REGISTER_CMD_ID		0x0100	/* u16 command id */
#define MIPC_INTERNAL_EIF_IND		0x4302	/* interface state */
#define MIPC_T_OPEN_VERSION		0x0100	/* u32 = 2 (meaning unverified) */
#define MIPC_T_OPEN_CLIENT_NAME		0x0101	/* string */
#define MIPC_T_OPEN_FLAG		0x0102	/* u8 = 1 */
#define MIPC_T_OPEN_CNF_TIMEOUT		0x0101	/* u32 default request timeout ms */
#define MIPC_T_REGISTER_IND_ID		0x0100	/* u16 indication id */
#define MIPC_OPEN_VERSION		2
#define MIPC_TEST_INTERVAL_MS		100

/* SYS group: AT tunnel */
#define MIPC_SYS_AT_REQ			0x0003
#define MIPC_SYS_AT_CNF			0x0004
#define MIPC_SYS_AT_IND			0x4001
#define MIPC_T_AT_STRING		0x8100

/* DATA group */
#define MIPC_DATA_ACT_CALL_REQ		0x0201
#define MIPC_DATA_ACT_CALL_CNF		0x0202
#define MIPC_DATA_DEACT_CALL_REQ	0x0203
#define MIPC_DATA_DEACT_CALL_CNF	0x0204
#define MIPC_DATA_ACT_CALL_IND		0x4201
#define MIPC_DATA_DEACT_CALL_IND	0x4202

/* DATA_ACT_CALL_REQ TLVs */
#define MIPC_T_ACT_APN			0x0101	/* string */
#define MIPC_T_ACT_APN_TYPE		0x0102	/* u32 mask, 1 = default (inferred) */
#define MIPC_T_ACT_PDP_TYPE		0x0103	/* u8 1 IP, 2 IPV6, 3 IPV4V6 */
#define MIPC_T_ACT_ROAM_PDP_TYPE	0x0104	/* u8 */
#define MIPC_T_ACT_AUTH_TYPE		0x0105	/* u8 (unverified) */
#define MIPC_T_ACT_USER			0x8106	/* string, "" */
#define MIPC_T_ACT_PASSWORD		0x8107	/* string, "" */
#define MIPC_T_ACT_CONST_7F		0x0108	/* u8 0x7F (meaning unverified) */
#define MIPC_T_ACT_BEARER_MASK		0x0109	/* u32 */
#define MIPC_T_ACT_SPECIAL		0x010A	/* u8 0 (meaning unverified) */
#define MIPC_T_ACT_APN_INDEX		0x010C	/* u32, only when in the APN table */
#define MIPC_T_ACT_TRAFFIC_DESCR	0x010D	/* 672 bytes (layout partly unverified) */
#define MIPC_T_ACT_NO_MATCH_ALL		0x010F	/* u32 0 */
#define MIPC_ACT_APN_TYPE_DEFAULT	1
#define MIPC_ACT_CONST_7F		0x7F
#define MIPC_ACT_BEARER_ALL		0x7FFFFFFFu
#define MIPC_TRAFFIC_DESCR_SIZE		672
#define MIPC_TRAFFIC_DESCR_DNN_OFF	4
#define MIPC_TRAFFIC_DESCR_DNN_MAX	99
#define MIPC_PDP_IP			1
#define MIPC_PDP_IPV6			2
#define MIPC_PDP_IPV4V6			3

/* DATA_ACT_CALL_CNF TLVs */
#define MIPC_T_CNF_AID			0x0100	/* u8 */
#define MIPC_T_CNF_APN			0x0101	/* string */
#define MIPC_T_CNF_PDP_TYPE		0x0102	/* u8 */
#define MIPC_T_CNF_V4_COUNT		0x0103	/* u8 */
#define MIPC_T_CNF_V4_ADDRS		0x8104	/* 4 bytes x count */
#define MIPC_T_CNF_V4_DNS_COUNT		0x010D	/* u8 */
#define MIPC_T_CNF_V4_DNS		0x810E	/* 4 bytes x count */
#define MIPC_T_CNF_V6_DNS_COUNT		0x0112	/* u8 */
#define MIPC_T_CNF_V6_DNS		0x8113	/* 16 bytes x count */
#define MIPC_T_CNF_MTU_V4		0x0123	/* u32, 0 = none */
#define MIPC_T_CNF_MTU_V6		0x0124	/* u32 */
#define MIPC_T_CNF_IF_ID		0x0125	/* u32 (direct interface id, absent in capture) */
#define MIPC_T_CNF_PRIMARY_AID		0x0126	/* u8 */
#define MIPC_T_CNF_TRANS_INTF_ID	0x012A	/* u32: link id = value mod 100 */
#define MIPC_T_CNF_RAN			0x012B	/* u8, 1 = cellular */
#define MIPC_T_CNF_EPS_BEARER		0x012C	/* u32 */
#define MIPC_TRANS_INTF_MOD		100

/* INTERNAL_EIF_IND TLVs */
#define MIPC_T_EIF_TRANS_INTF_ID	0x0100	/* u32 */
#define MIPC_T_EIF_CMD			0x0101	/* u8: 0 up, 1 down, 4 ho addr, 5 change, 6 mtu */
#define MIPC_T_EIF_REASON		0x0102	/* u32 */
#define MIPC_T_EIF_MTU			0x0103	/* u32 */
#define MIPC_T_EIF_V4_COUNT		0x0104	/* u8 */
#define MIPC_T_EIF_V4_RECORDS		0x8105	/* 12 bytes x count, address in bytes 0..3 */
#define MIPC_T_EIF_V6_COUNT		0x0106	/* u8 */
#define MIPC_T_EIF_V6_RECORDS		0x8107	/* records, size unverified */
#define MIPC_EIF_RECORD_SIZE		12
#define MIPC_EIF_UP			0
#define MIPC_EIF_DOWN			1
#define MIPC_EIF_HO_ADDR		4
#define MIPC_EIF_CHANGE			5
#define MIPC_EIF_MTU			6

/* APN_ADD_PROFILE_LIST_REQ / CNF: u8 count, an array descriptor 0x8101
 * and one 284-byte record per profile (elements 0xFFFF downwards) */
#define MIPC_APN_ADD_PROFILE_LIST_REQ	0x0113
#define MIPC_APN_ADD_PROFILE_LIST_CNF	0x0114
#define MIPC_T_APNLIST_COUNT		0x0100	/* u8 */
#define MIPC_T_APNLIST_ARRAY		0x8101	/* array descriptor */
#define MIPC_APN_REC_SIZE		284
#define MIPC_APN_REC_ID_OFF		0	/* u32 0xFFFFFFFF: modem assigns (unverified) */
#define MIPC_APN_REC_APN_OFF		12	/* char[100] */
#define MIPC_APN_REC_APN_MAX		100
#define MIPC_APN_REC_USER_OFF		112	/* char[64] (unverified) */
#define MIPC_APN_REC_PASS_OFF		176	/* char[64] (unverified) */
#define MIPC_APN_REC_BEARER_OFF		244	/* u32 */
#define MIPC_APN_REC_TYPE_OFF		248	/* u32, this message's own coding */
#define MIPC_APN_REC_PDP_OFF		252	/* u8 */
#define MIPC_APN_REC_ROAM_PDP_OFF	253	/* u8 */
#define MIPC_APN_REC_AUTH_OFF		254	/* u8 (unverified) */
#define MIPC_APN_REC_ENABLED_OFF	256	/* u32 1 (unverified) */
#define MIPC_APN_REC_BEARER_ALL		0x7FFDFFFFu	/* the captured value */
#define MIPC_APN_REC_TYPE_DEFAULT	0x29	/* default (+dun/supl/fota class bits, unverified) */
#define MIPC_APN_REC_TYPE_IMS		0x02
#define MIPC_APN_REC_TYPE_MMS		0x04
#define MIPC_APN_REC_TYPE_EMERGENCY	0x100
#define MIPC_APN_MAX_PROFILES		4

/* IMS_SET_CONFIG_REQ / CNF (as the factory firmware's RIL sends it): key/value form
 * (0x0100 u32 1, 0x0101 key string with NUL, 0x0102 u8 value) and the
 * feature-switch form (0x0100 u32 0, 0x0102 6 bytes volte, vilte, vowifi,
 * viwifi, sms, eims = the AT+EIMSCFG flags) */
#define MIPC_IMS_SET_CONFIG_REQ		0x0A00
#define MIPC_IMS_SET_CONFIG_CNF		0x0A01
#define MIPC_T_IMSCFG_FORM		0x0100	/* u32: 1 key/value, 0 feature switches */
#define MIPC_T_IMSCFG_KEY		0x0101	/* string (with the terminating NUL) */
#define MIPC_T_IMSCFG_VALUE		0x0102	/* u8 value / 6 bytes of flags */
#define MIPC_IMSCFG_FORM_KEY		1
#define MIPC_IMSCFG_FORM_FLAGS		0
#define MIPC_IMSCFG_FLAGS		6
#define MIPC_IMSCFG_KEY_VOLTE		"mtk_ct_volte_support"	/* 21 bytes with NUL */
#define MIPC_IMSCFG_KEY_VOLTE_VALUE	0x32	/* ASCII '2' (meaning unverified) */

/* IMS_PDN_IND (captured on the phone): short form = bookkeeping
 * reset (0x0100 u8 1, 0x0103 u32 0); full form when the modem brings the IMS
 * PDN up/down by itself (0x8100 20 bytes, 0x0100 u8 0, aid, action, type,
 * interface id, 0x0105 u8 1) */
#define MIPC_IMS_PDN_IND		0x4A03
#define MIPC_T_IMSPDN_IND_TYPE		0x0100	/* u8: 1 short form (reset), 0 full form */
#define MIPC_T_IMSPDN_AID		0x0101	/* u32 */
#define MIPC_T_IMSPDN_ACTION		0x0102	/* u8: 1 up, 0 down */
#define MIPC_T_IMSPDN_TYPE		0x0103	/* u32: 2 = ims */
#define MIPC_T_IMSPDN_IFACE		0x0104	/* u32 interface id = transIntfId (0x65 = 101) */
#define MIPC_IMSPDN_TYPE_IMS		2

/* INTERNAL_EIPSPI_CMD / _RSP: the modem asks for an IPsec SPI */
#define MIPC_INTERNAL_EIPSPI_CMD	0x8305
#define MIPC_INTERNAL_EIPSPI_RSP	0x8306
#define MIPC_T_SPI_INDEX		0x0100	/* u32 request index, echoed */
#define MIPC_T_SPI_ADDR1		0x8102	/* 20 bytes: u32 family/length 4, 16 address bytes (role unverified) */
#define MIPC_T_SPI_ADDR2		0x8103	/* 20 bytes: the IMS PDN's own address */
#define MIPC_T_SPI_PROTO		0x0104	/* u8, 50 = ESP (reading unverified) */
#define MIPC_T_SPI_MIN			0x0106	/* u32 lowest SPI allowed */
#define MIPC_T_SPI_MAX			0x0107	/* u32 highest SPI allowed */
#define MIPC_T_SPI_RSP_INDEX		0x0100	/* u32 = the request index */
#define MIPC_T_SPI_RSP_RESULT		0x0101	/* u8, 0 = success */
#define MIPC_T_SPI_RSP_SPI		0x8102	/* u32 the SPI */
#define MIPC_SPI_DEFAULT_MIN		0x00000100u	/* captured values, used when absent */
#define MIPC_SPI_DEFAULT_MAX		0x0fffffffu

/* DATA_DEACT_CALL_REQ/CNF, indications */
#define MIPC_T_DEACT_AID		0x0101	/* u8 */
#define MIPC_T_DEACT_CAUSE		0x0102	/* u8, values unverified; 1 = normal */
#define MIPC_DEACT_CAUSE_NORMAL		1
#define MIPC_T_IND_AID			0x0100	/* u8 in 0x4201/0x4202 */
#define MIPC_T_IND_CAUSE		0x0102	/* u32 in 0x4202 */

/* --- the API --- */

struct mipc_tlv {
	unsigned id;		/* with bit 15 masked */
	unsigned raw_id;
	const uint8_t *val;
	size_t len;		/* 0 = array descriptor (val points at the 12-byte body) */
};

struct mipc_buf {
	uint8_t *data;
	size_t cap, len;
	int overflow;
};

struct mipc_msg {
	unsigned msg_id;
	unsigned txid;
	unsigned slot;
	const uint8_t *payload;
	size_t len;
};

enum mipc_res {
	MIPC_PENDING,
	MIPC_OK,
	MIPC_TIMEOUT,
	MIPC_LOST,
	MIPC_REFUSED,
};

struct mipc_cnf {
	enum mipc_res res;
	unsigned msg_id;
	unsigned txid;
	uint8_t payload[MIPC_MSG_MAX];
	size_t len;
};

typedef void (*mipc_cb)(struct md *m, struct mipc_cnf *c, void *ctx);

#define MIPC_PENDING_MAX	8

struct mipc_pending {
	int used;
	unsigned txid;
	unsigned req_id;
	int64_t deadline;
	mipc_cb cb;
	void *ctx;
	struct mipc_cnf *out;
};

enum mipc_sess {
	SESS_NONE,	/* port closed */
	SESS_TEST,	/* INTERNAL_TEST_REQ every 100 ms until the CNF */
	SESS_OPEN,	/* INTERNAL_OPEN_REQ sent */
	SESS_REGISTER,	/* INTERNAL_REGISTER_IND_REQ for each indication */
	SESS_READY,
};

struct mipc {
	int fd;
	int enabled;
	uint8_t rx[2 * MIPC_MSG_MAX];
	size_t rxlen;
	struct mipc_pending pend[MIPC_PENDING_MAX];
	unsigned next_txid;
	int64_t reopen_at;
	enum mipc_sess sess;
	int64_t sess_next;
	int sess_tests;
	int reg_index;
	unsigned open_timeout_ms;
	unsigned long n_tx, n_rx, n_ind, n_timeouts, n_bad, n_at_ind;
};

/* bytes, little-endian */
void mipc_put(uint8_t *p, size_t size, uint32_t v);
uint32_t mipc_get(const uint8_t *p, size_t size);

/* TLV builder */
void mipc_buf_init(struct mipc_buf *b, uint8_t *mem, size_t cap);
int mipc_tlv_add(struct mipc_buf *b, unsigned id, const void *val, size_t len);
int mipc_tlv_add_u8(struct mipc_buf *b, unsigned id, uint8_t v);
int mipc_tlv_add_u16(struct mipc_buf *b, unsigned id, uint16_t v);
int mipc_tlv_add_u32(struct mipc_buf *b, unsigned id, uint32_t v);
int mipc_tlv_add_str(struct mipc_buf *b, unsigned id, const char *s);
/* array descriptor (len 0, body u32 0, u32 0, u16 first element id, u16
 * count); the elements are added with mipc_tlv_add() */
int mipc_tlv_add_array(struct mipc_buf *b, unsigned id, unsigned first, unsigned count);
/* element ids from 0x7FFF downwards with bit 15 copied from the descriptor */
#define MIPC_ARRAY_FIRST(descr_id, count) ((((descr_id) & 0x8000u) | 0x7FFFu) - (count) + 1)
/* TLV parser: iterate (*off starts at 0); 1 element, 0 end, -1 malformed */
int mipc_tlv_next(const uint8_t *p, size_t len, size_t *off, struct mipc_tlv *t);
/* lookup by id (bit 15 ignored), not by order */
const uint8_t *mipc_tlv_find(const uint8_t *p, size_t len, unsigned id, size_t *vlen);
int mipc_tlv_find_uint(const uint8_t *p, size_t len, unsigned id, uint32_t *v);
const uint8_t *mipc_tlv_find_rec(const uint8_t *p, size_t len, unsigned id, size_t *vlen);
int mipc_tlv_find_str(const uint8_t *p, size_t len, unsigned id, char *out, size_t cap);
int mipc_tlv_result(const uint8_t *p, size_t len, uint32_t *result);

/* framing: build a message; parse one from a byte stream (1 ok, 0 need
 * more, -1 malformed: the caller drops its buffer) */
size_t mipc_frame_build(uint8_t *out, size_t cap, unsigned msg_id, unsigned txid,
			unsigned slot, const uint8_t *payload, size_t len);
int mipc_frame_parse(const uint8_t *buf, size_t len, struct mipc_msg *msg,
		     size_t *consumed);

/* the transport */
void mipc_init(struct md *m);
void mipc_tick(struct md *m, int64_t now);
int64_t mipc_next_deadline(struct md *m);
void mipc_read(struct md *m);
void mipc_close(struct md *m, const char *why);
int mipc_send(struct md *m, unsigned msg_id, unsigned slot, const uint8_t *payload,
	      size_t len, int timeout_ms, mipc_cb cb, void *ctx, struct mipc_cnf *out);
enum mipc_res mipc_request(struct md *m, unsigned msg_id, unsigned slot,
			   const uint8_t *payload, size_t len, int timeout_ms,
			   struct mipc_cnf *out);
int mipc_ready(struct md *m);
unsigned mipc_slot(struct md *m);
const char *mipc_res_str(enum mipc_res r);
void mipc_status(struct md *m, char *out, size_t cap);
/* indications reach data.c here (and ims.c through it) */
void data_mipc_ind(struct md *m, const struct mipc_msg *msg);
void data_mipc_lost(struct md *m);
/* ...and ims.c: PDNs the modem raised by itself, IMS_PDN_IND, EIPSPI */
void ims_eif_up(struct md *m, unsigned cmd, int tid, const char *v4, const char *v6, int mtu);
void ims_eif_down(struct md *m, int tid);
void ims_eif_urc(struct md *m, int tid, const char *v4, const char *v6);
void ims_mipc_ind(struct md *m, const struct mipc_msg *msg);
void ims_eipspi_cmd(struct md *m, const struct mipc_msg *msg);
void ims_mipc_lost(struct md *m);
int mipc_send_rsp(struct md *m, unsigned msg_id, unsigned txid, unsigned slot,
		  const uint8_t *payload, size_t len);
/* AT tunnel (SYS_AT_REQ): the CNF result (0 = ok) or -1; resp = the text */
int mipc_at(struct md *m, const char *cmd, char *resp, size_t cap);

/* self-test of framing and TLV against the protocol description's worked example */
int mipc_selftest(void);

#endif
