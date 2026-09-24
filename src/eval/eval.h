/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * eval.h - the reference interpreter of the IR: slow on purpose, simple on
 * purpose, the yardstick the optimiser (and one day Meri) is measured by.
 * It runs a verified module from a function with no parameters and records
 * what the program printed, how it ended and what it returned. Not public.
 *
 * Where the IR does not fix a behaviour yet, the interpreter decides, and
 * the decision is provisional (job/docs/progetto_ir.md):
 *   division by zero, INT_MIN / -1             trap 11
 *   overflow of add.ov / sub.ov / mul.ov        trap 6
 *   shift by the width or more                  the count modulo the width
 *
 * fptosi and fptoui saturate (too large: the maximum, too small: the
 * minimum, NaN: 0), as WebAssembly's trunc_sat: they are pure, and a
 * language that wants an error checks before converting.
 */
#ifndef LIMBA_EVAL_H
#define LIMBA_EVAL_H

#include "limba/ir.h"

enum limba_eval_status {
    LIMBA_EVAL_OK,          /* returned */
    LIMBA_EVAL_TRAP,        /* a run-time error: code says which */
    LIMBA_EVAL_UNREACHABLE, /* executed unreachable */
    LIMBA_EVAL_LIMIT,       /* too many steps or calls too deep */
    LIMBA_EVAL_UNSUPPORTED, /* call.ext: no C here */
    LIMBA_EVAL_BAD,         /* no such entry, or it takes parameters */
    LIMBA_EVAL_HALT,        /* halt(code): code is the exit status */
};

typedef struct {
    uint64_t max_steps; /* instructions executed; 0 for 100 million */
    uint32_t max_depth; /* nested calls; 0 for 10000 */
    int argc;           /* the command line of the program, for arg() */
    char **argv;
    FILE *in; /* what read_line reads; NULL: nothing */
} limba_eval_limits;

typedef struct {
    int status;   /* enum limba_eval_status */
    int64_t code; /* the trap code */
    uint64_t ret; /* the returned value, in its bits; 0 for void */
    char *out;    /* what the program printed, malloc'd, NUL-terminated */
    size_t outlen;
    uint64_t steps; /* instructions executed */
    uint32_t pos;   /* a trap or a halt: the position of the instruction
                       in m->pos, 0 if it has none */
} limba_eval_result;

/* run function entry of a verified module; the result owns r->out */
void limba_eval(const limba_module *m, const char *entry,
                const limba_eval_limits *limits, limba_eval_result *r);
void limba_eval_result_free(limba_eval_result *r);

#endif
