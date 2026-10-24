// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 *
 * t2-reboot [-f] [MODE]: reboot with a reboot-mode argument, e.g.
 * "bootloader" (LK fastboot) or "recovery". busybox reboot cannot pass
 * one. The kernel (syscon-reboot-mode on the TOPRGU node, mt6878.dtsi)
 * turns MODE into TOPRGU 0x1c00a024[3:0] (bootloader 3, recovery 2, ...),
 * which LK reads on the next boot. No MODE (or "normal"): plain restart.
 *
 * Without -f: MODE goes to /run/t2-reboot-mode and PID 1 (busybox init)
 * gets SIGTERM, i.e. the normal reboot: the shutdown actions run (openrc
 * shutdown, mount-ro of the network root) and the last one,
 * /usr/lib/t2/reboot-mode, calls "t2-reboot -f MODE".
 * With -f: sync and reboot(2) right away (like "reboot -f").
 *
 * No libc, raw syscalls (aarch64), like tools/devmem.
 */
#define __NR_openat	56
#define __NR_close	57
#define __NR_write	64
#define __NR_sync	81
#define __NR_exit	93
#define __NR_kill	129
#define __NR_reboot	142
#define AT_FDCWD	-100
#define O_WRONLY	01
#define O_CREAT		0100
#define O_TRUNC		01000
#define SIGTERM		15

#define LINUX_REBOOT_MAGIC1	0xfee1deadL
#define LINUX_REBOOT_MAGIC2	672274793L
#define LINUX_REBOOT_CMD_RESTART	0x01234567L
#define LINUX_REBOOT_CMD_RESTART2	0xa1b2c3d4L

#define MODE_FILE	"/run/t2-reboot-mode"

static long sc(long n, long a, long b, long c, long d)
{
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;

	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static long len(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	return n;
}

static void say(const char *s)
{
	sc(__NR_write, 2, (long)s, len(s), 0);
}

static void __attribute__((noreturn)) die(const char *s, long code)
{
	say("t2-reboot: ");
	say(s);
	sc(__NR_exit, code, 0, 0, 0);
	__builtin_unreachable();
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* reboot-mode names: mode-<name> in DT, [a-z0-9-], kernel copies <= 255 */
static int valid(const char *m)
{
	long n = len(m), i;

	if (!n || n > 64)
		return 0;
	for (i = 0; i < n; i++)
		if (!((m[i] >= 'a' && m[i] <= 'z') || (m[i] >= '0' && m[i] <= '9') || m[i] == '-'))
			return 0;
	return 1;
}

static void __attribute__((noreturn)) now(const char *mode)
{
	long r;

	sc(__NR_sync, 0, 0, 0, 0);
	if (mode)
		r = sc(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		       LINUX_REBOOT_CMD_RESTART2, (long)mode);
	else
		r = sc(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		       LINUX_REBOOT_CMD_RESTART, 0);
	die(r == -1 ? "reboot: not permitted (root?)\n" : "reboot failed\n", 1);
}

__attribute__((used)) static void cmain(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	const char *mode = 0;
	int force = 0, i;
	long fd;

	for (i = 1; i < argc; i++) {
		if (same(argv[i], "-f"))
			force = 1;
		else if (argv[i][0] == '-' || mode)
			die("usage: t2-reboot [-f] [bootloader|recovery|...]\n"
			    "  modes: /sys/class/reboot-mode/*/reboot_modes\n", 2);
		else
			mode = argv[i];
	}
	if (mode && same(mode, "normal"))
		mode = 0;
	if (mode && !valid(mode))
		die("bad mode name\n", 2);
	if (force)
		now(mode);

	/* hand over to init: the shutdown hook reads the mode back */
	fd = sc(__NR_openat, AT_FDCWD, (long)MODE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		die("cannot write " MODE_FILE " (use -f)\n", 1);
	if (mode)
		sc(__NR_write, fd, (long)mode, len(mode), 0);
	sc(__NR_close, fd, 0, 0, 0);
	if (sc(__NR_kill, 1, SIGTERM, 0, 0))
		die("cannot signal init (use -f)\n", 1);
	sc(__NR_exit, 0, 0, 0, 0);
}

asm(".global _start\n_start:\n mov x0, sp\n b cmain\n");
