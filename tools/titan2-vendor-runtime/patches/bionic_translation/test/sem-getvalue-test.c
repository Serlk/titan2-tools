// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
/*
 * bionic_sem_getvalue of bionic_translation's pthread wrapper must report the count of the
 * same host semaphore that bionic_sem_init/post/wait/trywait/timedwait work on, and must not
 * change it. Compiled together with pthread_wrapper/libpthread.c (bionic_* entry points called
 * directly), like pthread-lazy-test.c. This is the wrapper half; the route a bionic library's
 * sem_getvalue import takes through the loader is checked by tools/fp/t2-fp-semtest against
 * the installed packages.
 *
 *   1. init 0: two reads give 0; one post: two reads give 1; wait: 0
 *   2. two semaphores do not share a count; an initial value is reported
 *   3. trywait/timedwait on an empty semaphore fail and leave the count at 0
 *   4. a zeroed, never initialized semaphore (lazy path) reads 0, then counts
 *   5. post only while the count is below one: three notifications make one pending post, the
 *      waiter takes it, the next notification posts again
 *   6. a thread already waiting is woken by exactly one post; the second waiter by the next
 *   7. all of it again on fresh objects, every other round at 4-byte-aligned addresses
 * The zeroed semaphore of step 4 is always 8-byte aligned: the lazy path publishes the host
 * pointer with an atomic store, and what that does at a 4-byte-aligned address is
 * pthread-lazy-test.c's subject, not this test's.
 * Exit 0 = all passed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../pthread_wrapper/libpthread.c"

#define ROUNDS	50

static int failures;

#define CHECK(cond, ...) do { \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

static int val(bionic_sem_t *s)
{
	int v = -12345;

	CHECK(bionic_sem_getvalue(s, &v) == 0, "sem_getvalue failed, errno %d", errno);
	return v;
}

/* the caller's side of a coalesced notification: post only when nothing is pending */
static void notify(bionic_sem_t *s)
{
	if (val(s) < 1)
		bionic_sem_post(s);
}

static volatile int woken;

static void *waiter(void *arg)
{
	if (bionic_sem_wait(arg) == 0)
		__atomic_add_fetch(&woken, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void settle(void)
{
	struct timespec ts = { 0, 30 * 1000 * 1000 };

	nanosleep(&ts, NULL);
}

static void one_round(bionic_sem_t *a, bionic_sem_t *b, bionic_sem_t *lazy)
{
	struct timespec past;
	pthread_t t1, t2;
	int w;

	/* 1 */
	CHECK(bionic_sem_init(a, 0, 0) == 0, "sem_init");
	CHECK(val(a) == 0 && val(a) == 0, "fresh semaphore reads %d", val(a));
	CHECK(bionic_sem_post(a) == 0, "sem_post");
	CHECK(val(a) == 1 && val(a) == 1, "after one post reads %d", val(a));
	CHECK(bionic_sem_wait(a) == 0, "sem_wait");
	CHECK(val(a) == 0, "after the wait reads %d", val(a));

	/* 2 */
	CHECK(bionic_sem_init(b, 0, 3) == 0, "sem_init");
	CHECK(val(b) == 3 && val(a) == 0, "a %d b %d, expected 0 and 3", val(a), val(b));
	bionic_sem_post(a);
	CHECK(val(a) == 1 && val(b) == 3, "a %d b %d, expected 1 and 3", val(a), val(b));
	bionic_sem_wait(b);
	CHECK(val(a) == 1 && val(b) == 2, "a %d b %d, expected 1 and 2", val(a), val(b));
	bionic_sem_wait(a);

	/* 3 */
	CHECK(bionic_sem_trywait(a) == -1, "trywait on an empty semaphore succeeded");
	clock_gettime(CLOCK_REALTIME, &past);
	CHECK(bionic_sem_timedwait(a, &past) == -1, "timedwait on an empty semaphore succeeded");
	CHECK(val(a) == 0, "failed waits left %d", val(a));

	/* 4 */
	memset(lazy, 0, sizeof(*lazy));
	CHECK(val(lazy) == 0, "zeroed semaphore reads %d", val(lazy));
	bionic_sem_post(lazy);
	CHECK(val(lazy) == 1, "zeroed semaphore after a post reads %d", val(lazy));
	CHECK(bionic_sem_trywait(lazy) == 0 && val(lazy) == 0, "zeroed semaphore after trywait");

	/* 5 */
	notify(a);
	notify(a);
	notify(a);
	CHECK(val(a) == 1, "three notifications left %d pending", val(a));
	CHECK(bionic_sem_wait(a) == 0 && val(a) == 0, "taking the pending notification");
	notify(a);
	CHECK(val(a) == 1, "a notification after the wait left %d", val(a));
	bionic_sem_wait(a);

	/* 6 */
	woken = 0;
	pthread_create(&t1, NULL, waiter, a);
	pthread_create(&t2, NULL, waiter, a);
	settle();
	CHECK(woken == 0 && val(a) == 0, "waiters ran without a post (woken %d, count %d)", woken, val(a));
	notify(a);
	for (w = 0; w < 100 && woken < 1; w++)
		settle();
	settle();
	CHECK(woken == 1, "one post woke %d waiters", woken);
	CHECK(val(a) == 0, "count %d after the woken waiter", val(a));
	notify(a);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
	CHECK(woken == 2 && val(a) == 0, "second post: woken %d, count %d", woken, val(a));

	bionic_sem_destroy(a);
	bionic_sem_destroy(b);
	bionic_sem_destroy(lazy);
}

int main(void)
{
	/* offset 4 in an 8-aligned area: only 4-byte aligned, as int-aligned bionic objects can be */
	static char area[ROUNDS][2 * sizeof(bionic_sem_t) + 8] __attribute__((aligned(8)));
	static bionic_sem_t lazy[ROUNDS];

	for (int i = 0; i < ROUNDS && !failures; i++) {
		bionic_sem_t *s = (bionic_sem_t *)(area[i] + ((i & 1) ? 4 : 0));

		one_round(&s[0], &s[1], &lazy[i]);
	}
	printf("sem-getvalue-test: %d rounds, %d failure(s)\n", ROUNDS, failures);
	return failures ? 1 : 0;
}
