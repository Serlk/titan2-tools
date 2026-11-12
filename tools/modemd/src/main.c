// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-modemd: command line, configuration file, logging, the poll() loop
 * (AT port, MIPC port, control socket) and the client mode.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "modemd.h"

#define DEF_PORT	"/dev/wwan0at0"
#define DEF_MIPC	"/dev/wwan0mipc1"
#define DEF_STATE	"/sys/kernel/debug/d180000.mddriver/fsm"
#define DEF_SOCK	"/run/t2-modemd.sock"
#define DEF_SPEECHD	"/run/t2-speechd.sock"
#define DEF_CONF	"/etc/t2-modemd.conf"
#define DEF_RESOLV	"/run/t2-modemd/resolv.conf"
#define DEF_DENY	"+CLCK;+CPWD;+CSIM;+CRSM;+CPIN;+CCHO;+CCHC;+CGLA;+CRLA;+CFUN=1,1"
/* the MediaTek AT activation; @linkup = create the link here */
#define DEF_BIND	"AT+EAPNACT=1,\"{apn}\",\"default\",0;" \
			"AT+EPDN={aid},\"ifst\",20;" \
			"AT+CGCONTRDP={aid};" \
			"AT+EPDN={aid},\"addr\";" \
			"@linkup;" \
			"AT+EIF={tid}, \"ifst\", \"up\", 1"
#define DEF_DEACT	"AT+EAPNACT=0,{aid},1"
/* once per stack */
#define DEF_VOICE_INIT	"AT+CMOD=0;ATS0=0;AT+CCWA=1;AT+CLIP=1;AT+CSSN=1,1;AT+CMUT=0;" \
			"AT+ECPI=4294967295;AT+EAIC=2;AT+EVOCD=1"

static volatile sig_atomic_t stop_sig;
static struct md md_state;

/* --- helpers --- */

void md_log(struct md *m, int debug, const char *fmt, ...)
{
	va_list ap;

	if (debug && !m->verbose)
		return;
	va_start(ap, fmt);
	fputs("t2-modemd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

size_t str_copy(char *dst, size_t cap, const char *src)
{
	size_t l = strlen(src);

	if (!cap)
		return l;
	if (l >= cap)
		l = cap - 1;
	memcpy(dst, src, l);
	dst[l] = 0;
	return l;
}

void str_append(char *out, size_t cap, const char *fmt, ...)
{
	size_t l = strlen(out);
	va_list ap;

	if (l + 1 >= cap)
		return;
	va_start(ap, fmt);
	vsnprintf(out + l, cap - l, fmt, ap);
	va_end(ap);
}

int parse_int(const char *s, int lo, int hi, int *out)
{
	char *end;
	long v;

	if (!s || !*s)
		return -1;
	while (*s == ' ')
		s++;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || (*end && *end != ' ' && *end != '\n' && *end != ','))
		return -1;
	if (v < lo || v > hi)
		return -1;
	*out = (int)v;
	return 0;
}

/* modem lines are untrusted: control characters become '?' */
void str_sanitize(char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x20 || *s == 0x7f)
			*s = '?';
}

/* fork/exec without a shell; returns the exit status, -1 on failure */
int run_argv(struct md *m, char *const argv[])
{
	pid_t pid;
	int st;

	if (m->verbose) {
		char line[512] = "";

		for (int i = 0; argv[i]; i++)
			str_append(line, sizeof(line), "%s%s", i ? " " : "", argv[i]);
		md_log(m, 1, "run: %s", line);
	}
	m->ip_calls++;
	pid = fork();
	if (pid < 0) {
		md_log(m, 0, "fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, 0);
			if (!m->verbose) {
				dup2(fd, 1);
				dup2(fd, 2);
			}
			if (fd > 2)
				close(fd);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	if (WIFEXITED(st)) {
		if (WEXITSTATUS(st) == 127)
			md_log(m, 0, "%s: cannot execute", argv[0]);
		return WEXITSTATUS(st);
	}
	return -1;
}

/* --- configuration --- */

static void cfg_defaults(struct cfg *c)
{
	memset(c, 0, sizeof(*c));
	str_copy(c->port, sizeof(c->port), DEF_PORT);
	str_copy(c->state_file, sizeof(c->state_file), DEF_STATE);
	str_copy(c->sock, sizeof(c->sock), DEF_SOCK);
	str_copy(c->speechd_sock, sizeof(c->speechd_sock), DEF_SPEECHD);
	c->audio = 1;
	c->call_rate = 48000;
	str_copy(c->speech_on, sizeof(c->speech_on), "dial");
	str_copy(c->voice_init_cmds, sizeof(c->voice_init_cmds), DEF_VOICE_INIT);
	c->eaic_auto_accept = 1;
	c->sim = 1;
	str_copy(c->ip_cmd, sizeof(c->ip_cmd), "/sbin/ip");
	c->route_metric = 600;
	str_copy(c->resolv_file, sizeof(c->resolv_file), DEF_RESOLV);
	str_copy(c->resolv_target, sizeof(c->resolv_target), "/etc/resolv.conf");
	str_copy(c->wwan_dev, sizeof(c->wwan_dev), "wwan0");
	str_copy(c->data_backend, sizeof(c->data_backend), "mipc");
	str_copy(c->mipc_port, sizeof(c->mipc_port), DEF_MIPC);
	str_copy(c->mipc_client_name, sizeof(c->mipc_client_name), DEF_MIPC);
	c->mipc_timeout_ms = 30000;
	str_copy(c->data_bind_cmds, sizeof(c->data_bind_cmds), DEF_BIND);
	str_copy(c->data_deact_cmds, sizeof(c->data_deact_cmds), DEF_DEACT);
	c->data_linkid = 0;
	c->data_cid = -1;
	str_copy(c->data_ip_type, sizeof(c->data_ip_type), "IP");
	c->data_mtu = 1500;
	c->allow_raw_at = 1;
	str_copy(c->raw_at_deny, sizeof(c->raw_at_deny), DEF_DENY);
	c->cmd_timeout_ms = 5000;
	c->call_timeout_ms = 30000;
	c->sms_timeout_ms = 60000;
	c->scan_timeout_ms = 120000;
	c->signal_poll_ms = 10000;
	c->call_poll_ms = 1000;
	c->ready_poll_ms = 500;
	c->reopen_ms = 1000;
	c->settle_ms = 200;
	str_copy(c->sms_ack_cmd, sizeof(c->sms_ack_cmd), "AT+CNMA");
	c->ims = 1;
	str_copy(c->ims_cfg, sizeof(c->ims_cfg), "1,0,0,0,1,1");
	str_copy(c->ims_eims, sizeof(c->ims_eims), "query");
	c->ims_mipc_key = 1;
	c->ims_reg_poll_ms = 5000;
	c->ims_reg_idle_poll_ms = 60000;
	c->apn_profiles = 1;
	str_copy(c->ims_apn, sizeof(c->ims_apn), "ims");
	str_copy(c->sos_apn, sizeof(c->sos_apn), "sos");
	c->radio_on_at_start = 1;
}

struct key {
	const char *name;
	enum { K_STR, K_INT, K_BOOL } type;
	size_t off, size;
	int lo, hi;
};

#define KS(n, f)		{ n, K_STR, offsetof(struct cfg, f), sizeof(((struct cfg *)0)->f), 0, 0 }
#define KI(n, f, lo, hi)	{ n, K_INT, offsetof(struct cfg, f), 0, lo, hi }
#define KB(n, f)		{ n, K_BOOL, offsetof(struct cfg, f), 0, 0, 1 }

static const struct key keys[] = {
	KS("port", port),
	KS("state_file", state_file),
	KB("assume_ready", assume_ready),
	KS("socket", sock),
	KS("speechd_socket", speechd_sock),
	KB("audio", audio),
	KI("call_rate", call_rate, 8000, 48000),
	KS("speech_on", speech_on),
	KS("voice_init_cmds", voice_init_cmds),
	KB("eaic_auto_accept", eaic_auto_accept),
	KS("radio_mask", radio_mask),
	KI("sim", sim, 1, 2),
	KS("ip_cmd", ip_cmd),
	KI("route_metric", route_metric, 0, 1000000),
	KS("resolv_file", resolv_file),
	KB("manage_resolv", manage_resolv),
	KS("resolv_target", resolv_target),
	KS("wwan_dev", wwan_dev),
	KS("data_backend", data_backend),
	KS("mipc_port", mipc_port),
	KS("mipc_client_name", mipc_client_name),
	KI("mipc_timeout_ms", mipc_timeout_ms, 100, 900000),
	KB("mipc_traffic_descriptor", mipc_traffic_descriptor),
	KS("data_deact_cmds", data_deact_cmds),
	KI("data_linkid", data_linkid, 0, 20),
	KI("data_cid", data_cid, -1, 99),
	KS("data_apn", data_apn),
	KS("data_ip_type", data_ip_type),
	KS("data_bind_cmds", data_bind_cmds),
	KI("data_mtu", data_mtu, 0, 9000),
	KB("keep_data_on_exit", keep_data_on_exit),
	KB("allow_raw_at", allow_raw_at),
	KS("raw_at_deny", raw_at_deny),
	KI("cmd_timeout_ms", cmd_timeout_ms, 100, 600000),
	KI("call_timeout_ms", call_timeout_ms, 100, 600000),
	KI("sms_timeout_ms", sms_timeout_ms, 100, 600000),
	KI("scan_timeout_ms", scan_timeout_ms, 1000, 600000),
	KI("signal_poll_ms", signal_poll_ms, 0, 3600000),
	KI("call_poll_ms", call_poll_ms, 50, 60000),
	KI("ready_poll_ms", ready_poll_ms, 10, 60000),
	KI("reopen_ms", reopen_ms, 10, 60000),
	KI("settle_ms", settle_ms, 0, 10000),
	KS("sms_ack_cmd", sms_ack_cmd),
	KB("sms_delete_read", sms_delete_read),
	KS("init_cmds", init_cmds),
	KB("ims", ims),
	KS("ims_cfg", ims_cfg),
	KS("ims_eims", ims_eims),
	KB("ims_mipc_key", ims_mipc_key),
	KI("ims_reg_poll_ms", ims_reg_poll_ms, 100, 3600000),
	KI("ims_reg_idle_poll_ms", ims_reg_idle_poll_ms, 100, 3600000),
	KB("apn_profiles", apn_profiles),
	KS("ims_apn", ims_apn),
	KS("sos_apn", sos_apn),
	KS("mms_apn", mms_apn),
	KB("radio_on_at_start", radio_on_at_start),
	KB("radio_off_before_setup", radio_off_before_setup),
};

static int cfg_set(struct cfg *c, const char *name, const char *value)
{
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		const struct key *k = &keys[i];
		int v;

		if (strcmp(k->name, name))
			continue;
		switch (k->type) {
		case K_STR:
			if (strlen(value) >= k->size)
				return -1;
			str_copy((char *)c + k->off, k->size, value);
			return 0;
		case K_INT:
		case K_BOOL:
			if (parse_int(value, k->lo, k->hi, &v))
				return -1;
			*(int *)((char *)c + k->off) = v;
			return 0;
		}
	}
	return -2;
}

static int cfg_check(struct cfg *c)
{
	if (strcmp(c->data_backend, "at") && strcmp(c->data_backend, "mipc")) {
		fprintf(stderr, "t2-modemd: data_backend must be at or mipc\n");
		return -1;
	}
	if (strcmp(c->speech_on, "dial") && strcmp(c->speech_on, "espeech")) {
		fprintf(stderr, "t2-modemd: speech_on must be dial or espeech\n");
		return -1;
	}
	if (strcasecmp(c->data_ip_type, "IP") && strcasecmp(c->data_ip_type, "IPV6") &&
	    strcasecmp(c->data_ip_type, "IPV4V6")) {
		fprintf(stderr, "t2-modemd: data_ip_type must be IP, IPV6 or IPV4V6\n");
		return -1;
	}
	if (c->radio_mask[0] && (strlen(c->radio_mask) != 1 || !strchr("123", c->radio_mask[0]))) {
		fprintf(stderr, "t2-modemd: radio_mask must be 1, 2, 3 or empty\n");
		return -1;
	}
	{
		/* six numbers 0..255: AT+EIMSCFG and the IMS_SET_CONFIG_REQ flags */
		char buf[sizeof(c->ims_cfg)], *save, *t;
		int n = 0, v, bad = !c->ims_cfg[0] ||
			strspn(c->ims_cfg, "0123456789,") != strlen(c->ims_cfg);

		str_copy(buf, sizeof(buf), c->ims_cfg);
		for (t = strtok_r(buf, ",", &save); t && !bad; t = strtok_r(NULL, ",", &save), n++)
			bad = parse_int(t, 0, 255, &v);
		if (bad || n != 6) {
			fprintf(stderr, "t2-modemd: ims_cfg must be six numbers 0..255 "
				"(volte,vilte,vowifi,viwifi,sms,eims)\n");
			return -1;
		}
	}
	if (strpbrk(c->ims_apn, "\"\r\n;{}") || strpbrk(c->sos_apn, "\"\r\n;{}") ||
	    strpbrk(c->mms_apn, "\"\r\n;{}") || strlen(c->ims_apn) > 99 || strlen(c->sos_apn) > 99 ||
	    strlen(c->mms_apn) > 99 || strlen(c->data_apn) > 99) {
		fprintf(stderr, "t2-modemd: bad APN in ims_apn/sos_apn/mms_apn/data_apn\n");
		return -1;
	}
	if (strcmp(c->ims_eims, "query") && strcmp(c->ims_eims, "always") &&
	    strcmp(c->ims_eims, "never")) {
		fprintf(stderr, "t2-modemd: ims_eims must be query, always or never\n");
		return -1;
	}
	if (strpbrk(c->wwan_dev, "/ ") || !c->wwan_dev[0]) {
		fprintf(stderr, "t2-modemd: bad wwan_dev\n");
		return -1;
	}
	return 0;
}

/* key = value, '#' comments; a missing file is an error only when named */
static int cfg_load(struct cfg *c, const char *path, int required)
{
	FILE *f = fopen(path, "re");
	char line[2048];
	int ln = 0, bad = 0;

	if (!f) {
		if (required) {
			fprintf(stderr, "t2-modemd: %s: %s\n", path, strerror(errno));
			return -1;
		}
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		char *k = line, *v, *e;

		ln++;
		line[strcspn(line, "\r\n")] = 0;
		while (isspace((unsigned char)*k))
			k++;
		if (!*k || *k == '#')
			continue;
		v = strchr(k, '=');
		if (!v) {
			fprintf(stderr, "t2-modemd: %s:%d: expected key=value\n", path, ln);
			bad++;
			continue;
		}
		*v++ = 0;
		e = k + strlen(k);
		while (e > k && isspace((unsigned char)e[-1]))
			*--e = 0;
		while (isspace((unsigned char)*v))
			v++;
		e = v + strlen(v);
		while (e > v && isspace((unsigned char)e[-1]))
			*--e = 0;
		switch (cfg_set(c, k, v)) {
		case -1:
			fprintf(stderr, "t2-modemd: %s:%d: bad value for %s\n", path, ln, k);
			bad++;
			break;
		case -2:
			fprintf(stderr, "t2-modemd: %s:%d: unknown key %s\n", path, ln, k);
			bad++;
			break;
		}
	}
	fclose(f);
	return bad ? -1 : 0;
}

/* --- the daemon --- */

static void on_signal(int sig)
{
	(void)sig;
	stop_sig = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: t2-modemd [options] -d " DEF_PORT "            daemon\n"
		"       t2-modemd [-s SOCKET] COMMAND [ARGS]             client\n"
		"       t2-modemd -T                                     self-test (no modem)\n"
		"commands: status | net | scan | radio on|off [mask] | sim 1|2 | pin <PIN>\n"
		"          data up [apn] [cid] | data down | data status\n"
		"          call dial <number> [--clir] | answer | hangup [id] | hold | resume\n"
		"          call swap | dtmf <digits> | list | device D | volume N | mute on|off\n"
		"          sms send <number> <text> | sms list | monitor | at <command>\n"
		"options:\n"
		"  -d DEV        AT port (" DEF_PORT ")\n"
		"  -c FILE       configuration (" DEF_CONF "), key=value\n"
		"  -o KEY=VALUE  one configuration value (after -c)\n"
		"  -S FILE       modem state file (" DEF_STATE "), none = no state source\n"
		"  -a            with no state source: ready as soon as the port is open\n"
		"  -s SOCK       control socket (" DEF_SOCK ")\n"
		"  -v            verbose (every AT line and event)\n");
}

static int mkdir_for(const char *path)
{
	char dir[300];
	char *slash;

	str_copy(dir, sizeof(dir), path);
	slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;
	*slash = 0;
	if (mkdir(dir, 0755) && errno != EEXIST)
		return -1;
	return 0;
}

static int serve(struct md *m)
{
	m->state = MD_CLOSED;
	m->at.fd = -1;
	m->reopen_at = 0;
	m->cfun = -1;
	m->rssi = m->ber = 99;
	m->ims_reg = -1;
	m->cs.stat = m->eps.stat = m->gprs.stat = -1;
	m->cs.act = m->eps.act = m->gprs.act = -1;
	m->sim_sel = 1;
	data_init(m);
	ims_init(m);
	call_init(m);
	mipc_init(m);
	if (m->cfg.resolv_file[0])
		mkdir_for(m->cfg.resolv_file);
	if (ctl_listen(m, m->cfg.sock))
		return 1;
	md_log(m, 0, "listening on %s", m->cfg.sock);

	while (!stop_sig) {
		struct pollfd p[3];
		int n = 0, ai = -1, mi = -1, to;
		int64_t now, dl;

		modem_tick(m, mono_ms());
		at_pump(m);
		now = mono_ms();
		dl = modem_next_deadline(m);
		to = dl == INT64_MAX ? 1000 : dl <= now ? 0 : dl - now > 1000 ? 1000 : (int)(dl - now);
		if (m->at.fd >= 0) {
			ai = n;
			p[n++] = (struct pollfd){ .fd = m->at.fd, .events = POLLIN };
		}
		if (m->mipc.fd >= 0) {
			mi = n;
			p[n++] = (struct pollfd){ .fd = m->mipc.fd, .events = POLLIN };
		}
		p[n++] = (struct pollfd){ .fd = m->ctl.lfd, .events = POLLIN };
		if (poll(p, (nfds_t)n, to) < 0) {
			if (errno == EINTR)
				continue;
			md_log(m, 0, "poll: %s", strerror(errno));
			break;
		}
		if (ai >= 0 && (p[ai].revents & (POLLIN | POLLHUP | POLLERR)))
			at_read(m);
		if (mi >= 0 && m->mipc.fd >= 0 && (p[mi].revents & (POLLIN | POLLHUP | POLLERR)))
			mipc_read(m);
		at_check_timeout(m, mono_ms());
		if (p[n - 1].revents & POLLIN)
			ctl_accept(m);
	}
	md_log(m, 0, "stopping");
	m->stopping = 0;	/* shutdown still needs to send commands */
	modem_shutdown(m);
	ctl_close(m);
	return 0;
}

int main(int argc, char **argv)
{
	struct md *m = &md_state;
	const char *conf = NULL, *port = NULL, *sock = NULL, *state = NULL;
	struct sigaction sa;
	int c, assume = 0, selftest = 0, verbose = 0;
	char *overrides[64];
	int nover = 0;

	cfg_defaults(&m->cfg);
	while ((c = getopt(argc, argv, "+d:c:o:S:as:vTh")) != -1) {
		switch (c) {
		case 'd':
			port = optarg;
			break;
		case 'c':
			conf = optarg;
			break;
		case 'o':
			if (nover < 64)
				overrides[nover++] = optarg;
			break;
		case 'S':
			state = optarg;
			break;
		case 'a':
			assume = 1;
			break;
		case 's':
			sock = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'T':
			selftest = 1;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (selftest) {
		int r = mipc_selftest();

		r |= sms_selftest();
		return r;
	}
	if (cfg_load(&m->cfg, conf ? conf : DEF_CONF, conf != NULL))
		return 2;
	for (int i = 0; i < nover; i++) {
		char *eq = strchr(overrides[i], '=');

		if (!eq) {
			fprintf(stderr, "t2-modemd: -o needs KEY=VALUE\n");
			return 2;
		}
		*eq = 0;
		if (cfg_set(&m->cfg, overrides[i], eq + 1)) {
			fprintf(stderr, "t2-modemd: -o %s: bad key or value\n", overrides[i]);
			return 2;
		}
	}
	if (sock)
		str_copy(m->cfg.sock, sizeof(m->cfg.sock), sock);
	if (!port) {
		/* client mode */
		if (optind == argc) {
			usage();
			return 2;
		}
		return client_main(m->cfg.sock, argc - optind, argv + optind);
	}
	if (optind != argc) {
		usage();
		return 2;
	}
	str_copy(m->cfg.port, sizeof(m->cfg.port), port);
	if (state)
		str_copy(m->cfg.state_file, sizeof(m->cfg.state_file),
			 strcmp(state, "none") ? state : "");
	if (assume)
		m->cfg.assume_ready = 1;
	m->verbose = verbose;
	if (cfg_check(&m->cfg))
		return 2;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	return serve(m);
}
