/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * t2-modemd: modem control for the Titan 2 (MT6878) on mainline Linux over
 * the WWAN AT port. Radio, SIM, registration, mobile data, voice calls, SMS.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#ifndef T2_MODEMD_H
#define T2_MODEMD_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

struct md;

#include "mipc.h"

/* --- AT engine (at.c) --- */

#define AT_RX_BUF	8192	/* unterminated input kept between reads */
#define AT_LINE_MAX	1024	/* one line from the modem (a PDU is < 400) */
#define AT_CMD_MAX	256	/* one command, without the CR */
#define AT_PDU_MAX	512	/* hex PDU sent after the "> " prompt */
#define AT_QUEUE	32
#define AT_RESP_BUF	8192	/* intermediate lines of one response */

enum at_result {
	AT_PENDING,
	AT_OK,
	AT_ERROR,
	AT_CME,		/* +CME ERROR */
	AT_CMS,		/* +CMS ERROR */
	AT_NO_CARRIER,
	AT_BUSY,
	AT_NO_ANSWER,
	AT_NO_DIALTONE,
	AT_CONNECT,
	AT_TIMEOUT,
	AT_LOST,	/* port lost or daemon stopping: never sent or never answered */
	AT_REFUSED,	/* queue full, bad command */
};

#define AT_F_CALL	0x1	/* ATD/ATA: NO CARRIER, BUSY, ... are final results */
#define AT_F_SILENT	0x2	/* do not log the command text (PIN) */

struct at_resp {
	enum at_result res;
	int err_code;			/* +CME/+CMS ERROR numeric code, -1 if none */
	char err_text[64];		/* +CME/+CMS ERROR verbose text */
	char buf[AT_RESP_BUF];		/* intermediate lines, each ending in '\n' */
	size_t len;
	int nlines;
	int truncated;
};

typedef void (*at_cb)(struct md *m, struct at_resp *r, void *ctx);

struct at_cmd {
	char text[AT_CMD_MAX];
	char pdu[AT_PDU_MAX];		/* empty: no prompt expected */
	unsigned flags;
	int timeout_ms;
	at_cb cb;
	void *ctx;
	struct at_resp *out;
};

struct at {
	int fd;
	char rx[AT_RX_BUF];
	size_t rxlen;
	struct at_cmd q[AT_QUEUE];
	unsigned qhead, qlen;
	int busy;			/* q[qhead] has been written */
	int prompt_sent;
	int64_t deadline;
	int64_t settle_until;		/* after a timeout: wait before the next command */
	struct at_resp cur;
	char urc_cont[16];		/* URC waiting for its payload line (+CMT ...) */
	char urc_first[AT_LINE_MAX];
	unsigned long n_cmds, n_timeouts, n_urcs, n_stray, n_errors;
};

void at_init(struct md *m, int fd);
void at_close(struct md *m);
int at_submit(struct md *m, const char *cmd, const char *pdu, unsigned flags,
	      int timeout_ms, at_cb cb, void *ctx);
/* synchronous: runs the port loop until the command completes */
enum at_result at_cmd(struct md *m, const char *cmd, struct at_resp *out);
enum at_result at_cmd_ex(struct md *m, const char *cmd, const char *pdu,
			 unsigned flags, int timeout_ms, struct at_resp *out);
void at_pump(struct md *m);
void at_read(struct md *m);
void at_check_timeout(struct md *m, int64_t now);
int64_t at_next_deadline(struct md *m);
void at_lost(struct md *m);
const char *at_result_str(enum at_result r);
/* helpers for responses and URC lines */
int at_next_line(const char **p, char *line, size_t cap);
const char *at_find(const struct at_resp *r, const char *prefix);
int at_split(char *s, char **argv, int max);
int at_is_error(enum at_result r);
void at_resp_error(const struct at_resp *r, char *out, size_t cap);

/* --- modem state (modem.c) --- */

enum md_state {
	MD_CLOSED,	/* port not open */
	MD_OPEN,	/* port open, modem not ready or init not done */
	MD_READY,
};

struct reg {
	int stat;	/* +CREG/+CEREG <stat>, -1 unknown */
	int act;	/* access technology, -1 unknown */
	char lac[16];
	char ci[16];
};

void modem_tick(struct md *m, int64_t now);
int64_t modem_next_deadline(struct md *m);
void modem_lost(struct md *m, const char *why);
void modem_refresh_sim(struct md *m);
void modem_refresh_reg(struct md *m);
void modem_refresh_ims(struct md *m);
void modem_refresh_signal(struct md *m);
void modem_urc(struct md *m, const char *line);
int modem_set_radio(struct md *m, int on, const char *mask, char *err, size_t cap);
int modem_select_sim(struct md *m, int sim, char *err, size_t cap);
int modem_enter_pin(struct md *m, const char *pin, char *err, size_t cap);
const char *reg_stat_str(int stat);
const char *cpin_str(const char *code);
const char *act_str(int act);
const char *md_state_str(enum md_state st);
void modem_status(struct md *m, char *out, size_t cap);
void modem_net(struct md *m, char *out, size_t cap);
int modem_scan(struct md *m, char *out, size_t cap);
void modem_shutdown(struct md *m);

/* --- mobile data (data.c) --- */

struct data_state {
	int up;			/* link configured */
	int aid;		/* modem PDN id (cid / aid), -1 none */
	int tid;		/* transIntfId, -1 unknown; linkid = tid mod 100 */
	int linkid;		/* modem data interface index = wwan0-N */
	int mtu;		/* 0 = leave */
	int activated_by_us;	/* deactivate on down */
	int via_mipc;		/* activated by the MIPC backend */
	int remote_down;	/* +EIF ifdown seen for our interface */
	int epdn_err;
	int tpl_idx;		/* next template after @linkup */
	char apn[101];
	char ifname[32];
	char v4[48];
	char v6[48];
	char gw[48];
	char dns[2][48];
	char bind_status[96];
	int last_cgev_cid;	/* +CGEV: ME PDN ACT <aid> during a command */
	int eif_tid;		/* last interface indication (+EIF / INTERNAL_EIF_IND) */
	char eif_v4[48];
	char eif_v6[48];
	int no_route;		/* IMS PDN: link with the address only, no route, no DNS */
};

void data_init(struct md *m);
void data_tick(struct md *m);
int data_up(struct md *m, const char *apn, int cid, char *out, size_t cap);
int data_down(struct md *m, char *out, size_t cap);
void data_status(struct md *m, char *out, size_t cap);
void data_urc(struct md *m, const char *line);
void data_modem_lost(struct md *m);
void data_shutdown(struct md *m);
/* a PDN the modem raised by itself (IMS): link only, own state, no route/DNS */
void data_pdn_clear(struct data_state *d);
int data_pdn_link(struct md *m, struct data_state *d, int tid, const char *v4, const char *v6,
		  int mtu, char *out, size_t cap);
void data_pdn_down(struct md *m, struct data_state *d);
int data_apn_profiles(struct md *m, char *out, size_t cap, int *with_internet);

/* --- IMS / VoLTE (ims.c) --- */

#define IMS_PDN_MAX	4	/* ims, emergency, spare */
#define IMS_EV_MAX	8
#define IMS_SPI_MAX	256	/* SPIs handed out in this MIPC session */

enum ims_pdn_st {
	IMS_PDN_FREE,
	IMS_PDN_LABELLED,	/* IMS_PDN_IND seen, its INTERNAL_EIF_IND not yet */
	IMS_PDN_UP,		/* link configured */
};

struct ims_pdn {
	enum ims_pdn_st st;
	int tid;		/* transIntfId (INTERNAL_EIF_IND / IMS_PDN_IND interface id) */
	int aid;		/* IMS_PDN_IND aid, -1 unknown */
	int type;		/* IMS_PDN_IND type (2 ims), -1 unknown */
	struct data_state d;	/* the link */
};

/* AT fallback: +EIMSPDN "notify" answered from the tick (unverified: never observed) */
struct ims_ev {
	int aid, action;
};

struct ims_state {
	int on;			/* config ims / command ims on|off */
	int setup;		/* start-up sequence: 0 pending, 1 done, -1 failed */
	int64_t next_setup;
	int64_t mipc_wait_since;	/* start-up waiting for the MIPC session, 0 = not */
	int radio_started;		/* radio_on_at_start done for this connection */
	int64_t next_cireg;		/* AT+CIREG? poll (no +CIREGU on the AT port), 0 = off */
	int apn_need_internet;		/* the list went out without the internet profile */
	int64_t apn_retry_at;
	int apn_tries;
	int urc_tid;			/* last +EIF ifup URC: the address of a PDN */
	char urc_v4[48], urc_v6[48];
	char setup_status[192];
	int eims;		/* +EIMS: <n> (stack enabling), -1 unknown */
	int eimcflag;		/* +EIMCFLAG: <n> (stack enabled), -1 unknown */
	int eims_by_us;		/* AT+EIMS=1 was sent by us */
	int ext;		/* +CIREG/+CIREGU <ext_info>, -1 unknown */
	struct ims_pdn pdn[IMS_PDN_MAX];
	struct ims_ev ev[IMS_EV_MAX];
	int nev;
	unsigned gen;		/* bumped when the bookkeeping is reset under a command */
	uint32_t spi[IMS_SPI_MAX];
	int nspi, spi_pos;
	char last_reject[64];
	unsigned long n_eif, n_ind, n_spi, n_at_notify, n_reject, n_oos;
};

void ims_init(struct md *m);
void ims_tick(struct md *m, int64_t now);
int64_t ims_next_deadline(struct md *m);
int ims_urc(struct md *m, const char *line);
int ims_command(struct md *m, int argc, char **argv, char *out, size_t cap);
int ims_apn_command(struct md *m, char *out, size_t cap);
void ims_status(struct md *m, char *out, size_t cap);
void ims_snapshot(struct md *m, char *out, size_t cap);
void ims_set_ext(struct md *m, int ext);
void ims_services_str(int ext, char *out, size_t cap);
void ims_stack_change(struct md *m);
void ims_modem_lost(struct md *m);
void ims_shutdown(struct md *m);

/* --- voice calls (call.c) --- */

#define CALL_MAX	8
#define NUMBER_MAX	48

enum call_stat {
	CALL_ACTIVE = 0,
	CALL_HELD = 1,
	CALL_DIALING = 2,
	CALL_ALERTING = 3,
	CALL_INCOMING = 4,
	CALL_WAITING = 5,
	CALL_ENDED = 6,
};

struct call {
	int id;			/* +CLCC <idx>, 1.. */
	int dir;		/* 0 MO, 1 MT */
	enum call_stat stat;
	int mode;		/* 0 voice */
	int mpty;
	char number[NUMBER_MAX];
};

struct call_state {
	struct call calls[CALL_MAX];
	int ncalls;
	int clcc_pending;
	int clcc_wanted;
	int64_t next_poll;
	int64_t watch_until;	/* keep polling a while after the last event */
	int64_t dial_grace;	/* after ATD/ATA: an empty +CLCC does not stop audio */
	int audio_on;
	int ring_seen;
	char ring_number[NUMBER_MAX];
	int codec;		/* +EVOCD, -1 unknown */
	int last_cause;		/* +ECPI 133 cause, -1 none */
	int espeech;		/* last +ESPEECH first field, -1 none */
	unsigned long n_clcc, n_ecpi, n_eaic;
};

int call_command(struct md *m, int argc, char **argv, char *out, size_t cap);
void call_urc(struct md *m, const char *line);
void call_tick(struct md *m, int64_t now);
int64_t call_next_deadline(struct md *m);
void call_modem_lost(struct md *m);
void call_shutdown(struct md *m);
void call_status(struct md *m, char *out, size_t cap);
int speechd_send(struct md *m, const char *cmd, char *reply, size_t cap);
const char *call_stat_str(enum call_stat st);
void call_init(struct md *m);
void call_stack_init(struct md *m);

/* --- SMS (sms.c) --- */

#define SMS_PARTS_MAX	16
#define SMS_PART_TEXT	(160 * 4 + 1)	/* UTF-8 of one part */
#define SMS_CONCAT_SLOTS 8

struct sms_concat {
	int used;
	char from[NUMBER_MAX];
	int ref, total, have;
	int64_t t;
	char text[SMS_PARTS_MAX][SMS_PART_TEXT];
	unsigned char present[SMS_PARTS_MAX];
};

struct sms_state {
	struct sms_concat concat[SMS_CONCAT_SLOTS];
	int next_ref;
	unsigned long n_rx, n_tx, n_bad;
};

int sms_command(struct md *m, int argc, char **argv, const char *rest,
		char *out, size_t cap);
void sms_urc(struct md *m, const char *line, const char *payload);
void sms_tick(struct md *m, int64_t now);
/* exported for the self-test: decode/encode without a modem */
int sms_decode_pdu(const char *hex, char *from, size_t fromcap, char *text,
		   size_t textcap, int *ref, int *total, int *seq, int *is_submit);
int sms_encode_parts(const char *number, const char *text, int ref,
		     char pdus[][AT_PDU_MAX], int *tpdu_len, int max);
int sms_selftest(void);

/* --- control socket (ctl.c) --- */

#define CTL_MON_MAX	8
#define REPLY_MAX	16384

struct ctl_state {
	int lfd;
	int mon[CTL_MON_MAX];
	int nmon;
};

int ctl_listen(struct md *m, const char *path);
void ctl_accept(struct md *m);
void ctl_close(struct md *m);
void ctl_command(struct md *m, const char *line, char *out, size_t cap);
void md_event(struct md *m, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int client_main(const char *sock, int argc, char **argv);

/* --- vendor (MediaTek) specifics (vendor.c) --- */

int vendor_radio_cmd(struct md *m, int on, const char *mask, char *cmd, size_t cap);
int vendor_stack_cmd(int sim, char *cmd, size_t cap);
int vendor_data_bind(struct md *m, int cid, int linkid, const char *apn,
		     char *status, size_t cap);
int vendor_urc(struct md *m, const char *line);
int vendor_raw_allowed(struct md *m, const char *cmd, char *why, size_t cap);
void vendor_init_cmds(struct md *m);
/* IMS */
struct eimspdn {
	char kind[16];		/* init | notify | reject | other */
	int aid, action, cause;
	char type[16];		/* ims | emergency */
};
int vendor_parse_eimspdn(const char *args, struct eimspdn *e);
void vendor_ims_onoff_cmd(int on, int observed_form, char *cmd, size_t cap);
void vendor_ims_confirm_cmd(int aid, int result, char *cmd, size_t cap);
void vendor_ims_cfg_cmd(const char *cfg, char *cmd, size_t cap);
void vendor_ims_eims_cmd(int on, char *cmd, size_t cap);
/* voice: +ECPI, +EAIC, +ESPEECH, +EVOCD */
struct ecpi {
	int call_id, msg_type, is_ibt, is_tch, dir, call_mode, type, cause;
	char number[NUMBER_MAX];
};
struct eaic {
	int call_id, type, call_mode, seq;
	char number[NUMBER_MAX];
};
int vendor_parse_ecpi(const char *args, struct ecpi *e);
int vendor_parse_eaic(const char *args, struct eaic *e);
int vendor_parse_espeech(const char *args);
void vendor_eaic_cmd(int accept, int call_id, int seq, char *cmd, size_t cap);
void vendor_dial_cmd(const char *number, int clir, int emergency, char *cmd, size_t cap);
void vendor_hangup_cmd(int call_id, char *cmd, size_t cap);
int vendor_ecpi_is_ims(int call_mode);

/* --- configuration and the daemon (main.c) --- */

struct cfg {
	char port[128];
	char state_file[256];
	int assume_ready;
	char sock[108];
	char speechd_sock[108];
	int audio;			/* 0: never talk to t2-speechd */
	int call_rate;
	char speech_on[16];		/* dial | espeech: when call-start is sent */
	char voice_init_cmds[512];	/* per-stack voice init */
	int eaic_auto_accept;		/* +EAIC: 1 accept, 0 reject with cause 21 */
	char radio_mask[8];		/* "" = leave the radio alone at init */
	int sim;			/* stack selected at init: 1 or 2 */
	char ip_cmd[256];
	int route_metric;
	char resolv_file[256];
	int manage_resolv;
	char resolv_target[256];
	char wwan_dev[16];
	char data_backend[8];		/* "at" | "mipc" */
	char mipc_port[128];
	char mipc_client_name[64];
	int mipc_timeout_ms;
	int mipc_traffic_descriptor;
	char data_deact_cmds[256];
	int data_linkid;
	int data_cid;			/* -1 auto */
	char data_apn[101];
	char data_ip_type[16];
	char data_bind_cmds[1024];	/* ';' separated templates */
	int data_mtu;
	int keep_data_on_exit;
	int allow_raw_at;
	char raw_at_deny[512];
	int cmd_timeout_ms;
	int call_timeout_ms;
	int sms_timeout_ms;
	int scan_timeout_ms;
	int signal_poll_ms;
	int call_poll_ms;
	int ready_poll_ms;
	int reopen_ms;
	int settle_ms;
	char sms_ack_cmd[32];
	int sms_delete_read;
	int ims;			/* IMS/VoLTE registration support */
	char ims_cfg[32];		/* AT+EIMSCFG=<volte>,<vilte>,<vowifi>,<viwifi>,<sms>,<eims> */
	char ims_eims[8];		/* AT+EIMS=1: query | always | never */
	int ims_mipc_key;		/* IMS_SET_CONFIG_REQ key/value form at start-up */
	int apn_profiles;		/* APN_ADD_PROFILE_LIST_REQ at start-up */
	char ims_apn[101];		/* APN of the ims profile ("" = left out) */
	char sos_apn[101];		/* APN of the emergency profile */
	char mms_apn[101];		/* APN of the mms profile ("" = left out) */
	int radio_on_at_start;		/* radio on after the start-up on every (re)connect */
	int radio_off_before_setup;	/* radio off first if it is on before the start-up */
	int ims_reg_poll_ms;		/* AT+CIREG? after the IMS PDN, until registered */
	int ims_reg_idle_poll_ms;	/* AT+CIREG? otherwise while IMS is on */
	char init_cmds[1024];		/* ';' separated extra commands after init */
};

struct md {
	struct cfg cfg;
	int verbose;
	int stopping;
	struct at at;
	struct mipc mipc;
	enum md_state state;
	int64_t reopen_at;
	int64_t next_ready_check;
	int64_t next_signal;
	int64_t next_init;
	int init_tries;
	/* modem facts */
	char model[64];
	char revision[64];
	int cfun;			/* -1 unknown */
	char cpin[32];			/* +CPIN code, "" unknown */
	int sim_sel;			/* stack on the port: 1 or 2 */
	struct reg cs, eps, gprs;
	int ims_reg;			/* +CIREG reg_info, -1 unknown */
	int radio_user_off;		/* "radio off" was given in this daemon run */
	int64_t sim_retry_at;		/* +CME ERROR: SIM busy: re-read the SIM then, 0 = no */
	char oper[64];
	int oper_act;
	int rssi, ber;			/* +CSQ, 99 unknown */
	struct data_state data;
	struct call_state call;
	struct sms_state sms;
	struct ims_state ims;
	struct ctl_state ctl;
	int ip_calls;			/* ip commands run (status) */
};

void md_log(struct md *m, int debug, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
int64_t mono_ms(void);
int md_ready(struct md *m);
int run_argv(struct md *m, char *const argv[]);
void str_append(char *out, size_t cap, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
size_t str_copy(char *dst, size_t cap, const char *src);
int parse_int(const char *s, int lo, int hi, int *out);
void str_sanitize(char *s);

#endif
