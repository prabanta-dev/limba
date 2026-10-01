/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * limba_luxia.h - the front end of Luxia as a library: a source in, the
 * IR out, a function at a time, as limba makes its .lir and as a program
 * that runs the IR in memory (prabanta) takes it.
 *
 * What a consumer may rely on:
 *
 * - every function, global and extern of the module is declared before
 *   the first function is given (begin): none is added later;
 * - while the bodies are made, strings, types and positions are added,
 *   at the end only: an id never changes, but the arrays of the module
 *   (types, members, positions, the bytes of the strings) may move, so a
 *   consumer keeps ids from one call to the next, never pointers;
 * - when a function is given, all that its body refers to exists but
 *   the bodies of the functions it calls: a routine declared later, a
 *   recursion, and main, function 0, given last. Calls are resolved by
 *   index, afterwards;
 * - a function given is never changed again;
 * - some errors are found while the bodies are made (a missing return, a
 *   variable read before it has a value): functions given before stay
 *   given, none is given after the first error, and end says so;
 * - no state is shared between two compilations: two threads may compile
 *   at once.
 */
#ifndef LIMBA_LUXIA_H
#define LIMBA_LUXIA_H

#include "limba/ir.h"
#include "limba/opt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int level;                    /* 0 or 1: limba -O0, -O1 */
    const limba_opt_options *opt; /* NULL: the defaults */
    /* verify each function before it is given (and after every pass if
       opt->verify_each); the builds that are not release always do */
    bool verify;
    /* the checks off in the whole source, separated by commas
       (index_check, ..., all_checks), as limba --suppress; NULL for none */
    const char *suppress;
    /* the platform the C types by name follow, as limba --target; NULL:
       the one that compiles */
    const char *target;
    bool no_external;    /* no boundary with C: limba --restrict */
    uint32_t max_errors; /* errors reported before giving up; 0: 20 */
    /* where the diagnoses are printed, as limba prints them; NULL for
       nowhere */
    FILE *diag_out;
    /* leave the memory of the compilation unfreed (the module given back
       too, if not asked for): only for a process that ends right after,
       where freeing piece by piece costs 3 % */
    bool no_free;
} limba_luxia_options;

enum { LIMBA_LUXIA_ERROR, LIMBA_LUXIA_WARNING, LIMBA_LUXIA_NOTE };

typedef struct {
    int severity;        /* LIMBA_LUXIA_ERROR... */
    char code[8];        /* "L0012", empty for none */
    const char *file;    /* NULL for a diagnosis of no place */
    uint32_t line, col;  /* from 1, the column in UTF-8 characters */
    uint32_t len;        /* the bytes marked from there, 0 or 1 a point */
    const char *message; /* valid during the call only */
} limba_luxia_diag;

typedef struct {
    void *ctx;
    /* the module is declared: once, before the first function; NULL if
       not needed */
    void (*begin)(void *ctx, const limba_module *m);
    /* function fid of m is complete, verified if asked, optimised at the
       level asked: 0 to go on, anything else to stop (no other function
       is given, and the result is LIMBA_LUXIA_STOPPED). Then its body is
       freed (limba_func_clear) unless keep_bodies. NULL if not needed */
    int (*func)(void *ctx, limba_module *m, limba_id fid);
    /* the end, called once if begin was: status as the result of the
       compilation, m the module if LIMBA_LUXIA_OK and NULL otherwise,
       when what was given is to be thrown away. NULL if not needed */
    void (*end)(void *ctx, const limba_module *m, int status);
    /* each diagnosis, printed on diag_out or not; NULL if not needed */
    void (*diag)(void *ctx, const limba_luxia_diag *d);
    bool keep_bodies;
} limba_luxia_consumer;

enum {
    LIMBA_LUXIA_OK = 0,
    LIMBA_LUXIA_ERRORS = 1,   /* the source has errors, or is not read */
    LIMBA_LUXIA_OPTIONS = 2,  /* an option is not valid */
    LIMBA_LUXIA_INTERNAL = 3, /* the IR made is not valid, or a pass broke
                                 it: a fault of Limba */
    LIMBA_LUXIA_STOPPED = 4,  /* func asked to stop */
};

/* compile the Luxia source at path; with LIMBA_LUXIA_OK and out not NULL,
   *out is the module (its declarations, strings and types; the bodies
   only with keep_bodies), freed with limba_module_free; *out is NULL
   otherwise. o and c may be NULL: the defaults, no consumer */
int limba_luxia_compile_file(const char *path, const limba_luxia_options *o,
                             const limba_luxia_consumer *c, limba_module **out);
/* the same on the len bytes at text (copied), named name in the
   diagnoses and the positions of the IR */
int limba_luxia_compile_text(const char *name, const char *text, size_t len,
                             const limba_luxia_options *o,
                             const limba_luxia_consumer *c, limba_module **out);

#endif
