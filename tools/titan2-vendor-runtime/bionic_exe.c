// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What a host executable has to provide for bionic_translation's linker
 * (after main_executable/bionic_compat.c of bionic_translation, Apache-2.0):
 *
 *  - _r_debug_ptr: the linker registers loaded bionic libraries with gdb
 *    through the executable's DT_DEBUG entry;
 *  - TLS[]: bionic code reads fixed slots relative to the thread pointer
 *    (tp+2 .. tp+7: APP, OPENGL, OPENGL_API, STACK_GUARD, SANITIZER,
 *    ART_THREAD_SELF). On AArch64 the executable's own PT_TLS block sits right
 *    after the 16-byte TCB, so the ONLY thread-local variable of the executable
 *    lands exactly there. Keep every other TLS variable out of t2-fp.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern ElfW(Dyn) _DYNAMIC[];
extern struct r_debug *_r_debug_ptr;

__attribute__((constructor)) static void t2_init_r_debug(void)
{
	int i;

	for (i = 0; _DYNAMIC[i].d_tag != DT_NULL; i++) {
		if (_DYNAMIC[i].d_tag == DT_DEBUG) {
			_r_debug_ptr = (struct r_debug *)_DYNAMIC[i].d_un.d_ptr;
			return;
		}
	}
	fprintf(stderr, "bionic_exe: no DT_DEBUG in the dynamic section\n");
	exit(1);
}

#if defined(__aarch64__)
_Thread_local uintptr_t TLS[] = {
	0x5555555555555555, /* tp+2 TLS_SLOT_APP */
	0x5555555555555555, /* tp+3 TLS_SLOT_OPENGL */
	0x5555555555555555, /* tp+4 TLS_SLOT_OPENGL_API */
	0x5555555555555555, /* tp+5 TLS_SLOT_STACK_GUARD (constant canary) */
	0x5555555555555555, /* tp+6 TLS_SLOT_SANITIZER */
	0x5555555555555555, /* tp+7 TLS_SLOT_ART_THREAD_SELF */
};
#endif
