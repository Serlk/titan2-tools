// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-propd: a minimal Android property service for the vendor container.
 *
 * Bionic processes read system properties straight from shared memory files
 * in /dev/__properties__ (one "prop_area" per SELinux context plus
 * properties_serial, and property_info that maps property names to those
 * files) and set them through the unix socket /dev/socket/property_service.
 * On Android both are provided by init; here nothing else does, and without
 * them e.g. libbinder's defaultServiceManager() spins forever waiting for
 * servicemanager.ready. This daemon creates the files with a single context
 * for every property, loads initial name=value lines from a file, and serves
 * the set protocol (PROP_MSG_SETPROP2 and the legacy prop_msg form).
 *
 * The on-disk formats and the update rules (dirty bit + backup area, serial
 * bumps, futex wakes) follow AOSP bionic libc/system_properties and
 * system/core/property_service/libpropertyinfoserializer (Apache-2.0); this is
 * a re-implementation of the writer side in C.
 *
 *   t2-propd [-d DIR] [-s SOCKET] [-f FILE]... [-D]
 *     -d DIR     property directory (default /dev/__properties__)
 *     -s SOCKET  property_service socket (default /dev/socket/property_service)
 *     -f FILE    name=value lines to load at start (may repeat)
 *     -D         do not daemonize; log sets to stderr
 *     -g NAME    print a property from DIR and exit (reader check)
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1			/* <linux/futex.h> is not in every build root */
#endif
#define PROP_NAME_MAX 32		/* legacy protocol limit only */
#define PROP_VALUE_MAX 92
#define PA_SIZE (128 * 1024)
#define PROP_AREA_MAGIC 0x504f5250
#define PROP_AREA_VERSION 0xfc6ed0ab
#define PROP_MSG_SETPROP 1
#define PROP_MSG_SETPROP2 0x00020001
#define PROP_SUCCESS 0
#define PROP_ERROR_READ_CMD 0x0004
#define PROP_ERROR_READ_DATA 0x0008
#define PROP_ERROR_READ_ONLY_PROPERTY 0x000B
#define PROP_ERROR_INVALID_NAME 0x0010
#define PROP_ERROR_INVALID_VALUE 0x0014
#define PROP_ERROR_INVALID_CMD 0x001B
#define PROP_ERROR_SET_FAILED 0x0024

#define ALIGN4(x) (((x) + 3) & ~3u)
#define CONTEXT "u:object_r:default_prop:s0"

/* ---- shared memory layouts (bionic prop_area.h / prop_info.h) ---- */

struct prop_area {
	uint32_t bytes_used;
	uint32_t serial;	/* atomic */
	uint32_t magic;
	uint32_t version;
	uint32_t reserved[28];
	char data[];
};

struct prop_bt {
	uint32_t namelen;
	uint32_t prop;		/* offsets from data[], atomic */
	uint32_t left;
	uint32_t right;
	uint32_t children;
	char name[];
};

struct prop_info {
	uint32_t serial;	/* atomic: value length << 24 | dirty bit 0 */
	char value[PROP_VALUE_MAX];
	char name[];
};

/* property_info (libpropertyinfoparser) */
struct property_entry {
	uint32_t name_offset, namelen, context_index, type_index;
};

struct trie_node {
	uint32_t property_entry;
	uint32_t num_child_nodes, child_nodes;
	uint32_t num_prefixes, prefix_entries;
	uint32_t num_exact_matches, exact_match_entries;
};

struct info_header {
	uint32_t current_version, minimum_supported_version, size;
	uint32_t contexts_offset, types_offset, root_offset;
};

static struct prop_area *area, *serial_area;
static int verbose;

#define ld(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define st_rel(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define st(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

static void futex_wake(uint32_t *addr)
{
	syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static void *to_obj(uint32_t off)
{
	return off ? area->data + off : NULL;
}

static void *alloc_obj(size_t size, uint32_t *off)
{
	size_t aligned = ALIGN4(size);

	if (area->bytes_used + aligned > PA_SIZE - sizeof(*area))
		return NULL;
	*off = area->bytes_used;
	area->bytes_used += aligned;
	return area->data + *off;
}

static struct prop_bt *new_bt(const char *name, uint32_t namelen, uint32_t *off)
{
	struct prop_bt *bt = alloc_obj(sizeof(*bt) + namelen + 1, off);

	if (!bt)
		return NULL;
	bt->namelen = namelen;
	memcpy(bt->name, name, namelen);
	bt->name[namelen] = 0;
	return bt;
}

static int cmp_name(const char *a, uint32_t alen, const char *b, uint32_t blen)
{
	if (alen < blen)
		return -1;
	if (alen > blen)
		return 1;
	return strncmp(a, b, alen);
}

/* binary tree of siblings, as prop_area::find_prop_bt */
static struct prop_bt *find_bt(struct prop_bt *cur, const char *name, uint32_t namelen, int alloc)
{
	while (cur) {
		int c = cmp_name(name, namelen, cur->name, cur->namelen);
		uint32_t *link, off;

		if (c == 0)
			return cur;
		link = c < 0 ? &cur->left : &cur->right;
		if (ld(link)) {
			cur = to_obj(ld(link));
			continue;
		}
		if (!alloc)
			return NULL;
		cur = new_bt(name, namelen, &off);
		if (cur)
			st_rel(link, off);
		return cur;
	}
	return NULL;
}

/* trie walk over '.'-separated tokens, as prop_area::find_property */
static struct prop_info *find_prop(const char *name, const char *value, int alloc)
{
	struct prop_bt *cur = (struct prop_bt *)area->data; /* root */
	const char *rest = name;
	uint32_t off;

	for (;;) {
		const char *sep = strchr(rest, '.');
		uint32_t len = sep ? (uint32_t)(sep - rest) : (uint32_t)strlen(rest);
		struct prop_bt *root;

		if (!len)
			return NULL;
		if (ld(&cur->children)) {
			root = to_obj(ld(&cur->children));
		} else if (alloc) {
			root = new_bt(rest, len, &off);
			if (root)
				st_rel(&cur->children, off);
		} else {
			return NULL;
		}
		if (!root)
			return NULL;
		cur = find_bt(root, rest, len, alloc);
		if (!cur)
			return NULL;
		if (!sep)
			break;
		rest = sep + 1;
	}
	if (ld(&cur->prop))
		return to_obj(ld(&cur->prop));
	if (!alloc)
		return NULL;
	{
		uint32_t namelen = strlen(name), vlen = strlen(value);
		struct prop_info *pi;

		if (vlen >= PROP_VALUE_MAX)
			return NULL;	/* long ro. values are not supported here */
		pi = alloc_obj(sizeof(*pi) + namelen + 1, &off);
		if (!pi)
			return NULL;
		memcpy(pi->name, name, namelen);
		pi->name[namelen] = 0;
		memcpy(pi->value, value, vlen);
		pi->value[vlen] = 0;
		st(&pi->serial, vlen << 24);
		st_rel(&cur->prop, off);
		return pi;
	}
}

static void bump_serial(struct prop_area *pa)
{
	st_rel(&pa->serial, ld(&pa->serial) + 1);
	futex_wake(&pa->serial);
}

/* SystemProperties::Update */
static int update_prop(struct prop_info *pi, const char *value)
{
	uint32_t len = strlen(value), serial = ld(&pi->serial), old_len = serial >> 24;

	if (len >= PROP_VALUE_MAX)
		return -1;
	memcpy(area->data + sizeof(struct prop_bt), pi->value, old_len + 1); /* dirty backup */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	st(&pi->serial, serial | 1);
	memcpy(pi->value, value, len + 1);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	st(&pi->serial, (len << 24) | ((serial + 1) & 0xffffff));
	futex_wake(&pi->serial);
	bump_serial(serial_area);
	return 0;
}

static int set_prop(const char *name, const char *value)
{
	struct prop_info *pi;

	if (!*name || strlen(name) > 1024 || name[0] == '.' || strstr(name, "..") || name[strlen(name) - 1] == '.')
		return PROP_ERROR_INVALID_NAME;
	if (!strncmp(name, "ctl.", 4)) {
		if (verbose)
			fprintf(stderr, "t2-propd: ignoring control message %s=%s\n", name, value);
		return PROP_SUCCESS;	/* no init: service control is not available */
	}
	pi = find_prop(name, value, 0);
	if (pi) {
		if (!strncmp(name, "ro.", 3))
			return PROP_ERROR_READ_ONLY_PROPERTY;
		if (update_prop(pi, value))
			return PROP_ERROR_INVALID_VALUE;
	} else {
		if (!find_prop(name, value, 1))
			return PROP_ERROR_SET_FAILED;
		bump_serial(area);
		bump_serial(serial_area);
	}
	if (verbose)
		fprintf(stderr, "t2-propd: %s=%s\n", name, value);
	return PROP_SUCCESS;
}

/* ---- files ---- */

static struct prop_area *create_area(const char *path)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
	struct prop_area *pa;

	if (fd < 0 || ftruncate(fd, PA_SIZE)) {
		perror(path);
		return NULL;
	}
	pa = mmap(NULL, PA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (pa == MAP_FAILED) {
		perror("mmap");
		return NULL;
	}
	memset(pa, 0, sizeof(*pa));
	pa->magic = PROP_AREA_MAGIC;
	pa->version = PROP_AREA_VERSION;
	/* the root node (zeroed, namelen 0) and the dirty backup area */
	pa->bytes_used = sizeof(struct prop_bt) + ALIGN4(PROP_VALUE_MAX);
	return pa;
}

/*
 * property_info with one context and one type and a root node that carries
 * them: GetPropertyInfoIndexes() then yields index 0 for every name.
 */
static int write_property_info(const char *path)
{
	uint8_t buf[512];
	struct info_header *h = (struct info_header *)buf;
	uint32_t off = sizeof(*h), ctx_str, type_str, root_name;
	uint32_t *arr;
	struct property_entry *pe;
	struct trie_node *root;
	int fd;

	memset(buf, 0, sizeof(buf));
	/* strings */
	ctx_str = off;
	strcpy((char *)buf + off, CONTEXT);
	off = ALIGN4(off + sizeof(CONTEXT));
	type_str = off;
	strcpy((char *)buf + off, "string");
	off = ALIGN4(off + sizeof("string"));
	root_name = off;
	buf[off] = 0;
	off = ALIGN4(off + 1);
	/* contexts: count + offsets */
	h->contexts_offset = off;
	arr = (uint32_t *)(buf + off);
	arr[0] = 1;
	arr[1] = ctx_str;
	off += 8;
	h->types_offset = off;
	arr = (uint32_t *)(buf + off);
	arr[0] = 1;
	arr[1] = type_str;
	off += 8;
	/* root property entry and node */
	pe = (struct property_entry *)(buf + off);
	pe->name_offset = root_name;
	pe->namelen = 0;
	pe->context_index = 0;
	pe->type_index = 0;
	{
		uint32_t pe_off = off;

		off += sizeof(*pe);
		h->root_offset = off;
		root = (struct trie_node *)(buf + off);
		root->property_entry = pe_off;
		off += sizeof(*root);
	}
	h->current_version = 1;
	h->minimum_supported_version = 1;
	h->size = off;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
	if (fd < 0 || write(fd, buf, off) != (ssize_t)off) {
		perror(path);
		return -1;
	}
	close(fd);
	return 0;
}

static int load_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1200];
	int n = 0;

	if (!f) {
		perror(path);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');
		char *name = line;

		if (nl)
			*nl = 0;
		while (*name == ' ' || *name == '\t')
			name++;
		if (!*name || *name == '#' || !eq)
			continue;
		*eq = 0;
		if (set_prop(name, eq + 1) == PROP_SUCCESS)
			n++;
	}
	fclose(f);
	return n;
}

/* ---- reader check (-g): the same lookup a bionic client does ---- */

static struct prop_area *map_ro(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct prop_area *pa;

	if (fd < 0)
		return NULL;
	pa = mmap(NULL, PA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	close(fd);
	return pa == MAP_FAILED ? NULL : pa;
}

/* ---- socket protocol ---- */

static int read_full(int fd, void *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = read(fd, (char *)buf + got, len - got);

		if (n <= 0)
			return -1;
		got += n;
	}
	return 0;
}

static int read_string(int fd, char *buf, size_t max)
{
	uint32_t len;

	if (read_full(fd, &len, 4) || len >= max)
		return -1;
	if (len && read_full(fd, buf, len))
		return -1;
	buf[len] = 0;
	return 0;
}

static void handle_client(int c)
{
	uint32_t cmd;
	char name[1100], value[1100];
	int32_t result;

	if (read_full(c, &cmd, 4))
		return;
	if (cmd == PROP_MSG_SETPROP) {
		/* legacy prop_msg: cmd, name[32], value[92]; the reply is the close */
		char rest[PROP_NAME_MAX + PROP_VALUE_MAX];

		if (read_full(c, rest, sizeof(rest)))
			return;
		memcpy(name, rest, PROP_NAME_MAX);
		name[PROP_NAME_MAX] = 0;
		memcpy(value, rest + PROP_NAME_MAX, PROP_VALUE_MAX);
		value[PROP_VALUE_MAX] = 0;
		set_prop(name, value);
		return;
	}
	if (cmd != PROP_MSG_SETPROP2) {
		result = PROP_ERROR_INVALID_CMD;
	} else if (read_string(c, name, sizeof(name)) || read_string(c, value, sizeof(value))) {
		result = PROP_ERROR_READ_DATA;
	} else {
		result = set_prop(name, value);
	}
	(void)!write(c, &result, sizeof(result));
}

int main(int argc, char **argv)
{
	const char *dir = "/dev/__properties__", *sock = "/dev/socket/property_service";
	const char *get = NULL;
	const char *files[16];
	int nfiles = 0, nodaemon = 0, opt, i, fd;
	char path[PATH_MAX];
	struct sockaddr_un sa = { .sun_family = AF_UNIX };

	while ((opt = getopt(argc, argv, "d:s:f:Dg:")) != -1) {
		switch (opt) {
		case 'd': dir = optarg; break;
		case 's': sock = optarg; break;
		case 'f': if (nfiles < 16) files[nfiles++] = optarg; break;
		case 'D': nodaemon = 1; verbose = 1; break;
		case 'g': get = optarg; break;
		default:
			fprintf(stderr, "usage: t2-propd [-d dir] [-s socket] [-f file]... [-D] [-g name]\n");
			return 2;
		}
	}
	if (get) {
		struct prop_info *pi;

		snprintf(path, sizeof(path), "%s/%s", dir, CONTEXT);
		area = map_ro(path);
		if (!area || area->magic != PROP_AREA_MAGIC) {
			fprintf(stderr, "t2-propd: no property area at %s\n", path);
			return 1;
		}
		pi = find_prop(get, NULL, 0);
		if (!pi)
			return 1;
		printf("%s\n", pi->value);
		return 0;
	}
	mkdir(dir, 0711);
	snprintf(path, sizeof(path), "%s/property_info", dir);
	if (write_property_info(path))
		return 1;
	snprintf(path, sizeof(path), "%s/properties_serial", dir);
	serial_area = create_area(path);
	snprintf(path, sizeof(path), "%s/%s", dir, CONTEXT);
	area = create_area(path);
	if (!serial_area || !area)
		return 1;
	/* bionic switches to the SETPROP2 protocol when this is set */
	set_prop("ro.property_service.version", "2");
	for (i = 0; i < nfiles; i++) {
		int n = load_file(files[i]);

		if (n >= 0)
			fprintf(stderr, "t2-propd: %d properties from %s\n", n, files[i]);
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
	unlink(sock);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(fd, 8)) {
		perror(sock);
		return 1;
	}
	chmod(sock, 0666);
	fprintf(stderr, "t2-propd: %s, socket %s\n", dir, sock);
	if (!nodaemon && daemon(0, 0))
		return 1;
	for (;;) {
		int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);

		if (c < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			return 1;
		}
		handle_client(c);
		close(c);
	}
}
