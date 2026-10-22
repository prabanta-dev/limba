/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * xalloc.h - allocation that cannot fail (it exits), and a growth helper
 * for the arrays with a count and a capacity.
 */
#ifndef LIMBA_XALLOC_H
#define LIMBA_XALLOC_H

#include <stddef.h>
#include <stdint.h>

void *limba_xmalloc(size_t n);
void *limba_xcalloc(size_t count, size_t size);
void *limba_xrealloc(void *p, size_t count, size_t size);

/* make room for one more element in (ptr, count, cap) */
#define LIMBA_GROW(ptr, count, cap)                                            \
    do {                                                                       \
        if ((count) == (cap)) {                                                \
            (cap) = (cap) ? 2 * (cap) : 8;                                     \
            (ptr) = limba_xrealloc((ptr), (cap), sizeof(*(ptr)));              \
        }                                                                      \
    } while (0)

#endif
