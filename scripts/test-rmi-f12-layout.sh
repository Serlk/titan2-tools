#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Host test for the RMI4 F12 object layout (report enable control, ctrl28).
# Extracts the layout helpers from a kernel tree's drivers/input/rmi4/rmi_f12.c,
# compiles them in a userspace harness and parses objects packed for
# ctrl28 = 0xff, 0xdf (the Titan 2 touchscreen: no Z), 0xbf and 0x1f.
#   scripts/test-rmi-f12-layout.sh [KERNEL_TREE]
# KERNEL_TREE: a titan2-linux checkout (default: KERNEL_SRC, else linux/ in the root of this
# repository, the SRC of scripts/build-mainline.sh). Needs cc on the PC.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TREE=${1:-${KERNEL_SRC:-$ROOT/linux}}
F12=$TREE/drivers/input/rmi4/rmi_f12.c
[ -f "$F12" ] || { echo "no $F12" >&2; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Pieces of rmi_f12.c: the object type enum, the F12_REPORT_* defines (with
# continuation lines), the layout struct, the default layout and the two helpers.
awk '
	function grab(start) { if ($0 ~ start) on = 1 }
	{
		if (!on) {
			grab("^enum rmi_f12_object_type")
			grab("^#define (RMI_F12_CTRL_REPORT_ENABLE|F12_REPORT_)")
			grab("^struct f12_obj_layout \\{")
			grab("^static const struct f12_obj_layout f12_default_layout")
			grab("^static bool rmi_f12_layout_from_enables\\(")
			grab("^static void rmi_f12_decode_object\\(")
			if (on) def = ($0 ~ /^#define/)
		}
		if (on) {
			print
			if (def) { if ($0 !~ /\\$/) { on = 0; print "" } }
			else if ($0 ~ /^\}/) { on = 0; print "" }
		}
	}' "$F12" > "$WORK/extract.h"

grep -q rmi_f12_decode_object "$WORK/extract.h" || { echo "helpers not found in $F12" >&2; exit 1; }

cat > "$WORK/harness.c" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define MT_TOOL_FINGER 0
#define MT_TOOL_PEN 1
#define MT_TOOL_PALM 2
static u16 get_unaligned_le16(const void *p)
{
	const u8 *b = p;
	return b[0] | (b[1] << 8);
}

enum rmi_2d_sensor_object_type {
	RMI_2D_OBJECT_NONE,
	RMI_2D_OBJECT_FINGER,
	RMI_2D_OBJECT_STYLUS,
	RMI_2D_OBJECT_PALM,
	RMI_2D_OBJECT_UNCLASSIFIED,
};

struct rmi_2d_sensor_abs_object {
	enum rmi_2d_sensor_object_type type;
	int mt_tool;
	u16 x, y;
	u8 z, wx, wy;
};

#include "extract.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct touch { u8 type; u16 x, y; u8 wx, wy; };

/* One object in the documented order (type, X LSB/MSB, Y LSB/MSB, [Z], WX, WY) */
static int put(u8 *p, u8 enables, const struct touch *t, u8 z)
{
	int n = 0;

	if (enables & F12_REPORT_TYPE)  p[n++] = t->type;
	if (enables & F12_REPORT_X_LSB) p[n++] = t->x & 0xff;
	if (enables & F12_REPORT_X_MSB) p[n++] = t->x >> 8;
	if (enables & F12_REPORT_Y_LSB) p[n++] = t->y & 0xff;
	if (enables & F12_REPORT_Y_MSB) p[n++] = t->y >> 8;
	if (enables & F12_REPORT_Z)     p[n++] = z;
	if (enables & F12_REPORT_WX)    p[n++] = t->wx;
	if (enables & F12_REPORT_WY)    p[n++] = t->wy;
	return n;
}

/* What rmi_f12_process_objects() does per object, without the input part */
static int parse(const struct f12_obj_layout *l, const u8 *data1, u32 size,
		 u32 num_subpackets, struct rmi_2d_sensor_abs_object *objs)
{
	u32 objects = num_subpackets < size / l->size ? num_subpackets : size / l->size;
	u32 i;

	for (i = 0; i < objects; i++, data1 += l->size)
		rmi_f12_decode_object(l, data1, &objs[i]);
	return objects;
}

int main(void)
{
	struct f12_obj_layout l;
	/* two fingers apart, raw range 0..3238 */
	const struct touch t[2] = {
		{ RMI_F12_OBJECT_FINGER, 500, 2900, 4, 6 },
		{ RMI_F12_OBJECT_FINGER, 2700, 300, 5, 3 },
	};
	struct rmi_2d_sensor_abs_object objs[10];
	u8 pkt[87];
	int n, i;

	/* all fields: identical to the old fixed layout */
	CHECK(rmi_f12_layout_from_enables(0xff, &l));
	CHECK(!memcmp(&l, &f12_default_layout, sizeof(l)));
	CHECK(l.size == 8 && l.z == 5 && l.wx == 6 && l.wy == 7);

	/* 0xdf: no Z, 7-byte objects, WX/WY move up */
	CHECK(rmi_f12_layout_from_enables(0xdf, &l));
	CHECK(l.size == 7 && l.z == -1 && l.wx == 5 && l.wy == 6);

	/* position only */
	CHECK(rmi_f12_layout_from_enables(0x1f, &l));
	CHECK(l.size == 5 && l.z == -1 && l.wx == -1 && l.wy == -1);

	/* no WX: WY right after Z */
	CHECK(rmi_f12_layout_from_enables(0xbf, &l));
	CHECK(l.size == 7 && l.z == 5 && l.wx == -1 && l.wy == 6);

	/* missing type, X MSB or Y LSB: rejected */
	CHECK(!rmi_f12_layout_from_enables(0xde, &l));
	CHECK(!rmi_f12_layout_from_enables(0xfb, &l));
	CHECK(!rmi_f12_layout_from_enables(0xf7, &l));
	CHECK(!rmi_f12_layout_from_enables(0x00, &l));

	/* 0xdf packet: 10 objects x 7 bytes read from Data1's own address */
	memset(pkt, 0, sizeof(pkt));
	n = put(pkt, 0xdf, &t[0], 0);
	n += put(pkt + n, 0xdf, &t[1], 0);
	CHECK(n == 14);
	CHECK(rmi_f12_layout_from_enables(0xdf, &l));
	CHECK(parse(&l, pkt, 7 * 10, 10, objs) == 10);
	for (i = 0; i < 2; i++) {
		CHECK(objs[i].type == RMI_2D_OBJECT_FINGER);
		CHECK(objs[i].x == t[i].x && objs[i].y == t[i].y);
		CHECK(objs[i].wx == t[i].wx && objs[i].wy == t[i].wy);
		CHECK(objs[i].z == 0);
		printf("0xdf obj%d: type %d x %u y %u z %u wx %u wy %u\n", i,
		       objs[i].type, objs[i].x, objs[i].y, objs[i].z,
		       objs[i].wx, objs[i].wy);
	}
	for (i = 2; i < 10; i++)
		CHECK(objs[i].type == RMI_2D_OBJECT_NONE);

	/* the same packet through the old fixed 8-byte layout loses object 1 */
	parse(&f12_default_layout, pkt, sizeof(pkt), 10, objs);
	printf("8-byte parse of 0xdf data: obj0 z %u (= WX) wx %u wy %u (= next type), obj1 type %d x %u\n",
	       objs[0].z, objs[0].wx, objs[0].wy, objs[1].type, objs[1].x);
	CHECK(objs[0].z == t[0].wx && objs[0].wy == t[1].type);
	CHECK(objs[1].type == RMI_2D_OBJECT_NONE);

	/* 0xff packet with Z, 8-byte objects in one block read: unchanged parsing */
	memset(pkt, 0, sizeof(pkt));
	n = put(pkt, 0xff, &t[0], 40);
	n += put(pkt + n, 0xff, &t[1], 70);
	CHECK(n == 16);
	CHECK(parse(&f12_default_layout, pkt, sizeof(pkt), 10, objs) == 10);
	CHECK(objs[0].z == 40 && objs[1].z == 70);
	for (i = 0; i < 2; i++) {
		CHECK(objs[i].type == RMI_2D_OBJECT_FINGER);
		CHECK(objs[i].x == t[i].x && objs[i].y == t[i].y);
		CHECK(objs[i].wx == t[i].wx && objs[i].wy == t[i].wy);
	}

	/* a short (HID attention style) buffer limits the object count */
	CHECK(rmi_f12_layout_from_enables(0xdf, &l));
	CHECK(parse(&l, pkt, 20, 10, objs) == 2);

	printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
	return !!fails;
}
EOF

${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -I"$WORK" \
	-o "$WORK/harness" "$WORK/harness.c"
"$WORK/harness"
