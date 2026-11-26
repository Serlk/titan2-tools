// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-powerd: the system sleep broker of the Titan 2 (mainline Linux).
 *
 * One root daemon decides when the phone sleeps, the way Android's
 * SystemSuspend and Doze do it on top of the kernel's suspend-to-idle:
 *
 *  - screen off (seen on the DRM connector, or told by the compositor) and
 *    a delay later the system suspends, unless an inhibitor is held;
 *  - inhibitors are leases: a client holds one as long as its connection
 *    to the control socket is open (or for a given time), a call in
 *    progress on ModemManager is one, and so is the kernel's own wakeup
 *    source machinery (the wakeup_count protocol refuses to suspend while
 *    a wakeup event is being handled);
 *  - after a wake-up the sources are read (wakeup sources, the SPM wake
 *    status of the mtk-lpm driver, the wake interrupt) and classified:
 *    a key wakes the user interface, a modem event (call, SMS, data) keeps
 *    the phone awake just long enough for the telephony stack to handle
 *    it, an alarm runs its hook, a maintenance window runs its hooks with
 *    the screen off and the phone sleeps again ("doze");
 *  - the single RTC alarm is shared: user alarms and the maintenance
 *    schedule go through the broker, the earliest one is armed before
 *    every suspend.
 *
 * Control: a line protocol on a Unix socket (t2-powerctl is this same
 * binary). Policy knobs: /etc/t2-powerd.conf. Hooks: /etc/t2-powerd/hooks.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>
#endif

#define VERSION "0.1"
#define DEFAULT_CONF "/etc/t2-powerd.conf"
#define DEFAULT_SOCK "/run/t2-powerd.sock"
#define DEFAULT_RUNDIR "/run/t2-powerd"
#define DEFAULT_HOOKS "/etc/t2-powerd/hooks"

#define MAX_CLIENTS 32
#define MAX_LEASES 64
#define MAX_ALARMS 32
#define MAX_CALLS 8
#define MAX_WS 128
#define MAX_INTERVALS 8
#define MAX_INPUTS 8
#define LINE_MAX_LEN 512

/* ---- configuration ----------------------------------------------------- */

struct config {
	char conf_path[PATH_MAX];
	char sock_path[PATH_MAX];
	char run_dir[PATH_MAX];
	char hooks_dir[PATH_MAX];
	char root[PATH_MAX];		/* prefix for /sys and /proc (tests) */
	char rtc[32];
	char connector[64];		/* DRM connector name, empty = first connected */
	char power_key_device[64];	/* substring of the evdev device name */
	int suspend_delay;		/* s after screen off */
	int poll_ms;
	bool inhibit_on_ssh;
	bool inhibit_on_usb_host;
	bool inhibit_on_nbd_root;
	bool power_key;
	int wake_grace_user;
	int wake_grace_modem;
	int wake_grace_modem_quiet;	/* s: a modem wake-up without a call or SMS (0: wake_grace_modem) */
	int wake_grace_network;
	int wake_grace_other;
	int wake_grace_alarm;
	int maintenance[MAX_INTERVALS];
	int n_maintenance;
	int maintenance_window;
	int backoff[MAX_INTERVALS];
	int n_backoff;
	int short_sleep;		/* s: a sleep shorter than this with no known reason counts for the backoff */
	int hook_timeout;
	bool dbus_modem;
	bool dry_run;			/* never write /sys/power/state */
	int dry_run_asleep;		/* s: a dry run counts this as time suspended (tests) */
	int log_level;
	/* GPU boost on input: devfreq min_freq raised for gpu_boost_ms after a touch or a key */
	bool gpu_boost;
	int gpu_boost_ms;
	long gpu_boost_hz;
	char gpu_boost_devices[256];	/* substrings of evdev device names */
	char gpu_devfreq[PATH_MAX];
	/* battery guard: hooks at battery_low/critical, a shutdown at battery_shutdown */
	bool battery;
	char battery_supply[64];	/* /sys/class/power_supply/<name>, empty = the first "Battery" */
	int battery_low, battery_critical, battery_shutdown;	/* %, 0 = that level off */
	int battery_poll;		/* s between readings while awake */
	char battery_shutdown_cmd[PATH_MAX];	/* run at battery_shutdown, empty = hooks only */
	char class_user[256];
	char class_modem[256];
	char class_network[256];
	char class_rtc[256];
};

static struct config cfg = {
	.conf_path = DEFAULT_CONF,
	.sock_path = DEFAULT_SOCK,
	.run_dir = DEFAULT_RUNDIR,
	.hooks_dir = DEFAULT_HOOKS,
	.rtc = "rtc0",
	.power_key_device = "mtk-pmic-keys",
	.suspend_delay = 20,
	.poll_ms = 1000,
	.inhibit_on_ssh = true,
	.inhibit_on_usb_host = true,
	.inhibit_on_nbd_root = true,
	.power_key = true,
	.wake_grace_user = 10,
	.wake_grace_modem = 20,
	.wake_grace_modem_quiet = 3,
	.wake_grace_network = 3,
	.wake_grace_other = 5,
	.wake_grace_alarm = 60,
	.maintenance = { 900, 1800, 3600, 7200 },
	.n_maintenance = 4,
	.maintenance_window = 30,
	.backoff = { 2, 5, 15, 60 },
	.n_backoff = 4,
	.short_sleep = 3,
	.hook_timeout = 60,
	.dbus_modem = true,
	.log_level = LOG_INFO,
	.gpu_boost = true,
	.gpu_boost_ms = 400,
	.gpu_boost_hz = 664000000,
	.gpu_boost_devices = "Synaptics,TitanKey,keyboard,keypad,aw9523,mtk-kpd",
	.gpu_devfreq = "/sys/class/devfreq/13000000.gpu",
	.battery = true,
	.battery_low = 15,
	.battery_critical = 5,
	.battery_shutdown = 2,
	.battery_poll = 60,
	.battery_shutdown_cmd = "/sbin/poweroff",
	.class_user = "gpio-keys,keys,keypad,keyboard,aw9523,focaltech,fingerprint,touch",
	.class_modem = "mddriver,ccci,modem",
	.class_network = "wifi,wlan,mt7921,bt@,btmtkbgf,bluetooth",
	.class_rtc = "rtc",
};

/* ---- state -------------------------------------------------------------- */

enum wake_class { WAKE_NONE, WAKE_USER, WAKE_MODEM, WAKE_NETWORK, WAKE_ALARM,
		  WAKE_MAINTENANCE, WAKE_OTHER, WAKE_CLASSES };
static const char * const wake_class_names[] = {
	"none", "user", "modem", "network", "alarm", "maintenance", "other"
};

struct lease {
	bool used;
	char name[48];
	char reason[96];
	int client;			/* index, -1 for a timed lease */
	struct timespec expires;	/* MONOTONIC, zero = until released */
};

struct alarm {
	bool used;
	char name[48];
	time_t when;			/* REALTIME */
	bool wake_ui;			/* a user alarm: the UI shows it */
	int client;			/* subscriber to notify, -1 = none */
};

struct client {
	int fd;
	char buf[LINE_MAX_LEN];
	size_t len;
	bool subscribed;
};

struct call {
	bool used;
	char path[96];
	int state;
};

struct ws_count {
	char name[64];
	unsigned long events;
};

struct state {
	/* screen */
	bool screen_off;
	struct timespec screen_off_since;	/* BOOTTIME */
	bool screen_from_socket;	/* the compositor tells us */
	struct timespec screen_socket_at;
	char connector_path[PATH_MAX];

	/* leases */
	struct lease leases[MAX_LEASES];
	struct alarm alarms[MAX_ALARMS];
	struct client clients[MAX_CLIENTS];
	int listen_fd;
	int key_fd;
	struct timespec key_down;

	/* GPU boost on input */
	int in_fds[MAX_INPUTS];
	int n_in;
	struct timespec in_scan;	/* last scan for input devices */
	bool gpu_boosted;
	struct timespec gpu_boost_until;
	char gpu_min_saved[32];		/* devfreq min_freq before the boost */
	bool gpu_boost_broken;		/* min_freq not writable: logged once, boost off */
	unsigned long gpu_boosts;

	/* battery guard */
	char battery_path[PATH_MAX];	/* /sys/class/power_supply/<name>, empty = not found yet */
	int battery_capacity;		/* %, -1 unknown */
	char battery_status[32];
	int battery_level;		/* 0 ok, 1 low, 2 critical, 3 shutdown */
	int battery_shutdown_hits;	/* readings in a row at the shutdown level */
	struct timespec battery_checked;
	bool battery_check_due;		/* read now (after a resume) */

	/* modem */
	struct call calls[MAX_CALLS];
	bool mm_ok;
	struct timespec mm_retry;

	/* sleep bookkeeping */
	struct timespec awake_until;	/* MONOTONIC: no suspend before */
	char awake_reason[64];
	int backoff_level;
	int maintenance_level;
	bool maintenance_armed;		/* the armed RTC alarm is ours for a window */
	time_t armed_alarm;		/* REALTIME of the RTC alarm written, 0 = none */
	char armed_name[48];
	unsigned long suspends, suspend_fail, wakes[WAKE_CLASSES];
	enum wake_class last_class;
	char last_sources[256];
	char last_spm[256];
	unsigned long last_slept_ms;
	time_t last_resume;		/* REALTIME, for the status */
	struct timespec last_resume_boot; /* BOOTTIME: "right after a wake-up" */
	double asleep_total;		/* s suspended (BOOTTIME minus MONOTONIC) by our suspends */
	long kernel_suspends;		/* /sys/power/suspend_stats/success seen, -1 unknown */
	struct ws_count ws[MAX_WS];
	int n_ws;
	bool running;
};

static struct state st;
static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t alarm_flag;
static bool log_stderr;

/* ---- helpers ------------------------------------------------------------ */

static void logmsg(int prio, const char *fmt, ...)
{
	va_list ap;

	if (prio > cfg.log_level)
		return;
	va_start(ap, fmt);
	if (log_stderr) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else {
		vsyslog(prio, fmt, ap);
	}
	va_end(ap);
}
#define LOGE(...) logmsg(LOG_ERR, __VA_ARGS__)
#define LOGW(...) logmsg(LOG_WARNING, __VA_ARGS__)
#define LOGI(...) logmsg(LOG_INFO, __VA_ARGS__)
#define LOGD(...) logmsg(LOG_DEBUG, __VA_ARGS__)

/*
 * Clocks: CLOCK_MONOTONIC stands still while the system is suspended (s2idle
 * too), CLOCK_BOOTTIME runs on. Intervals measured while awake (leases, the
 * awake-until deadline, the maintenance window, retries, key presses) use
 * MONOTONIC; anything that spans a suspend (how long we slept, "since the
 * screen went off", "right after a wake-up") uses BOOTTIME.
 */
static void now_mono(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }
static void now_boot(struct timespec *ts) { clock_gettime(CLOCK_BOOTTIME, ts); }
static double ts_diff(const struct timespec *a, const struct timespec *b)
{
	return (a->tv_sec - b->tv_sec) + (a->tv_nsec - b->tv_nsec) / 1e9;
}
static double since(const struct timespec *ts)
{
	struct timespec n;

	now_mono(&n);
	return ts_diff(&n, ts);
}
static double since_boot(const struct timespec *ts)
{
	struct timespec n;

	now_boot(&n);
	return ts_diff(&n, ts);
}
static void in_seconds(struct timespec *ts, int s)
{
	now_mono(ts);
	ts->tv_sec += s;
}
static bool ts_zero(const struct timespec *ts) { return !ts->tv_sec && !ts->tv_nsec; }

static const char *path(const char *p)
{
	static char buf[4][PATH_MAX];
	static int i;
	char *b = buf[i++ & 3];

	if (!cfg.root[0] || (strncmp(p, "/sys", 4) && strncmp(p, "/proc", 5) &&
			     strncmp(p, "/dev", 4)))
		return p;
	snprintf(b, PATH_MAX, "%s%s", cfg.root, p);
	return b;
}

static int read_file(const char *p, char *buf, size_t n)
{
	int fd = open(path(p), O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -errno;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -errno;
	buf[r] = 0;
	while (r > 0 && isspace((unsigned char)buf[r - 1]))
		buf[--r] = 0;
	return r;
}

static int write_file(const char *p, const char *val)
{
	int fd = open(path(p), O_WRONLY | O_TRUNC);
	ssize_t r;

	if (fd < 0)
		return -errno;
	r = write(fd, val, strlen(val));
	close(fd);
	return r < 0 ? -errno : 0;
}

static bool list_has(const char *list, const char *name)
{
	char tmp[256], *save, *tok;

	snprintf(tmp, sizeof(tmp), "%s", list);
	for (tok = strtok_r(tmp, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
		if (strcasestr(name, tok))
			return true;
	return false;
}

static void parse_int_list(const char *v, int *out, int *n, int max)
{
	char tmp[256], *save, *tok;

	snprintf(tmp, sizeof(tmp), "%s", v);
	*n = 0;
	for (tok = strtok_r(tmp, ", ", &save); tok && *n < max; tok = strtok_r(NULL, ", ", &save))
		if (atoi(tok) > 0)
			out[(*n)++] = atoi(tok);
}

static bool parse_bool(const char *v)
{
	return !strcasecmp(v, "yes") || !strcasecmp(v, "1") || !strcasecmp(v, "true") ||
	       !strcasecmp(v, "on");
}

static int load_config(const char *p)
{
	FILE *f = fopen(p, "r");
	char line[LINE_MAX_LEN];
	int n = 0;

	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		char *k = line, *v, *e;

		while (isspace((unsigned char)*k))
			k++;
		if (!*k || *k == '#')
			continue;
		v = strchr(k, '=');
		if (!v)
			continue;
		*v++ = 0;
		e = k + strlen(k);
		while (e > k && isspace((unsigned char)e[-1]))
			*--e = 0;
		while (isspace((unsigned char)*v))
			v++;
		e = v + strlen(v);
		while (e > v && (isspace((unsigned char)e[-1]) || e[-1] == '"'))
			*--e = 0;
		if (*v == '"')
			v++;
		if ((e = strchr(v, '#')) && e > v && isspace((unsigned char)e[-1]))
			*e = 0;
		n++;
#define SET_INT(key, field) if (!strcmp(k, key)) { cfg.field = atoi(v); continue; }
#define SET_BOOL(key, field) if (!strcmp(k, key)) { cfg.field = parse_bool(v); continue; }
#define SET_STR(key, field) if (!strcmp(k, key)) { snprintf(cfg.field, sizeof(cfg.field), "%s", v); continue; }
		SET_INT("suspend_delay", suspend_delay)
		SET_INT("poll_ms", poll_ms)
		SET_BOOL("inhibit_on_ssh", inhibit_on_ssh)
		SET_BOOL("inhibit_on_usb_host", inhibit_on_usb_host)
		SET_BOOL("inhibit_on_nbd_root", inhibit_on_nbd_root)
		SET_BOOL("power_key", power_key)
		SET_STR("power_key_device", power_key_device)
		SET_INT("wake_grace_user", wake_grace_user)
		SET_INT("wake_grace_modem", wake_grace_modem)
		SET_INT("wake_grace_modem_quiet", wake_grace_modem_quiet)
		SET_INT("wake_grace_network", wake_grace_network)
		SET_INT("wake_grace_other", wake_grace_other)
		SET_INT("wake_grace_alarm", wake_grace_alarm)
		SET_INT("maintenance_window", maintenance_window)
		SET_INT("short_sleep", short_sleep)
		SET_INT("hook_timeout", hook_timeout)
		SET_BOOL("dbus_modem", dbus_modem)
		SET_BOOL("dry_run", dry_run)
		SET_INT("dry_run_asleep", dry_run_asleep)
		SET_BOOL("gpu_boost", gpu_boost)
		SET_INT("gpu_boost_ms", gpu_boost_ms)
		SET_STR("gpu_boost_devices", gpu_boost_devices)
		SET_STR("gpu_devfreq", gpu_devfreq)
		if (!strcmp(k, "gpu_boost_hz")) {
			cfg.gpu_boost_hz = atol(v);
			continue;
		}
		SET_BOOL("battery", battery)
		SET_STR("battery_supply", battery_supply)
		SET_INT("battery_low", battery_low)
		SET_INT("battery_critical", battery_critical)
		SET_INT("battery_shutdown", battery_shutdown)
		SET_INT("battery_poll", battery_poll)
		SET_STR("battery_shutdown_cmd", battery_shutdown_cmd)
		SET_STR("rtc", rtc)
		SET_STR("connector", connector)
		SET_STR("socket", sock_path)
		SET_STR("hooks_dir", hooks_dir)
		SET_STR("class_user", class_user)
		SET_STR("class_modem", class_modem)
		SET_STR("class_network", class_network)
		SET_STR("class_rtc", class_rtc)
		if (!strcmp(k, "maintenance")) {
			parse_int_list(v, cfg.maintenance, &cfg.n_maintenance, MAX_INTERVALS);
			continue;
		}
		if (!strcmp(k, "backoff")) {
			parse_int_list(v, cfg.backoff, &cfg.n_backoff, MAX_INTERVALS);
			continue;
		}
		if (!strcmp(k, "log_level")) {
			cfg.log_level = !strcmp(v, "debug") ? LOG_DEBUG :
					!strcmp(v, "warning") ? LOG_WARNING : LOG_INFO;
			continue;
		}
		LOGW("config: unknown key %s", k);
	}
	fclose(f);
	return n;
}

/* ---- hooks -------------------------------------------------------------- */

static int run_one(const char *prog, char *const argv[], int timeout_s)
{
	pid_t pid = fork();
	int status = 0, waited = 0;

	if (pid < 0)
		return -errno;
	if (!pid) {
		int null = open("/dev/null", O_RDONLY);

		if (null >= 0) {
			dup2(null, 0);
			close(null);
		}
		setsid();
		execv(prog, argv);
		_exit(127);
	}
	while (waitpid(pid, &status, WNOHANG) == 0) {
		if (waited >= timeout_s * 20) {
			LOGW("hook %s: timeout after %d s, killed", prog, timeout_s);
			kill(-pid, SIGKILL);
			waitpid(pid, &status, 0);
			return -ETIME;
		}
		usleep(50000);
		waited++;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

/* <hooks_dir>/<name> (one file) and every executable in <hooks_dir>/<name>.d */
static int run_hooks(const char *name, int timeout_s)
{
	char p[PATH_MAX], d[PATH_MAX];
	char *argv[] = { p, NULL };
	struct stat sb;
	DIR *dir;
	struct dirent *de;
	char *names[64];
	int n = 0, i, ran = 0;

	snprintf(p, sizeof(p), "%s/%s", cfg.hooks_dir, name);
	if (!stat(p, &sb) && S_ISREG(sb.st_mode) && (sb.st_mode & S_IXUSR)) {
		LOGD("hook %s", p);
		run_one(p, argv, timeout_s);
		ran++;
	}
	snprintf(d, sizeof(d), "%s/%s.d", cfg.hooks_dir, name);
	dir = opendir(d);
	if (!dir)
		return ran;
	while ((de = readdir(dir)) && n < 64) {
		if (de->d_name[0] == '.')
			continue;
		names[n++] = strdup(de->d_name);
	}
	closedir(dir);
	qsort(names, n, sizeof(names[0]), cmp_str);
	for (i = 0; i < n; i++) {
		snprintf(p, sizeof(p), "%s/%s", d, names[i]);
		if (!stat(p, &sb) && S_ISREG(sb.st_mode) && (sb.st_mode & S_IXUSR)) {
			LOGD("hook %s", p);
			run_one(p, argv, timeout_s);
			ran++;
		}
		free(names[i]);
	}
	return ran;
}

/* ---- battery ------------------------------------------------------------ */

static void broadcast(const char *fmt, ...);

/*
 * A phone left alone must not run its battery down to nothing: the fuel
 * gauge loses its calibration, the kernel and the file systems get no
 * clean stop, and a dead phone misses the morning alarm. While the battery
 * discharges, the hooks battery-low and battery-critical run once each at
 * their levels (the desktop shows the warning, a hook may stop services),
 * and at battery_shutdown the battery-shutdown hooks run and
 * battery_shutdown_cmd is executed -- after two readings in a row at that
 * level, so one odd value from the gauge does not switch the phone off.
 * A charger, or a few percent more, resets the levels. The battery is read
 * every battery_poll seconds while awake and right after every wake-up: a
 * sleeping phone checks it in its maintenance windows (CLOCK_MONOTONIC
 * stands still in s2idle, so the poll interval alone would not do).
 */
static const char * const battery_level_names[] = { "ok", "low", "critical", "shutdown" };

static void find_battery(void)
{
	const char *base = "/sys/class/power_supply";
	DIR *dir;
	struct dirent *de;

	st.battery_path[0] = 0;
	if (cfg.battery_supply[0]) {
		snprintf(st.battery_path, sizeof(st.battery_path), "%s/%s", base,
			 cfg.battery_supply);
		return;
	}
	dir = opendir(path(base));
	if (!dir)
		return;
	while ((de = readdir(dir))) {
		char p[PATH_MAX], type[32];

		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s/type", base, de->d_name);
		if (read_file(p, type, sizeof(type)) > 0 && !strcmp(type, "Battery")) {
			snprintf(st.battery_path, sizeof(st.battery_path), "%s/%s", base,
				 de->d_name);
			break;
		}
	}
	closedir(dir);
}

static void battery_env(int level)
{
	char b[16];

	snprintf(b, sizeof(b), "%d", st.battery_capacity);
	setenv("T2_BATTERY_CAPACITY", b, 1);
	setenv("T2_BATTERY_STATUS", st.battery_status, 1);
	setenv("T2_BATTERY_LEVEL", battery_level_names[level], 1);
}

static void battery_check(void)
{
	char p[PATH_MAX], b[32];
	int cap, level;
	bool discharging;

	if (!cfg.battery)
		return;
	if (!st.battery_check_due && !ts_zero(&st.battery_checked) &&
	    since(&st.battery_checked) < cfg.battery_poll)
		return;
	st.battery_check_due = false;
	now_mono(&st.battery_checked);
	if (!st.battery_path[0]) {
		find_battery();
		if (!st.battery_path[0])
			return;	/* no battery (yet): a bench board, the gauge driver not loaded */
		LOGI("battery: %s", st.battery_path);
	}
	snprintf(p, sizeof(p), "%s/capacity", st.battery_path);
	if (read_file(p, b, sizeof(b)) <= 0) {
		st.battery_capacity = -1;
		return;
	}
	cap = atoi(b);
	snprintf(p, sizeof(p), "%s/status", st.battery_path);
	if (read_file(p, st.battery_status, sizeof(st.battery_status)) <= 0)
		snprintf(st.battery_status, sizeof(st.battery_status), "Unknown");
	st.battery_capacity = cap;
	discharging = !strcmp(st.battery_status, "Discharging");

	if (!discharging || cap > cfg.battery_low + 5) {
		st.battery_shutdown_hits = 0;
		if (st.battery_level) {
			LOGI("battery %d%% (%s): back to normal", cap, st.battery_status);
			st.battery_level = 0;
			battery_env(0);
			run_hooks("battery-ok", cfg.hook_timeout);
			broadcast("battery ok %d", cap);
		}
		return;
	}
	level = cfg.battery_shutdown > 0 && cap <= cfg.battery_shutdown ? 3 :
		cfg.battery_critical > 0 && cap <= cfg.battery_critical ? 2 :
		cfg.battery_low > 0 && cap <= cfg.battery_low ? 1 : 0;
	if (level < 3)
		st.battery_shutdown_hits = 0;
	while (st.battery_level < level) {
		int next = st.battery_level + 1;

		if (next == 3 && ++st.battery_shutdown_hits < 2) {
			LOGW("battery %d%% (%s): at the shutdown level, confirming at the next reading",
			     cap, st.battery_status);
			break;
		}
		st.battery_level = next;
		battery_env(next);
		LOGW("battery %d%% (%s): %s", cap, st.battery_status, battery_level_names[next]);
		broadcast("battery %s %d", battery_level_names[next], cap);
		if (next == 1) {
			run_hooks("battery-low", cfg.hook_timeout);
		} else if (next == 2) {
			run_hooks("battery-critical", cfg.hook_timeout);
		} else {
			run_hooks("battery-shutdown", cfg.hook_timeout);
			if (cfg.battery_shutdown_cmd[0]) {
				char *argv[] = { cfg.battery_shutdown_cmd, NULL };

				LOGW("battery: running %s", cfg.battery_shutdown_cmd);
				sync();
				run_one(cfg.battery_shutdown_cmd, argv, cfg.hook_timeout);
			}
		}
	}
}

/* ---- screen --------------------------------------------------------------- */

static void find_connector(void)
{
	glob_t g;
	size_t i;
	char pat[PATH_MAX], buf[64];

	st.connector_path[0] = 0;
	snprintf(pat, sizeof(pat), "%s/sys/class/drm/card*-%s/dpms", cfg.root,
		 cfg.connector[0] ? cfg.connector : "*");
	if (glob(pat, 0, NULL, &g))
		return;
	for (i = 0; i < g.gl_pathc; i++) {
		char status[PATH_MAX];

		snprintf(status, sizeof(status), "%.*s/status",
			 (int)(strlen(g.gl_pathv[i]) - 5), g.gl_pathv[i]);
		if (read_file(status + strlen(cfg.root), buf, sizeof(buf)) > 0 &&
		    !strcmp(buf, "connected")) {
			snprintf(st.connector_path, sizeof(st.connector_path), "%s",
				 g.gl_pathv[i] + strlen(cfg.root));
			break;
		}
	}
	globfree(&g);
	if (st.connector_path[0])
		LOGI("screen state from %s", st.connector_path);
	else
		LOGW("no connected DRM connector: screen state only from the socket");
}

static void set_screen(bool off, const char *how)
{
	if (off == st.screen_off)
		return;
	st.screen_off = off;
	now_boot(&st.screen_off_since);
	LOGI("screen %s (%s)", off ? "off" : "on", how);
	/* subscribers (t2-fpd: the fingerprint HAL arms the sensor only with the panel on) */
	broadcast("screen %s", off ? "off" : "on");
	if (!off) {
		/* the user is back: maintenance starts over */
		st.maintenance_level = 0;
		st.backoff_level = 0;
	}
}

static void poll_screen(void)
{
	char buf[32];

	/* the compositor's word stands for a while over the DRM state */
	if (st.screen_from_socket && since(&st.screen_socket_at) < 5)
		return;
	if (!st.connector_path[0]) {
		if (!st.screen_from_socket)
			find_connector();
		return;
	}
	if (read_file(st.connector_path, buf, sizeof(buf)) < 0) {
		find_connector();
		return;
	}
	set_screen(strcmp(buf, "On") != 0, "drm");
}

/* ---- leases --------------------------------------------------------------- */

static struct lease *find_lease(const char *name, int client)
{
	int i;

	for (i = 0; i < MAX_LEASES; i++)
		if (st.leases[i].used && !strcmp(st.leases[i].name, name) &&
		    st.leases[i].client == client)
			return &st.leases[i];
	return NULL;
}

static struct lease *add_lease(const char *name, const char *reason, int client, int secs)
{
	struct lease *l = find_lease(name, client);
	int i;

	if (!l) {
		for (i = 0; i < MAX_LEASES; i++)
			if (!st.leases[i].used) {
				l = &st.leases[i];
				break;
			}
		if (!l)
			return NULL;
		memset(l, 0, sizeof(*l));
		l->used = true;
		snprintf(l->name, sizeof(l->name), "%s", name);
		l->client = client;
	}
	snprintf(l->reason, sizeof(l->reason), "%s", reason ? reason : "");
	if (secs > 0)
		in_seconds(&l->expires, secs);
	else
		memset(&l->expires, 0, sizeof(l->expires));
	LOGD("lease %s (%s) %s", name, l->reason, secs > 0 ? "timed" : "held");
	return l;
}

static void drop_lease(struct lease *l)
{
	LOGD("lease %s released", l->name);
	l->used = false;
}

static void expire_leases(void)
{
	int i;

	for (i = 0; i < MAX_LEASES; i++) {
		struct lease *l = &st.leases[i];

		if (l->used && !ts_zero(&l->expires) && since(&l->expires) >= 0)
			drop_lease(l);
	}
}

/* ---- soft blockers (observed, not leased) --------------------------------- */

static bool ssh_session(void)
{
	static const char * const files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
	char line[512];
	int i;

	for (i = 0; i < 2; i++) {
		FILE *f = fopen(path(files[i]), "r");

		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			char local[64], remote[64];
			unsigned int sl, state;

			if (sscanf(line, "%u: %63s %63s %x", &sl, local, remote, &state) != 4)
				continue;
			if (state == 1 && strlen(local) > 5 &&
			    !strcasecmp(local + strlen(local) - 5, ":0016")) {
				fclose(f);
				return true;
			}
		}
		fclose(f);
	}
	return false;
}

static bool usb_host_connected(void)
{
	glob_t g;
	size_t i;
	char pat[PATH_MAX], buf[32];
	bool yes = false;

	snprintf(pat, sizeof(pat), "%s/sys/class/udc/*/state", cfg.root);
	if (glob(pat, 0, NULL, &g))
		return false;
	for (i = 0; i < g.gl_pathc && !yes; i++)
		if (read_file(g.gl_pathv[i] + strlen(cfg.root), buf, sizeof(buf)) > 0 &&
		    !strcmp(buf, "configured"))
			yes = true;
	globfree(&g);
	return yes;
}

static bool nbd_root(void)
{
	FILE *f;
	char dev[128], mnt[128], line[512];
	bool yes = false;

	if (!access("/run/t2-netroot", F_OK))
		return true;
	f = fopen(path("/proc/mounts"), "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%127s %127s", dev, mnt) == 2 && !strcmp(mnt, "/") &&
		    strstr(dev, "nbd")) {
			yes = true;
			break;
		}
	fclose(f);
	return yes;
}

static bool call_active(void)
{
	int i;

	for (i = 0; i < MAX_CALLS; i++)
		/* MM call states: 1 dialing .. 6 waiting are live, 7 terminated */
		if (st.calls[i].used && st.calls[i].state >= 1 && st.calls[i].state <= 6)
			return true;
	return false;
}

/* the first reason not to sleep, or NULL */
static const char *blocker(char *buf, size_t n)
{
	int i;

	for (i = 0; i < MAX_LEASES; i++)
		if (st.leases[i].used) {
			snprintf(buf, n, "lease %s%s%s", st.leases[i].name,
				 st.leases[i].reason[0] ? ": " : "", st.leases[i].reason);
			return buf;
		}
	if (call_active())
		return "call in progress";
	if (since(&st.awake_until) < 0) {
		snprintf(buf, n, "awake for %.0f s more (%s)", -since(&st.awake_until),
			 st.awake_reason);
		return buf;
	}
	if (cfg.inhibit_on_nbd_root && nbd_root())
		return "root on NBD";
	if (cfg.inhibit_on_ssh && ssh_session())
		return "SSH session";
	if (cfg.inhibit_on_usb_host && usb_host_connected())
		return "USB host connected";
	return NULL;
}

/* ---- wakeup sources ------------------------------------------------------- */

static void snapshot_ws(void)
{
	glob_t g;
	size_t i;
	char pat[PATH_MAX], name[64], cnt[32];

	st.n_ws = 0;
	snprintf(pat, sizeof(pat), "%s/sys/class/wakeup/wakeup*/name", cfg.root);
	if (glob(pat, 0, NULL, &g))
		return;
	for (i = 0; i < g.gl_pathc && st.n_ws < MAX_WS; i++) {
		char p[PATH_MAX];

		snprintf(p, sizeof(p), "%.*s/event_count",
			 (int)(strlen(g.gl_pathv[i]) - 5), g.gl_pathv[i]);
		if (read_file(g.gl_pathv[i] + strlen(cfg.root), name, sizeof(name)) <= 0 ||
		    read_file(p + strlen(cfg.root), cnt, sizeof(cnt)) <= 0)
			continue;
		snprintf(st.ws[st.n_ws].name, sizeof(st.ws[0].name), "%s", name);
		st.ws[st.n_ws].events = strtoul(cnt, NULL, 10);
		st.n_ws++;
	}
	globfree(&g);
}

/* names whose event_count grew since the snapshot, space separated */
static void diff_ws(char *out, size_t n)
{
	struct ws_count old[MAX_WS];
	int n_old = st.n_ws, i, j;
	size_t len = 0;

	memcpy(old, st.ws, sizeof(old));
	snapshot_ws();
	out[0] = 0;
	for (i = 0; i < st.n_ws; i++)
		for (j = 0; j < n_old; j++)
			if (!strcmp(st.ws[i].name, old[j].name)) {
				if (st.ws[i].events > old[j].events && len < n - 1)
					len += snprintf(out + len, n - len, "%s%s",
							len ? " " : "", st.ws[i].name);
				break;
			}
}

static void wake_irq_name(char *out, size_t n)
{
	char irq[16], line[512];
	FILE *f;
	int num;

	out[0] = 0;
	if (read_file("/sys/power/pm_wakeup_irq", irq, sizeof(irq)) <= 0)
		return;
	num = atoi(irq);
	f = fopen(path("/proc/interrupts"), "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *last;

		while (isspace((unsigned char)*p))
			p++;
		if (atoi(p) != num || !strchr(p, ':'))
			continue;
		/* the name is the last field */
		last = strrchr(line, ' ');
		if (last) {
			snprintf(out, n, "irq%d:%s", num, last + 1);
			out[strcspn(out, "\n")] = 0;
		}
		break;
	}
	fclose(f);
}

/* the mtk-lpm driver: "sources ..." and "slept_ms N" of the last wake-up */
static void spm_wake(char *sources, size_t n, unsigned long *slept_ms, bool *deepest)
{
	glob_t g;
	char pat[PATH_MAX], buf[2048], *line, *save;

	sources[0] = 0;
	*slept_ms = 0;
	*deepest = false;
	snprintf(pat, sizeof(pat), "%s/sys/bus/platform/drivers/mtk-lpm/*/lpm/last_wake", cfg.root);
	if (glob(pat, 0, NULL, &g))
		return;
	if (read_file(g.gl_pathv[0] + strlen(cfg.root), buf, sizeof(buf)) > 0) {
		for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (!strncmp(line, "sources ", 8))
				snprintf(sources, n, "%s", line + 8);
			else if (!strncmp(line, "slept_ms ", 9))
				*slept_ms = strtoul(line + 9, NULL, 10);
			else if (!strncmp(line, "deepest ", 8))
				*deepest = atoi(line + 8);
		}
	}
	globfree(&g);
}

/* ---- the RTC alarm -------------------------------------------------------- */

static int rtc_arm(time_t when, const char *name)
{
	char p[PATH_MAX], v[32];
	time_t now = time(NULL);
	long delta = (long)(when - now);

	snprintf(p, sizeof(p), "/sys/class/rtc/%s/wakealarm", cfg.rtc);
	write_file(p, "0");
	if (delta < 1)
		delta = 1;
	snprintf(v, sizeof(v), "+%ld", delta);
	if (write_file(p, v) < 0) {
		LOGE("cannot arm the RTC alarm (%s): %s", p, strerror(errno));
		st.armed_alarm = 0;
		return -1;
	}
	st.armed_alarm = when;
	snprintf(st.armed_name, sizeof(st.armed_name), "%s", name);
	LOGD("RTC alarm in %ld s for %s", delta, name);
	return 0;
}

static void rtc_disarm(void)
{
	char p[PATH_MAX];

	if (!st.armed_alarm)
		return;
	snprintf(p, sizeof(p), "/sys/class/rtc/%s/wakealarm", cfg.rtc);
	write_file(p, "0");
	st.armed_alarm = 0;
}

static struct alarm *next_alarm(void)
{
	struct alarm *best = NULL;
	int i;

	for (i = 0; i < MAX_ALARMS; i++)
		if (st.alarms[i].used && (!best || st.alarms[i].when < best->when))
			best = &st.alarms[i];
	return best;
}

/* an RTC alarm in the future that is not ours (t2-pmtest, a shell) */
static bool foreign_alarm_pending(void)
{
	char p[PATH_MAX], v[32];
	long when;

	if (st.armed_alarm)
		return false;
	snprintf(p, sizeof(p), "/sys/class/rtc/%s/wakealarm", cfg.rtc);
	if (read_file(p, v, sizeof(v)) < 0)
		return false;
	when = strtol(v, NULL, 10);
	return when > (long)time(NULL);
}

/* the earliest of the user alarms and the maintenance schedule */
static void arm_before_sleep(void)
{
	struct alarm *a = next_alarm();
	time_t when = 0, now = time(NULL);
	const char *name = "";

	st.maintenance_armed = false;
	if (a) {
		when = a->when;
		name = a->name;
	}
	if (cfg.n_maintenance) {
		int lvl = st.maintenance_level < cfg.n_maintenance ? st.maintenance_level :
			  cfg.n_maintenance - 1;
		time_t m = now + cfg.maintenance[lvl];

		if (!when || m < when) {
			when = m;
			name = "maintenance";
			st.maintenance_armed = true;
		}
	}
	if (when)
		rtc_arm(when, name);
	else
		rtc_disarm();
}

/* ---- clients ------------------------------------------------------------- */

static void client_send(struct client *c, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	if (c->fd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0 && write(c->fd, buf, n) < 0)
		LOGD("client write: %s", strerror(errno));
}

static void broadcast(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int i;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (i = 0; i < MAX_CLIENTS; i++)
		if (st.clients[i].fd >= 0 && st.clients[i].subscribed)
			client_send(&st.clients[i], "event %s\n", buf);
}

static void client_close(int idx)
{
	struct client *c = &st.clients[idx];
	int i;

	if (c->fd < 0)
		return;
	close(c->fd);
	c->fd = -1;
	c->len = 0;
	c->subscribed = false;
	for (i = 0; i < MAX_LEASES; i++)
		if (st.leases[i].used && st.leases[i].client == idx)
			drop_lease(&st.leases[i]);
	for (i = 0; i < MAX_ALARMS; i++)
		if (st.alarms[i].used && st.alarms[i].client == idx)
			st.alarms[i].client = -1;
}

static void status_text(struct client *c)
{
	char b[160];
	const char *blk = blocker(b, sizeof(b));
	struct alarm *a;
	int i;

	client_send(c, "state %s\n", st.screen_off ? "screen-off" : "awake");
	client_send(c, "screen %s since %.0f s (%s)\n", st.screen_off ? "off" : "on",
		    since_boot(&st.screen_off_since), st.screen_from_socket ? "socket" : "drm");
	client_send(c, "blocker %s\n", blk ? blk : "none");
	client_send(c, "suspend_delay %d\n", cfg.suspend_delay);
	client_send(c, "gpu_boost %s inputs=%d boosts=%lu\n",
		    !cfg.gpu_boost ? "off" : st.gpu_boost_broken ? "broken" :
		    st.gpu_boosted ? "active" : "armed", st.n_in, st.gpu_boosts);
	for (i = 0; i < MAX_LEASES; i++)
		if (st.leases[i].used)
			client_send(c, "lease %s client=%d %s%s\n", st.leases[i].name,
				    st.leases[i].client,
				    ts_zero(&st.leases[i].expires) ? "held" : "timed",
				    st.leases[i].reason[0] ? st.leases[i].reason : "");
	for (i = 0; i < MAX_CALLS; i++)
		if (st.calls[i].used)
			client_send(c, "call %s state=%d\n", st.calls[i].path, st.calls[i].state);
	for (i = 0; i < MAX_ALARMS; i++)
		if (st.alarms[i].used)
			client_send(c, "alarm %s %ld %s\n", st.alarms[i].name,
				    (long)st.alarms[i].when, st.alarms[i].wake_ui ? "ui" : "quiet");
	a = next_alarm();
	client_send(c, "maintenance level=%d next=%d s window=%d\n", st.maintenance_level,
		    cfg.n_maintenance ? cfg.maintenance[st.maintenance_level < cfg.n_maintenance ?
				   st.maintenance_level : cfg.n_maintenance - 1] : 0,
		    cfg.maintenance_window);
	client_send(c, "rtc_armed %ld %s\n", (long)st.armed_alarm, st.armed_name);
	client_send(c, "next_user_alarm %ld\n", a ? (long)a->when : 0L);
	client_send(c, "mm %s\n", st.mm_ok ? "connected" : "off");
	if (cfg.battery)
		client_send(c, "battery %d%% %s level=%s\n", st.battery_capacity,
			    st.battery_status[0] ? st.battery_status : "unknown",
			    battery_level_names[st.battery_level]);
	client_send(c, "suspends %lu failed %lu backoff %d\n", st.suspends, st.suspend_fail,
		    st.backoff_level);
	{
		struct timespec m, bt;

		now_mono(&m);
		now_boot(&bt);
		/* ours, and all the kernel's since boot (BOOTTIME minus MONOTONIC) */
		client_send(c, "asleep %.0f s (since boot: %.0f s)\n", st.asleep_total, ts_diff(&bt, &m));
	}
	client_send(c, "wakes user=%lu modem=%lu network=%lu alarm=%lu maintenance=%lu other=%lu\n",
		    st.wakes[WAKE_USER], st.wakes[WAKE_MODEM], st.wakes[WAKE_NETWORK],
		    st.wakes[WAKE_ALARM], st.wakes[WAKE_MAINTENANCE], st.wakes[WAKE_OTHER]);
	client_send(c, "last_wake %s sources=[%s] spm=[%s] slept_ms=%lu at %ld\n",
		    wake_class_names[st.last_class], st.last_sources, st.last_spm,
		    st.last_slept_ms, (long)st.last_resume);
	client_send(c, "ssh %d usb_host %d nbd_root %d\n", ssh_session(), usb_host_connected(),
		    nbd_root());
}

static void write_status_file(void)
{
	char p[PATH_MAX];
	struct client tmp = { .fd = -1 };

	snprintf(p, sizeof(p), "%s/status", cfg.run_dir);
	tmp.fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (tmp.fd < 0)
		return;
	status_text(&tmp);
	close(tmp.fd);
}

static int do_suspend(bool force);
static long kernel_suspend_count(void);
static void gpu_boost_start(int ms, const char *how);

static void handle_command(int idx, char *line)
{
	struct client *c = &st.clients[idx];
	char *save, *cmd = strtok_r(line, " \t", &save);
	char *a1 = strtok_r(NULL, " \t", &save);
	char *a2 = strtok_r(NULL, " \t", &save);
	char *rest = strtok_r(NULL, "", &save);

	if (!cmd)
		return;
	if (!strcmp(cmd, "inhibit") && a1) {
		char reason[96];

		snprintf(reason, sizeof(reason), "%s%s%s", a2 ? a2 : "", rest ? " " : "",
			 rest ? rest : "");
		client_send(c, add_lease(a1, reason, idx, 0) ? "ok\n" : "error: no lease slot\n");
	} else if (!strcmp(cmd, "block") && a1 && a2) {
		client_send(c, add_lease(a1, rest, -1, atoi(a2)) ? "ok\n" : "error\n");
	} else if ((!strcmp(cmd, "unblock") || !strcmp(cmd, "release")) && a1) {
		struct lease *l = find_lease(a1, -1);

		if (!l)
			l = find_lease(a1, idx);
		if (l)
			drop_lease(l);
		client_send(c, l ? "ok\n" : "error: no such lease\n");
	} else if (!strcmp(cmd, "screen") && a1) {
		st.screen_from_socket = true;
		now_mono(&st.screen_socket_at);
		set_screen(!strcmp(a1, "off"), "socket");
		client_send(c, "ok\n");
	} else if (!strcmp(cmd, "activity")) {
		in_seconds(&st.awake_until, cfg.wake_grace_user);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "activity");
		client_send(c, "ok\n");
	} else if (!strcmp(cmd, "gpu") && a1 && !strcmp(a1, "boost")) {
		if (!cfg.gpu_boost || st.gpu_boost_broken) {
			client_send(c, "error: gpu boost off\n");
		} else {
			gpu_boost_start(a2 ? atoi(a2) : 0, "socket");
			client_send(c, st.gpu_boosted ? "ok\n" : "error: screen off\n");
		}
	} else if (!strcmp(cmd, "suspend")) {
		int r = do_suspend(a1 && !strcmp(a1, "force"));

		if (r)
			client_send(c, "error: %s\n", strerror(-r));
		else
			client_send(c, "ok %s\n", wake_class_names[st.last_class]);
	} else if (!strcmp(cmd, "alarm") && a1) {
		if (!strcmp(a1, "set") && a2 && rest) {
			char *when = strtok_r(rest, " ", &save);
			char *flag = strtok_r(NULL, " ", &save);
			struct alarm *a = NULL;
			int i;

			for (i = 0; i < MAX_ALARMS; i++)
				if (st.alarms[i].used && !strcmp(st.alarms[i].name, a2))
					a = &st.alarms[i];
			for (i = 0; !a && i < MAX_ALARMS; i++)
				if (!st.alarms[i].used)
					a = &st.alarms[i];
			if (!a || !when) {
				client_send(c, "error\n");
				return;
			}
			a->used = true;
			snprintf(a->name, sizeof(a->name), "%s", a2);
			a->when = when[0] == '+' ? time(NULL) + atol(when + 1) : atol(when);
			a->wake_ui = !flag || strcmp(flag, "quiet") != 0;
			a->client = c->subscribed ? idx : -1;
			client_send(c, "ok %ld\n", (long)a->when);
		} else if (!strcmp(a1, "clear") && a2) {
			int i, n = 0;

			for (i = 0; i < MAX_ALARMS; i++)
				if (st.alarms[i].used && !strcmp(st.alarms[i].name, a2)) {
					st.alarms[i].used = false;
					n++;
				}
			client_send(c, n ? "ok\n" : "error: no such alarm\n");
		} else if (!strcmp(a1, "list")) {
			int i;

			for (i = 0; i < MAX_ALARMS; i++)
				if (st.alarms[i].used)
					client_send(c, "alarm %s %ld %s\n", st.alarms[i].name,
						    (long)st.alarms[i].when,
						    st.alarms[i].wake_ui ? "ui" : "quiet");
			client_send(c, "ok\n");
		} else {
			client_send(c, "error: alarm set NAME +SEC|EPOCH [quiet] | clear NAME | list\n");
		}
	} else if (!strcmp(cmd, "subscribe")) {
		c->subscribed = true;
		client_send(c, "ok\n");
		client_send(c, "event screen %s\n", st.screen_off ? "off" : "on");
	} else if (!strcmp(cmd, "status")) {
		status_text(c);
		client_send(c, "ok\n");
	} else if (!strcmp(cmd, "reload")) {
		load_config(cfg.conf_path);
		st.battery_path[0] = 0;	/* battery_supply may have changed */
		st.battery_check_due = true;
		client_send(c, "ok\n");
	} else if (!strcmp(cmd, "quit")) {
		client_close(idx);
	} else {
		client_send(c, "error: unknown command %s\n", cmd);
	}
}

static void client_read(int idx)
{
	struct client *c = &st.clients[idx];
	ssize_t r = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
	char *nl;

	if (r <= 0) {
		client_close(idx);
		return;
	}
	c->len += r;
	c->buf[c->len] = 0;
	while ((nl = memchr(c->buf, '\n', c->len))) {
		*nl = 0;
		handle_command(idx, c->buf);
		if (c->fd < 0)
			return;
		c->len -= nl + 1 - c->buf;
		memmove(c->buf, nl + 1, c->len);
		c->buf[c->len] = 0;
	}
	if (c->len == sizeof(c->buf) - 1)
		c->len = 0;	/* a line too long: drop it */
}

static int listen_socket(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -errno;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", cfg.sock_path);
	unlink(cfg.sock_path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
		close(fd);
		return -errno;
	}
	chmod(cfg.sock_path, 0666);
	return fd;
}

/* ---- the power key -------------------------------------------------------- */

static int open_power_key(void)
{
	glob_t g;
	size_t i;
	char pat[PATH_MAX], name[128];
	int fd = -1;

	snprintf(pat, sizeof(pat), "%s/dev/input/event*", cfg.root);
	if (glob(pat, 0, NULL, &g))
		return -1;
	for (i = 0; i < g.gl_pathc && fd < 0; i++) {
		int f = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (f < 0)
			continue;
		if (ioctl(f, EVIOCGNAME(sizeof(name)), name) > 0 &&
		    strcasestr(name, cfg.power_key_device)) {
			LOGI("power key on %s (%s)", g.gl_pathv[i], name);
			fd = f;
		} else {
			close(f);
		}
	}
	globfree(&g);
	return fd;
}

static void read_power_key(void)
{
	struct input_event ev[16];
	ssize_t r = read(st.key_fd, ev, sizeof(ev));
	size_t i;

	if (r <= 0) {
		if (r < 0 && errno != EAGAIN) {
			close(st.key_fd);
			st.key_fd = -1;
		}
		return;
	}
	for (i = 0; i < r / sizeof(ev[0]); i++) {
		if (ev[i].type != EV_KEY || ev[i].code != KEY_POWER)
			continue;
		if (ev[i].value == 1) {
			now_mono(&st.key_down);
		} else if (ev[i].value == 0 && !ts_zero(&st.key_down)) {
			double held = since(&st.key_down);

			memset(&st.key_down, 0, sizeof(st.key_down));
			if (held > 0.8)
				continue;	/* long press: not ours */
			/* a short press right after a wake-up woke us: screen on */
			if (st.screen_off ||
			    (!ts_zero(&st.last_resume_boot) && since_boot(&st.last_resume_boot) < 2)) {
				LOGI("power key: screen on");
				run_hooks("screen-on", cfg.hook_timeout);
				set_screen(false, "power key");
			} else {
				LOGI("power key: screen off");
				run_hooks("screen-off", cfg.hook_timeout);
				set_screen(true, "power key");
			}
		}
	}
}

/* ---- GPU boost on input -------------------------------------------------------
 *
 * The GPU governor (panthor devfreq, simple_ondemand) samples the load every
 * polling_interval ms and raises the clock only after a busy period; the first
 * frames of a scroll or an animation that starts from an idle GPU (265 MHz,
 * runtime suspended) are rendered at the lowest clock and miss their TE. The
 * phone's own input (touch screen, keyboard) is the earliest sign that frames
 * are coming: raise devfreq min_freq to gpu_boost_hz for gpu_boost_ms (every
 * event extends it), then put the previous min_freq back. The governor stays in
 * charge above the floor. Nothing is done while the screen is off, and a
 * min_freq that cannot be written switches the feature off for this run.
 * "gpu boost [MS]" on the socket does the same for a compositor or a launcher.
 */

static void open_input_devices(void)
{
	glob_t g;
	size_t i;
	char pat[PATH_MAX], name[128];

	if (since(&st.in_scan) < 10 && !ts_zero(&st.in_scan))
		return;
	now_mono(&st.in_scan);
	snprintf(pat, sizeof(pat), "%s/dev/input/event*", cfg.root);
	if (glob(pat, 0, NULL, &g))
		return;
	for (i = 0; i < g.gl_pathc && st.n_in < MAX_INPUTS; i++) {
		int f, j;
		bool have = false;

		/* already open (same path)? the fd table keeps the paths' order by node */
		f = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (f < 0)
			continue;
		if (ioctl(f, EVIOCGNAME(sizeof(name)), name) <= 0 ||
		    !list_has(cfg.gpu_boost_devices, name)) {
			close(f);
			continue;
		}
		for (j = 0; j < st.n_in; j++) {
			struct stat a, b;

			if (!fstat(st.in_fds[j], &a) && !fstat(f, &b) && a.st_rdev == b.st_rdev)
				have = true;
		}
		if (have) {
			close(f);
			continue;
		}
		LOGI("gpu boost: input %s (%s)", g.gl_pathv[i], name);
		st.in_fds[st.n_in++] = f;
	}
	globfree(&g);
}

static void gpu_boost_end(void)
{
	char p[PATH_MAX];

	if (!st.gpu_boosted)
		return;
	st.gpu_boosted = false;
	snprintf(p, sizeof(p), "%s/min_freq", cfg.gpu_devfreq);
	if (write_file(p, st.gpu_min_saved) < 0)
		LOGW("gpu boost: cannot restore min_freq %s", st.gpu_min_saved);
}

static void gpu_boost_start(int ms, const char *how)
{
	char p[PATH_MAX], val[32];
	int r;

	if (!cfg.gpu_boost || st.gpu_boost_broken || st.screen_off)
		return;
	if (ms <= 0)
		ms = cfg.gpu_boost_ms;
	now_mono(&st.gpu_boost_until);
	st.gpu_boost_until.tv_nsec += (long)(ms % 1000) * 1000000L;
	st.gpu_boost_until.tv_sec += ms / 1000 + st.gpu_boost_until.tv_nsec / 1000000000L;
	st.gpu_boost_until.tv_nsec %= 1000000000L;
	if (st.gpu_boosted)
		return;
	snprintf(p, sizeof(p), "%s/min_freq", cfg.gpu_devfreq);
	r = read_file(p, st.gpu_min_saved, sizeof(st.gpu_min_saved));
	if (r <= 0) {
		st.gpu_boost_broken = true;
		LOGW("gpu boost: no %s (%s), feature off", p, strerror(r < 0 ? -r : EINVAL));
		return;
	}
	snprintf(val, sizeof(val), "%ld", cfg.gpu_boost_hz);
	if (write_file(p, val) < 0) {
		st.gpu_boost_broken = true;
		LOGW("gpu boost: cannot write %s to %s, feature off", val, p);
		return;
	}
	st.gpu_boosted = true;
	st.gpu_boosts++;
	LOGD("gpu boost: min_freq %s for %d ms (%s)", val, ms, how);
}

/* the poll timeout: the configured period, or sooner if a boost expires */
static int gpu_boost_poll_ms(int poll_ms)
{
	double left;

	if (!st.gpu_boosted)
		return poll_ms;
	left = -since(&st.gpu_boost_until);
	if (left <= 0) {
		gpu_boost_end();
		return poll_ms;
	}
	if (left * 1000 < poll_ms)
		return (int)(left * 1000) + 1;
	return poll_ms;
}

static void read_input(int i)
{
	struct input_event ev[32];
	ssize_t r = read(st.in_fds[i], ev, sizeof(ev));
	size_t k;
	bool input = false;

	if (r <= 0) {
		if (r < 0 && errno != EAGAIN) {
			/* the device went away (rescan picks it up again) */
			close(st.in_fds[i]);
			st.in_fds[i] = st.in_fds[--st.n_in];
			memset(&st.in_scan, 0, sizeof(st.in_scan));
		}
		return;
	}
	for (k = 0; k < (size_t)r / sizeof(ev[0]); k++)
		if (ev[k].type == EV_KEY || ev[k].type == EV_ABS || ev[k].type == EV_REL)
			input = true;
	if (input)
		gpu_boost_start(0, "input");
}

/* ---- ModemManager over D-Bus ---------------------------------------------- */

#ifdef HAVE_DBUS
static DBusConnection *dbus;

static struct call *find_call(const char *p)
{
	int i;

	for (i = 0; i < MAX_CALLS; i++)
		if (st.calls[i].used && !strcmp(st.calls[i].path, p))
			return &st.calls[i];
	return NULL;
}

static int mm_call_state(const char *p)
{
	DBusMessage *m, *r;
	DBusMessageIter it, var;
	const char *iface = "org.freedesktop.ModemManager1.Call", *prop = "State";
	int state = -1;

	m = dbus_message_new_method_call("org.freedesktop.ModemManager1", p,
					 "org.freedesktop.DBus.Properties", "Get");
	if (!m)
		return -1;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
				 DBUS_TYPE_INVALID);
	r = dbus_connection_send_with_reply_and_block(dbus, m, 1000, NULL);
	dbus_message_unref(m);
	if (!r)
		return -1;
	if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&it, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_INT32)
			dbus_message_iter_get_basic(&var, &state);
	}
	dbus_message_unref(r);
	return state;
}

static void call_update(const char *p, int state)
{
	struct call *c = find_call(p);
	int i;

	if (!c) {
		for (i = 0; i < MAX_CALLS; i++)
			if (!st.calls[i].used) {
				c = &st.calls[i];
				break;
			}
		if (!c)
			return;
		c->used = true;
		snprintf(c->path, sizeof(c->path), "%s", p);
	}
	c->state = state;
	LOGI("call %s state %d", p, state);
	if (state >= 1 && state <= 6) {
		snprintf(st.awake_reason, sizeof(st.awake_reason), "call");
		broadcast("call %s %d", p, state);
	}
	if (state == 7 || state == 0) {
		c->used = false;
		/* let the UI finish with the call before the screen goes off */
		in_seconds(&st.awake_until, cfg.wake_grace_modem);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "call ended");
	}
}

static DBusHandlerResult mm_filter(DBusConnection *c, DBusMessage *m, void *data)
{
	const char *p;

	(void)c;
	(void)data;
	if (dbus_message_is_signal(m, "org.freedesktop.ModemManager1.Modem.Voice", "CallAdded")) {
		if (dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID))
			call_update(p, mm_call_state(p));
	} else if (dbus_message_is_signal(m, "org.freedesktop.ModemManager1.Modem.Voice", "CallDeleted")) {
		if (dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID))
			call_update(p, 7);
	} else if (dbus_message_is_signal(m, "org.freedesktop.ModemManager1.Call", "StateChanged")) {
		dbus_int32_t old, new, reason;

		if (dbus_message_get_args(m, NULL, DBUS_TYPE_INT32, &old, DBUS_TYPE_INT32, &new,
					  DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID))
			call_update(dbus_message_get_path(m), new);
	} else if (dbus_message_is_signal(m, "org.freedesktop.ModemManager1.Modem.Messaging", "Added")) {
		LOGI("SMS received: staying awake %d s", cfg.wake_grace_modem);
		in_seconds(&st.awake_until, cfg.wake_grace_modem);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "sms");
		broadcast("sms");
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.Local", "Disconnected")) {
		LOGW("D-Bus connection lost");
		st.mm_ok = false;
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void mm_connect(void)
{
	DBusError err;
	static const char * const matches[] = {
		"type='signal',interface='org.freedesktop.ModemManager1.Modem.Voice'",
		"type='signal',interface='org.freedesktop.ModemManager1.Call'",
		"type='signal',interface='org.freedesktop.ModemManager1.Modem.Messaging',member='Added'",
	};
	size_t i;

	if (st.mm_ok || !cfg.dbus_modem || since(&st.mm_retry) < 0)
		return;
	in_seconds(&st.mm_retry, 30);
	dbus_error_init(&err);
	if (dbus) {
		dbus_connection_close(dbus);
		dbus_connection_unref(dbus);
		dbus = NULL;
	}
	dbus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
	if (!dbus) {
		LOGW("system bus: %s", err.message ? err.message : "unavailable");
		dbus_error_free(&err);
		return;
	}
	dbus_connection_set_exit_on_disconnect(dbus, FALSE);
	for (i = 0; i < sizeof(matches) / sizeof(matches[0]); i++)
		dbus_bus_add_match(dbus, matches[i], NULL);
	dbus_connection_add_filter(dbus, mm_filter, NULL, NULL);
	dbus_connection_flush(dbus);
	st.mm_ok = true;
	LOGI("watching ModemManager calls and messages on the system bus");
}

static void mm_poll(void)
{
	if (!st.mm_ok || !dbus)
		return;
	if (!dbus_connection_read_write_dispatch(dbus, 0))
		st.mm_ok = false;
	while (dbus_connection_get_dispatch_status(dbus) == DBUS_DISPATCH_DATA_REMAINS)
		dbus_connection_dispatch(dbus);
}

static int mm_fd(void)
{
	int fd = -1;

	if (st.mm_ok && dbus && dbus_connection_get_unix_fd(dbus, &fd))
		return fd;
	return -1;
}
#else
static void mm_connect(void) {}
static void mm_poll(void) {}
static int mm_fd(void) { return -1; }
#endif

/* ---- suspend -------------------------------------------------------------- */

static void on_alarm(int sig) { (void)sig; alarm_flag = 1; }
static void on_stop(int sig) { (void)sig; stop_flag = 1; }

/* the wakeup_count protocol: -EBUSY when a wakeup event is in flight */
static int wakeup_count_handshake(void)
{
	char buf[32];
	int fd, r;
	ssize_t n;

	fd = open(path("/sys/power/wakeup_count"), O_RDWR);
	if (fd < 0)
		return -errno;
	alarm_flag = 0;
	alarm(3);
	n = read(fd, buf, sizeof(buf) - 1);
	alarm(0);
	if (n <= 0) {
		close(fd);
		if (alarm_flag || errno == EINTR)
			return -EBUSY;
		return -errno;
	}
	buf[n] = 0;
	if (write(fd, buf, strlen(buf)) < 0) {
		r = -errno;
		close(fd);
		/* EINVAL: a wakeup event happened since the read */
		return r == -EINVAL ? -EBUSY : r;
	}
	close(fd);
	return 0;
}

static enum wake_class classify(const char *sources, const char *irq, const char *spm,
				bool *alarm_due, bool *maint_due)
{
	time_t now = time(NULL);
	bool rtc = false, user = false, modem = false, network = false;

	*alarm_due = *maint_due = false;
	if (st.armed_alarm && now >= st.armed_alarm - 1) {
		if (st.maintenance_armed)
			*maint_due = true;
		else
			*alarm_due = true;
	}
	if (list_has(cfg.class_rtc, sources) || list_has(cfg.class_rtc, irq))
		rtc = true;
	if (list_has(cfg.class_user, sources) || list_has(cfg.class_user, irq))
		user = true;
	if (list_has(cfg.class_modem, sources) || list_has(cfg.class_modem, irq) ||
	    strstr(spm, "CCIF") || strstr(spm, "MD2AP") || strstr(spm, "MD1_WDT"))
		modem = true;
	if (list_has(cfg.class_network, sources) || list_has(cfg.class_network, irq) ||
	    strstr(spm, "CONN2AP"))
		network = true;

	if (user)
		return WAKE_USER;
	if (modem)
		return WAKE_MODEM;
	if (network)
		return WAKE_NETWORK;
	if (*maint_due && (rtc || !sources[0]))
		return WAKE_MAINTENANCE;
	if (*alarm_due && (rtc || !sources[0]))
		return WAKE_ALARM;
	if (rtc)
		return *maint_due ? WAKE_MAINTENANCE : *alarm_due ? WAKE_ALARM : WAKE_OTHER;
	return WAKE_OTHER;
}

static void fire_alarms(void)
{
	time_t now = time(NULL);
	int i;

	for (i = 0; i < MAX_ALARMS; i++) {
		struct alarm *a = &st.alarms[i];

		if (!a->used || a->when > now)
			continue;
		LOGI("alarm %s fired", a->name);
		setenv("T2_ALARM", a->name, 1);
		run_hooks("wake-alarm", cfg.hook_timeout);
		broadcast("alarm %s %s", a->name, a->wake_ui ? "ui" : "quiet");
		if (a->client >= 0 && st.clients[a->client].fd >= 0)
			client_send(&st.clients[a->client], "alarm-fired %s\n", a->name);
		if (a->wake_ui) {
			in_seconds(&st.awake_until, cfg.wake_grace_alarm);
			snprintf(st.awake_reason, sizeof(st.awake_reason), "alarm %s", a->name);
		}
		a->used = false;
	}
}

static void maintenance_window(void)
{
	struct timespec start;
	int lvl = st.maintenance_level;

	now_mono(&start);
	LOGI("maintenance window %d s (level %d)", cfg.maintenance_window, lvl);
	setenv("T2_MAINTENANCE_LEVEL", (char[]){ '0' + (lvl % 10), 0 }, 1);
	broadcast("maintenance");
	run_hooks("maintenance", cfg.maintenance_window);
	if (st.maintenance_level + 1 < cfg.n_maintenance)
		st.maintenance_level++;
	/* whatever the hooks took, the window is at least this long */
	in_seconds(&st.awake_until, cfg.maintenance_window - (int)since(&start) > 0 ?
		   cfg.maintenance_window - (int)since(&start) : 1);
	snprintf(st.awake_reason, sizeof(st.awake_reason), "maintenance");
}

static int do_suspend(bool force)
{
	char b[160], sources[256], irq[64], spm[256];
	const char *blk = force ? NULL : blocker(b, sizeof(b));
	enum wake_class cls;
	bool alarm_due, maint_due, deepest;
	unsigned long slept_ms;
	struct timespec t0, t1, b0, b1;
	double total, transitions, asleep;
	int r;

	if (blk) {
		LOGD("not suspending: %s", blk);
		return -EBUSY;
	}
	setenv("T2_WAKE_CLASS", "", 1);
	run_hooks("pre-suspend", cfg.hook_timeout);
	/* a dry run leaves a pending RTC alarm to whoever tests the real sleep */
	if (!cfg.dry_run || !foreign_alarm_pending())
		arm_before_sleep();
	snapshot_ws();
	r = wakeup_count_handshake();
	if (r) {
		LOGI("not suspending: %s", r == -EBUSY ? "a wakeup event is being handled" :
		     strerror(-r));
		rtc_disarm();
		/* do not spin on it: the event has a few seconds to settle */
		in_seconds(&st.awake_until, 2);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "wakeup event");
		return r;
	}
	write_file("/sys/power/mem_sleep", "s2idle");
	sync();
	LOGI("suspend (%s%s)", st.armed_alarm ? "RTC alarm for " : "no RTC alarm",
	     st.armed_alarm ? st.armed_name : "");
	broadcast("suspend");
	write_status_file();
	now_mono(&t0);
	now_boot(&b0);
	if (cfg.dry_run) {
		LOGI("dry run: not writing /sys/power/state");
		sleep(2);	/* room for a fake wake-up source to fire */
		r = 0;
	} else {
		r = write_file("/sys/power/state", "mem");
	}
	now_mono(&t1);
	now_boot(&b1);
	if (cfg.dry_run)
		b1.tv_sec += cfg.dry_run_asleep;
	st.last_resume = time(NULL);
	now_boot(&st.last_resume_boot);	/* the real clock, also in a dry run */
	/* BOOTTIME runs on in suspend, MONOTONIC does not: the difference is the
	 * time asleep, MONOTONIC alone the time entering and leaving it */
	total = ts_diff(&b1, &b0);
	transitions = ts_diff(&t1, &t0);
	asleep = total > transitions ? total - transitions : 0;
	if (!r)
		st.asleep_total += asleep;
	if (r) {
		st.suspend_fail++;
		LOGW("suspend failed: %s", strerror(-r));
	} else {
		st.suspends++;
		st.kernel_suspends = kernel_suspend_count();
	}
	rtc_disarm();

	/* what woke us */
	diff_ws(sources, sizeof(sources));
	wake_irq_name(irq, sizeof(irq));
	spm_wake(spm, sizeof(spm), &slept_ms, &deepest);
	cls = classify(sources, irq, spm, &alarm_due, &maint_due);
	st.last_class = cls;
	st.wakes[cls]++;
	snprintf(st.last_sources, sizeof(st.last_sources), "%s%s%s", sources,
		 sources[0] && irq[0] ? " " : "", irq);
	snprintf(st.last_spm, sizeof(st.last_spm), "%s", spm);
	st.last_slept_ms = slept_ms;
	LOGI("resumed after %.1f s asleep (%.1f s in transitions): %s wake (sources: %s; spm: %s; slept %lu ms, deepest %s)",
	     asleep, transitions, wake_class_names[cls], st.last_sources[0] ? st.last_sources : "-",
	     spm[0] ? spm : "-", slept_ms, deepest ? "yes" : "no");
	broadcast("wake %s %s", wake_class_names[cls], st.last_sources);

	setenv("T2_WAKE_CLASS", wake_class_names[cls], 1);
	setenv("T2_WAKE_SOURCES", st.last_sources, 1);
	setenv("T2_WAKE_SPM", spm, 1);
	snprintf(b, sizeof(b), "%lu", slept_ms);
	setenv("T2_SLEPT_MS", b, 1);
	run_hooks("post-resume", cfg.hook_timeout);

	/* an unexplained short sleep: back off before trying again. The whole
	 * span on BOOTTIME: on MONOTONIC every sleep looked short (it stands
	 * still in suspend), so the back-off also grew after long sleeps */
	if (!r && cls == WAKE_OTHER && !sources[0] && total < cfg.short_sleep) {
		if (st.backoff_level + 1 < cfg.n_backoff)
			st.backoff_level++;
	} else if (!r) {
		st.backoff_level = 0;
	}

	switch (cls) {
	case WAKE_USER:
		in_seconds(&st.awake_until, cfg.wake_grace_user);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "user");
		st.maintenance_level = 0;
		run_hooks("wake-user", cfg.hook_timeout);
		break;
	case WAKE_MODEM:
		/*
		 * Most modem wake-ups are indications nobody waits for
		 * (registration, signal, IMS): stay up only for the readers
		 * (the kernel holds every packet 500 ms). A call or an SMS from
		 * ModemManager extends this to wake_grace_modem, a call holds a
		 * lease.
		 */
		in_seconds(&st.awake_until, cfg.wake_grace_modem_quiet > 0 ?
			   cfg.wake_grace_modem_quiet : cfg.wake_grace_modem);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "modem");
		run_hooks("wake-modem", cfg.hook_timeout);
		break;
	case WAKE_NETWORK:
		/* a Wi-Fi frame or a Bluetooth event: let the apps handle it */
		in_seconds(&st.awake_until, cfg.wake_grace_network);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "network");
		run_hooks("wake-network", cfg.hook_timeout);
		break;
	case WAKE_MAINTENANCE:
		maintenance_window();
		break;
	case WAKE_ALARM:
		in_seconds(&st.awake_until, cfg.wake_grace_other);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "alarm");
		break;
	default:
		in_seconds(&st.awake_until, cfg.n_backoff ? cfg.backoff[st.backoff_level] :
			   cfg.wake_grace_other);
		snprintf(st.awake_reason, sizeof(st.awake_reason), "other");
		run_hooks("wake-other", cfg.hook_timeout);
		break;
	}
	fire_alarms();
	st.battery_check_due = true;
	if (r == -EBUSY || r == -EINVAL)
		r = 0;	/* aborted by a wakeup event: a wake, not an error */
	write_status_file();
	return r;
}

/*
 * A suspend done by someone else (t2-pmtest, a shell writing /sys/power/state):
 * the kernel's success counter moves without us. Note the resume time, so the
 * power key that woke the phone is not taken for a "screen off" press.
 */
static long kernel_suspend_count(void)
{
	char b[32];

	if (read_file(path("/sys/power/suspend_stats/success"), b, sizeof(b)) < 0)
		return -1;
	return strtol(b, NULL, 10);
}

static void poll_foreign_resume(void)
{
	long n = kernel_suspend_count();

	if (n < 0 || n == st.kernel_suspends)
		return;
	if (st.kernel_suspends >= 0) {
		st.last_resume = time(NULL);
		now_boot(&st.last_resume_boot);
		LOGI("resume from a suspend not done by t2-powerd (%ld kernel suspends)", n);
	}
	st.kernel_suspends = n;
}

/* ---- main loop --------------------------------------------------------------- */

static void loop(void)
{
	struct pollfd pfd[MAX_CLIENTS + MAX_INPUTS + 3];
	int i, n, fd;

	while (!stop_flag) {
		char b[160];
		const char *blk;

		expire_leases();
		mm_connect();
		poll_screen();
		poll_foreign_resume();
		fire_alarms();
		battery_check();
		if (cfg.power_key && st.key_fd < 0)
			st.key_fd = open_power_key();
		if (cfg.gpu_boost && !st.gpu_boost_broken)
			open_input_devices();
		if (st.screen_off)
			gpu_boost_end();

		if (st.screen_off && since_boot(&st.screen_off_since) >= cfg.suspend_delay) {
			blk = blocker(b, sizeof(b));
			if (!blk)
				do_suspend(false);
			else
				LOGD("screen off, not sleeping: %s", blk);
		}

		n = 0;
		pfd[n].fd = st.listen_fd;
		pfd[n++].events = POLLIN;
		fd = mm_fd();
		if (fd >= 0) {
			pfd[n].fd = fd;
			pfd[n++].events = POLLIN;
		}
		if (st.key_fd >= 0) {
			pfd[n].fd = st.key_fd;
			pfd[n++].events = POLLIN;
		}
		for (i = 0; i < st.n_in; i++) {
			pfd[n].fd = st.in_fds[i];
			pfd[n++].events = POLLIN;
		}
		for (i = 0; i < MAX_CLIENTS; i++)
			if (st.clients[i].fd >= 0) {
				pfd[n].fd = st.clients[i].fd;
				pfd[n++].events = POLLIN;
			}
		if (poll(pfd, n, gpu_boost_poll_ms(cfg.poll_ms)) < 0) {
			if (errno == EINTR)
				continue;
			LOGE("poll: %s", strerror(errno));
			break;
		}
		for (i = 0; i < n; i++) {
			if (!pfd[i].revents)
				continue;
			if (pfd[i].fd == st.listen_fd) {
				int c = accept4(st.listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
				int j;

				if (c < 0)
					continue;
				for (j = 0; j < MAX_CLIENTS && st.clients[j].fd >= 0; j++)
					;
				if (j == MAX_CLIENTS) {
					close(c);
					continue;
				}
				st.clients[j].fd = c;
				st.clients[j].len = 0;
				st.clients[j].subscribed = false;
			} else if (pfd[i].fd == fd && fd >= 0) {
				mm_poll();
			} else if (pfd[i].fd == st.key_fd) {
				read_power_key();
			} else {
				int j;
				bool is_input = false;

				for (j = 0; j < st.n_in; j++)
					if (st.in_fds[j] == pfd[i].fd) {
						read_input(j);
						is_input = true;
						break;
					}
				if (is_input)
					continue;
				for (j = 0; j < MAX_CLIENTS; j++)
					if (st.clients[j].fd == pfd[i].fd) {
						client_read(j);
						break;
					}
			}
		}
		mm_poll();
		gpu_boost_poll_ms(cfg.poll_ms);	/* expire a boost */
	}
	gpu_boost_end();
}

/* ---- t2-powerctl: a client of the socket -------------------------------------- */

static int ctl(int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), i;
	char line[LINE_MAX_LEN], buf[4096];
	size_t len = 0;
	ssize_t r;
	bool hold = false;

	if (fd < 0)
		return 1;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", cfg.sock_path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "t2-powerctl: %s: %s\n", cfg.sock_path, strerror(errno));
		return 1;
	}
	for (i = 0; i < argc; i++)
		len += snprintf(line + len, sizeof(line) - len, "%s%s", i ? " " : "", argv[i]);
	/* "inhibit NAME ... -- CMD ARGS": hold the lease while CMD runs */
	hold = argc > 0 && !strcmp(argv[0], "inhibit");
	if (hold) {
		char *dash = strstr(line, " -- ");

		if (dash) {
			*dash = 0;
			len = strlen(line);
		}
	}
	line[len++] = '\n';
	if (write(fd, line, len) < 0)
		return 1;
	if (hold) {
		int cmdi;

		for (cmdi = 1; cmdi < argc && strcmp(argv[cmdi], "--"); cmdi++)
			;
		r = read(fd, buf, sizeof(buf) - 1);
		if (r <= 0 || strncmp(buf, "ok", 2)) {
			fprintf(stderr, "t2-powerctl: lease refused\n");
			return 1;
		}
		if (cmdi + 1 < argc) {
			pid_t pid = fork();
			int status = 0;

			if (!pid) {
				execvp(argv[cmdi + 1], argv + cmdi + 1);
				_exit(127);
			}
			waitpid(pid, &status, 0);
			close(fd);
			return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		}
		/* no command: hold until stdin closes */
		while (read(0, buf, sizeof(buf)) > 0)
			;
		close(fd);
		return 0;
	}
	if (argc > 0 && !strcmp(argv[0], "subscribe")) {
		while ((r = read(fd, buf, sizeof(buf))) > 0)
			if (write(1, buf, r) < 0)
				break;
		return 0;
	}
	while ((r = read(fd, buf, sizeof(buf) - 1)) > 0) {
		char *end;

		buf[r] = 0;
		fputs(buf, stdout);
		end = strstr(buf, "ok");
		if ((end && (end == buf || end[-1] == '\n')) || strstr(buf, "error"))
			break;
	}
	close(fd);
	return 0;
}

static void usage(void)
{
	fputs("t2-powerd [-c CONF] [-s SOCKET] [-r ROOT] [-n] [-d] [-f]\n"
	      "  -c CONF    configuration file (" DEFAULT_CONF ")\n"
	      "  -s SOCKET  control socket (" DEFAULT_SOCK ")\n"
	      "  -r ROOT    prefix for /sys, /proc and /dev (tests)\n"
	      "  -n         dry run: never write /sys/power/state\n"
	      "  -d         debug log\n"
	      "  -f         foreground: log to stderr (default: syslog, unless a terminal)\n"
	      "t2-powerctl COMMAND...   (or: t2-powerd ctl COMMAND...)\n"
	      "  status | suspend [force] | screen off|on | activity | subscribe\n"
	      "  inhibit NAME [REASON] [-- CMD...]  hold a lease while CMD runs\n"
	      "  block NAME SECONDS [REASON] | unblock NAME\n"
	      "  alarm set NAME +SEC|EPOCH [quiet] | alarm clear NAME | alarm list\n"
	      "  reload\n", stderr);
}

int main(int argc, char **argv)
{
	const char *base = strrchr(argv[0], '/');
	int opt, i;

	base = base ? base + 1 : argv[0];
	log_stderr = isatty(2);
	if (getenv("T2_POWERD_SOCK"))
		snprintf(cfg.sock_path, sizeof(cfg.sock_path), "%s", getenv("T2_POWERD_SOCK"));
	/* options first, then "ctl COMMAND..." (or the t2-powerctl name) */
	while ((opt = getopt(argc, argv, "+c:s:r:ndfhV")) != -1) {
		switch (opt) {
		case 'c': snprintf(cfg.conf_path, sizeof(cfg.conf_path), "%s", optarg); break;
		case 's': snprintf(cfg.sock_path, sizeof(cfg.sock_path), "%s", optarg); break;
		case 'r': snprintf(cfg.root, sizeof(cfg.root), "%s", optarg); break;
		case 'n': cfg.dry_run = true; break;
		case 'd': cfg.log_level = LOG_DEBUG; break;
		case 'f': log_stderr = true; break;
		case 'V': puts("t2-powerd " VERSION); return 0;
		default: usage(); return 1;
		}
	}
	if (!strcmp(base, "t2-powerctl"))
		return ctl(argc - optind, argv + optind);
	if (optind < argc && !strcmp(argv[optind], "ctl"))
		return ctl(argc - optind - 1, argv + optind + 1);
	if (optind < argc) {
		usage();
		return 1;
	}
	openlog("t2-powerd", LOG_PID, LOG_DAEMON);
	if (load_config(cfg.conf_path) < 0)
		LOGI("no %s, using the defaults", cfg.conf_path);
	if (cfg.poll_ms < 100)
		cfg.poll_ms = 100;

	memset(&st, 0, sizeof(st));
	for (i = 0; i < MAX_CLIENTS; i++)
		st.clients[i].fd = -1;
	st.key_fd = -1;
	st.n_in = 0;
	st.kernel_suspends = -1;
	st.battery_capacity = -1;
	now_boot(&st.screen_off_since);
	mkdir(cfg.run_dir, 0755);
	st.listen_fd = listen_socket();
	if (st.listen_fd < 0) {
		LOGE("cannot listen on %s: %s", cfg.sock_path, strerror(-st.listen_fd));
		return 1;
	}
	{
		/* SIGALRM must interrupt the blocking read of wakeup_count */
		struct sigaction sa = { .sa_handler = on_alarm };

		sigemptyset(&sa.sa_mask);
		sigaction(SIGALRM, &sa, NULL);
		sa.sa_handler = on_stop;
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
	}
	signal(SIGPIPE, SIG_IGN);

	find_connector();
	snapshot_ws();
	LOGI("t2-powerd " VERSION ": suspend %d s after screen off, maintenance %s, modem watch %s%s",
	     cfg.suspend_delay, cfg.n_maintenance ? "on" : "off",
	     cfg.dbus_modem ? "on" : "off", cfg.dry_run ? ", DRY RUN" : "");
	write_status_file();
	loop();
	rtc_disarm();
	for (i = 0; i < MAX_CLIENTS; i++)
		client_close(i);
	close(st.listen_fd);
	unlink(cfg.sock_path);
	LOGI("stopped");
	return 0;
}
