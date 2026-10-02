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
 * Calls nested deeper than max_depth, or than the stack of the thread the
 * run has for itself holds, are the trap STACK, mem_free of what
 * is not a live block the trap INVALID_FREE, ptr_live answers whether a
 * block is alive: freed blocks are never reused, so a dangling pointer
 * stays different from any new one. The counts of retain, release and of
 * store str are kept only with check_mem. Strings and BigInts are freed
 * by a collection when nothing holds them: no value of a call alive, no
 * word of memory alive, no count of check_mem; one held only by a freed
 * block goes too.
 *
 * read_line and io_read read one stream, in; its end is final, and an
 * error in reading it is the trap IO. The output stays in memory, in out:
 * whoever writes it checks the writing (lir_run).
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
    LIMBA_EVAL_LIMIT,       /* too many steps */
    LIMBA_EVAL_UNSUPPORTED, /* call.ext: no C here */
    LIMBA_EVAL_BAD,         /* no such entry, or it takes parameters, or the
                               module skips a check a call of the runtime needs
                               before it (a BigInt divided by 0) */
    LIMBA_EVAL_HALT,        /* halt(code): code is the exit status */
    /* check_mem: a rule of the strings in memory broken (progetto_ir.md
       § 11c): a str read or written as another type, or as a part, a
       release below zero, counts that do not match memory at the end */
    LIMBA_EVAL_BADMEM,
};

typedef struct {
    uint64_t max_steps; /* instructions executed; 0 for 100 million */
    uint32_t max_depth; /* nested calls; 0 for 10000 */
    int argc;           /* the command line of the program, for arg() */
    char **argv;
    FILE *in; /* what read_line and io_read read; NULL: nothing */
    /* count the references to strings from memory and check the rules
       of § 11c (LIMBA_EVAL_BADMEM); slower, the output does not change */
    bool check_mem;
    /* the memory of the program: blocks of mem_alloc alive, the slots of
       the calls alive, and the strings and BigInts still held (a value of
       a call alive, a word of memory alive: a collection frees the others
       before the budget runs out); past it, the trap NOMEM. 0 for 1 GiB,
       a limit of this oracle, not of the IR */
    uint64_t max_memory;
    /* a collection before every string or BigInt made, not only now and
       then: slow, for the tests, so that every run tries it */
    bool collect_often;
} limba_eval_limits;

typedef struct {
    int status;   /* enum limba_eval_status */
    int64_t code; /* the trap code */
    uint64_t ret; /* the returned value, in its bits; 0 for void */
    char *out;    /* what the program printed, malloc'd, NUL-terminated */
    size_t outlen;
    uint64_t steps; /* instructions executed */
    size_t live;    /* blocks of mem_alloc never freed */
    uint32_t pos;   /* a trap or a halt: the position of the instruction
                       in m->pos, 0 if it has none */
} limba_eval_result;

/* run function entry of a verified module; the result owns r->out */
void limba_eval(const limba_module *m, const char *entry,
                const limba_eval_limits *limits, limba_eval_result *r);
void limba_eval_result_free(limba_eval_result *r);

#endif
