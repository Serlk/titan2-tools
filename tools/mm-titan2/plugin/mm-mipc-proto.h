/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIPC: the MediaTek modem's binary request/confirm/indication protocol on
 * the WWAN "MIPC" ports (/dev/wwan0mipcN) of the MT6878 integrated modem.
 * Framing and TLV coding only; no I/O. Layout and ids from a clean-room
 * behavioural description of the protocol (no code of the factory firmware
 * was used); items not confirmed on the device are marked (unverified). Ported to GLib from tools/modemd/src/mipc.c
 * (t2-modemd) of the same project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

#ifndef MM_MIPC_PROTO_H
#define MM_MIPC_PROTO_H

#include <glib.h>

G_BEGIN_DECLS

/* --- header, little-endian --- */

#define MM_MIPC_HDR_SIZE        16
#define MM_MIPC_MAGIC           0x24541984u /* bytes 84 19 54 24 */
#define MM_MIPC_HDR_MAGIC_OFF   0   /* u32 */
#define MM_MIPC_HDR_RES_OFF     4   /* u32, always 0 */
#define MM_MIPC_HDR_SLOT_OFF    8   /* u8 slot bit mask: 0x01 SIM1, 0x02 SIM2, 0xFF any */
#define MM_MIPC_HDR_UNUSED_OFF  9   /* u8 0 */
#define MM_MIPC_HDR_MSGID_OFF   10  /* u16 */
#define MM_MIPC_HDR_TXID_OFF    12  /* u16 */
#define MM_MIPC_HDR_LEN_OFF     14  /* u16 payload length */
#define MM_MIPC_SLOT_ANY        0xFF
#define MM_MIPC_SLOT_SIM1       0x01
#define MM_MIPC_SLOT_SIM2       0x02
#define MM_MIPC_TXID_NONE       0xFFFF /* indications and notifications */
#define MM_MIPC_PAYLOAD_MAX     0xFFF0 /* the serializer refuses more */
#define MM_MIPC_MSG_MAX         (MM_MIPC_HDR_SIZE + 8192) /* what we accept in one message */

/* message classes: bits 15..14 of the id */
#define MM_MIPC_CLASS(id)       (((id) >> 14) & 3)
#define MM_MIPC_CLASS_REQCNF    0
#define MM_MIPC_CLASS_IND       1
#define MM_MIPC_CLASS_CMDRSP    2
#define MM_MIPC_CLASS_NTF       3
#define MM_MIPC_IS_IND(id)      (MM_MIPC_CLASS (id) == MM_MIPC_CLASS_IND)
#define MM_MIPC_IS_CMD(id)      (MM_MIPC_CLASS (id) == MM_MIPC_CLASS_CMDRSP && ((id) & 1))
#define MM_MIPC_CNF_OF(req)     ((req) + 1)

/* --- TLV: u16 id (bit 15 = privacy flag), u16 len, value,
 * padded to 8 bytes; len == 0 is a 16-byte array descriptor --- */

#define MM_MIPC_TLV_HDR         4
#define MM_MIPC_TLV_ALIGN       8
#define MM_MIPC_TLV_ID_MASK     0x7FFF
#define MM_MIPC_TLV_DESCR_SIZE  16
#define MM_MIPC_TLV_SIZE(len)   ((len) ? ((MM_MIPC_TLV_HDR + (len) + 7) & ~(gsize) 7) : MM_MIPC_TLV_DESCR_SIZE)
#define MM_MIPC_T_RESULT        0x0000 /* u32 in every CNF, 0 = success */
#define MM_MIPC_RESULT_MISSING  2      /* a CNF without a result TLV counts as 2 */

/* --- message ids --- */

/* INTERNAL group */
#define MM_MIPC_INTERNAL_OPEN_REQ           0x0301
#define MM_MIPC_INTERNAL_OPEN_CNF           0x0302
#define MM_MIPC_INTERNAL_CLOSE_REQ          0x0303
#define MM_MIPC_INTERNAL_CLOSE_CNF          0x0304
#define MM_MIPC_INTERNAL_TEST_REQ           0x0305
#define MM_MIPC_INTERNAL_TEST_CNF           0x0306
#define MM_MIPC_INTERNAL_REGISTER_IND_REQ   0x0307
#define MM_MIPC_INTERNAL_REGISTER_IND_CNF   0x0308
#define MM_MIPC_INTERNAL_UNREGISTER_IND_REQ 0x0309
#define MM_MIPC_INTERNAL_REGISTER_CMD_REQ 0x030B
#define MM_MIPC_INTERNAL_REGISTER_CMD_CNF 0x030C
#define MM_MIPC_INTERNAL_EIF_IND            0x4302 /* interface state */
#define MM_MIPC_T_OPEN_VERSION              0x0100 /* u32 = 2 (meaning unverified) */
#define MM_MIPC_T_OPEN_CLIENT_NAME          0x0101 /* string */
#define MM_MIPC_T_OPEN_FLAG                 0x0102 /* u8 = 1 */
#define MM_MIPC_T_OPEN_CNF_TIMEOUT          0x0101 /* u32 default request timeout ms */
#define MM_MIPC_T_REGISTER_IND_ID           0x0100 /* u16 indication id */
#define MM_MIPC_OPEN_VERSION                2

/* SYS group: AT tunnel */
#define MM_MIPC_SYS_AT_REQ                  0x0003
#define MM_MIPC_SYS_AT_CNF                  0x0004
#define MM_MIPC_SYS_AT_IND                  0x4001
#define MM_MIPC_T_AT_STRING                 0x8100

/* DATA group */
#define MM_MIPC_DATA_ACT_CALL_REQ           0x0201
#define MM_MIPC_DATA_ACT_CALL_CNF           0x0202
#define MM_MIPC_DATA_DEACT_CALL_REQ         0x0203
#define MM_MIPC_DATA_DEACT_CALL_CNF         0x0204
#define MM_MIPC_DATA_ACT_CALL_IND           0x4201
#define MM_MIPC_DATA_DEACT_CALL_IND         0x4202

/* DATA_ACT_CALL_REQ TLVs */
#define MM_MIPC_T_ACT_APN                   0x0101 /* string */
#define MM_MIPC_T_ACT_APN_TYPE              0x0102 /* u32 mask, 1 = default (inferred) */
#define MM_MIPC_T_ACT_PDP_TYPE              0x0103 /* u8 1 IP, 2 IPV6, 3 IPV4V6 */
#define MM_MIPC_T_ACT_ROAM_PDP_TYPE         0x0104 /* u8 */
#define MM_MIPC_T_ACT_AUTH_TYPE             0x0105 /* u8 (unverified) */
#define MM_MIPC_T_ACT_USER                  0x8106 /* string, "" */
#define MM_MIPC_T_ACT_PASSWORD              0x8107 /* string, "" */
#define MM_MIPC_T_ACT_CONST_7F              0x0108 /* u8 0x7F (meaning unverified) */
#define MM_MIPC_T_ACT_BEARER_MASK           0x0109 /* u32 */
#define MM_MIPC_T_ACT_SPECIAL               0x010A /* u8 0 (meaning unverified) */
#define MM_MIPC_T_ACT_APN_INDEX             0x010C /* u32, only when in the APN table */
#define MM_MIPC_T_ACT_TRAFFIC_DESCR         0x010D /* 672 bytes (layout partly unverified) */
#define MM_MIPC_T_ACT_NO_MATCH_ALL          0x010F /* u32 0 */
#define MM_MIPC_ACT_APN_TYPE_DEFAULT        1
#define MM_MIPC_ACT_APN_TYPE_IMS            0x40
#define MM_MIPC_ACT_CONST_7F                0x7F
#define MM_MIPC_ACT_BEARER_ALL              0x7FFFFFFFu
#define MM_MIPC_TRAFFIC_DESCR_SIZE          672
#define MM_MIPC_TRAFFIC_DESCR_DNN_OFF       4
#define MM_MIPC_TRAFFIC_DESCR_DNN_MAX       99
#define MM_MIPC_PDP_IP                      1
#define MM_MIPC_PDP_IPV6                    2
#define MM_MIPC_PDP_IPV4V6                  3

/* DATA_ACT_CALL_CNF TLVs */
#define MM_MIPC_T_CNF_AID                   0x0100 /* u8 */
#define MM_MIPC_T_CNF_APN                   0x0101 /* string */
#define MM_MIPC_T_CNF_PDP_TYPE              0x0102 /* u8 */
#define MM_MIPC_T_CNF_V4_COUNT              0x0103 /* u8 */
#define MM_MIPC_T_CNF_V4_ADDRS              0x8104 /* 4 bytes x count */
#define MM_MIPC_T_CNF_V4_DNS_COUNT          0x010D /* u8 */
#define MM_MIPC_T_CNF_V4_DNS                0x810E /* 4 bytes x count */
#define MM_MIPC_T_CNF_V6_DNS_COUNT          0x0112 /* u8 */
#define MM_MIPC_T_CNF_V6_DNS                0x8113 /* 16 bytes x count */
#define MM_MIPC_T_CNF_MTU_V4                0x0123 /* u32, 0 = none */
#define MM_MIPC_T_CNF_MTU_V6                0x0124 /* u32 */
#define MM_MIPC_T_CNF_IF_ID                 0x0125 /* u32 (direct interface id, absent in capture) */
#define MM_MIPC_T_CNF_PRIMARY_AID           0x0126 /* u8 */
#define MM_MIPC_T_CNF_TRANS_INTF_ID         0x012A /* u32: link id = value mod 100 */
#define MM_MIPC_T_CNF_RAN                   0x012B /* u8, 1 = cellular */
#define MM_MIPC_T_CNF_EPS_BEARER            0x012C /* u32 */
#define MM_MIPC_TRANS_INTF_MOD              100

/* INTERNAL_EIF_IND TLVs */
#define MM_MIPC_T_EIF_TRANS_INTF_ID         0x0100 /* u32 */
#define MM_MIPC_T_EIF_CMD                   0x0101 /* u8: 0 up, 1 down, 4 ho addr, 5 change, 6 mtu */
#define MM_MIPC_T_EIF_REASON                0x0102 /* u32 */
#define MM_MIPC_T_EIF_MTU                   0x0103 /* u32 */
#define MM_MIPC_T_EIF_V4_COUNT              0x0104 /* u8 */
#define MM_MIPC_T_EIF_V4_RECORDS            0x8105 /* 12 bytes x count, address in bytes 0..3 */
#define MM_MIPC_T_EIF_V6_COUNT              0x0106 /* u8 */
#define MM_MIPC_T_EIF_V6_RECORDS            0x8107 /* records, size unverified */
#define MM_MIPC_EIF_RECORD_SIZE             12
#define MM_MIPC_EIF_UP                      0
#define MM_MIPC_EIF_DOWN                    1
#define MM_MIPC_EIF_HO_ADDR                 4
#define MM_MIPC_EIF_CHANGE                  5
#define MM_MIPC_EIF_MTU                     6

/* DATA_DEACT_CALL_REQ/CNF, indications */
#define MM_MIPC_T_DEACT_AID                 0x0101 /* u8 */
#define MM_MIPC_T_DEACT_CAUSE               0x0102 /* u8, values unverified; 1 = normal */
#define MM_MIPC_DEACT_CAUSE_NORMAL          1
#define MM_MIPC_T_IND_AID                   0x0100 /* u8 in 0x4201/0x4202 */
#define MM_MIPC_T_IND_CAUSE                 0x0102 /* u32 in 0x4202 */

/* APN_ADD_PROFILE_LIST_REQ / CNF: u8 count, an array descriptor 0x8101
 * and one 284-byte record per profile (elements 0xFFFF downwards) */
#define MM_MIPC_APN_ADD_PROFILE_LIST_REQ	0x0113
#define MM_MIPC_APN_ADD_PROFILE_LIST_CNF	0x0114
#define MM_MIPC_T_APNLIST_COUNT		0x0100	/* u8 */
#define MM_MIPC_T_APNLIST_ARRAY		0x8101	/* array descriptor */
#define MM_MIPC_APN_REC_SIZE		284
#define MM_MIPC_APN_REC_ID_OFF		0	/* u32 0xFFFFFFFF: modem assigns (unverified) */
#define MM_MIPC_APN_REC_APN_OFF		12	/* char[100] */
#define MM_MIPC_APN_REC_APN_MAX		100
#define MM_MIPC_APN_REC_USER_OFF		112	/* char[64] (unverified) */
#define MM_MIPC_APN_REC_PASS_OFF		176	/* char[64] (unverified) */
#define MM_MIPC_APN_REC_BEARER_OFF		244	/* u32 */
#define MM_MIPC_APN_REC_TYPE_OFF		248	/* u32, this message's own coding */
#define MM_MIPC_APN_REC_PDP_OFF		252	/* u8 */
#define MM_MIPC_APN_REC_ROAM_PDP_OFF	253	/* u8 */
#define MM_MIPC_APN_REC_AUTH_OFF		254	/* u8 (unverified) */
#define MM_MIPC_APN_REC_ENABLED_OFF	256	/* u32 1 (unverified) */
#define MM_MIPC_APN_REC_BEARER_ALL		0x7FFDFFFFu	/* the captured value */
#define MM_MIPC_APN_REC_TYPE_DEFAULT	0x29	/* default (+dun/supl/fota class bits, unverified) */
#define MM_MIPC_APN_REC_TYPE_IMS		0x02
#define MM_MIPC_APN_REC_TYPE_MMS		0x04
#define MM_MIPC_APN_REC_TYPE_EMERGENCY	0x100
#define MM_MIPC_APN_MAX_PROFILES		4

/* IMS_SET_CONFIG_REQ / CNF (as the factory firmware's RIL sends it): key/value form
 * (0x0100 u32 1, 0x0101 key string with NUL, 0x0102 u8 value) and the
 * feature-switch form (0x0100 u32 0, 0x0102 6 bytes volte, vilte, vowifi,
 * viwifi, sms, eims = the AT+EIMSCFG flags) */
#define MM_MIPC_IMS_SET_CONFIG_REQ		0x0A00
#define MM_MIPC_IMS_SET_CONFIG_CNF		0x0A01
#define MM_MIPC_T_IMSCFG_FORM		0x0100	/* u32: 1 key/value, 0 feature switches */
#define MM_MIPC_T_IMSCFG_KEY		0x0101	/* string (with the terminating NUL) */
#define MM_MIPC_T_IMSCFG_VALUE		0x0102	/* u8 value / 6 bytes of flags */
#define MM_MIPC_IMSCFG_FORM_KEY		1
#define MM_MIPC_IMSCFG_FORM_FLAGS		0
#define MM_MIPC_IMSCFG_FLAGS		6
#define MM_MIPC_IMSCFG_KEY_VOLTE		"mtk_ct_volte_support"	/* 21 bytes with NUL */
#define MM_MIPC_IMSCFG_KEY_VOLTE_VALUE	0x32	/* ASCII '2' (meaning unverified) */

/* IMS_PDN_IND (captured on the phone): short form = bookkeeping
 * reset (0x0100 u8 1, 0x0103 u32 0); full form when the modem brings the IMS
 * PDN up/down by itself (0x8100 20 bytes, 0x0100 u8 0, aid, action, type,
 * interface id, 0x0105 u8 1) */
#define MM_MIPC_IMS_PDN_IND		0x4A03
#define MM_MIPC_T_IMSPDN_IND_TYPE		0x0100	/* u8: 1 short form (reset), 0 full form */
#define MM_MIPC_T_IMSPDN_AID		0x0101	/* u32 */
#define MM_MIPC_T_IMSPDN_ACTION		0x0102	/* u8: 1 up, 0 down */
#define MM_MIPC_T_IMSPDN_TYPE		0x0103	/* u32: 2 = ims */
#define MM_MIPC_T_IMSPDN_IFACE		0x0104	/* u32 interface id = transIntfId (0x65 = 101) */
#define MM_MIPC_IMSPDN_TYPE_IMS		2

/* INTERNAL_EIPSPI_CMD / _RSP: the modem asks for an IPsec SPI */
#define MM_MIPC_INTERNAL_EIPSPI_CMD	0x8305
#define MM_MIPC_INTERNAL_EIPSPI_RSP	0x8306
#define MM_MIPC_T_SPI_INDEX		0x0100	/* u32 request index, echoed */
#define MM_MIPC_T_SPI_ADDR1		0x8102	/* 20 bytes: u32 family/length 4, 16 address bytes (role unverified) */
#define MM_MIPC_T_SPI_ADDR2		0x8103	/* 20 bytes: the IMS PDN's own address */
#define MM_MIPC_T_SPI_PROTO		0x0104	/* u8, 50 = ESP (reading unverified) */
#define MM_MIPC_T_SPI_MIN			0x0106	/* u32 lowest SPI allowed */
#define MM_MIPC_T_SPI_MAX			0x0107	/* u32 highest SPI allowed */
#define MM_MIPC_T_SPI_RSP_INDEX		0x0100	/* u32 = the request index */
#define MM_MIPC_T_SPI_RSP_RESULT		0x0101	/* u8, 0 = success */
#define MM_MIPC_T_SPI_RSP_SPI		0x8102	/* u32 the SPI */
#define MM_MIPC_SPI_DEFAULT_MIN		0x00000100u	/* captured values, used when absent */
#define MM_MIPC_SPI_DEFAULT_MAX		0x0fffffffu


/* slot-addressed call indications and incoming approval. */
#define MM_MIPC_CALL_STATUS_IND 0x4901
#define MM_MIPC_CALL_EVENT_IND 0x4902
#define MM_MIPC_CALL_APPROVE_CMD 0x8901
#define MM_MIPC_CALL_APPROVE_RSP 0x8902

/* SMS group 0x06 (ids and names from the protocol description's id
 * tables). The factory firmware's RIL registers these indications for both slots at boot
 * and the modem accepts every registration with result 0 (CCCI trace
 * 2026-10-28, out/android-capture-dpmaif/ccci-boot-trace.txt, 0x0307/0x0308
 * pairs); 0x4601 arrives once per slot after SIM start-up. The payload of the
 * new-message indications was never captured: the plugin only takes them as
 * "look at the storage now" and reads the message with +CMGL on the AT port.
 * The modem->AP commands SMS_NEW_SMS_CMD 0x8601 / SMS_NEW_STATUS_REPORT_CMD
 * 0x8603 (the factory firmware's RIL registers them too) need an answer whose format is
 * unknown, so they are NOT registered: an unanswered delivery could be lost. */
#define MM_MIPC_SMS_CFG_IND                 0x4601
#define MM_MIPC_SMS_NEW_SMS_IND             0x4602
#define MM_MIPC_SMS_STORE_STATUS_IND        0x4603
#define MM_MIPC_SMS_NEW_STATUS_REPORT_IND   0x4606
#define MM_MIPC_IS_SMS_IND(id)              (((id) & 0xFF00) == 0x4600)

/* Network group 0x04: the RAT mode. The factory firmware's RIL
 * sends NW_SET_RAT_REQ per slot at start-up with rat 22 and prefer_rat 128
 * (factory-firmware logcat "handleSetPreferredNetworkTypeRequest: rat = 22, prefer_rat =
 * 128", MIPC trace 2026-10-27, out/android-capture-calls); the CNF carries
 * only the result and the modem then sends NW_CURRENT_RAT_IND. */
#define MM_MIPC_NW_SET_RAT_REQ              0x0427
#define MM_MIPC_NW_SET_RAT_CNF              0x0428
#define MM_MIPC_NW_CURRENT_RAT_IND          0x441D
#define MM_MIPC_T_SET_RAT_RAT               0x0100 /* u8 rat mode */
#define MM_MIPC_T_SET_RAT_PREFER            0x0101 /* u8 prefer_rat */
#define MM_MIPC_T_CURRENT_RAT_UNKNOWN       0x0100 /* u32 0x0F seen (meaning unverified) */
#define MM_MIPC_T_CURRENT_RAT_RAT           0x0101 /* u8, = the rat sent */
#define MM_MIPC_T_CURRENT_RAT_PREFER        0x0102 /* u8, = the prefer_rat sent */

/* --- the API --- */

/* a parsed message: payload points into the caller's buffer */
typedef struct {
    guint16       msg_id;
    guint16       txid;
    guint8        slot;
    const guint8 *payload;
    gsize         len;
} MMMipcMsg;

typedef struct {
    guint16       id;     /* with bit 15 masked */
    guint16       raw_id;
    const guint8 *val;
    gsize         len;    /* 0 = array descriptor (val points at the 12-byte body) */
} MMMipcTlv;

/* little-endian bytes */
void    mm_mipc_put (guint8 *p, gsize size, guint32 v);
guint32 mm_mipc_get (const guint8 *p, gsize size);

/* TLV builder: appends to a GByteArray that will become the payload.
 * Returns FALSE (and appends nothing) when the TLV cannot be encoded. */
gboolean mm_mipc_tlv_add     (GByteArray *b, guint16 id, gconstpointer val, gsize len);
gboolean mm_mipc_tlv_add_u8  (GByteArray *b, guint16 id, guint8 v);
gboolean mm_mipc_tlv_add_u16 (GByteArray *b, guint16 id, guint16 v);
gboolean mm_mipc_tlv_add_u32 (GByteArray *b, guint16 id, guint32 v);
/* string with its terminating NUL (len includes it) */
gboolean mm_mipc_tlv_add_str (GByteArray *b, guint16 id, const gchar *s);

/* TLV parser: iterate (*off starts at 0); 1 element, 0 end, -1 malformed */
gint          mm_mipc_tlv_next      (const guint8 *p, gsize len, gsize *off, MMMipcTlv *t);
/* lookup by id (bit 15 ignored), not by order; array descriptors never match */
const guint8 *mm_mipc_tlv_find      (const guint8 *p, gsize len, guint16 id, gsize *vlen);
gboolean      mm_mipc_tlv_find_uint (const guint8 *p, gsize len, guint16 id, guint32 *v);
/* newly allocated, control characters replaced, NULL when absent */
gchar        *mm_mipc_tlv_find_str  (const guint8 *p, gsize len, guint16 id);
/* AT text preserves CR/LF/TAB for parsing; other control bytes are replaced. */
gchar        *mm_mipc_tlv_find_at_string (const guint8 *p, gsize len);
/* result TLV 0x0000; MM_MIPC_RESULT_MISSING when absent */
guint32       mm_mipc_tlv_result    (const guint8 *p, gsize len);

/* framing: build a whole message (NULL when the payload is too long); parse
 * one from a byte stream (1 ok, 0 need more bytes, -1 malformed: the caller
 * drops its buffer) */
GByteArray *mm_mipc_frame_build (guint16       msg_id,
                                 guint16       txid,
                                 guint8        slot,
                                 const guint8 *payload,
                                 gsize         len);
gint        mm_mipc_frame_parse (const guint8 *buf,
                                 gsize         len,
                                 MMMipcMsg    *msg,
                                 gsize        *consumed);

gboolean mm_mipc_request_is_valid (guint16 id);
const guint8 *mm_mipc_tlv_find_record (const guint8 *p, gsize len, guint16 id, gsize *vlen);
GByteArray *mm_mipc_build_ims_config (gboolean key_form, const guint8 flags[6]);
GByteArray *mm_mipc_build_apn_profiles (const gchar *internet, const gchar *ims, const gchar *sos);
/* NW_SET_RAT_REQ payload: TLV 0x0100 u8 rat, 0x0101 u8 prefer_rat */
GByteArray *mm_mipc_build_set_rat (guint8 rat, guint8 prefer);

/* names of the ids this plugin knows, for logs ("0x1234" otherwise) */
const gchar *mm_mipc_msg_id_get_string (guint16 msg_id);

G_END_DECLS

#endif /* MM_MIPC_PROTO_H */
