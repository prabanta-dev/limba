/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * xalloc.c - allocation that cannot fail (see xalloc.h).
 */
#include "xalloc.h"

#include "limba/ir.h"

#include <stdio.h>
#include <stdlib.h>

/* null until the program gives its name: Limba has none of its own */
static const char *program;

/* a mistake of the program that calls Limba, not of its user */
static void caller_error(const char *what)
{
    fprintf(stderr, "liblimba: %s\n", what);
    abort();
}

void limba_set_program(const char *name)
{
    if (!name || !*name)
        caller_error("limba_set_program: the name is missing");
    program = name;
}

const char *limba_program(void)
{
    if (!program)
        caller_error("the program did not give its name: call "
                     "limba_set_program at the start of main");
    return program;
}

static void out_of_memory(void)
{
    fprintf(stderr, "%s: out of memory\n", limba_program());
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
