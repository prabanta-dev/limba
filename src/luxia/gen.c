/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gen.c - random Luxia programs and their expected output (see gen.h).
 *
 * The program is a small tree: types, expressions, statements, blocks,
 * routines. It is grown from the variables in scope, so it is valid by
 * construction: both operands of an operator have one type, a constant
 * never meets another constant (the front end would compute it, and
 * complain when it does not fit), a literal always fits where it goes,
 * loops count to a small bound. Functions are pure (no output, no global
 * written, no var parameter), so the order in which an expression calls
 * them does not show; routines call only the routines made before them.
 *
 * The run follows the rules of the specification: checked arithmetic on
 * IntN and UIntN, modular arithmetic on BitsN, div towards zero, mod with
 * the sign of the divisor, rem with the sign of the dividend, and and or
 * on Boolean that stop early, conversions that must hold the value (the
 * BitsN take the low bits), operations on a range done in its base type
 * and the range checked where a value is stored (assignment, declaration,
 * argument, return, the bounds of a for), array indices always checked;
 * operands, arguments and the items of writeln from left to right, each
 * item printed before the next is computed, the target of an assignment
 * before its value, continue in repeat going to the test. The run-time
 * error codes are not in the specification yet: 6 overflow, 11 division
 * by zero, 100 index, 101 range, 103 conversion, 104 shift.
 */
#include "gen.h"

#include "common/xalloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef __int128 v128;
typedef unsigned __int128 u128;

/* the scalar types of the language; ranges and arrays follow them in the
   table of a program */
enum {
    T_I8,
    T_I16,
    T_I32,
    T_I64,
    T_U8,
    T_U16,
    T_U32,
    T_U64,
    T_B8,
    T_B16,
    T_B32,
    T_B64,
    T_BOOL,
    NTYPES
};

static const char *const tname[NTYPES] = {
    "Int8",   "Int16", "Int32",  "Int64",  "UInt8",  "UInt16",  "UInt32",
    "UInt64", "Bits8", "Bits16", "Bits32", "Bits64", "Boolean",
};

static char fam(unsigned t)
{
    return t == T_BOOL ? 'L' : t < T_U8 ? 'S' : t < T_B8 ? 'U' : 'B';
}

static unsigned tbits(unsigned t)
{
    return 8u << (t & 3);
}

static v128 tmin(unsigned t)
{
    return fam(t) == 'S' ? -((v128)1 << (tbits(t) - 1)) : 0;
}

static v128 tmax(unsigned t)
{
    if (t == T_BOOL)
        return 1;
    if (fam(t) == 'S')
        return ((v128)1 << (tbits(t) - 1)) - 1;
    return ((v128)1 << tbits(t)) - 1;
}

/* K_RECORD: fields index..index+elem-1 of the table of fields; K_PTR:
   a pointer to record elem */
enum { K_BASE, K_RANGE, K_ARRAY, K_OPEN, K_RECORD, K_PTR };

typedef struct {
    uint8_t k, base;      /* base: the scalar type of a range or of itself */
    uint32_t index, elem; /* of an open array, the index is a scalar */
    v128 lo, hi;          /* of a range; of the index of an array */
} xt;

enum {
    O_ADD,
    O_SUB,
    O_MUL,
    O_DIV,
    O_MOD,
    O_REM,
    O_AND,
    O_OR,
    O_XOR,
    O_SHL,
    O_SHR,
    O_EQ,
    O_NE,
    O_LT,
    O_LE,
    O_GT,
    O_GE,
    O_NEG,
    O_ABS,
    O_NOT,
    O_POW
};

static const char *const otext[] = {
    "+", "-",  "*", "div", "mod", "rem", "and", "or",  "xor", "shl", "shr",
    "=", "<>", "<", "<=",  ">",   ">=",  "-",   "abs", "not", "**",
};

/* E_IN: a in type fn, or a in b..c when fn is 0; E_LOW, E_HIGH, E_LEN:
   of the array variable var */
enum {
    E_LIT,
    E_VAR,
    E_BIN,
    E_UN,
    E_CONV,
    E_CALL,
    E_STR,
    E_INDEX,
    E_IN,
    E_LOW,
    E_HIGH,
    E_LEN,
    E_FIELD, /* field b of variable var, a record or a pointer to one */
    E_NIL
};

typedef struct {
    uint8_t k, op;
    bool cst; /* a constant: the front end knows its value, which is lit */
    uint32_t t;
    v128 lit;
    uint32_t var, fn, a, b, c, args, nargs;
    uint32_t line, col; /* where a run-time error here is reported */
} xe;

enum {
    S_VAR,
    S_ASSIGN,
    S_IF,
    S_WHILE,
    S_REPEAT,
    S_FOR,
    S_CASE,
    S_WRITE,
    S_CALL,
    S_EXIT,
    S_CONT,
    S_RETURN,
    S_CONST,
    S_NEW, /* var := new(its record) */
    S_COPY /* var := the record variable of e */
};

typedef struct {
    uint8_t k;
    bool down, has_alt;
    uint32_t var, idx, e, e2; /* idx, e, e2: 0 for none (node 0 unused) */
    uint32_t blk, nblk;       /* the body */
    uint32_t alt, nalt;       /* else */
    uint32_t arm, narm;       /* if and case */
    uint32_t fn, args, nargs;
    uint32_t line, col;   /* the statement */
    uint32_t nline, ncol; /* the name it declares */
    uint32_t iline, icol; /* the [ or the . of its target */
    uint32_t fld;         /* a field of the target, plus 1; 0 for none */
} xs;

typedef struct {
    uint32_t cond;      /* if */
    uint32_t lab, nlab; /* case */
    uint32_t blk, nblk;
} xarm;

typedef struct {
    v128 lo, hi;
} xlab;

/* V_CONST: a constant without a type (t is Int64, for printing only);
   V_TCONST: a constant of type t */
enum { V_GLOBAL, V_LOCAL, V_IN, V_VAR, V_LOOP, V_COUNT, V_CONST, V_TCONST };
static const char vprefix[] = {'g', 'v', 'a', 'a', 'i', 'w', 'k', 'k'};

typedef struct {
    uint32_t t;
    uint8_t kind;
    v128 val; /* of a constant */
} xv;

typedef struct {
    bool func;
    uint32_t rt;
    uint32_t par[4];
    uint8_t np;
    uint32_t blk, nblk;
} xr;

typedef struct {
    uint64_t s;
    xt *ty;
    uint32_t nty, capty;
    xe *e;
    uint32_t ne, cape;
    xs *st;
    uint32_t ns, caps;
    xarm *arm;
    uint32_t narm, caparm;
    xlab *lab;
    uint32_t nlab, caplab;
    uint32_t *ls; /* blocks and argument lists */
    uint32_t nls, capls;
    xv *v;
    uint32_t nv, capv;
    xr *r;
    uint32_t nr, capr;
    uint32_t *scope; /* the visible variables */
    uint32_t nscope, capscope;
    int cur;     /* the routine being made, -1 for the program */
    int loops;   /* loops around the statement being made */
    int budget;  /* statements left */
    int nesting; /* blocks around the statement being made */
    uint32_t main_blk, main_n;
    uint32_t *gc; /* the statements of the global constants */
    uint32_t ngc, capgc;
    uint32_t *fld; /* the types of the fields of the records */
    uint32_t nfld, capfld;
} G;

/* ---- randomness ---- */

static uint64_t rnd(G *g)
{
    uint64_t z = (g->s += 0x9e3779b97f4a7c15ull); /* splitmix64 */
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint32_t below(G *g, uint32_t n)
{
    return n ? (uint32_t)(rnd(g) % n) : 0;
}

static bool chance(G *g, unsigned pct)
{
    return below(g, 100) < pct;
}

static unsigned int_type(G *g)
{
    return below(g, T_BOOL);
}

static unsigned any_type(G *g)
{
    return chance(g, 15) ? T_BOOL : int_type(g);
}

static v128 clamp_in(v128 lo, v128 hi, v128 v)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* a value of lo..hi, often one at an edge */
static v128 rand_in(G *g, v128 lo, v128 hi)
{
    switch (below(g, 12)) {
    case 0:
        return lo;
    case 1:
        return hi;
    case 2:
        return clamp_in(lo, hi, lo + 1);
    case 3:
        return clamp_in(lo, hi, hi - 1);
    case 4:
        return clamp_in(lo, hi, lo < 0 ? -1 : 2);
    case 5: {
        u128 range = (u128)(hi - lo) + 1;
        u128 r = ((u128)rnd(g) << 64 | rnd(g)) % range;
        return lo + (v128)r;
    }
    default:
        return clamp_in(lo, hi, (v128)below(g, 41) - 20);
    }
}

static v128 rand_value(G *g, unsigned t)
{
    return t == T_BOOL ? below(g, 2) : rand_in(g, tmin(t), tmax(t));
}

/* ---- types ---- */

static unsigned base(const G *g, uint32_t t)
{
    return g->ty[t].base;
}

static v128 lo_of(const G *g, uint32_t t)
{
    return g->ty[t].lo;
}

static v128 hi_of(const G *g, uint32_t t)
{
    return g->ty[t].hi;
}

static bool is_scalar(const G *g, uint32_t t)
{
    return g->ty[t].k == K_BASE || g->ty[t].k == K_RANGE;
}

static bool is_record(const G *g, uint32_t t)
{
    return g->ty[t].k == K_RECORD;
}

static bool is_ptr(const G *g, uint32_t t)
{
    return g->ty[t].k == K_PTR;
}

/* the record a variable of type t names: itself, or what it points to */
static uint32_t record_of(const G *g, uint32_t t)
{
    return is_ptr(g, t) ? g->ty[t].elem : t;
}

static bool is_array(const G *g, uint32_t t)
{
    return g->ty[t].k == K_ARRAY || g->ty[t].k == K_OPEN;
}

/* the base type of the index of an array type */
static unsigned index_base(const G *g, uint32_t at)
{
    return g->ty[g->ty[at].index].base;
}

static uint32_t new_type(G *g, xt x)
{
    LIMBA_GROW(g->ty, g->nty, g->capty);
    g->ty[g->nty] = x;
    return g->nty++;
}

/* a range of an integer type (not Bits): short (the index of an array)
   or any */
static uint32_t new_range(G *g, bool short_range)
{
    unsigned b = below(g, T_B8);
    v128 lo = rand_value(g, b), hi;
    if (short_range) {
        lo = clamp_in(tmin(b), tmax(b) - 5, lo);
        hi = lo + below(g, 6);
    } else {
        hi = rand_value(g, b);
        if (hi < lo) {
            v128 x = lo;
            lo = hi;
            hi = x;
        }
    }
    return new_type(g, (xt){K_RANGE, (uint8_t)b, 0, 0, lo, hi});
}

/* the type of a variable: a scalar, sometimes a range of the program */
static uint32_t var_type(G *g)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t t = NTYPES; t < g->nty; t++)
        if (g->ty[t].k == K_RANGE && below(g, ++n) == 0)
            chosen = t;
    return n && chance(g, 30) ? chosen : any_type(g);
}

/* ---- the tree ---- */

static uint32_t new_e(G *g, unsigned k, uint32_t t)
{
    LIMBA_GROW(g->e, g->ne, g->cape);
    memset(&g->e[g->ne], 0, sizeof(xe));
    g->e[g->ne].k = (uint8_t)k;
    g->e[g->ne].t = t;
    return g->ne++;
}

static uint32_t new_s(G *g, unsigned k)
{
    LIMBA_GROW(g->st, g->ns, g->caps);
    memset(&g->st[g->ns], 0, sizeof(xs));
    g->st[g->ns].k = (uint8_t)k;
    return g->ns++;
}

static uint32_t new_v(G *g, uint32_t t, unsigned kind)
{
    LIMBA_GROW(g->v, g->nv, g->capv);
    g->v[g->nv] = (xv){t, (uint8_t)kind, 0};
    return g->nv++;
}

static void show(G *g, uint32_t v)
{
    LIMBA_GROW(g->scope, g->nscope, g->capscope);
    g->scope[g->nscope++] = v;
}

/* copy a list built aside into the pool; its start */
static uint32_t keep_list(G *g, const uint32_t *x, uint32_t n)
{
    uint32_t at = g->nls;
    for (uint32_t i = 0; i < n; i++) {
        LIMBA_GROW(g->ls, g->nls, g->capls);
        g->ls[g->nls++] = x[i];
    }
    return at;
}

static uint32_t lit(G *g, uint32_t t, v128 v)
{
    uint32_t i = new_e(g, E_LIT, t);
    g->e[i].lit = v;
    g->e[i].cst = true;
    return i;
}

static bool is_const(const G *g, uint32_t v)
{
    return g->v[v].kind == V_CONST || g->v[v].kind == V_TCONST;
}

static uint32_t var_ref(G *g, uint32_t v)
{
    uint32_t i = new_e(g, E_VAR, g->v[v].t);
    g->e[i].var = v;
    if (is_const(g, v)) {
        g->e[i].cst = true;
        g->e[i].lit = g->v[v].val;
    }
    return i;
}

static uint32_t binop(G *g, unsigned op, uint32_t t, uint32_t a, uint32_t b)
{
    uint32_t i = new_e(g, E_BIN, t);
    g->e[i].op = (uint8_t)op;
    g->e[i].a = a;
    g->e[i].b = b;
    return i;
}

static bool in_function(const G *g)
{
    return g->cur >= 0 && g->r[g->cur].func;
}

static bool writable(const G *g, uint32_t v)
{
    unsigned k = g->v[v].kind;
    if (k == V_IN || k == V_LOOP || k == V_COUNT || is_const(g, v))
        return false;
    return !(k == V_GLOBAL && in_function(g));
}

/* a visible scalar variable whose type has base t (NTYPES: any integer;
   exact: the type is t itself); -1 if none */
static int pick_var(G *g, uint32_t t, bool write, bool exact)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        uint32_t vt = g->v[v].t;
        if (!is_scalar(g, vt) || is_const(g, v))
            continue;
        if (exact         ? vt != t
            : t == NTYPES ? base(g, vt) == T_BOOL
                          : base(g, vt) != t)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a visible array whose elements have base t (NTYPES: any); -1 if none */
static int pick_array(G *g, uint32_t t, bool write)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        uint32_t vt = g->v[v].t;
        if (!is_array(g, vt))
            continue;
        if (t != NTYPES && base(g, g->ty[vt].elem) != t)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a function made before the current routine, whose result has base t;
   -1 if none */
static int pick_func(G *g, unsigned t)
{
    uint32_t top = g->cur >= 0 ? (uint32_t)g->cur : g->nr, n = 0, chosen = 0;
    for (uint32_t i = 0; i < top; i++)
        if (g->r[i].func && base(g, g->r[i].rt) == t && below(g, ++n) == 0)
            chosen = i;
    return n ? (int)chosen : -1;
}

static uint32_t expr(G *g, unsigned t, int d, bool need_var);

/* a value to store where type t is: a literal is made to fit it, any
   other value is checked when stored */
static uint32_t value_for(G *g, uint32_t t, int d)
{
    uint32_t e = expr(g, base(g, t), d, false);
    /* a constant must fit where it goes, or the front end refuses it */
    if (g->e[e].cst && base(g, t) != T_BOOL &&
        (g->e[e].lit < lo_of(g, t) || g->e[e].lit > hi_of(g, t)))
        e = lit(g, t, rand_in(g, lo_of(g, t), hi_of(g, t)));
    return e;
}

static int depth(G *g)
{
    return 1 + (int)below(g, 4);
}

/* an index of array type at: a literal inside it, or any value */
/* low(a), high(a) or length(a) of array variable v: constants for an
   array of the program, values for an open parameter */
static uint32_t bound(G *g, unsigned k, uint32_t v)
{
    uint32_t at = g->v[v].t;
    uint32_t i = new_e(g, k, index_base(g, at));
    g->e[i].var = v;
    if (g->ty[at].k == K_ARRAY) {
        const xt *x = &g->ty[at];
        g->e[i].cst = true;
        g->e[i].lit = k == E_LOW    ? x->lo
                      : k == E_HIGH ? x->hi
                                    : x->hi - x->lo + 1;
    }
    return i;
}

/* an index of array variable v: a literal inside a static array, or low
   and high and near them, or any value */
static uint32_t index_for(G *g, uint32_t v, int d)
{
    uint32_t at = g->v[v].t;
    if (g->ty[at].k == K_OPEN && chance(g, 70)) {
        unsigned b = index_base(g, at);
        uint32_t e = bound(g, chance(g, 50) ? E_LOW : E_HIGH, v);
        if (chance(g, 50))
            e = binop(g, g->e[e].k == E_LOW ? O_ADD : O_SUB, b, e,
                      lit(g, b, below(g, 3)));
        return e;
    }
    return value_for(g, g->ty[at].index, d);
}

/* a visible array variable whose index has base b; -1 if none */
static int pick_indexed(G *g, unsigned b)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        if (is_array(g, g->v[v].t) && index_base(g, g->v[v].t) == b &&
            below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a visible array variable to pass to open parameter type ot: an index
   of the same base, the same elements; -1 if none */
static int pick_argument(G *g, uint32_t ot, bool write)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i], vt = g->v[v].t;
        if (!is_array(g, vt) || index_base(g, vt) != index_base(g, ot) ||
            g->ty[vt].elem != g->ty[ot].elem)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

static uint32_t element(G *g, uint32_t v, int d)
{
    uint32_t at = g->v[v].t;
    uint32_t ix = index_for(g, v, d - 1);
    uint32_t i = new_e(g, E_INDEX, g->ty[at].elem);
    g->e[i].var = v;
    g->e[i].a = ix;
    return i;
}

static uint32_t typed_arg(G *g, uint32_t pt, bool write);

static uint32_t call_expr(G *g, uint32_t fn, int d)
{
    uint32_t a[4];
    uint32_t np = g->r[fn].np;
    for (uint32_t k = 0; k < np; k++) {
        uint32_t pt = g->v[g->r[fn].par[k]].t;
        if (g->ty[pt].k == K_OPEN) {
            /* the array its type came from is a global: always there */
            a[k] = var_ref(g, (uint32_t)pick_argument(g, pt, false));
        } else if (is_record(g, pt) || is_ptr(g, pt)) {
            /* a global of every such type exists */
            a[k] = typed_arg(g, pt, false);
        } else {
            a[k] = value_for(g, pt, d - 1);
        }
    }
    uint32_t i = new_e(g, E_CALL, g->r[fn].rt);
    g->e[i].fn = fn;
    g->e[i].args = keep_list(g, a, np);
    g->e[i].nargs = np;
    return i;
}

static uint32_t conv(G *g, uint32_t t, uint32_t a)
{
    uint32_t i = new_e(g, E_CONV, t);
    g->e[i].a = a;
    return i;
}

/* an integer type whose values all fit t: a conversion that holds */
static unsigned within(G *g, unsigned t)
{
    unsigned c[NTYPES], n = 0;
    for (unsigned u = 0; u < T_BOOL; u++)
        if (fam(t) == 'B' || (tmin(u) >= tmin(t) && tmax(u) <= tmax(t)))
            c[n++] = u;
    return c[below(g, n)];
}

/* ---- records and pointers ---- */

static uint32_t field_type(const G *g, uint32_t rt, uint32_t f)
{
    return g->fld[g->ty[rt].index + f];
}

/* a field of a record variable, or through a pointer variable, whose type
   has base t (NTYPES: any); its index in *f; -1 if none. write: the
   field is to be assigned (through a pointer only where the heap may be
   written, not in a function) */
static int pick_field(G *g, unsigned t, bool write, uint32_t *f)
{
    uint32_t n = 0;
    int chosen = -1;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i], vt = g->v[v].t;
        if (is_const(g, v) || !(is_record(g, vt) || is_ptr(g, vt)))
            continue;
        if (write && (is_record(g, vt) ? !writable(g, v) : in_function(g)))
            continue;
        uint32_t rt = record_of(g, vt);
        for (uint32_t k = 0; k < g->ty[rt].elem; k++) {
            if (t != NTYPES && base(g, field_type(g, rt, k)) != t)
                continue;
            if (below(g, ++n) == 0) {
                chosen = (int)v;
                *f = k;
            }
        }
    }
    return chosen;
}

static uint32_t field_ref(G *g, uint32_t v, uint32_t f)
{
    uint32_t i = new_e(g, E_FIELD, field_type(g, record_of(g, g->v[v].t), f));
    g->e[i].var = v;
    g->e[i].b = f;
    return i;
}

/* a visible variable of type t exactly (a record or a pointer) that is
   not a constant, writable if asked; with a predicate of kind instead
   when t is 0: 1 records, 2 pointers; -1 if none */
static int pick_typed(G *g, uint32_t t, bool write)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i], vt = g->v[v].t;
        if (is_const(g, v))
            continue;
        if (t == 1 ? !is_record(g, vt) : t == 2 ? !is_ptr(g, vt) : vt != t)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

static uint32_t nil_of(G *g, uint32_t pt)
{
    return new_e(g, E_NIL, pt);
}

/* an argument for a record or pointer parameter of type pt: a variable
   of that type (writable for a var parameter), or nil; 0 if none */
static uint32_t typed_arg(G *g, uint32_t pt, bool write)
{
    if (is_ptr(g, pt) && chance(g, 25))
        return nil_of(g, pt);
    int v = pick_typed(g, pt, write);
    return v < 0 ? 0 : var_ref(g, (uint32_t)v);
}

/* p := new(R) and a value for each field, p := q or nil, r1 := r2: into
   out, the count (0 if nothing fits) */
static uint32_t heap_stmt(G *g, uint32_t *out)
{
    if (chance(g, 30)) {
        int d = pick_typed(g, 1, true);
        if (d < 0)
            return 0;
        int src = pick_typed(g, g->v[d].t, false);
        for (int k = 0; k < 4 && src == d; k++)
            src = pick_typed(g, g->v[d].t, false); /* another, if any */
        uint32_t s = new_s(g, S_COPY);
        g->st[s].var = (uint32_t)d;
        g->st[s].e = var_ref(g, (uint32_t)src);
        out[0] = s;
        return 1;
    }
    int p = pick_typed(g, 2, true);
    if (p < 0)
        return 0;
    uint32_t pt = g->v[p].t, rt = g->ty[pt].elem;
    if (chance(g, 60)) {
        uint32_t n = 0;
        uint32_t s = new_s(g, S_NEW);
        g->st[s].var = (uint32_t)p;
        out[n++] = s;
        for (uint32_t k = 0; k < g->ty[rt].elem; k++) {
            /* literals: a field of a new record has no value yet, and an
               expression could read one */
            uint32_t ft = field_type(g, rt, k);
            uint32_t e = lit(g, ft,
                             base(g, ft) == T_BOOL
                                 ? below(g, 2)
                                 : rand_in(g, lo_of(g, ft), hi_of(g, ft)));
            uint32_t a = new_s(g, S_ASSIGN);
            g->st[a].var = (uint32_t)p;
            g->st[a].fld = k + 1;
            g->st[a].e = e;
            out[n++] = a;
        }
        return n;
    }
    int q = pick_typed(g, pt, false);
    uint32_t e =
        q >= 0 && chance(g, 70) ? var_ref(g, (uint32_t)q) : nil_of(g, pt);
    uint32_t s = new_s(g, S_ASSIGN);
    g->st[s].var = (uint32_t)p;
    g->st[s].e = e;
    out[0] = s;
    return 1;
}

/* ---- constants computed exactly ---- */

#define CONST_LIMIT ((v128)1 << 100)

static bool small(v128 v)
{
    return v <= CONST_LIMIT && v >= -CONST_LIMIT;
}

/* a * b within the limit, into *out */
static bool mul_small(v128 a, v128 b, v128 *out)
{
    v128 ma = a < 0 ? -a : a, mb = b < 0 ? -b : b;
    if (ma && mb > CONST_LIMIT / ma)
        return false;
    *out = a * b;
    return true;
}

static uint32_t cst_node(G *g, uint32_t i, v128 v)
{
    g->e[i].cst = true;
    g->e[i].lit = v;
    return i;
}

/* an exact constant expression without a type (§ 4.4): literals, the
   constants without a type in scope, + - * div mod rem **, abs and the
   minus; the front end computes it with no limit, here every value stays
   within 2^100, and its value is in lit */
static uint32_t cexpr(G *g, int d)
{
    if (d <= 0 || chance(g, 30)) {
        uint32_t n = 0, c = 0;
        for (uint32_t i = 0; i < g->nscope; i++) {
            uint32_t v = g->scope[i];
            if (g->v[v].kind == V_CONST && small(g->v[v].val) &&
                below(g, ++n) == 0)
                c = v;
        }
        if (n && chance(g, 50))
            return var_ref(g, c);
        v128 v = chance(g, 20) ? (v128)(rnd(g) >> 24) : (v128)below(g, 41) - 20;
        return lit(g, T_I64, chance(g, 10) ? -v : v);
    }
    unsigned op = below(g, 9);
    uint32_t a = cexpr(g, d - 1);
    v128 x = g->e[a].lit, v;
    if (op >= 7) {
        uint32_t i = new_e(g, E_UN, T_I64);
        g->e[i].op = op == 7 ? O_NEG : O_ABS;
        g->e[i].a = a;
        return cst_node(g, i, op == 7 || x < 0 ? -x : x);
    }
    if (op == 6) {
        unsigned n = below(g, 6);
        v = 1;
        for (unsigned k = 0; k < n; k++)
            if (!mul_small(v, x, &v))
                return a;
        uint32_t e = lit(g, T_I64, n);
        return cst_node(g, binop(g, O_POW, T_I64, a, e), v);
    }
    uint32_t b = cexpr(g, d - 1);
    v128 y = g->e[b].lit;
    switch (op) {
    case 0:
        v = x + y;
        break;
    case 1:
        v = x - y;
        break;
    case 2:
        if (!mul_small(x, y, &v))
            return a;
        break;
    default:
        if (y == 0)
            return a; /* the front end refuses a constant division by 0 */
        v = op == 3 ? x / y : x % y;
        if (op == 4 && v != 0 && (v < 0) != (y < 0))
            v += y; /* mod has the sign of the divisor */
    }
    if (!small(v))
        return a;
    static const unsigned ops[] = {O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_REM};
    return cst_node(g, binop(g, ops[op], T_I64, a, b), v);
}

/* a constant expression that fits type t: (e) mod 97 when e does not */
static uint32_t cexpr_in(G *g, uint32_t t, int d)
{
    uint32_t e = cexpr(g, d);
    v128 v = g->e[e].lit;
    if (v >= lo_of(g, t) && v <= hi_of(g, t))
        return e;
    v128 m = v % 97;
    if (m < 0)
        m += 97;
    if (m >= lo_of(g, t) && m <= hi_of(g, t))
        return cst_node(g, binop(g, O_MOD, T_I64, e, lit(g, T_I64, 97)), m);
    return lit(g, t, rand_in(g, lo_of(g, t), hi_of(g, t)));
}

/* const k = e; or const k: T = e; its statement */
static uint32_t constant(G *g)
{
    uint32_t s = new_s(g, S_CONST);
    uint32_t t = T_I64, e;
    unsigned kind = V_CONST;
    if (chance(g, 40)) {
        t = var_type(g);
        if (base(g, t) == T_BOOL)
            t = int_type(g);
        kind = V_TCONST;
        e = cexpr_in(g, t, depth(g));
    } else {
        e = cexpr(g, depth(g));
    }
    uint32_t v = new_v(g, t, kind);
    g->v[v].val = g->e[e].lit;
    g->st[s].var = v;
    g->st[s].e = e;
    show(g, v);
    return s;
}

/* a constant of base t: one declared, or an expression of constants */
static uint32_t const_leaf(G *g, unsigned t)
{
    uint32_t n = 0, c = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        const xv *x = &g->v[v];
        bool ok =
            x->kind == V_TCONST
                ? base(g, x->t) == t
                : x->kind == V_CONST && x->val >= tmin(t) && x->val <= tmax(t);
        if (ok && below(g, ++n) == 0)
            c = v;
    }
    if (n && chance(g, 50))
        return var_ref(g, c);
    return cexpr_in(g, t, 1 + (int)below(g, 3));
}

/* a variable, an element, a literal, or a variable of another type made
   into t */
static uint32_t leaf(G *g, unsigned t, bool need_var, int d)
{
    if (!need_var && t != T_BOOL && chance(g, 12))
        return const_leaf(g, t);
    if (chance(g, 15)) {
        uint32_t f;
        int v = pick_field(g, t, false, &f);
        if (v >= 0)
            return field_ref(g, (uint32_t)v, f);
    }
    int v = pick_var(g, t, false, false);
    int a = d > 0 && chance(g, 25) ? pick_array(g, t, false) : -1;
    if (a >= 0)
        return element(g, (uint32_t)a, d);
    int x = t != T_BOOL && chance(g, 10) ? pick_indexed(g, t) : -1;
    if (x >= 0) {
        uint32_t b = bound(g, E_LOW + below(g, 3), (uint32_t)x);
        if (!need_var || !g->e[b].cst)
            return b;
    }
    if (v >= 0 && (need_var || chance(g, 65)))
        return var_ref(g, (uint32_t)v);
    if (!need_var)
        return lit(g, t, rand_value(g, t));
    int w = pick_var(g, NTYPES, false, false);
    if (w < 0) {
        /* none of an integer type: a Boolean compared with a literal */
        w = pick_var(g, T_BOOL, false, false);
        return binop(g, O_EQ, T_BOOL, var_ref(g, (uint32_t)w),
                     lit(g, T_BOOL, below(g, 2)));
    }
    if (t == T_BOOL) {
        unsigned u = base(g, g->v[w].t);
        return binop(g, O_EQ + below(g, 6), T_BOOL, var_ref(g, (uint32_t)w),
                     lit(g, u, rand_value(g, u)));
    }
    if (chance(g, 70)) {
        int x = pick_var(g, within(g, t), false, false);
        if (x >= 0)
            w = x;
    }
    return conv(g, t, var_ref(g, (uint32_t)w));
}

/* x in R, or x in lo..hi */
static uint32_t membership(G *g, int d)
{
    uint32_t n = 0, range = 0;
    for (uint32_t t = NTYPES; t < g->nty; t++)
        if (g->ty[t].k == K_RANGE && below(g, ++n) == 0)
            range = t;
    unsigned b = n && chance(g, 60) ? base(g, range) : int_type(g);
    /* the parts first: making them may move the nodes */
    uint32_t a = expr(g, b, d - 1, true), fn = 0, lo = 0, hi = 0;
    if (n && base(g, range) == b && chance(g, 60)) {
        fn = range;
    } else {
        v128 l = rand_value(g, b), h = rand_value(g, b);
        if (h < l && chance(g, 80)) {
            v128 x = l;
            l = h;
            h = x;
        }
        lo = chance(g, 70) ? lit(g, b, l) : expr(g, b, d - 1, false);
        hi = chance(g, 70) ? lit(g, b, h) : expr(g, b, d - 1, false);
    }
    uint32_t i = new_e(g, E_IN, T_BOOL);
    g->e[i].a = a;
    g->e[i].fn = fn;
    g->e[i].b = lo;
    g->e[i].c = hi;
    return i;
}

static uint32_t expr(G *g, unsigned t, int d, bool need_var)
{
    if (d <= 0 || chance(g, 20))
        return leaf(g, t, need_var, d);
    unsigned f = fam(t);
    unsigned choice = below(g, 10);
    if (choice == 9) {
        int fn = pick_func(g, t);
        if (fn >= 0)
            return call_expr(g, (uint32_t)fn, d);
        choice = 0;
    }
    if (f == 'L') {
        int p = chance(g, 10) ? pick_typed(g, 2, false) : -1;
        if (p >= 0) {
            uint32_t pt = g->v[p].t;
            int q = pick_typed(g, pt, false);
            uint32_t other = q >= 0 && chance(g, 50) ? var_ref(g, (uint32_t)q)
                                                     : nil_of(g, pt);
            return binop(g, chance(g, 50) ? O_EQ : O_NE, T_BOOL,
                         var_ref(g, (uint32_t)p), other);
        }
        if (choice < 4) {
            unsigned u = any_type(g);
            uint32_t a = expr(g, u, d - 1, false);
            uint32_t b = expr(g, u, d - 1, g->e[a].cst);
            return binop(g, O_EQ + below(g, 6), T_BOOL, a, b);
        }
        if (choice < 5)
            return membership(g, d);
        if (choice < 8) {
            uint32_t a = expr(g, T_BOOL, d - 1, false);
            uint32_t b = expr(g, T_BOOL, d - 1, g->e[a].cst);
            return binop(g, O_AND + below(g, 3), T_BOOL, a, b);
        }
        uint32_t i = new_e(g, E_UN, T_BOOL);
        g->e[i].op = O_NOT;
        uint32_t a = expr(g, T_BOOL, d - 1, true);
        g->e[i].a = a;
        return i;
    }
    if (choice < 6) {
        unsigned op = f == 'B' ? below(g, 11) : below(g, 6);
        if (op == O_SHL || op == O_SHR) {
            uint32_t a = expr(g, t, d - 1, true);
            uint32_t n = chance(g, 50) ? lit(g, T_I32, below(g, tbits(t)))
                                       : expr(g, int_type(g), d - 1, true);
            return binop(g, op, t, a, n);
        }
        bool divide = op >= O_DIV && op <= O_REM;
        uint32_t a = expr(g, t, d - 1, divide);
        /* most divisors are literals, so that fewer programs stop early */
        uint32_t b = divide && chance(g, 70) ? lit(g, t, rand_value(g, t))
                                             : expr(g, t, d - 1, g->e[a].cst);
        if (divide && g->e[b].k == E_LIT && g->e[b].lit == 0)
            g->e[b].lit = 1; /* the front end may reject a literal 0 */
        return binop(g, op, t, a, b);
    }
    if (choice < 8) {
        unsigned op = f == 'S'   ? (chance(g, 50) ? O_NEG : O_ABS)
                      : f == 'U' ? O_ABS
                                 : (chance(g, 50) ? O_NEG : O_NOT);
        uint32_t a = expr(g, t, d - 1, true);
        uint32_t i = new_e(g, E_UN, t);
        g->e[i].op = (uint8_t)op;
        g->e[i].a = a;
        return i;
    }
    /* a conversion, sometimes to a range of the same base */
    uint32_t to = t;
    for (uint32_t r = NTYPES; r < g->nty; r++)
        if (g->ty[r].k == K_RANGE && base(g, r) == t && chance(g, 30))
            to = r;
    return conv(
        g, to,
        expr(g, chance(g, 60) ? within(g, t) : int_type(g), d - 1, true));
}

/* ---- statements ---- */

static uint32_t block(G *g, int n, uint32_t *count);

/* a counter that bounds a while or a repeat: var w := 0 */
static uint32_t counter(G *g, uint32_t *decl)
{
    uint32_t w = new_v(g, T_I32, V_COUNT);
    *decl = new_s(g, S_VAR);
    g->st[*decl].var = w;
    g->st[*decl].e = lit(g, T_I32, 0);
    show(g, w);
    return w;
}

static uint32_t bump(G *g, uint32_t w)
{
    uint32_t s = new_s(g, S_ASSIGN);
    uint32_t e = binop(g, O_ADD, T_I32, var_ref(g, w), lit(g, T_I32, 1));
    g->st[s].var = w;
    g->st[s].e = e;
    return s;
}

/* the body of a loop, whose first statement is first (0: none) */
static void loop_body(G *g, uint32_t s, uint32_t first)
{
    uint32_t mark = g->nscope;
    g->loops++;
    uint32_t n;
    uint32_t b = block(g, 1 + (int)below(g, 4), &n);
    g->loops--;
    g->nscope = mark;
    if (first) {
        /* the counter goes first, so continue cannot skip it */
        uint32_t *x = limba_xmalloc((n + 1) * sizeof(uint32_t));
        x[0] = first;
        memcpy(x + 1, g->ls + b, n * sizeof(uint32_t));
        b = keep_list(g, x, n + 1);
        n++;
        free(x);
    }
    g->st[s].blk = b;
    g->st[s].nblk = n;
}

static bool overlaps(const xlab *l, uint32_t n, v128 lo, v128 hi)
{
    for (uint32_t i = 0; i < n; i++)
        if (!(hi < l[i].lo || lo > l[i].hi))
            return true;
    return false;
}

static uint32_t declare(G *g, uint32_t t)
{
    uint32_t s = new_s(g, S_VAR);
    uint32_t e = value_for(g, t, depth(g));
    uint32_t v = new_v(g, t, V_LOCAL);
    g->st[s].var = v;
    g->st[s].e = e;
    show(g, v);
    return s;
}

/* the type of the values a selector can take: its range, if a variable
   or an element of a range type gives it */
static uint32_t selector_type(const G *g, uint32_t e)
{
    unsigned k = g->e[e].k;
    return k == E_VAR || k == E_INDEX || k == E_CALL || k == E_CONV ||
                   k == E_FIELD
               ? g->e[e].t
               : base(g, g->e[e].t);
}

/* one statement, or two when a loop needs its counter declared first;
   they go to out, and the count is returned */
static uint32_t stmt(G *g, uint32_t *out)
{
    g->budget--;
    unsigned c = below(g, 100);
    bool pure = in_function(g);
    bool deep = g->nesting < 4 && g->budget > 4;
    if (!pure && chance(g, 10))
        c = 84; /* a call of a procedure, more often */
    if (chance(g, 4)) {
        out[0] = constant(g);
        return 1;
    }
    if (!pure && chance(g, 6)) {
        uint32_t k = heap_stmt(g, out);
        if (k)
            return k;
    }
    if (c < 14) {
        out[0] = declare(g, var_type(g));
        return 1;
    }
    if (c < 36 && chance(g, 25)) {
        uint32_t f;
        int v = pick_field(g, NTYPES, true, &f);
        if (v >= 0) {
            uint32_t ft = field_type(g, record_of(g, g->v[v].t), f);
            uint32_t e = value_for(g, ft, depth(g));
            uint32_t s = new_s(g, S_ASSIGN);
            g->st[s].var = (uint32_t)v;
            g->st[s].fld = f + 1;
            g->st[s].e = e;
            out[0] = s;
            return 1;
        }
    }
    if (c < 36) {
        int a = chance(g, 30) ? pick_array(g, NTYPES, true) : -1;
        if (a >= 0) {
            uint32_t at = g->v[a].t;
            uint32_t s = new_s(g, S_ASSIGN);
            uint32_t ix = index_for(g, (uint32_t)a, depth(g));
            uint32_t e = value_for(g, g->ty[at].elem, depth(g));
            g->st[s].var = (uint32_t)a;
            g->st[s].idx = ix;
            g->st[s].e = e;
            out[0] = s;
            return 1;
        }
        int v = pick_var(g, any_type(g), true, false);
        if (v < 0)
            v = pick_var(g, NTYPES, true, false);
        if (v >= 0) {
            uint32_t s = new_s(g, S_ASSIGN);
            uint32_t e = value_for(g, g->v[v].t, depth(g));
            g->st[s].var = (uint32_t)v;
            g->st[s].e = e;
            out[0] = s;
            return 1;
        }
    }
    if (c < 48 && deep) {
        uint32_t s = new_s(g, S_IF);
        uint32_t na = 1 + below(g, 3);
        xarm arms[3];
        for (uint32_t i = 0; i < na; i++) {
            arms[i].cond = expr(g, T_BOOL, depth(g), true);
            uint32_t mark = g->nscope;
            arms[i].blk = block(g, 1 + (int)below(g, 3), &arms[i].nblk);
            g->nscope = mark;
        }
        g->st[s].arm = g->narm;
        g->st[s].narm = na;
        for (uint32_t i = 0; i < na; i++) {
            LIMBA_GROW(g->arm, g->narm, g->caparm);
            g->arm[g->narm++] = arms[i];
        }
        if (chance(g, 50)) {
            uint32_t mark = g->nscope;
            uint32_t n;
            uint32_t b = block(g, 1 + (int)below(g, 3), &n);
            g->nscope = mark;
            g->st[s].alt = b;
            g->st[s].nalt = n;
            g->st[s].has_alt = true;
        }
        out[0] = s;
        return 1;
    }
    if (c < 56 && deep) {
        uint32_t decl, w = counter(g, &decl);
        uint32_t s = new_s(g, S_WHILE);
        uint32_t cond = binop(g, O_LT, T_BOOL, var_ref(g, w),
                              lit(g, T_I32, 1 + below(g, 4)));
        if (chance(g, 50))
            cond =
                binop(g, O_AND, T_BOOL, cond, expr(g, T_BOOL, depth(g), true));
        g->st[s].e = cond;
        loop_body(g, s, bump(g, w));
        out[0] = decl;
        out[1] = s;
        return 2;
    }
    if (c < 62 && deep) {
        uint32_t decl, w = counter(g, &decl);
        uint32_t s = new_s(g, S_REPEAT);
        loop_body(g, s, bump(g, w));
        uint32_t cond = binop(g, O_GE, T_BOOL, var_ref(g, w),
                              lit(g, T_I32, 1 + below(g, 4)));
        if (chance(g, 50))
            cond =
                binop(g, O_OR, T_BOOL, cond, expr(g, T_BOOL, depth(g), true));
        g->st[s].e = cond;
        out[0] = decl;
        out[1] = s;
        return 2;
    }
    if (c < 72 && deep) {
        /* an integer type, or a range, for the loop variable */
        uint32_t t = var_type(g);
        if (base(g, t) == T_BOOL)
            t = int_type(g);
        unsigned b = base(g, t);
        uint32_t s = new_s(g, S_FOR);
        bool down = chance(g, 30);
        uint32_t lo, hi;
        int x = pick_var(g, b, false, false);
        int arr = chance(g, 35) ? pick_indexed(g, b) : -1;
        if (arr >= 0) {
            /* over an array: t is the base of its index */
            t = b;
            lo = bound(g, down ? E_HIGH : E_LOW, (uint32_t)arr);
            hi = bound(g, down ? E_LOW : E_HIGH, (uint32_t)arr);
        } else if (x >= 0 && chance(g, 50)) {
            /* from a variable to a few steps away: the bound may overflow
               or leave the range */
            lo = var_ref(g, (uint32_t)x);
            hi = binop(g, down ? O_SUB : O_ADD, b, var_ref(g, (uint32_t)x),
                       lit(g, b, below(g, 4)));
        } else {
            v128 a = rand_in(g, lo_of(g, t), hi_of(g, t));
            v128 z = clamp_in(lo_of(g, t), hi_of(g, t),
                              down ? a - (v128)below(g, 5) + 1
                                   : a + (v128)below(g, 5) - 1);
            lo = lit(g, b, a);
            hi = lit(g, b, z);
        }
        uint32_t v = new_v(g, t, V_LOOP);
        g->st[s].var = v;
        g->st[s].e = lo;
        g->st[s].e2 = hi;
        g->st[s].down = down;
        uint32_t mark = g->nscope;
        show(g, v);
        loop_body(g, s, 0);
        g->nscope = mark;
        out[0] = s;
        return 1;
    }
    if (c < 78 && deep) {
        unsigned b = int_type(g);
        uint32_t s = new_s(g, S_CASE);
        uint32_t sel = expr(g, b, depth(g), true);
        uint32_t st = selector_type(g, sel);
        g->st[s].e = sel;
        uint32_t na = 1 + below(g, 3);
        xarm arms[3];
        xlab labs[12];
        uint32_t nl = 0;
        for (uint32_t i = 0; i < na; i++) {
            arms[i].lab = nl;
            arms[i].nlab = 0;
            for (uint32_t k = 0, want = 1 + below(g, 2); k < want; k++) {
                v128 lo = rand_in(g, lo_of(g, st), hi_of(g, st));
                v128 hi = chance(g, 30) ? clamp_in(lo_of(g, st), hi_of(g, st),
                                                   lo + below(g, 6))
                                        : lo;
                if (overlaps(labs, nl, lo, hi))
                    continue;
                labs[nl++] = (xlab){lo, hi};
                arms[i].nlab++;
            }
            if (!arms[i].nlab) {
                na = i;
                break;
            }
            uint32_t mark = g->nscope;
            arms[i].blk = block(g, 1 + (int)below(g, 3), &arms[i].nblk);
            g->nscope = mark;
        }
        uint32_t first = g->nlab;
        for (uint32_t i = 0; i < nl; i++) {
            LIMBA_GROW(g->lab, g->nlab, g->caplab);
            g->lab[g->nlab++] = labs[i];
        }
        g->st[s].arm = g->narm;
        g->st[s].narm = na;
        for (uint32_t i = 0; i < na; i++) {
            arms[i].lab += first;
            LIMBA_GROW(g->arm, g->narm, g->caparm);
            g->arm[g->narm++] = arms[i];
        }
        uint32_t mark = g->nscope;
        uint32_t n;
        uint32_t bl = block(g, (int)below(g, 3), &n);
        g->nscope = mark;
        g->st[s].alt = bl;
        g->st[s].nalt = n;
        g->st[s].has_alt = true;
        out[0] = s;
        return 1;
    }
    if (c < 84 && g->loops) {
        uint32_t s = new_s(g, chance(g, 50) ? S_EXIT : S_CONT);
        g->st[s].e = expr(g, T_BOOL, depth(g), true);
        out[0] = s;
        return 1;
    }
    if (c < 90 && !pure) {
        /* a procedure made before */
        uint32_t top = g->cur >= 0 ? (uint32_t)g->cur : g->nr, n = 0;
        uint32_t fn = 0;
        for (uint32_t i = 0; i < top; i++)
            if (!g->r[i].func && below(g, ++n) == 0)
                fn = i;
        if (n) {
            uint32_t a[4];
            bool ok = true;
            for (uint32_t k = 0; k < g->r[fn].np && ok; k++) {
                uint32_t p = g->r[fn].par[k];
                uint32_t pt = g->v[p].t;
                if (g->ty[pt].k == K_OPEN) {
                    int v = pick_argument(g, pt, g->v[p].kind == V_VAR);
                    if (v < 0)
                        ok = false;
                    else
                        a[k] = var_ref(g, (uint32_t)v);
                } else if (is_record(g, pt) || is_ptr(g, pt)) {
                    a[k] = typed_arg(g, pt, g->v[p].kind == V_VAR);
                    ok = a[k] != 0;
                } else if (g->v[p].kind == V_VAR) {
                    /* a var parameter takes a variable of its very type */
                    int v = pick_var(g, g->v[p].t, true, true);
                    if (v < 0)
                        ok = false;
                    else
                        a[k] = var_ref(g, (uint32_t)v);
                } else {
                    a[k] = value_for(g, g->v[p].t, depth(g));
                }
            }
            if (ok) {
                uint32_t s = new_s(g, S_CALL);
                g->st[s].fn = fn;
                g->st[s].args = keep_list(g, a, g->r[fn].np);
                g->st[s].nargs = g->r[fn].np;
                out[0] = s;
                return 1;
            }
        }
    }
    if (pure) {
        /* a function has nothing to print: one more variable */
        out[0] = declare(g, var_type(g));
        return 1;
    }
    uint32_t items[5];
    uint32_t n = 1 + below(g, 3), k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i)
            items[k++] = new_e(g, E_STR, 0);
        items[k++] = expr(g, any_type(g), depth(g), true);
    }
    uint32_t s = new_s(g, S_WRITE);
    g->st[s].args = keep_list(g, items, k);
    g->st[s].nargs = k;
    out[0] = s;
    return 1;
}

/* a jump that ends a block: return, or exit or continue in a loop */
static uint32_t jump(G *g)
{
    if (g->cur >= 0 && (!g->loops || chance(g, 50))) {
        uint32_t s = new_s(g, S_RETURN);
        if (g->r[g->cur].func)
            g->st[s].e = value_for(g, g->r[g->cur].rt, depth(g));
        return s;
    }
    return new_s(g, chance(g, 50) ? S_EXIT : S_CONT);
}

static uint32_t block(G *g, int n, uint32_t *count)
{
    uint32_t *x = NULL, nx = 0, cap = 0;
    g->nesting++;
    for (int i = 0; i < n && g->budget > 0; i++) {
        uint32_t some[8];
        uint32_t k = stmt(g, some);
        for (uint32_t j = 0; j < k; j++) {
            LIMBA_GROW(x, nx, cap);
            x[nx++] = some[j];
        }
    }
    if (g->nesting > 1 && (g->cur >= 0 || g->loops) && chance(g, 12)) {
        LIMBA_GROW(x, nx, cap);
        x[nx++] = jump(g);
    }
    g->nesting--;
    uint32_t at = keep_list(g, x, nx);
    free(x);
    *count = nx;
    return at;
}

/* append statement s to the block (b, n) */
static void append(G *g, uint32_t *b, uint32_t *n, uint32_t s, bool front)
{
    uint32_t *x = limba_xmalloc((*n + 1) * sizeof(uint32_t));
    memcpy(x + front, g->ls + *b, *n * sizeof(uint32_t));
    x[front ? 0 : *n] = s;
    *b = keep_list(g, x, *n + 1);
    (*n)++;
    free(x);
}

static void routine(G *g)
{
    LIMBA_GROW(g->r, g->nr, g->capr);
    uint32_t id = g->nr++;
    memset(&g->r[id], 0, sizeof(xr));
    g->r[id].func = chance(g, 50);
    g->r[id].rt = var_type(g);
    g->r[id].np = (uint8_t)below(g, 4);
    uint32_t mark = g->nscope;
    for (uint32_t k = 0; k < g->r[id].np; k++) {
        uint32_t t = var_type(g);
        /* sometimes an open array, made from an array of the program */
        uint32_t n = 0, from = 0;
        for (uint32_t u = NTYPES; u < g->nty; u++)
            if (g->ty[u].k == K_ARRAY && below(g, ++n) == 0)
                from = u;
        if (n && chance(g, 30))
            t = new_type(g, (xt){K_OPEN, g->ty[from].base, index_base(g, from),
                                 g->ty[from].elem, 0, 0});
        /* or a record (var in a procedure, in in a function: a function
           writes nothing) or a pointer (in) */
        uint32_t nr = 0, rec = 0;
        for (uint32_t u = NTYPES; u < g->nty; u++)
            if ((is_record(g, u) || is_ptr(g, u)) && below(g, ++nr) == 0)
                rec = u;
        if (nr && chance(g, 25))
            t = rec;
        bool byref = !g->r[id].func && chance(g, 40);
        if (is_record(g, t))
            byref = !g->r[id].func;
        if (is_ptr(g, t))
            byref = false;
        uint32_t p = new_v(g, t, byref ? V_VAR : V_IN);
        g->r[id].par[k] = p;
        show(g, p);
    }
    g->cur = (int)id;
    uint32_t n;
    uint32_t b = block(g, 2 + (int)below(g, 5), &n);
    /* an open parameter, so that its elements reach the output */
    int open = -1;
    for (uint32_t k = 0; k < g->r[id].np; k++)
        if (g->ty[g->v[g->r[id].par[k]].t].k == K_OPEN)
            open = (int)g->r[id].par[k];
    if (open >= 0 && !g->r[id].func && chance(g, 70)) {
        /* for var i := low(a) to high(a) do writeln(a[i]); end; */
        uint32_t at = g->v[open].t;
        uint32_t s = new_s(g, S_FOR);
        uint32_t i = new_v(g, index_base(g, at), V_LOOP);
        g->st[s].var = i;
        g->st[s].e = bound(g, E_LOW, (uint32_t)open);
        g->st[s].e2 = bound(g, E_HIGH, (uint32_t)open);
        uint32_t ix = var_ref(g, i); /* before: it may move the nodes */
        uint32_t el = new_e(g, E_INDEX, g->ty[at].elem);
        g->e[el].var = (uint32_t)open;
        g->e[el].a = ix;
        uint32_t w = new_s(g, S_WRITE);
        g->st[w].args = keep_list(g, &el, 1);
        g->st[w].nargs = 1;
        g->st[s].blk = keep_list(g, &w, 1);
        g->st[s].nblk = 1;
        append(g, &b, &n, s, chance(g, 50));
    }
    if (g->r[id].func) {
        uint32_t s = new_s(g, S_RETURN);
        uint32_t rt = g->r[id].rt;
        if (open >= 0 && base(g, g->ty[g->v[open].t].elem) == base(g, rt) &&
            chance(g, 60)) {
            /* an element of the open parameter */
            uint32_t el = new_e(g, E_INDEX, g->ty[g->v[open].t].elem);
            uint32_t ix =
                bound(g, chance(g, 50) ? E_LOW : E_HIGH, (uint32_t)open);
            g->e[el].var = (uint32_t)open;
            g->e[el].a = ix;
            g->st[s].e = el;
        } else {
            g->st[s].e = value_for(g, rt, depth(g));
        }
        append(g, &b, &n, s, false);
    }
    g->r[id].blk = b;
    g->r[id].nblk = n;
    g->cur = -1;
    g->nscope = mark;
}

/* ---- the source ---- */

typedef struct {
    char *b;
    size_t n, cap;
    uint32_t line, col;
} text;

static void put(text *o, const char *s)
{
    for (; *s; s++) {
        LIMBA_GROW(o->b, o->n, o->cap);
        o->b[o->n++] = *s;
        if (*s == '\n') {
            o->line++;
            o->col = 1;
        } else {
            o->col++;
        }
    }
}

static void putf(text *o, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put(o, buf);
}

static void put_u128(text *o, u128 v)
{
    char buf[48];
    int i = (int)sizeof(buf) - 1;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    put(o, buf + i);
}

static void put_name(text *o, const G *g, uint32_t v)
{
    putf(o, "%c%u", vprefix[g->v[v].kind], v);
}

static void put_rname(text *o, const G *g, uint32_t r)
{
    putf(o, "%c%u", g->r[r].func ? 'f' : 'p', r);
}

static void put_type(text *o, const G *g, uint32_t t)
{
    if (t < NTYPES)
        put(o, tname[t]);
    else
        putf(o, "%c%u", "RRAOTP"[g->ty[t].k], t);
}

static void put_value(text *o, unsigned t, v128 v)
{
    if (t == T_BOOL) {
        put(o, v ? "true" : "false");
    } else if (v < 0) {
        put(o, "(-");
        put_u128(o, (u128)(-(v + 1)) + 1);
        put(o, ")");
    } else if (fam(t) == 'B' && v > 9) {
        putf(o, "0x%llx", (unsigned long long)v);
    } else {
        put_u128(o, (u128)v);
    }
}

static void here(xe *x, const text *o)
{
    x->line = o->line;
    x->col = o->col;
}

static void pexpr(G *g, text *o, uint32_t i)
{
    xe *x = &g->e[i];
    switch (x->k) {
    case E_LIT:
        put_value(o, base(g, x->t), x->lit);
        break;
    case E_VAR:
        put_name(o, g, x->var);
        break;
    case E_STR:
        put(o, "\" \"");
        break;
    case E_BIN:
        put(o, "(");
        pexpr(g, o, x->a);
        put(o, " ");
        here(&g->e[i], o);
        put(o, otext[g->e[i].op]);
        put(o, " ");
        pexpr(g, o, g->e[i].b);
        put(o, ")");
        break;
    case E_UN:
        put(o, "(");
        here(x, o);
        put(o, x->op == O_NEG ? "-(" : x->op == O_ABS ? "abs (" : "not (");
        pexpr(g, o, g->e[i].a);
        put(o, "))");
        break;
    case E_CONV:
        here(x, o);
        put_type(o, g, x->t);
        put(o, "(");
        pexpr(g, o, g->e[i].a);
        put(o, ")");
        break;
    case E_CALL:
        here(x, o);
        put_rname(o, g, x->fn);
        put(o, "(");
        for (uint32_t k = 0; k < g->e[i].nargs; k++) {
            if (k)
                put(o, ", ");
            pexpr(g, o, g->ls[g->e[i].args + k]);
        }
        put(o, ")");
        break;
    case E_INDEX:
        put_name(o, g, x->var);
        here(x, o);
        put(o, "[");
        pexpr(g, o, g->e[i].a);
        put(o, "]");
        break;
    case E_FIELD:
        put_name(o, g, x->var);
        here(x, o); /* a nil pointer is reported at the . */
        putf(o, ".f%u", x->b);
        break;
    case E_NIL:
        put(o, "nil");
        break;
    case E_LOW:
    case E_HIGH:
    case E_LEN:
        put(o, x->k == E_LOW ? "low(" : x->k == E_HIGH ? "high(" : "length(");
        put_name(o, g, x->var);
        put(o, ")");
        break;
    case E_IN:
        put(o, "(");
        pexpr(g, o, x->a);
        put(o, " in ");
        if (g->e[i].fn) {
            put_type(o, g, g->e[i].fn);
        } else {
            pexpr(g, o, g->e[i].b);
            put(o, "..");
            pexpr(g, o, g->e[i].c);
        }
        put(o, ")");
        break;
    }
}

static void pblock(G *g, text *o, uint32_t b, uint32_t n, int ind);

/* k = e; or k: T = e; */
static void pconst(G *g, text *o, uint32_t si)
{
    uint32_t v = g->st[si].var;
    put_name(o, g, v);
    if (g->v[v].kind == V_TCONST) {
        put(o, ": ");
        put_type(o, g, g->v[v].t);
    }
    put(o, " = ");
    pexpr(g, o, g->st[si].e);
    put(o, ";\n");
}

static void indent(text *o, int ind)
{
    for (int i = 0; i < ind; i++)
        put(o, "  ");
}

static void pargs(G *g, text *o, uint32_t args, uint32_t n)
{
    put(o, "(");
    for (uint32_t k = 0; k < n; k++) {
        if (k)
            put(o, ", ");
        pexpr(g, o, g->ls[args + k]);
    }
    put(o, ")");
}

static void pstmt(G *g, text *o, uint32_t si, int ind)
{
    xs s = g->st[si];
    indent(o, ind);
    g->st[si].line = o->line;
    g->st[si].col = o->col;
    switch (s.k) {
    case S_VAR:
        put(o, "var ");
        g->st[si].nline = o->line;
        g->st[si].ncol = o->col;
        put_name(o, g, s.var);
        put(o, ": ");
        put_type(o, g, g->v[s.var].t);
        put(o, " := ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_ASSIGN:
        put_name(o, g, s.var);
        if (s.fld) {
            g->st[si].iline = o->line; /* nil is reported at the . */
            g->st[si].icol = o->col;
            putf(o, ".f%u", s.fld - 1);
        }
        if (s.idx) {
            g->st[si].iline = o->line; /* the index is checked at the [ */
            g->st[si].icol = o->col;
            put(o, "[");
            pexpr(g, o, s.idx);
            put(o, "]");
        }
        put(o, " := ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_IF:
        for (uint32_t k = 0; k < s.narm; k++) {
            if (k)
                indent(o, ind);
            put(o, k ? "elsif " : "if ");
            pexpr(g, o, g->arm[s.arm + k].cond);
            put(o, " then\n");
            pblock(g, o, g->arm[s.arm + k].blk, g->arm[s.arm + k].nblk,
                   ind + 1);
        }
        if (s.has_alt) {
            indent(o, ind);
            put(o, "else\n");
            pblock(g, o, s.alt, s.nalt, ind + 1);
        }
        indent(o, ind);
        put(o, "end;\n");
        break;
    case S_WHILE:
        put(o, "while ");
        pexpr(g, o, s.e);
        put(o, " do\n");
        pblock(g, o, s.blk, s.nblk, ind + 1);
        indent(o, ind);
        put(o, "end;\n");
        break;
    case S_REPEAT:
        put(o, "repeat\n");
        pblock(g, o, s.blk, s.nblk, ind + 1);
        indent(o, ind);
        put(o, "until ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_FOR:
        put(o, "for var ");
        put_name(o, g, s.var);
        put(o, ": ");
        put_type(o, g, g->v[s.var].t);
        put(o, " := ");
        pexpr(g, o, s.e);
        put(o, s.down ? " downto " : " to ");
        pexpr(g, o, s.e2);
        put(o, " do\n");
        pblock(g, o, s.blk, s.nblk, ind + 1);
        indent(o, ind);
        put(o, "end;\n");
        break;
    case S_CASE: {
        unsigned t = base(g, g->e[s.e].t);
        put(o, "case ");
        pexpr(g, o, s.e);
        put(o, " of\n");
        for (uint32_t k = 0; k < s.narm; k++) {
            const xarm *a = &g->arm[s.arm + k];
            indent(o, ind + 1);
            put(o, "when ");
            for (uint32_t j = 0; j < a->nlab; j++) {
                const xlab *l = &g->lab[a->lab + j];
                if (j)
                    put(o, ", ");
                put_value(o, t, l->lo);
                if (l->hi != l->lo) {
                    put(o, "..");
                    put_value(o, t, l->hi);
                }
            }
            put(o, ":\n");
            pblock(g, o, a->blk, a->nblk, ind + 2);
        }
        indent(o, ind + 1);
        put(o, "else\n");
        pblock(g, o, s.alt, s.nalt, ind + 2);
        indent(o, ind);
        put(o, "end;\n");
        break;
    }
    case S_WRITE:
        put(o, "writeln");
        pargs(g, o, s.args, s.nargs);
        put(o, ";\n");
        break;
    case S_CALL:
        put_rname(o, g, s.fn);
        pargs(g, o, s.args, s.nargs);
        put(o, ";\n");
        break;
    case S_EXIT:
    case S_CONT:
        put(o, s.k == S_EXIT ? "exit" : "continue");
        if (s.e) {
            put(o, " when ");
            pexpr(g, o, s.e);
        }
        put(o, ";\n");
        break;
    case S_RETURN:
        put(o, "return");
        if (s.e) {
            put(o, " ");
            pexpr(g, o, s.e);
        }
        put(o, ";\n");
        break;
    case S_CONST:
        put(o, "const ");
        pconst(g, o, si);
        break;
    case S_NEW:
        put_name(o, g, s.var);
        put(o, " := new(");
        put_type(o, g, g->ty[g->v[s.var].t].elem);
        put(o, ");\n");
        break;
    case S_COPY:
        put_name(o, g, s.var);
        put(o, " := ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    }
}

static void pblock(G *g, text *o, uint32_t b, uint32_t n, int ind)
{
    for (uint32_t i = 0; i < n; i++)
        pstmt(g, o, g->ls[b + i], ind);
}

static void program(G *g, text *o, uint32_t nglob)
{
    put(o, "program Random;\n");
    if (g->ngc) {
        put(o, "\nconst\n");
        for (uint32_t k = 0; k < g->ngc; k++) {
            put(o, "  ");
            pconst(g, o, g->gc[k]);
        }
    }
    if (g->nty > NTYPES) {
        put(o, "\ntype\n");
        for (uint32_t t = NTYPES; t < g->nty; t++) {
            const xt *x = &g->ty[t];
            put(o, "  ");
            put_type(o, g, t);
            put(o, " = ");
            if (x->k == K_RANGE) {
                put(o, tname[x->base]);
                put(o, " range ");
                put_value(o, x->base, x->lo);
                put(o, "..");
                put_value(o, x->base, x->hi);
            } else if (x->k == K_RECORD) {
                put(o, "record");
                for (uint32_t f = 0; f < x->elem; f++) {
                    putf(o, " f%u: ", f);
                    put_type(o, g, g->fld[x->index + f]);
                    put(o, ";");
                }
                put(o, " end");
            } else if (x->k == K_PTR) {
                put(o, "^");
                put_type(o, g, x->elem);
            } else {
                put(o, "array[");
                put_type(o, g, x->index);
                put(o, x->k == K_OPEN ? " range <>] of " : "] of ");
                put_type(o, g, x->elem);
            }
            put(o, ";\n");
        }
    }
    if (nglob) {
        put(o, "\nvar\n");
        for (uint32_t v = 0; v < nglob; v++) {
            put(o, "  ");
            put_name(o, g, v);
            put(o, ": ");
            put_type(o, g, g->v[v].t);
            if (is_scalar(g, g->v[v].t) || is_ptr(g, g->v[v].t)) {
                put(o, " := ");
                pexpr(g, o, g->st[v].e);
            }
            put(o, ";\n");
        }
    }
    for (uint32_t r = 0; r < g->nr; r++) {
        const xr *x = &g->r[r];
        put(o, x->func ? "\nfunction " : "\nprocedure ");
        put_rname(o, g, r);
        put(o, "(");
        for (uint32_t k = 0; k < x->np; k++) {
            uint32_t p = x->par[k];
            if (k)
                put(o, "; ");
            if (g->v[p].kind == V_VAR)
                put(o, "var ");
            put_name(o, g, p);
            put(o, ": ");
            put_type(o, g, g->v[p].t);
        }
        put(o, ")");
        if (x->func) {
            put(o, ": ");
            put_type(o, g, x->rt);
        }
        put(o, ";\nbegin\n");
        pblock(g, o, x->blk, x->nblk, 1);
        put(o, "end ");
        put_rname(o, g, r);
        put(o, ";\n");
    }
    put(o, "\nbegin\n");
    pblock(g, o, g->main_blk, g->main_n, 1);
    put(o, "end Random.\n");
    LIMBA_GROW(o->b, o->n, o->cap);
    o->b[o->n] = 0;
}

/* ---- the run ---- */

enum { X_NEXT, X_EXIT, X_CONT, X_RET };

#define STEP_LIMIT 200000

typedef struct {
    G *g;
    v128 *cell;
    uint32_t *ref;   /* the first cell of a variable, a var parameter's too */
    v128 *alo, *ahi; /* the bounds of an array variable, or of an open
                        parameter's argument */
    v128 *heap;      /* the records made by new: a pointer is the index of
                        the first field plus 1, nil is 0 */
    uint32_t nheap, capheap;
    text out;
    bool trap, toolong;
    int code;
    uint32_t line, col;
    uint64_t steps;
    v128 ret;
    uint32_t rt; /* the type of the result of the running function */
} X;

static v128 wrap(unsigned t, v128 v)
{
    u128 mask = ((u128)1 << tbits(t)) - 1;
    return (v128)((u128)v & mask);
}

static bool fits(unsigned t, v128 v)
{
    return v >= tmin(t) && v <= tmax(t);
}

static void stop(X *x, int code, uint32_t line, uint32_t col)
{
    if (!x->trap) {
        x->trap = true;
        x->code = code;
        x->line = line;
        x->col = col;
    }
}

static v128 fail(X *x, uint32_t at, int code)
{
    stop(x, code, x->g->e[at].line, x->g->e[at].col);
    return 0;
}

/* v stored where type t is: a range is checked, reported at line:col */
static bool store_ok(X *x, uint32_t t, v128 v, uint32_t line, uint32_t col)
{
    const G *g = x->g;
    if (g->ty[t].k == K_RANGE && (v < lo_of(g, t) || v > hi_of(g, t))) {
        stop(x, 101, line, col);
        return false;
    }
    return true;
}

static int run_block(X *x, uint32_t b, uint32_t n);

static v128 ev(X *x, uint32_t i);

/* the arguments from left to right, each checked at the call at */
static v128 call(X *x, uint32_t fn, uint32_t args, uint32_t nargs,
                 uint32_t line, uint32_t col)
{
    G *g = x->g;
    v128 val[4];
    uint32_t target[4];
    for (uint32_t k = 0; k < nargs; k++) {
        uint32_t a = g->ls[args + k], p = g->r[fn].par[k];
        if (g->ty[g->v[p].t].k == K_OPEN || is_record(g, g->v[p].t)) {
            target[k] = g->e[a].var; /* by reference: bound below */
        } else if (g->v[p].kind == V_VAR) {
            target[k] = x->ref[g->e[a].var];
        } else {
            val[k] = ev(x, a);
            if (!x->trap)
                store_ok(x, g->v[p].t, val[k], line, col);
        }
        if (x->trap)
            return 0;
    }
    for (uint32_t k = 0; k < nargs; k++) {
        uint32_t p = g->r[fn].par[k];
        if (g->ty[g->v[p].t].k == K_OPEN) {
            uint32_t a = target[k];
            x->ref[p] = x->ref[a];
            x->alo[p] = x->alo[a];
            x->ahi[p] = x->ahi[a];
        } else if (is_record(g, g->v[p].t)) {
            /* in a function a record is only read: by copy or by
               reference cannot be told apart (§ 8) */
            x->ref[p] = x->ref[target[k]];
        } else if (g->v[p].kind == V_VAR) {
            x->ref[p] = target[k];
        } else {
            x->cell[x->ref[p]] = val[k];
        }
    }
    uint32_t outer = x->rt;
    x->ret = 0;
    x->rt = g->r[fn].rt;
    run_block(x, g->r[fn].blk, g->r[fn].nblk);
    x->rt = outer;
    return x->ret;
}

/* the cell of element index of array variable v; false if outside */
static bool element_cell(X *x, uint32_t v, v128 index, uint32_t *cell)
{
    if (index < x->alo[v] || index > x->ahi[v])
        return false;
    *cell = x->ref[v] + (uint32_t)(index - x->alo[v]);
    return true;
}

static v128 binary(X *x, uint32_t i)
{
    const xe *e = &x->g->e[i];
    unsigned op = e->op, t = base(x->g, e->t);
    if (t == T_BOOL && (op == O_AND || op == O_OR)) {
        v128 l = ev(x, e->a);
        if (x->trap)
            return 0;
        if ((op == O_AND) != (l != 0))
            return l;
        return ev(x, e->b);
    }
    v128 l = ev(x, e->a);
    v128 r = ev(x, e->b);
    if (x->trap)
        return 0;
    bool mod = fam(t) == 'B';
    v128 v;
    switch (op) {
    case O_EQ:
        return l == r;
    case O_NE:
        return l != r;
    case O_LT:
        return l < r;
    case O_LE:
        return l <= r;
    case O_GT:
        return l > r;
    case O_GE:
        return l >= r;
    case O_ADD:
        v = l + r;
        break;
    case O_SUB:
        v = l - r;
        break;
    case O_MUL:
        if (fam(t) == 'S')
            v = l * r;
        else if (mod)
            return wrap(t, (v128)((u128)l * (u128)r & (((u128)1 << 64) - 1)));
        else if ((u128)l * (u128)r > (u128)tmax(t))
            return fail(x, i, 6);
        else
            v = (v128)((u128)l * (u128)r);
        break;
    case O_DIV:
    case O_MOD:
    case O_REM:
        if (r == 0)
            return fail(x, i, 11);
        if (op == O_DIV) {
            v = l / r;
            break;
        }
        v = l % r;
        if (op == O_MOD && v != 0 && (v < 0) != (r < 0))
            v += r;
        return v;
    case O_AND:
        return l & r;
    case O_OR:
        return l | r;
    case O_XOR:
        return l ^ r;
    case O_SHL:
    case O_SHR:
        if (r < 0 || r >= (v128)tbits(t))
            return fail(x, i, 104);
        if (op == O_SHR)
            return l >> (int)r;
        return wrap(t, (v128)(((u128)l << (int)r) & (((u128)1 << 64) - 1)));
    default:
        return 0;
    }
    if (mod)
        return wrap(t, v);
    if (!fits(t, v))
        return fail(x, i, 6);
    return v;
}

static v128 ev(X *x, uint32_t i)
{
    const G *g = x->g;
    const xe *e = &g->e[i];
    if (x->trap)
        return 0;
    if (++x->steps > STEP_LIMIT) {
        x->toolong = x->trap = true;
        return 0;
    }
    if (e->cst)
        return e->lit; /* computed by the front end, exactly */
    unsigned t = base(g, e->t);
    switch (e->k) {
    case E_LIT:
        return e->lit;
    case E_VAR:
        return x->cell[x->ref[e->var]];
    case E_BIN:
        return binary(x, i);
    case E_UN: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        if (e->op == O_NOT)
            return t == T_BOOL ? !v : wrap(t, ~v);
        if (e->op == O_NEG && fam(t) == 'B')
            return wrap(t, -v);
        v = e->op == O_NEG ? -v : v < 0 ? -v : v;
        return fits(t, v) ? v : fail(x, i, 6);
    }
    case E_CONV: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        if (fam(t) == 'B')
            return wrap(t, v);
        return v >= lo_of(g, e->t) && v <= hi_of(g, e->t) ? v : fail(x, i, 103);
    }
    case E_CALL:
        return call(x, e->fn, e->args, e->nargs, e->line, e->col);
    case E_INDEX: {
        v128 k = ev(x, e->a);
        uint32_t c;
        if (x->trap)
            return 0;
        if (!element_cell(x, e->var, k, &c))
            return fail(x, i, 100);
        return x->cell[c];
    }
    case E_FIELD: {
        uint32_t vt = g->v[e->var].t;
        if (is_record(g, vt))
            return x->cell[x->ref[e->var] + e->b];
        v128 pv = x->cell[x->ref[e->var]];
        if (pv == 0)
            return fail(x, i, 102);
        return x->heap[pv - 1 + e->b];
    }
    case E_NIL:
        return 0;
    case E_LOW:
        return x->alo[e->var];
    case E_HIGH:
        return x->ahi[e->var];
    case E_LEN:
        return x->ahi[e->var] < x->alo[e->var]
                   ? 0
                   : x->ahi[e->var] - x->alo[e->var] + 1;
    case E_IN: {
        v128 v = ev(x, e->a), lo, hi;
        if (e->fn) {
            lo = lo_of(g, e->fn);
            hi = hi_of(g, e->fn);
        } else {
            lo = ev(x, e->b);
            hi = ev(x, e->c);
        }
        return lo <= v && v <= hi;
    }
    }
    return 0;
}

static void print_value(text *o, unsigned t, v128 v)
{
    if (t == T_BOOL) {
        put(o, v ? "true" : "false");
    } else if (v < 0) {
        put(o, "-");
        put_u128(o, (u128)(-(v + 1)) + 1);
    } else {
        put_u128(o, (u128)v);
    }
}

static int run_stmt(X *x, uint32_t si)
{
    G *g = x->g;
    const xs *s = &g->st[si];
    if (++x->steps > STEP_LIMIT) {
        x->toolong = x->trap = true;
        return X_RET;
    }
    switch (s->k) {
    case S_VAR: {
        v128 v = ev(x, s->e);
        if (x->trap || !store_ok(x, g->v[s->var].t, v, s->nline, s->ncol))
            return X_RET;
        x->cell[x->ref[s->var]] = v;
        return X_NEXT;
    }
    case S_ASSIGN: {
        uint32_t c = x->ref[s->var];
        uint32_t t = g->v[s->var].t;
        if (s->fld) {
            /* the target first: through a pointer, checked */
            uint32_t f = s->fld - 1;
            uint32_t ft = field_type(g, record_of(g, t), f);
            v128 pv = 0;
            if (is_ptr(g, t)) {
                pv = x->cell[c];
                if (pv == 0) {
                    stop(x, 102, s->iline, s->icol);
                    return X_RET;
                }
            }
            v128 v = ev(x, s->e);
            if (x->trap || !store_ok(x, ft, v, s->line, s->col))
                return X_RET;
            if (pv)
                x->heap[pv - 1 + f] = v; /* ev made no record: no move */
            else
                x->cell[c + f] = v;
            return X_NEXT;
        }
        if (s->idx) {
            /* the target first: its index, checked */
            v128 k = ev(x, s->idx);
            if (x->trap)
                return X_RET;
            if (!element_cell(x, s->var, k, &c)) {
                stop(x, 100, s->iline, s->icol);
                return X_RET;
            }
            t = g->ty[t].elem;
        }
        v128 v = ev(x, s->e);
        if (x->trap || !store_ok(x, t, v, s->line, s->col))
            return X_RET;
        x->cell[c] = v;
        return X_NEXT;
    }
    case S_IF:
        for (uint32_t k = 0; k < s->narm; k++) {
            const xarm *a = &g->arm[s->arm + k];
            v128 c = ev(x, a->cond);
            if (x->trap)
                return X_RET;
            if (c)
                return run_block(x, a->blk, a->nblk);
        }
        return s->has_alt ? run_block(x, s->alt, s->nalt) : X_NEXT;
    case S_WHILE:
        for (;;) {
            v128 c = ev(x, s->e);
            if (x->trap)
                return X_RET;
            if (!c)
                return X_NEXT;
            int r = run_block(x, s->blk, s->nblk);
            if (r == X_RET)
                return X_RET;
            if (r == X_EXIT)
                return X_NEXT;
        }
    case S_REPEAT:
        for (;;) {
            int r = run_block(x, s->blk, s->nblk);
            if (r == X_RET)
                return X_RET;
            if (r == X_EXIT)
                return X_NEXT;
            v128 c = ev(x, s->e);
            if (x->trap)
                return X_RET;
            if (c)
                return X_NEXT;
        }
    case S_FOR: {
        uint32_t t = g->v[s->var].t;
        v128 lo = ev(x, s->e);
        if (x->trap || !store_ok(x, t, lo, s->line, s->col))
            return X_RET;
        v128 hi = ev(x, s->e2);
        if (x->trap || !store_ok(x, t, hi, s->line, s->col))
            return X_RET;
        if (s->down ? lo < hi : lo > hi)
            return X_NEXT;
        for (v128 v = lo;; v += s->down ? -1 : 1) {
            x->cell[x->ref[s->var]] = v;
            int r = run_block(x, s->blk, s->nblk);
            if (r == X_RET)
                return X_RET;
            if (r == X_EXIT || v == hi)
                return X_NEXT;
        }
    }
    case S_CASE: {
        v128 v = ev(x, s->e);
        if (x->trap)
            return X_RET;
        for (uint32_t k = 0; k < s->narm; k++) {
            const xarm *a = &g->arm[s->arm + k];
            for (uint32_t j = 0; j < a->nlab; j++)
                if (v >= g->lab[a->lab + j].lo && v <= g->lab[a->lab + j].hi)
                    return run_block(x, a->blk, a->nblk);
        }
        return run_block(x, s->alt, s->nalt);
    }
    case S_WRITE:
        for (uint32_t k = 0; k < s->nargs; k++) {
            uint32_t a = g->ls[s->args + k];
            if (g->e[a].k == E_STR) {
                put(&x->out, " ");
                continue;
            }
            v128 v = ev(x, a);
            if (x->trap)
                return X_RET;
            print_value(&x->out, base(g, g->e[a].t), v);
        }
        put(&x->out, "\n");
        return X_NEXT;
    case S_CALL:
        call(x, s->fn, s->args, s->nargs, s->line, s->col);
        return x->trap ? X_RET : X_NEXT;
    case S_EXIT:
    case S_CONT:
        if (s->e) {
            v128 c = ev(x, s->e);
            if (x->trap)
                return X_RET;
            if (!c)
                return X_NEXT;
        }
        return s->k == S_EXIT ? X_EXIT : X_CONT;
    case S_RETURN:
        if (s->e) {
            x->ret = ev(x, s->e);
            if (!x->trap)
                store_ok(x, x->rt, x->ret, s->line, s->col);
        }
        return X_RET;
    case S_CONST:
        return X_NEXT; /* computed by the front end */
    case S_NEW: {
        uint32_t n = g->ty[g->ty[g->v[s->var].t].elem].elem;
        uint32_t at = x->nheap;
        for (uint32_t k = 0; k < n; k++) {
            LIMBA_GROW(x->heap, x->nheap, x->capheap);
            x->heap[x->nheap++] = 0;
        }
        x->cell[x->ref[s->var]] = (v128)at + 1;
        return X_NEXT;
    }
    case S_COPY: {
        uint32_t n = g->ty[g->v[s->var].t].elem;
        memmove(&x->cell[x->ref[s->var]], &x->cell[x->ref[g->e[s->e].var]],
                n * sizeof(v128));
        return X_NEXT;
    }
    }
    return X_NEXT;
}

static int run_block(X *x, uint32_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int r = run_stmt(x, x->g->ls[b + i]);
        if (r != X_NEXT)
            return r;
    }
    return X_NEXT;
}

/* ---- one attempt ---- */

static void g_free(G *g)
{
    free(g->ty);
    free(g->e);
    free(g->st);
    free(g->arm);
    free(g->lab);
    free(g->ls);
    free(g->v);
    free(g->r);
    free(g->scope);
    free(g->gc);
    free(g->fld);
}

static bool attempt(uint64_t seed, limba_lxgen *p)
{
    G g;
    memset(&g, 0, sizeof(g));
    g.s = seed;
    g.cur = -1;
    new_e(&g, E_LIT, 0); /* node 0 stands for none */
    for (unsigned t = 0; t < NTYPES; t++)
        new_type(&g, (xt){K_BASE, (uint8_t)t, 0, 0, t == T_BOOL ? 0 : tmin(t),
                          tmax(t)});

    /* the ranges, then the arrays, each indexed by a short range of its
       own */
    for (uint32_t k = 0, n = below(&g, 4); k < n; k++)
        new_range(&g, false);
    uint32_t narrays = 0, arrays[12];
    for (uint32_t k = 0, n = below(&g, 3); k < n; k++) {
        uint32_t ix = new_range(&g, true);
        uint32_t el = var_type(&g);
        arrays[narrays++] =
            new_type(&g, (xt){K_ARRAY, (uint8_t)base(&g, el), ix, el,
                              lo_of(&g, ix), hi_of(&g, ix)});
    }
    /* records of 1 to 4 scalar fields, some with a pointer type; two
       global variables of each type, so that copies show */
    for (uint32_t k = 0, n = below(&g, 3); k < n; k++) {
        uint32_t first = g.nfld, nf = 1 + below(&g, 4);
        for (uint32_t f = 0; f < nf; f++) {
            uint32_t ft = var_type(&g);
            LIMBA_GROW(g.fld, g.nfld, g.capfld);
            g.fld[g.nfld++] = ft;
        }
        uint32_t rt = new_type(&g, (xt){K_RECORD, T_BOOL, first, nf, 0, 0});
        arrays[narrays++] = rt;
        arrays[narrays++] = rt;
        if (chance(&g, 60)) {
            uint32_t pt = new_type(&g, (xt){K_PTR, T_BOOL, 0, rt, 0, 0});
            arrays[narrays++] = pt;
            arrays[narrays++] = pt;
        }
    }

    /* the globals: one of an integer type and one Boolean at least, then
       the arrays; their initialisers are the statements of the same
       numbers (none for an array) */
    uint32_t nglob = 2 + below(&g, 5);
    for (uint32_t k = 0; k < nglob + narrays; k++) {
        uint32_t t = k == 0       ? int_type(&g)
                     : k == 1     ? T_BOOL
                     : k >= nglob ? arrays[k - nglob]
                                  : var_type(&g);
        uint32_t v = new_v(&g, t, V_GLOBAL);
        uint32_t s = new_s(&g, S_VAR);
        g.st[s].var = v;
        if (!is_array(&g, t))
            g.st[s].e = is_ptr(&g, t) ? nil_of(&g, t)
                        : is_record(&g, t)
                            ? 0
                            : lit(&g, t,
                                  t == T_BOOL ? below(&g, 2)
                                              : rand_in(&g, lo_of(&g, t),
                                                        hi_of(&g, t)));
        show(&g, v);
    }
    nglob += narrays;
    /* the global constants, after the variables: those take the first
       numbers */
    for (uint32_t k = 0, n = below(&g, 5); k < n; k++) {
        uint32_t c = constant(&g);
        LIMBA_GROW(g.gc, g.ngc, g.capgc);
        g.gc[g.ngc++] = c;
    }
    g.budget = 30 + (int)below(&g, 60);
    for (uint32_t k = 0, nr = below(&g, 5); k < nr && g.budget > 10; k++)
        routine(&g);
    g.main_blk = block(&g, 4 + (int)below(&g, 12), &g.main_n);

    /* the program begins giving a value to every field of the records, and
       to half of the pointers a new record */
    {
        uint32_t *init = NULL, ni = 0, cap = 0;
        for (uint32_t v = 0; v < nglob; v++) {
            uint32_t t = g.v[v].t;
            if (!is_record(&g, t) && !(is_ptr(&g, t) && chance(&g, 50)))
                continue;
            if (is_ptr(&g, t)) {
                uint32_t s = new_s(&g, S_NEW);
                g.st[s].var = v;
                LIMBA_GROW(init, ni, cap);
                init[ni++] = s;
            }
            uint32_t rt = record_of(&g, t);
            for (uint32_t f = 0; f < g.ty[rt].elem; f++) {
                uint32_t ft = field_type(&g, rt, f);
                uint32_t e =
                    lit(&g, ft,
                        base(&g, ft) == T_BOOL
                            ? below(&g, 2)
                            : rand_in(&g, lo_of(&g, ft), hi_of(&g, ft)));
                uint32_t s = new_s(&g, S_ASSIGN);
                g.st[s].var = v;
                g.st[s].fld = f + 1;
                g.st[s].e = e;
                LIMBA_GROW(init, ni, cap);
                init[ni++] = s;
            }
        }
        while (ni)
            append(&g, &g.main_blk, &g.main_n, init[--ni], true);
        free(init);
    }

    /* the program begins filling the arrays and ends printing every
       scalar global */
    for (uint32_t v = 0; v < nglob; v++) {
        uint32_t at = g.v[v].t;
        if (!is_array(&g, at))
            continue;
        for (v128 k = g.ty[at].hi; k >= g.ty[at].lo; k--) {
            uint32_t s = new_s(&g, S_ASSIGN);
            uint32_t el = g.ty[at].elem;
            g.st[s].var = v;
            g.st[s].idx = lit(&g, base(&g, g.ty[at].index), k);
            g.st[s].e = lit(&g, el,
                            base(&g, el) == T_BOOL
                                ? below(&g, 2)
                                : rand_in(&g, lo_of(&g, el), hi_of(&g, el)));
            append(&g, &g.main_blk, &g.main_n, s, true);
        }
    }
    {
        uint32_t items[96], k = 0;
        for (uint32_t v = 0; v < nglob; v++) {
            uint32_t t = g.v[v].t;
            if (is_array(&g, t))
                continue;
            if (is_record(&g, t)) {
                for (uint32_t f = 0; f < g.ty[t].elem; f++) {
                    if (k)
                        items[k++] = new_e(&g, E_STR, 0);
                    items[k++] = field_ref(&g, v, f);
                }
                continue;
            }
            if (k)
                items[k++] = new_e(&g, E_STR, 0);
            items[k++] = is_ptr(&g, t) ? binop(&g, O_EQ, T_BOOL, var_ref(&g, v),
                                               nil_of(&g, t))
                                       : var_ref(&g, v);
        }
        uint32_t s = new_s(&g, S_WRITE);
        g.st[s].args = keep_list(&g, items, k);
        g.st[s].nargs = k;
        append(&g, &g.main_blk, &g.main_n, s, false);
    }

    text src = {NULL, 0, 0, 1, 1};
    program(&g, &src, nglob);

    X x;
    memset(&x, 0, sizeof(x));
    x.g = &g;
    uint32_t ncell = 0;
    x.ref = limba_xmalloc((g.nv ? g.nv : 1) * sizeof(uint32_t));
    for (uint32_t v = 0; v < g.nv; v++) {
        x.ref[v] = ncell;
        uint32_t t = g.v[v].t;
        ncell += is_array(&g, t)    ? (uint32_t)(g.ty[t].hi - g.ty[t].lo) + 1
                 : is_record(&g, t) ? g.ty[t].elem
                                    : 1;
    }
    x.cell = limba_xcalloc(ncell ? ncell : 1, sizeof(v128));
    x.alo = limba_xcalloc(g.nv ? g.nv : 1, sizeof(v128));
    x.ahi = limba_xcalloc(g.nv ? g.nv : 1, sizeof(v128));
    for (uint32_t v = 0; v < g.nv; v++)
        if (g.ty[g.v[v].t].k == K_ARRAY) {
            x.alo[v] = g.ty[g.v[v].t].lo;
            x.ahi[v] = g.ty[g.v[v].t].hi;
        }
    x.out.line = x.out.col = 1;
    for (uint32_t v = 0; v < nglob; v++)
        if (!is_array(&g, g.v[v].t))
            x.cell[x.ref[v]] = g.e[g.st[v].e].lit;
    run_block(&x, g.main_blk, g.main_n);
    bool ok = !x.toolong;
    if (ok) {
        p->src = src.b;
        src.b = NULL;
        LIMBA_GROW(x.out.b, x.out.n, x.out.cap);
        x.out.b[x.out.n] = 0;
        p->out = x.out.b;
        p->outlen = x.out.n;
        x.out.b = NULL;
        if (x.trap)
            snprintf(p->end, sizeof(p->end), "trap %d at %u:%u", x.code, x.line,
                     x.col);
        else
            snprintf(p->end, sizeof(p->end), "ok");
    }
    free(src.b);
    free(x.out.b);
    free(x.cell);
    free(x.ref);
    free(x.alo);
    free(x.ahi);
    free(x.heap);
    g_free(&g);
    return ok;
}

bool limba_lxgen_make(uint64_t seed, limba_lxgen *p)
{
    memset(p, 0, sizeof(*p));
    for (uint64_t k = 0; k < 16; k++)
        if (attempt(seed * 16 + k, p))
            return true;
    return false;
}

void limba_lxgen_free(limba_lxgen *p)
{
    free(p->src);
    free(p->out);
    memset(p, 0, sizeof(*p));
}
