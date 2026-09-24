/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower.h - from the checked tree of a Luxia program to the IR.
 *
 * Every routine becomes a function, the body of the program the function
 * main, the variables of the program globals. A scalar variable whose
 * address is never needed becomes SSA values (front/ssa.h); records,
 * arrays, globals and the variables passed as var or out live in memory.
 * The checks of Luxia at run time (overflow, index, range, nil, division
 * by zero, conversion) become check instructions with the codes below;
 * definite assignment and the return on every path are checked here, on
 * the SSA form.
 */
#ifndef LIMBA_LUXIA_LOWER_H
#define LIMBA_LUXIA_LOWER_H

#include "front/ssa.h"
#include "limba/ir.h"
#include "sema.h"

/* the codes of the errors at run time (include/limba/traps.def) */
enum {
    LXR_OVERFLOW = LIMBA_TRAP_OVERFLOW,
    LXR_DIVZERO = LIMBA_TRAP_DIVZERO,
    LXR_INDEX = LIMBA_TRAP_INDEX,
    LXR_RANGE = LIMBA_TRAP_RANGE,
    LXR_NIL = LIMBA_TRAP_NIL,
    LXR_CONVERSION = LIMBA_TRAP_CONVERSION,
    LXR_SHIFT = LIMBA_TRAP_SHIFT,
};

/* the module of a checked program; NULL if errors were reported */
limba_module *limba_lxl_program(limba_lxs *S);

/* ---- inside the generator ---- */

enum { LXL_NONE, LXL_SSA, LXL_MEM, LXL_GLOBAL, LXL_OPEN, LXL_DYN };

typedef struct {
    uint8_t kind;
    uint32_t var;    /* SSA */
    limba_id addr;   /* MEM, OPEN, DYN: the address, a value */
    limba_id len;    /* OPEN: the length, an i64 value */
    limba_id lo, hi; /* DYN: the bounds, values of the index type */
    limba_id global; /* GLOBAL */
} lxl_store;

typedef struct {
    limba_id exit, cont;
} lxl_loop;

typedef struct {
    limba_lxs *S;
    limba_module *m;
    lxl_store *store;  /* per symbol */
    uint8_t *taken;    /* per symbol: its address is needed */
    limba_id *tmap;    /* per language type: the IR type, 0 unknown */
    limba_id *func_of; /* per symbol of a routine: its function */
    /* the function being built */
    limba_ssa *ssa;
    limba_id fid, cur;
    limba_ltype result; /* 0 for a procedure or the program */
    uint32_t ret_var;   /* never defined: a use of it at the end of a
                           function is a missing return */
    lxl_loop *loops;
    uint32_t nloops, caploops;
    uint32_t routine_node;
} lxl;

/* IR types */
limba_id lxl_type(lxl *L, limba_ltype t);
bool lxl_scalar(const lxl *L, limba_ltype t);

/* emitting into the current block */
limba_id lxl_emit(lxl *L, unsigned op, limba_id type, unsigned cc, int64_t imm,
                  int64_t imm2, const uint32_t *ops, uint32_t nops);
limba_id lxl_iconst(lxl *L, limba_id type, int64_t v);
limba_id lxl_rt(lxl *L, unsigned rt, limba_id type, const uint32_t *args,
                uint32_t n);
void lxl_check(lxl *L, limba_id cond, int64_t code);
/* a new block that is where the code goes now */
void lxl_goto_new(lxl *L, limba_id b);
/* the instructions made from now on come from node */
void lxl_at(lxl *L, uint32_t node);

/* expressions (lower_expr.c) */
limba_id lxl_value(lxl *L, uint32_t node);
limba_id lxl_addr(lxl *L, uint32_t node);
void lxl_branch(lxl *L, uint32_t node, limba_id t, limba_id f);
/* v, of type from, as a value of type to: range checks and widths */
limba_id lxl_coerce(lxl *L, limba_id v, limba_ltype from, limba_ltype to);
/* the assignment node: store value into target, the target first */
void lxl_assign(lxl *L, uint32_t node, uint32_t target, uint32_t value);
void lxl_call(lxl *L, uint32_t node, limba_id *result);
/* the storage of a symbol, as a use in the current function */
limba_id lxl_var_addr(lxl *L, limba_sym s);
/* T(x) between numbers, and a range narrowing */
limba_id lxl_conv(lxl *L, limba_id v, limba_ltype from, limba_ltype to);
/* an array: its address, and the length (open) or bounds (computed) */
void lxl_array_parts(lxl *L, uint32_t base, limba_id *p, limba_id *len,
                     limba_id *lo, limba_id *hi);
limba_id lxl_to_i64(lxl *L, limba_id v, limba_ltype t);
bool lxl_signed(const lxl *L, limba_ltype t);

#endif
