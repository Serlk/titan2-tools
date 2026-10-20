// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 *
 * Minimal /init for the first mainline boots on Titan 2: no libc, raw
 * syscalls. Says hello on the console and in the kernel log (which pstore
 * keeps for Android to read), shows a status line every second for 30 s,
 * then reboots so the bootloader falls back to the working slot.
 */
#define __NR_write	64
#define __NR_openat	56
#define __NR_close	57
#define __NR_mkdirat	34
#define __NR_mount	40
#define __NR_nanosleep	101
#define __NR_reboot	142
#define AT_FDCWD	-100
#define O_WRONLY	1

static long sc(long n, long a, long b, long c, long d, long e)
{
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	register long x4 asm("x4") = e;

	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
	return x0;
}

static long len(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	return n;
}

static void say(int fd, const char *s)
{
	sc(__NR_write, fd, (long)s, len(s), 0, 0);
}

void _start(void)
{
	struct { long sec, nsec; } one = { 1, 0 };
	char line[] = "titan2 mini-init: 00 s\n";
	int kmsg, con, i;

	sc(__NR_mkdirat, AT_FDCWD, (long)"/dev", 0755, 0, 0);
	sc(__NR_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
	kmsg = sc(__NR_openat, AT_FDCWD, (long)"/dev/kmsg", O_WRONLY, 0, 0);
	con = sc(__NR_openat, AT_FDCWD, (long)"/dev/console", O_WRONLY, 0, 0);

	say(kmsg, "<2>titan2 mini-init: userspace reached\n");
	say(con, "\n\n  Titan 2: mainline kernel is up, /init running\n\n");
	for (i = 0; i < 30; i++) {
		line[19] = '0' + i / 10;
		line[20] = '0' + i % 10;
		say(con, line);
		sc(__NR_nanosleep, (long)&one, 0, 0, 0, 0);
	}
	say(kmsg, "<2>titan2 mini-init: rebooting\n");
	/* LINUX_REBOOT_MAGIC1/2, LINUX_REBOOT_CMD_RESTART */
	sc(__NR_reboot, 0xfee1dead, 672274793, 0x01234567, 0, 0);
	for (;;)
		;
}
