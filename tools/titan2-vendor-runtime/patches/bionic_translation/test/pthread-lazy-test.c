// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
/*
 * Stress test of bionic_translation's pthread wrapper with statically initialized, only
 * 4-byte-aligned bionic objects (as in vendor HALs: bionic mutex/cond are int32_t words).
 * Compiled together with pthread_wrapper/libpthread.c (bionic_* entry points called directly),
 * run on aarch64 (qemu-aarch64 with QEMU_CPU=cortex-a76: no FEAT_LSE2, so a misaligned
 * acquire/release or exclusive access faults as on the Titan 2's A78/A55).
 *
 *   1. first-use race on mutexes: NT threads start together on fresh zeroed mutexes and
 *      increment a plain counter under each; any lost increment = no mutual exclusion
 *   2. first-use race on condition variables: a waiter and a signaller start together on fresh
 *      zeroed cond+mutex pairs; a waiter that times out although the flag was set = lost wakeup
 *   3. statically initialized objects at 4-byte-aligned addresses (offset 4 in an 8-aligned
 *      buffer), as a HAL's .data/.bss can place them: must not SIGBUS
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

/* the wrapper's types and entry points (libpthread.c is compiled into this program) */
#include "../pthread_wrapper/libpthread.c"

#define NT	8
#define NOBJ	400
#define ITER	200

/* objects at offset 4 of an 8-aligned area: only 4-byte aligned, like int32_t[] in bionic */
struct mobj { bionic_mutex_t m; } __attribute__((packed, aligned(4)));
struct cobj { bionic_cond_t c; bionic_mutex_t m; volatile int flag; int32_t pad; } __attribute__((packed, aligned(4)));

static char mbuf[NOBJ * sizeof(struct mobj) + 16] __attribute__((aligned(8)));
static char cbuf[NOBJ * sizeof(struct cobj) + 16] __attribute__((aligned(8)));
static long counters[NOBJ];
static pthread_barrier_t start;
static int lost_wakeups, timeouts_ok;

static bionic_mutex_t *M(int i) { return &((struct mobj *)(mbuf + 4))[i].m; }
static struct cobj *C(int i) { return &((struct cobj *)(cbuf + 4))[i]; }

static void *mutex_worker(void *arg)
{
	(void)arg;
	pthread_barrier_wait(&start);
	for (int i = 0; i < NOBJ; i++)
		for (int k = 0; k < ITER / 20; k++) {
			bionic_pthread_mutex_lock(M(i));
			long v = counters[i];
			for (volatile int d = 0; d < 20; d++);
			counters[i] = v + 1;
			bionic_pthread_mutex_unlock(M(i));
		}
	return NULL;
}

static void *cond_waiter(void *arg)
{
	(void)arg;
	pthread_barrier_wait(&start);
	for (int i = 0; i < NOBJ; i++) {
		struct cobj *o = C(i);
		struct timespec ts;

		bionic_pthread_mutex_lock(&o->m);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 2;
		while (!o->flag) {
			if (bionic_pthread_cond_timedwait(&o->c, &o->m, &ts) == ETIMEDOUT) {
				if (o->flag)
					__atomic_add_fetch(&lost_wakeups, 1, __ATOMIC_RELAXED);
				else
					__atomic_add_fetch(&timeouts_ok, 1, __ATOMIC_RELAXED);
				break;
			}
		}
		bionic_pthread_mutex_unlock(&o->m);
	}
	return NULL;
}

static void *cond_signaller(void *arg)
{
	(void)arg;
	pthread_barrier_wait(&start);
	for (int i = 0; i < NOBJ; i++) {
		struct cobj *o = C(i);

		bionic_pthread_mutex_lock(&o->m);
		o->flag = 1;
		bionic_pthread_cond_signal(&o->c);
		bionic_pthread_mutex_unlock(&o->m);
	}
	return NULL;
}

int main(void)
{
	pthread_t t[NT];
	int fail = 0;

	printf("objects at %p (mutex) %p (cond): 4-byte aligned: %s\n", (void *)M(0), (void *)&C(0)->c,
	       ((uintptr_t)M(0) % 8 && (uintptr_t)&C(0)->c % 8 && (uintptr_t)&C(0)->m % 8) ? "yes" : "no");

	pthread_barrier_init(&start, NULL, NT);
	for (int i = 0; i < NT; i++)
		pthread_create(&t[i], NULL, mutex_worker, NULL);
	for (int i = 0; i < NT; i++)
		pthread_join(t[i], NULL);
	for (int i = 0; i < NOBJ; i++)
		if (counters[i] != NT * (ITER / 20)) {
			fail++;
			if (fail < 5)
				printf("mutex %d: count %ld, want %d\n", i, counters[i], NT * (ITER / 20));
		}
	printf("1. mutex first-use race: %d/%d objects with lost increments\n", fail, NOBJ);

	pthread_barrier_destroy(&start);
	pthread_barrier_init(&start, NULL, 2);
	pthread_create(&t[0], NULL, cond_waiter, NULL);
	pthread_create(&t[1], NULL, cond_signaller, NULL);
	pthread_join(t[0], NULL);
	pthread_join(t[1], NULL);
	printf("2. cond first-use race: %d lost wakeups, %d waits timed out with the flag unset\n",
	       lost_wakeups, timeouts_ok);

	printf("%s\n", fail || lost_wakeups || timeouts_ok ? "FAIL" : "PASS");
	return fail || lost_wakeups || timeouts_ok;
}
