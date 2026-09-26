/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * pass.h - what a pass of the optimiser looks like. Not public.
 */
#ifndef LIMBA_OPT_PASS_H
#define LIMBA_OPT_PASS_H

#include "ir/internal.h"
#include "limba/opt.h"

/* What the passes share on one function: its module, its CFG and one
   edit. The CFG is built when a pass first asks for it and kept while no
   pass changes a branch. The edit gathers what every pass changes, and
   the manager applies it once, at the end: a pass reads the operands
   through it (limba_edit_resolve) and passes over the instructions and
   the blocks it marks dead, as if they were gone. A pass may still
   rewrite an instruction in place (limba_inst_set_*). */
typedef struct {
    limba_module *m;
    limba_cfg cfg;
    bool have_cfg;
    bool fold; /* gvn folds too; not when "fold" is skipped */
    limba_edit e;
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
uint32_t limba_pass_gvn(limba_pass_ctx *x, limba_func *f);
uint32_t limba_pass_dce(limba_pass_ctx *x, limba_func *f);

/* limba_optimizer_func on a function whose edit e, begun by its maker, is
   not applied yet (limba_ssa_finish_edit): the passes add to it, and it
   is applied once; e is consumed */
int limba_optimizer_func_edit(limba_optimizer *z, limba_module *m, limba_id fid,
                              limba_edit *e, limba_diag *d);

/* fold instruction id of f, recording in e: true if it changed; gvn
   calls it on each instruction (fold.c) */
bool limba_fold_inst(limba_func *f, limba_edit *e, uint32_t id);

#endif
