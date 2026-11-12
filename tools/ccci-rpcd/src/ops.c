// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-ccci-rpcd: the four user-space operations.
 *
 * Only argument 0 of a request is used; a missing argument 0 counts as
 * an empty one.
 *
 * Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <stdio.h>
#include <string.h>

#include "rpcd.h"

static void printable(char *dst, size_t size, const uint8_t *src, size_t len)
{
	size_t j = 0;

	for (size_t i = 0; i < len && j + 1 < size; i++) {
		uint8_t c = src[i];

		if (!c)
			break;
		dst[j++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
	}
	dst[j] = '\0';
}

/* arg 0 = name; reply value length and the value with its NUL */
static void query_property(struct rpcd *r, const struct rpcd_arg *a0,
			   struct rpcd_out *out)
{
	char name[PROP_NAME_MAX], shown[PROP_NAME_MAX];
	const char *value = NULL;
	size_t len = 0;

	if (a0->len < sizeof(name)) {
		/* the name ends at the argument length or at its first NUL */
		memcpy(name, a0->data, a0->len);
		name[a0->len] = '\0';
		value = rpcd_prop_get(r, name);
		printable(shown, sizeof(shown), (const uint8_t *)name, a0->len);
	} else {
		/* no property name is that long: unset */
		snprintf(shown, sizeof(shown), "(%u bytes)", a0->len);
	}
	if (value)
		len = strlen(value);	/* < PROP_VALUE_MAX, see props.c */
	rpcd_log(r, 0, "property '%s' -> %s%s%s", shown, value ? "'" : "unset",
		 value ? value : "", value ? "'" : "");
	rpcd_out_u32(out, (uint32_t)len);
	rpcd_out_add(out, value ? value : "", len + 1);
}

/* never written anywhere; the values are logged */
static void save_capid(struct rpcd *r, const struct rpcd_arg *a0,
		       struct rpcd_out *out)
{
	if (a0->len != 8) {
		rpcd_log(r, 0, "CAPID: argument of %u bytes, want 8", a0->len);
		rpcd_out_u32(out, (uint32_t)-1);
		rpcd_out_u32(out, (uint32_t)r->capid_status);
		return;
	}
	rpcd_log(r, 0, "CAPID: md_capid 0x%08x md_aac 0x%08x (not saved)",
		 get_le32(a0->data), get_le32(a0->data + 4));
	r->capid_status = 0;
	rpcd_out_u32(out, 0);
	rpcd_out_u32(out, 0);
}

void rpcd_dispatch(struct rpcd *r, unsigned int slot, uint32_t op,
		   const struct rpcd_args *args, struct rpcd_out *out)
{
	static const struct rpcd_arg none = { (const uint8_t *)"", 0 };
	const struct rpcd_arg *a0 = args->n ? &args->a[0] : &none;

	rpcd_log(r, 1, "slot %u op 0x%x: %u args, arg 0 %u bytes", slot, op,
		 args->n, a0->len);
	switch (op) {
	case OP_QUERY_AP_SYS_PROPERTY:
		query_property(r, a0, out);
		break;
	case OP_SAR_TABLE_IDX_QUERY:
		rpcd_log(r, 0, "SAR table index query: 0");
		rpcd_out_u32(out, 0);
		rpcd_out_u32(out, 0);
		break;
	case OP_AMMS_DRDI_CONTROL:
		drdi_request(r, a0, out);
		break;
	case OP_SAVE_MD_CAPID:
		save_capid(r, a0, out);
		break;
	default:
		rpcd_log(r, 0, "slot %u: unknown op 0x%x, rejected", slot, op);
		rpcd_out_u32(out, (uint32_t)RPC_REJECT);
		break;
	}
}
