// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 *
 * devmem ADDR [VALUE]: read or write a 32-bit register through /dev/mem
 * (mmap, so it works for MMIO). No libc, raw syscalls, for the initramfs.
 */
#define __NR_write	64
#define __NR_openat	56
#define __NR_mmap	222
#define __NR_exit	93
#define AT_FDCWD	-100
#define O_RDWR		2
#define O_SYNC		04010000

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	register long x4 asm("x4") = e;
	register long x5 asm("x5") = f;

	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}

static unsigned long num(const char *s)
{
	unsigned long v = 0;
	int base = 10;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (; *s; s++) {
		int d = *s >= 'a' ? *s - 'a' + 10 : *s >= 'A' ? *s - 'A' + 10 : *s - '0';
		v = v * base + d;
	}
	return v;
}

static void out(unsigned int v)
{
	char b[11] = "0x";
	int i;

	for (i = 0; i < 8; i++)
		b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xf];
	b[10] = '\n';
	sc(__NR_write, 1, (long)b, 11, 0, 0, 0);
}

__attribute__((used)) static void cmain(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	unsigned long addr, page;
	volatile unsigned int *r;
	long fd, m;

	if (argc < 2)
		sc(__NR_exit, 2, 0, 0, 0, 0, 0);
	addr = num(argv[1]);
	page = addr & ~0xfffUL;
	fd = sc(__NR_openat, AT_FDCWD, (long)"/dev/mem", O_RDWR | O_SYNC, 0, 0, 0);
	if (fd < 0)
		sc(__NR_exit, 1, 0, 0, 0, 0, 0);
	/* PROT_READ|PROT_WRITE, MAP_SHARED */
	m = sc(__NR_mmap, 0, 0x1000, 3, 1, fd, page);
	if (m < 0 && m > -4096)
		sc(__NR_exit, 1, 0, 0, 0, 0, 0);
	r = (volatile unsigned int *)(m + (addr & 0xfff));
	if (argc > 2)
		*r = (unsigned int)num(argv[2]);
	out(*r);
	sc(__NR_exit, 0, 0, 0, 0, 0, 0);
}

asm(".global _start\n_start:\n mov x0, sp\n b cmain\n");
