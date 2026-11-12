// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-hal: loading legacy Android HAL modules in-process through
 * bionic_translation (see t2-hal.h). Shared by titan2-fingerprint (t2-fp,
 * t2-fpd) and titan2-gnss (t2-gnssd).
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "t2-hal.h"

/* bionic_translation (libdl_bio); it installs no header */
void *bionic_dlopen(const char *filename, int flag);
void *bionic_dlsym(void *handle, const char *symbol);
const char *bionic_dlerror(void);
#ifndef RTLD_NOW
#define RTLD_NOW 2
#endif

const char *t2_prog = "t2-hal";

uint64_t t2_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void t2_mkdirs(const char *path, unsigned mode)
{
	char buf[512], *p;

	snprintf(buf, sizeof(buf), "%s", path);
	for (p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(buf, mode);
			*p = '/';
		}
	mkdir(buf, mode);
}

int t2_vendor_reexec(const char *root, int diag, char **argv)
{
	char props[512];

	if (getenv("BIONIC_VENDOR_ROOT") && !strcmp(getenv("BIONIC_VENDOR_ROOT"), root))
		return 0;
	setenv("BIONIC_VENDOR_ROOT", root, 1);
	snprintf(props, sizeof(props), "%s/t2-props", root);
	setenv("BIONIC_PROPS_FILE", props, 0);
	if (diag) {
		setenv("LINKER_DIE_AT_RUNTIME", "backtrace", 1);
		setenv("LINKER_PRINT_MISSING_SYMBOLS_AND_DIE", "", 1);
	}
	execv("/proc/self/exe", argv);
	fprintf(stderr, "%s: execv: %s\n", t2_prog, strerror(errno));
	return -1;
}

void *t2_open_hal(const char *lib, const char *id, struct hw_device_t **devp)
{
	void *h;
	struct hw_module_t *hmi;
	int rc;

	printf("%s: loading %s\n", t2_prog, lib);
	h = bionic_dlopen(lib, RTLD_NOW);
	if (!h) {
		fprintf(stderr, "%s: dlopen %s: %s\n", t2_prog, lib, bionic_dlerror() ?: "?");
		return NULL;
	}
	hmi = bionic_dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
	if (!hmi) {
		fprintf(stderr, "%s: %s: no HMI symbol: %s\n", t2_prog, lib, bionic_dlerror() ?: "?");
		return NULL;
	}
	if (hmi->tag != HARDWARE_MODULE_TAG) {
		fprintf(stderr, "%s: %s: HMI tag %#x is not a hw_module_t\n", t2_prog, lib, hmi->tag);
		return NULL;
	}
	printf("%s: module '%s' id %s api %u.%u hal %u.%u author '%s'\n", t2_prog,
	       hmi->name ?: "", hmi->id ?: "", hmi->module_api_version >> 8,
	       hmi->module_api_version & 0xff, hmi->hal_api_version >> 8,
	       hmi->hal_api_version & 0xff, hmi->author ?: "");
	if (hmi->id && strncmp(hmi->id, id, strlen(id)))
		fprintf(stderr, "%s: warning: module id '%s', expected '%s'\n", t2_prog, hmi->id, id);
	if (!hmi->methods || !hmi->methods->open) {
		fprintf(stderr, "%s: %s: module has no open method\n", t2_prog, lib);
		return NULL;
	}
	*devp = NULL;
	rc = hmi->methods->open(hmi, id, devp);
	if (rc || !*devp) {
		fprintf(stderr, "%s: open %s: %d (%s)\n", t2_prog, id, rc, strerror(-rc));
		return NULL;
	}
	printf("%s: %s device api %u.%u\n", t2_prog, id, (*devp)->version >> 8, (*devp)->version & 0xff);
	return h;
}
