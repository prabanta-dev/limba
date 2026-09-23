/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * xalloc.c - allocation that cannot fail (see xalloc.h).
 */
#include "xalloc.h"

#include <stdio.h>
#include <stdlib.h>

static void out_of_memory(void)
{
    fputs("limba: out of memory\n", stderr);
    exit(70);
}

void *limba_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        out_of_memory();
    return p;
}

void *limba_xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p)
        out_of_memory();
    return p;
}

void *limba_xrealloc(void *p, size_t count, size_t size)
{
    size_t n;
    if (__builtin_mul_overflow(count, size, &n))
        out_of_memory();
    p = realloc(p, n ? n : 1);
    if (!p)
        out_of_memory();
    return p;
}
