/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * internal.h - helpers shared by the files of src/ir, not public.
 */
#ifndef LIMBA_IR_INTERNAL_H
#define LIMBA_IR_INTERNAL_H

#include "limba/ir.h"

#include <stdarg.h>

/* the name of a scalar type ("i64"), NULL for the others */
const char *limba_scalar_name(limba_id t);
/* the scalar type spelled so, LIMBA_NONE if none */
limba_id limba_scalar_find(const char *s, size_t len);

/* a call signature, from a function type of the module or from the text
   of runtime.def, without adding anything to the module */
#define LIMBA_RT_MAXPARAMS 16
typedef struct {
    limba_id ret;
    uint32_t n;
    bool variadic;
    const limba_member *mem; /* module types; NULL for the runtime */
    limba_id rt[LIMBA_RT_MAXPARAMS];
} limba_sig;

static inline limba_id limba_sig_param(const limba_sig *s, uint32_t i)
{
    return s->mem ? s->mem[i].type : s->rt[i];
}

/* false if t is not a function type */
bool limba_type_sig(const limba_module *m, limba_id t, limba_sig *s);
/* false if rt is out of range or its text is malformed */
bool limba_rt_sig(limba_id rt, limba_sig *s);
/* the signature of a call instruction; false if the callee is invalid */
bool limba_call_sig(const limba_module *m, const limba_inst *in, limba_sig *s);

/* printf into a diagnostic */
void limba_diag_set(limba_diag *d, unsigned line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* the argument list of a branch target at operand index k of in: sets
 *block and *nargs, returns the index of the first argument */
uint32_t limba_target(const limba_func *f, uint32_t k, limba_id *block,
                      uint32_t *nargs);

/* ---- control flow (cfg.c) ---- */

/* successors and predecessors of every block, in compressed rows:
   the successors of b are succ[sfirst[b] .. sfirst[b + 1]) */
typedef struct {
    uint32_t n;
    uint32_t *sfirst, *succ;
    uint32_t *pfirst, *pred;
    /* dominator tree: idom[b] (the entry is its own), LIMBA_NONE for a
       block the entry does not reach; pre/post numbering of the tree */
    uint32_t *idom;
    uint32_t *pre, *post;
} limba_cfg;

/* false if a terminator names a block that does not exist */
bool limba_cfg_build(const limba_func *f, limba_cfg *c);
void limba_cfg_free(limba_cfg *c);
bool limba_cfg_reachable(const limba_cfg *c, limba_id b);
/* a dominates b (a block dominates itself); both reachable */
bool limba_cfg_dominates(const limba_cfg *c, limba_id a, limba_id b);

/* ---- operand kinds, shared by the printer and the binary form ---- */

enum { LIMBA_OK_VALUE, LIMBA_OK_BLOCK, LIMBA_OK_RAW };

/* the kind of every operand of in, into kinds[nops]; false if the operand
   list does not have the shape its format asks for */
bool limba_operand_kinds(const limba_func *f, const limba_inst *in,
                         uint8_t *kinds);

#endif
