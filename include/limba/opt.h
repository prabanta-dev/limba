/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * opt.h - the optimiser of the Limba IR: one pipeline, the same for every
 * program that optimises (limba, prabanta), run to a fixed point on each
 * function.
 */
#ifndef LIMBA_OPT_H
#define LIMBA_OPT_H

#include "limba/ir.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct {
    /* verify the module after every pass and stop at the first pass that
       breaks it; always on in the builds that are not release */
    bool verify_each;
    /* names of passes not to run, separated by commas; NULL for none;
       "fold" turns off the folding gvn does. The environment variable
       LIMBA_OPTSKIP adds to it. */
    const char *skip;
    /* where to write what each pass changed, NULL for nowhere */
    FILE *stats;
} limba_opt_options;

/* 0, or -1 with d saying which pass broke the module and how */
int limba_optimize(limba_module *m, const limba_opt_options *o, limba_diag *d);

/* the same a function at a time, as a front end completes them: each is
   taken to its own fixed point (no pass looks at another function), so
   the result is the one of limba_optimize. The statistics are written by
   limba_optimizer_free, for all the functions given */
typedef struct limba_optimizer limba_optimizer;
limba_optimizer *limba_optimizer_new(const limba_opt_options *o);
/* 0, or -1 with d saying which pass broke function fid of m and how */
int limba_optimizer_func(limba_optimizer *z, limba_module *m, limba_id fid,
                         limba_diag *d);
void limba_optimizer_free(limba_optimizer *z);

/* the name of pass i in pipeline order, NULL past the last */
const char *limba_pass_name(unsigned i);

#endif
