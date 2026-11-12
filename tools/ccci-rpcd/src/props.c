// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-rpcd: the property table for 0x400F.
 *
 * A text file of "name=value" lines. Blank lines and lines starting with
 * '#' are ignored; whitespace around name and value is dropped. A later
 * line for the same name wins. Values must fit Android's PROP_VALUE_MAX
 * (91 characters).
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcd.h"

static char *trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

int rpcd_props_load(struct rpcd *r, const char *path)
{
	FILE *f = fopen(path, "re");
	char *line = NULL;
	size_t cap = 0;
	unsigned int lineno = 0;
	int ret = 0;

	if (!f) {
		rpcd_log(r, 0, "%s: %s", path, strerror(errno));
		return -1;
	}
	while (getline(&line, &cap, f) >= 0) {
		struct rpcd_prop *np;
		char *name, *value, *eq;

		lineno++;
		name = trim(line);
		if (!*name || *name == '#')
			continue;
		eq = strchr(name, '=');
		if (!eq) {
			rpcd_log(r, 0, "%s:%u: no '=', ignored", path, lineno);
			continue;
		}
		*eq = '\0';
		name = trim(name);
		value = trim(eq + 1);
		if (!*name || strlen(name) >= PROP_NAME_MAX) {
			rpcd_log(r, 0, "%s:%u: bad name, ignored", path, lineno);
			continue;
		}
		if (strlen(value) >= PROP_VALUE_MAX) {
			rpcd_log(r, 0, "%s:%u: value over %d characters, ignored",
				 path, lineno, PROP_VALUE_MAX - 1);
			continue;
		}
		np = realloc(r->props, (r->nprops + 1) * sizeof(*np));
		if (!np) {
			ret = -1;
			break;
		}
		r->props = np;
		np[r->nprops].name = strdup(name);
		np[r->nprops].value = strdup(value);
		if (!np[r->nprops].name || !np[r->nprops].value) {
			free(np[r->nprops].name);
			free(np[r->nprops].value);
			ret = -1;
			break;
		}
		r->nprops++;
	}
	if (ferror(f))
		ret = -1;
	if (ret)
		rpcd_log(r, 0, "%s: read failed", path);
	free(line);
	fclose(f);
	rpcd_log(r, 1, "%s: %zu properties", path, r->nprops);
	return ret;
}

void rpcd_props_free(struct rpcd *r)
{
	for (size_t i = 0; i < r->nprops; i++) {
		free(r->props[i].name);
		free(r->props[i].value);
	}
	free(r->props);
	r->props = NULL;
	r->nprops = 0;
}

/* NULL when unset; the last definition wins */
const char *rpcd_prop_get(const struct rpcd *r, const char *name)
{
	for (size_t i = r->nprops; i-- > 0;)
		if (!strcmp(r->props[i].name, name))
			return r->props[i].value;
	return NULL;
}
