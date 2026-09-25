/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * pass.h - what a pass of the optimiser looks like. Not public.
 */
#ifndef LIMBA_OPT_PASS_H
#define LIMBA_OPT_PASS_H

#include "ir/internal.h"

/* What the passes share on one function: its module and its CFG, built
   when a pass first asks for it and kept while no pass changes a branch
   or drops a block (renumbering the instructions leaves it true). */
typedef struct {
    limba_module *m;
    limba_cfg cfg;
    bool have_cfg;
} limba_pass_ctx;

/* the CFG of f, built if the context has none */
const limba_cfg *limba_pass_cfg_of(limba_pass_ctx *x, const limba_func *f);
/* the branches or the blocks of f have changed: the CFG is no more */
void limba_pass_cfg_drop(limba_pass_ctx *x);

/* A pass works on one function and returns how many changes it made: the
   manager counts them (a pass that never changes anything on the corpus
   is a pass that looks for a shape the IR no longer has) and stops
   iterating at the fixed point. The module is well formed on entry and
   must be on exit. */
typedef uint32_t (*limba_pass_fn)(limba_pass_ctx *x, limba_func *f);

uint32_t limba_pass_cfg(limba_pass_ctx *x, limba_func *f);
uint32_t limba_pass_fold(limba_pass_ctx *x, limba_func *f);
uint32_t limba_pass_gvn(limba_pass_ctx *x, limba_func *f);
uint32_t limba_pass_dce(limba_pass_ctx *x, limba_func *f);

#endif
