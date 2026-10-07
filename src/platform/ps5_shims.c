/* Nubix — libc functions missing from the PS5 payload SDK.
 * Copyright (C) 2026 Nubix contributors
 * SPDX-License-Identifier: GPL-3.0-only */
#ifdef XC_PS5
#include <stddef.h>

/* usrsctp (sctp_auth.c, sctp_input.c) needs the BSD constant-time compare. */
int timingsafe_bcmp(const void *b1, const void *b2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)b1, *p2 = (const unsigned char *)b2;
    int ret = 0;
    for (; n > 0; n--) ret |= *p1++ ^ *p2++;
    return (ret != 0);
}
#else
/* Host builds get timingsafe_bcmp from libc (or usrsctp's own fallback). */
typedef int xc_ps5_shims_empty_translation_unit;
#endif
