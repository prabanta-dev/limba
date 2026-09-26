/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ssa.h - building a function of the IR in SSA form while a front end
 * walks its tree: the algorithm of Braun, Buchwald, Hack, Leißa, Mallon
 * and Zwinkau, "Simple and Efficient Construction of Static Single
 * Assignment Form" (CC 2013), with block parameters in place of phis.
 *
 * The front end declares variables, defines and uses them in blocks, and
 * seals a block when all its predecessors are known. Terminators are
 * recorded and emitted at the end, when the parameters of every block and
 * the arguments of every edge are known; then the parameters go first in
 * their blocks, the trivial ones are removed, the blocks no path reaches
 * are dropped and the function is numbered again.
 *
 * At the end the builder also reports every use that some path reaches
 * without a definition: the check of definite assignment.
 */
#ifndef LIMBA_FRONT_SSA_H
#define LIMBA_FRONT_SSA_H

#include "ir/internal.h"
#include "limba/ir.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct limba_ssa limba_ssa;

/* for the function func of m, whose entry block (0) must exist, with
   the function parameters as its first parameters */
limba_ssa *limba_ssa_new(limba_module *m, limba_id func);
void limba_ssa_free(limba_ssa *s);
/* the function: the pointer changes when m gains functions */
limba_func *limba_ssa_func(limba_ssa *s);

uint32_t limba_ssa_var(limba_ssa *s, limba_id type);
void limba_ssa_def(limba_ssa *s, uint32_t var, limba_id block, limba_id value);
/* the value of var in block; tag is the front end's, reported if a path
   reaches the use without a definition (UINT32_MAX: no check) */
limba_id limba_ssa_use(limba_ssa *s, uint32_t var, limba_id block,
                       uint32_t tag);

/* a new block, not sealed; seal it once all its predecessors are there */
limba_id limba_ssa_block(limba_ssa *s);
void limba_ssa_seal(limba_ssa *s, limba_id b);

/* the terminator of a block; the edges they make are the predecessors
   of their targets */
void limba_ssa_br(limba_ssa *s, limba_id from, limba_id to);
void limba_ssa_cbr(limba_ssa *s, limba_id from, limba_id cond, limba_id then_,
                   limba_id else_);
/* one value per case; targets of a switch take no arguments: each must
   have the switch as its only predecessor */
void limba_ssa_switch(limba_ssa *s, limba_id from, limba_id value,
                      limba_id dflt, const int64_t *values,
                      const limba_id *targets, uint32_t n);
void limba_ssa_ret(limba_ssa *s, limba_id from, limba_id value);
void limba_ssa_unreachable(limba_ssa *s, limba_id from);
bool limba_ssa_terminated(const limba_ssa *s, limba_id b);

/* emit the function; undefined(ctx, tag) for every use a path reaches
   without a definition, once per tag */
void limba_ssa_finish(limba_ssa *s, void (*undefined)(void *ctx, uint32_t tag),
                      void *ctx);
/* the same, the last edit (trivial parameters replaced, the blocks no path
   reaches dropped with what they hold) left to apply in *e: the function
   is canonical only after limba_edit_end, which an optimiser may call
   once for its own changes too */
void limba_ssa_finish_edit(limba_ssa *s,
                           void (*undefined)(void *ctx, uint32_t tag),
                           void *ctx, limba_edit *e);

#endif
