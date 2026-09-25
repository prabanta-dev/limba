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

/* every operand of in is a value: all but the branches */
static inline bool limba_only_values(const limba_inst *in)
{
    unsigned fmt = limba_ops[in->op].format;
    return fmt != LIMBA_F_BR && fmt != LIMBA_F_CBR && fmt != LIMBA_F_SWITCH;
}

/* the kind of every operand of in, into kinds[nops]; false if the operand
   list does not have the shape its format asks for */
bool limba_operand_kinds(const limba_func *f, const limba_inst *in,
                         uint8_t *kinds);

/* ---- editing a function in one go (edit.c) ---- */

/* A pass records what changes, then limba_edit_end applies it all at once:
   operands follow the replacements, dead instructions and blocks go, and
   what is left is renumbered in the canonical order (block order,
   parameters first), the order the text and binary forms use. A pass may
   also rewrite an instruction in place (limba_inst_set_*). */
typedef struct {
    limba_func *f;
    uint32_t *map;       /* map[x]: the value x stands for, x if unchanged */
    uint8_t *dead;       /* instructions to remove */
    uint8_t *dead_block; /* blocks to remove; never b0 */
    uint32_t ninsts, nblocks;
} limba_edit;

void limba_edit_begin(limba_edit *e, limba_func *f);
/* the value x stands for now, following chains of replacements */
uint32_t limba_edit_resolve(limba_edit *e, uint32_t x);
/* every use of from becomes a use of to; from is then dead */
void limba_edit_replace(limba_edit *e, uint32_t from, uint32_t to);
void limba_edit_end(limba_edit *e);
/* forget the edit: nothing recorded is applied */
void limba_edit_cancel(limba_edit *e);

/* rewrite an instruction in place: a constant of its own type, or a jump */
void limba_inst_set_iconst(limba_inst *in, int64_t v);
void limba_inst_set_fconst(limba_inst *in, int64_t bits);
/* br to the target whose (block, count, args) start at operand k */
void limba_inst_set_br(limba_func *f, limba_inst *in, uint32_t k);

/* an integer of type t in its canonical form: sign-extended from the
   width of t, 0 or 1 for i1 */
int64_t limba_int_norm(int64_t v, limba_id t);

#endif
