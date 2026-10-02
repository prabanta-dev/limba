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
#define LXS_LOOPVAR 4u    /* the variable of a for: constant in the body */
#define LXS_TOP 8u        /* declared at the level of the program */
#define LXS_CBORDER 16u   /* a name of the boundary with C (§ 3.13, § 9.9) */
#define LXS_CPLATFORM 32u /* a C type by name: binds to the platform */

/* the platforms whose C the C types by name follow (limba --target) */
enum { LXS_X86_64_LINUX, LXS_AARCH64_LINUX, LXS_X86_64_WINDOWS };
/* the name of a platform, as in the IR; the one of a name, -1 if none */
const char *lxs_target_name(unsigned target);
int lxs_target_find(const char *name, size_t len);
#define LXS_NCTYPES 16

/* modes of a parameter, the op of a PARAM node mapped */
enum { LXS_IN, LXS_VAR, LXS_OUT };

/* the checks a pragma names (§ 9), bits of the flags of a PRAGMA node */
enum {
    LXS_CHECK_INDEX = 1,
    LXS_CHECK_RANGE = 2,
    LXS_CHECK_OVERFLOW = 4,
    LXS_CHECK_DIVISION = 8,
    LXS_CHECK_CONVERSION = 16,
    LXS_CHECK_SHIFT = 32,
    LXS_CHECK_NIL = 64,
    LXS_CHECK_DANGLING = 128,
    LXS_CHECK_ALL = 255,
    LXS_UNSUPPRESS = 256 /* the pragma turns them back on */
};
/* the bits of a check name (index_check, ..., all_checks), 0 if none */
unsigned lxs_check_bits(const char *name, size_t len);

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
    LXB_MOVE,
    LXB_NEWCSTRING,
    LXB_CVALUE,
    LXB_FREECSTRING,
    LXB_TRANSLATE,
    LXB_REVERSE,
    LXB_OCCURRENCES,
    LXB_READBYTES,
    LXB_WRITEBYTES,
};

/* a constant: every number and discrete value is a rational (integers,
   characters, booleans and enumeration values have denominator 1) */
enum { LXV_NUM = 1, LXV_STR, LXV_NIL };
typedef struct {
    uint8_t kind;
    uint32_t str; /* LXV_STR: the id in the string table */
    limba_rat num;
} limba_lxs_value;

/* a file of the program (§ 11): the program, or a unit. The driver
   lists them, parsed into one tree, and sets b of each REF of a USES
   node to the unit it names, + 1 (0: not found, reported) */
typedef struct {
    uint32_t root;        /* its PROGRAM or UNIT node */
    uint32_t first, last; /* its nodes: from first to before last */
    uint32_t name;        /* the name id of a unit; 0 for the program */
    uint32_t intf, impl;  /* its scopes; the program has only impl */
    bool library;         /* a unit of the standard library */
    bool restricted;      /* its own pragma restrictions(no_external) */
    uint32_t order;       /* its place in the initialisation, from 0 */
    bool uses;            /* it uses some unit: names may come from them */
} limba_lxs_unit;

/* what a routine or an initialisation does that the order of the
   initialisations and the routines kept depend on (§ 11.5, § 7 of the
   proposal): a call of a routine, a read or a write of a variable of a
   unit; who is a routine symbol, or LXS_INIT | unit for the
   initialisation of a unit (the program's too) */
#define LXS_INIT 0x80000000u
typedef struct {
    uint32_t who;
    limba_sym what; /* a routine, or a variable of a unit or program */
    uint32_t node;  /* where */
    bool write;
} limba_lxs_use;

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
    /* the checks turned off for the whole file from outside (limba
       --suppress), LXS_CHECK_* bits; the pragmas add to them */
    unsigned suppress;
    /* p^ of an array created by new may be read here as a whole: the
       argument of low, high, length, move, translate, reverse and
       occurrences (§ 3.10) */
    bool open_ok;
    /* the boundary with C (§ 3.13, § 8.5, § 10.4): the platform of the C
       types by name (set before limba_lxs_check); no external routine
       nor C type allowed (pragma restrictions, limba --restrict); the
       program uses a C type by name or a record with the C convention */
    unsigned target;
    bool no_external;
    bool c_bound;
    limba_ltype ty_cpointer, ty_cstring, ty_cbool;
    limba_sym csym[LXS_NCTYPES];
    /* units (§ 11): the files, the one compiled (the program, or a unit
       only checked), per scope its file + 1 and in bit 31 whether it is
       an implementation (or the program), per pair of files whether the
       first names the second, per symbol the heading in the interface
       of a routine whose body is in the implementation */
    limba_lxs_unit *units;
    uint32_t nunits, capunits, main;
    uint32_t *sunit;
    uint32_t nsunit;
    uint8_t *named;
    uint32_t *heading;
    uint32_t nheading;
    /* the initialisation of a unit being checked: return is an error */
    bool in_init;
    /* what is being checked, for the uses: a routine symbol or
       LXS_INIT | unit; the uses found */
    uint32_t who;
    limba_lxs_use *uses;
    uint32_t nuses, capuses;
    /* pragma hides(Unit.Name) of the files: REF nodes (qualified), and
       whether each hid something */
    uint32_t *hides;
    uint8_t *hid;
    uint32_t nhides, caphides;
    /* per symbol: a routine or a variable some initialisation reaches,
       through the calls (§ 7 of the proposal: only that goes in the IR) */
    uint8_t *reached;
} limba_lxs;

void limba_lxs_init(limba_lxs *S, limba_lx_ast *t, limba_lx *lx,
                    const limba_source *src, limba_report *rep);
void limba_lxs_free(limba_lxs *S);
/* the whole program at t->root */
void limba_lxs_check(limba_lxs *S);

/* a file of the program, for the driver before limba_lxs_check: its
   index (the first one added is units[0]) */
uint32_t limba_lxs_unit_add(limba_lxs *S, uint32_t root, uint32_t first,
                            uint32_t last, bool library);

/* ---- inside the semantic phase ---- */

/* the text at a place, the length of the name that starts there */
const char *lxs_text_at(const limba_lxs *S, limba_loc loc);
uint32_t lxs_ident_len(const char *s);

/* units (sema_unit.c) */
/* a new scope, in the file and part of its parent */
uint32_t lxs_scope_new(limba_lxs *S, uint32_t parent, unsigned kind);
/* the file of a scope, of a node; is the scope an implementation */
uint32_t lxs_unit_of(const limba_lxs *S, uint32_t scope, bool *impl);
uint32_t lxs_unit_at(const limba_lxs *S, uint32_t node);
/* the name in scope, through the units it uses (§ 11.3): 0 if none;
   node, if not 0, is where the errors and warnings go */
limba_sym lxs_find(limba_lxs *S, uint32_t scope, uint32_t name, uint32_t node);
/* Unit.Name: the symbol, 0 (reported) if none */
limba_sym lxs_qualified(limba_lxs *S, uint32_t scope, uint32_t unit,
                        uint32_t name, uint32_t node);
/* is there a restriction no_external where node is (§ 10.4, § 11.6) */
bool lxs_restricted(const limba_lxs *S, uint32_t node);
/* a declaration at the level of a file: the names of units it may not
   take, the names of units it hides (§ 11.3) */
bool lxs_unit_declare(limba_lxs *S, uint32_t scope, uint32_t name_node);
/* record what is being checked uses: a routine called, a variable of a
   file read or written */
void lxs_use(limba_lxs *S, limba_sym what, uint32_t node, bool write);
/* the units: their scopes, names and declarations, bodies and
   initialisations; then the order of the initialisations */
void lxs_check_units(limba_lxs *S);
/* after the bodies: pragmas hides that hid nothing, units never named,
   what is reached, the order of the initialisations */
void lxs_unit_warnings(limba_lxs *S);
void lxs_unit_free(limba_lxs *S);
/* the heading of a routine whose body is in the implementation, 0 if
   none */
uint32_t lxs_heading(const limba_lxs *S, limba_sym s);

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

/* the boundary with C (sema.c): is the PRAGMA node named so; pragma
   convention or restrictions, in scope (0: at the start of the file,
   before any declaration is known); the signature of an external
   routine */
bool lxs_pragma_is(limba_lxs *S, uint32_t node, const char *name);
void lxs_c_pragma(limba_lxs *S, uint32_t node, uint32_t scope);
void lxs_c_routine(limba_lxs *S, limba_sym s);

/* declarations and types (sema.c) */
limba_sym lxs_lookup(limba_lxs *S, uint32_t scope, uint32_t node);
void lxs_force(limba_lxs *S, limba_sym s);
/* where a type is written: a variable may have computed bounds, a
   parameter may be an open array, and a type declaration may name one */
#define LXT_VAR 1u
#define LXT_PARAM 2u
#define LXT_DECL 4u
#define LXT_PTR 8u /* the target of a pointer: an open array may be */
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
/* a PRAGMA node: its names checked, its op set */
void lxs_pragma(limba_lxs *S, uint32_t node);
void lxs_routine_body(limba_lxs *S, limba_sym routine);

#endif
