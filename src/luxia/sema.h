/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema.h - the semantic phase of Luxia 0: names, types, constants and the
 * rules of job/docs/luxia_0.md § 4-9, on the tree the parser made. It
 * annotates the tree (a type, a symbol and a constant value per node) and
 * reports what breaks the rules; it rewrites nothing.
 *
 * Types and routines are visible in all their scope, constants and
 * variables only after their declaration: the names of a declaration
 * part are declared first, then resolved on demand, so that a type may
 * point to one declared after it, with a check for what is defined in
 * terms of itself. Definite assignment and the return on every path are
 * checked later, on the IR.
 */
#ifndef LIMBA_LUXIA_SEMA_H
#define LIMBA_LUXIA_SEMA_H

#include "ast.h"
#include "front/bigint.h"
#include "front/diag.h"
#include "front/symtab.h"
#include "front/types.h"

#include <stdbool.h>

/* flags of a symbol */
#define LXS_UNRESOLVED 1u
#define LXS_RESOLVING 2u
#define LXS_LOOPVAR 4u /* the variable of a for: constant in the body */
#define LXS_TOP 8u     /* declared at the level of the program */

/* modes of a parameter, the op of a PARAM node mapped */
enum { LXS_IN, LXS_VAR, LXS_OUT };

/* kinds of scope */
enum { LXS_UNIVERSE = 1, LXS_PROGRAM, LXS_ROUTINE, LXS_BLOCK };

/* the routines of the language */
enum {
    LXB_NONE,
    LXB_WRITE,
    LXB_WRITELN,
    LXB_WRITEBYTE,
    LXB_READLINE,
    LXB_LENGTH,
    LXB_LOW,
    LXB_HIGH,
    LXB_COPY,
    LXB_CHR,
    LXB_ORD,
    LXB_SUCC,
    LXB_PRED,
    LXB_STR,
    LXB_VAL,
    LXB_SQRT,
    LXB_SIN,
    LXB_COS,
    LXB_TAN,
    LXB_ARCTAN,
    LXB_EXP,
    LXB_LN,
    LXB_TRUNC,
    LXB_ROUND,
    LXB_FLOOR,
    LXB_CEIL,
    LXB_DISPOSE,
    LXB_ARGCOUNT,
    LXB_ARG,
    LXB_HALT,
};

/* a constant: every number and discrete value is a rational (integers,
   characters, booleans and enumeration values have denominator 1) */
enum { LXV_NUM = 1, LXV_STR, LXV_NIL };
typedef struct {
    uint8_t kind;
    uint32_t str; /* LXV_STR: the id in the string table */
    limba_rat num;
} limba_lxs_value;

typedef struct {
    limba_lx_ast *t;
    limba_lx *lx;
    const limba_source *src;
    limba_report *rep;
    limba_types ts;
    limba_symtab st;
    /* per node */
    limba_ltype *type;
    limba_sym *sym;
    uint32_t *val;
    /* constants, v[0] unused */
    limba_lxs_value *v;
    uint32_t nv, capv;
    /* the types of the language */
    limba_ltype ty_int[4], ty_uint[4], ty_bits[4], ty_f32, ty_f64;
    uint32_t universe, program;
    /* the routine being checked: its result (void for a procedure, 0
       for the body of the program) and the loops around the statement */
    limba_ltype result;
    bool in_routine;
    unsigned loops;
    /* canonical spellings of the names of the language, by symbol */
    const char **spelling;
    uint32_t nspelling;
} limba_lxs;

void limba_lxs_init(limba_lxs *S, limba_lx_ast *t, limba_lx *lx,
                    const limba_source *src, limba_report *rep);
void limba_lxs_free(limba_lxs *S);
/* the whole program at t->root */
void limba_lxs_check(limba_lxs *S);

/* ---- inside the semantic phase ---- */

void lxs_error(limba_lxs *S, unsigned code, uint32_t node, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void lxs_note(limba_lxs *S, limba_loc loc, uint32_t len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
const char *lxs_name(const limba_lxs *S, uint32_t id, size_t *len);
/* the type as messages say it; buf of 128 bytes */
const char *lxs_tname(const limba_lxs *S, limba_ltype t, char *buf);

static inline limba_lx_node *lxs_node(limba_lxs *S, uint32_t n)
{
    return &S->t->node[n];
}
static inline const limba_typeinfo *lxs_ty(const limba_lxs *S, limba_ltype t)
{
    return &S->ts.t[t];
}

/* declarations and types (sema.c) */
limba_sym lxs_lookup(limba_lxs *S, uint32_t scope, uint32_t node);
void lxs_force(limba_lxs *S, limba_sym s);
/* where a type is written: a variable may have computed bounds, a
   parameter may be an open array */
#define LXT_VAR 1u
#define LXT_PARAM 2u
limba_ltype lxs_type(limba_lxs *S, uint32_t node, uint32_t scope,
                     unsigned where);
/* the spelling of a symbol, as declared */
const char *lxs_spell(const limba_lxs *S, limba_sym s, size_t *len);
/* predeclare the names of a declaration list, then resolve them */
void lxs_declare_all(limba_lxs *S, uint32_t list, uint32_t scope, bool top);
void lxs_resolve_all(limba_lxs *S, uint32_t list);
limba_sym lxs_declare(limba_lxs *S, uint32_t scope, uint32_t name_node,
                      unsigned kind, uint32_t decl);
/* the base of a range, and the type operations work in */
limba_ltype lxs_base(const limba_lxs *S, limba_ltype t);
bool lxs_compatible(const limba_lxs *S, limba_ltype a, limba_ltype b);

/* constants (sema_const.c) */
uint32_t lxs_value_new(limba_lxs *S, uint8_t kind);
uint32_t lxs_value_int(limba_lxs *S, __int128 v);
bool lxs_value_to_int(const limba_lxs *S, uint32_t v, __int128 *out);
/* the value of an INT or REAL literal, read again from the source */
uint32_t lxs_literal(limba_lxs *S, uint32_t node);
/* fold an operator on constants; 0 and an error if it cannot */
uint32_t lxs_fold_unary(limba_lxs *S, uint32_t node, unsigned op, uint32_t a,
                        limba_ltype type);
uint32_t lxs_fold_binary(limba_lxs *S, uint32_t node, unsigned op, uint32_t a,
                         uint32_t b, limba_ltype type);
/* does the value fit type (after wrapping, for a modular type: then the
   value is replaced); errors at node */
bool lxs_fit(limba_lxs *S, uint32_t node, uint32_t *v, limba_ltype type);
int lxs_value_cmp(const limba_lxs *S, uint32_t a, uint32_t b);

/* expressions (sema_expr.c) */
/* the type of an expression; expected guides the constants without a
   type (0: none); the node is left with its final type */
limba_ltype lxs_expr(limba_lxs *S, uint32_t node, uint32_t scope,
                     limba_ltype expected);
/* the expression must be assignable to target: reports and converts */
bool lxs_assign_to(limba_lxs *S, uint32_t node, limba_ltype target,
                   const char *what);
/* a variable that may be written (for :=, var and out arguments) */
bool lxs_writable(limba_lxs *S, uint32_t node, bool report);
/* a call as a statement: it must be a procedure */
limba_ltype lxs_call_stmt(limba_lxs *S, uint32_t node, uint32_t scope);

/* statements (sema_stmt.c) */
void lxs_stmts(limba_lxs *S, uint32_t list, uint32_t scope);
void lxs_routine_body(limba_lxs *S, limba_sym routine);

#endif
