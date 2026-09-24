/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gen.c - random Luxia programs and their expected output (see gen.h).
 *
 * The program is a small tree: expressions, statements, blocks, routines.
 * It is grown from the variables in scope, so it is valid by construction:
 * both operands of an operator have one type, a constant never meets
 * another constant (the front end would compute it, and complain when it
 * does not fit), a literal always fits its type, loops count to a small
 * bound. Functions are pure (no output, no global written, no var
 * parameter), so the order in which an expression calls them does not
 * show; routines call only the routines made before them.
 *
 * The run follows the rules of the specification: checked arithmetic on
 * IntN and UIntN, modular arithmetic on BitsN, div towards zero, mod with
 * the sign of the divisor, rem with the sign of the dividend, and and or
 * on Boolean that stop early, conversions that must hold the value (the
 * BitsN take the low bits), operands, arguments and the items of writeln
 * from left to right, each item printed before the next is computed, and
 * continue in repeat going to the test. The run-time error codes are not
 * in the specification yet: 6 overflow, 11 division by zero, 103
 * conversion, 104 shift.
 */
#include "gen.h"

#include "common/xalloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef __int128 v128;
typedef unsigned __int128 u128;

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
    O_NOT
};

static const char *const otext[] = {
    "+",   "-", "*",  "div", "mod", "rem", "and", "or", "xor", "shl",
    "shr", "=", "<>", "<",   "<=",  ">",   ">=",  "-",  "abs", "not",
};

enum { E_LIT, E_VAR, E_BIN, E_UN, E_CONV, E_CALL, E_STR };

typedef struct {
    uint8_t k, t, op;
    bool cst; /* a literal: the front end knows its value */
    v128 lit;
    uint32_t var, fn, a, b, args, nargs;
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
    S_RETURN
};

typedef struct {
    uint8_t k;
    bool down;
    uint32_t var, e, e2; /* e, e2: 0 for none (node 0 is never used) */
    uint32_t blk, nblk;  /* the body */
    uint32_t alt, nalt;  /* else */
    bool has_alt;
    uint32_t arm, narm; /* if and case */
    uint32_t fn, args, nargs;
} xs;

typedef struct {
    uint32_t cond;      /* if */
    uint32_t lab, nlab; /* case */
    uint32_t blk, nblk;
} xarm;

typedef struct {
    v128 lo, hi;
} xlab;

enum { V_GLOBAL, V_LOCAL, V_IN, V_VAR, V_LOOP, V_COUNT };
static const char vprefix[] = {'g', 'v', 'a', 'a', 'i', 'w'};

typedef struct {
    uint8_t t, kind;
} xv;

typedef struct {
    bool func;
    uint8_t rt;
    uint32_t par[4];
    uint8_t np;
    uint32_t blk, nblk;
} xr;

typedef struct {
    uint64_t s;
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

static v128 clamp(unsigned t, v128 v)
{
    return v < tmin(t) ? tmin(t) : v > tmax(t) ? tmax(t) : v;
}

/* a value of type t, often one at an edge */
static v128 rand_value(G *g, unsigned t)
{
    if (t == T_BOOL)
        return below(g, 2);
    v128 lo = tmin(t), hi = tmax(t);
    switch (below(g, 12)) {
    case 0:
        return lo;
    case 1:
        return hi;
    case 2:
        return clamp(t, lo + 1);
    case 3:
        return clamp(t, hi - 1);
    case 4:
        return fam(t) == 'S' ? -1 : 2;
    case 5: {
        u128 range = (u128)(hi - lo) + 1;
        u128 r = ((u128)rnd(g) << 64 | rnd(g)) % range;
        return lo + (v128)r;
    }
    default:
        return clamp(t, (v128)below(g, 41) - 20);
    }
}

/* ---- the tree ---- */

static uint32_t new_e(G *g, unsigned k, unsigned t)
{
    LIMBA_GROW(g->e, g->ne, g->cape);
    memset(&g->e[g->ne], 0, sizeof(xe));
    g->e[g->ne].k = (uint8_t)k;
    g->e[g->ne].t = (uint8_t)t;
    return g->ne++;
}

static uint32_t new_s(G *g, unsigned k)
{
    LIMBA_GROW(g->st, g->ns, g->caps);
    memset(&g->st[g->ns], 0, sizeof(xs));
    g->st[g->ns].k = (uint8_t)k;
    return g->ns++;
}

static uint32_t new_v(G *g, unsigned t, unsigned kind)
{
    LIMBA_GROW(g->v, g->nv, g->capv);
    g->v[g->nv] = (xv){(uint8_t)t, (uint8_t)kind};
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

static uint32_t lit(G *g, unsigned t, v128 v)
{
    uint32_t i = new_e(g, E_LIT, t);
    g->e[i].lit = v;
    g->e[i].cst = true;
    return i;
}

static uint32_t var_ref(G *g, uint32_t v)
{
    uint32_t i = new_e(g, E_VAR, g->v[v].t);
    g->e[i].var = v;
    return i;
}

static uint32_t binop(G *g, unsigned op, unsigned t, uint32_t a, uint32_t b)
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
    if (k == V_IN || k == V_LOOP || k == V_COUNT)
        return false;
    return !(k == V_GLOBAL && in_function(g));
}

/* a visible variable of type t (NTYPES: of an integer type); -1 if none */
static int pick_var(G *g, unsigned t, bool write)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        unsigned vt = g->v[v].t;
        if (t == NTYPES ? vt == T_BOOL : vt != t)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a function made before the current routine, returning t; -1 if none */
static int pick_func(G *g, unsigned t)
{
    uint32_t top = g->cur >= 0 ? (uint32_t)g->cur : g->nr, n = 0, chosen = 0;
    for (uint32_t i = 0; i < top; i++)
        if (g->r[i].func && g->r[i].rt == t && below(g, ++n) == 0)
            chosen = i;
    return n ? (int)chosen : -1;
}

static uint32_t expr(G *g, unsigned t, int d, bool need_var);

static uint32_t call_expr(G *g, uint32_t fn, int d)
{
    uint32_t a[4];
    const xr *r = &g->r[fn];
    uint32_t np = r->np;
    for (uint32_t k = 0; k < np; k++)
        a[k] = expr(g, g->v[g->r[fn].par[k]].t, d - 1, false);
    uint32_t i = new_e(g, E_CALL, g->r[fn].rt);
    g->e[i].fn = fn;
    g->e[i].args = keep_list(g, a, np);
    g->e[i].nargs = np;
    return i;
}

static uint32_t conv(G *g, unsigned t, uint32_t a)
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

/* a variable, a literal, or a variable of another type made into t */
static uint32_t leaf(G *g, unsigned t, bool need_var)
{
    int v = pick_var(g, t, false);
    if (v >= 0 && (need_var || chance(g, 65)))
        return var_ref(g, (uint32_t)v);
    if (!need_var)
        return lit(g, t, rand_value(g, t));
    int w = pick_var(g, NTYPES, false);
    if (w < 0) {
        /* none of an integer type: a Boolean compared with a literal */
        w = pick_var(g, T_BOOL, false);
        return binop(g, O_EQ, T_BOOL, var_ref(g, (uint32_t)w),
                     lit(g, T_BOOL, below(g, 2)));
    }
    if (t == T_BOOL) {
        unsigned u = g->v[w].t;
        return binop(g, O_EQ + below(g, 6), T_BOOL, var_ref(g, (uint32_t)w),
                     lit(g, u, rand_value(g, u)));
    }
    if (chance(g, 70)) {
        int x = pick_var(g, within(g, t), false);
        if (x >= 0)
            w = x;
    }
    return conv(g, t, var_ref(g, (uint32_t)w));
}

static uint32_t expr(G *g, unsigned t, int d, bool need_var)
{
    if (d <= 0 || chance(g, 20))
        return leaf(g, t, need_var);
    unsigned f = fam(t);
    unsigned choice = below(g, 10);
    if (choice == 9) {
        int fn = pick_func(g, t);
        if (fn >= 0)
            return call_expr(g, (uint32_t)fn, d);
        choice = 0;
    }
    if (f == 'L') {
        if (choice < 5) {
            unsigned u = any_type(g);
            uint32_t a = expr(g, u, d - 1, false);
            uint32_t b = expr(g, u, d - 1, g->e[a].cst);
            return binop(g, O_EQ + below(g, 6), T_BOOL, a, b);
        }
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
        if (divide && g->e[b].cst && g->e[b].lit == 0)
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
    return conv(
        g, t, expr(g, chance(g, 60) ? within(g, t) : int_type(g), d - 1, true));
}

/* ---- statements ---- */

static uint32_t block(G *g, int n, uint32_t *count);

static int depth(G *g)
{
    return 1 + (int)below(g, 4);
}

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

/* one statement, or two when a loop needs its counter declared first;
   they go to out, and the count is returned */
static uint32_t stmt(G *g, uint32_t *out)
{
    g->budget--;
    unsigned c = below(g, 100);
    bool pure = in_function(g);
    bool deep = g->nesting < 4 && g->budget > 4;
    if (c < 14) {
        unsigned t = any_type(g);
        uint32_t s = new_s(g, S_VAR);
        uint32_t e = expr(g, t, depth(g), false);
        uint32_t v = new_v(g, t, V_LOCAL);
        g->st[s].var = v;
        g->st[s].e = e;
        show(g, v);
        out[0] = s;
        return 1;
    }
    if (c < 36) {
        int v = pick_var(g, any_type(g), true);
        if (v < 0)
            v = pick_var(g, NTYPES, true);
        if (v >= 0) {
            uint32_t s = new_s(g, S_ASSIGN);
            uint32_t e = expr(g, g->v[v].t, depth(g), false);
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
        unsigned t = int_type(g);
        uint32_t s = new_s(g, S_FOR);
        bool down = chance(g, 30);
        uint32_t lo, hi;
        int x = pick_var(g, t, false);
        if (x >= 0 && chance(g, 50)) {
            /* from a variable to a few steps away: the bound may overflow */
            lo = var_ref(g, (uint32_t)x);
            hi = binop(g, down ? O_SUB : O_ADD, t, var_ref(g, (uint32_t)x),
                       lit(g, t, below(g, 4)));
        } else {
            v128 a = rand_value(g, t);
            v128 b = clamp(t, down ? a - (v128)below(g, 5) + 1
                                   : a + (v128)below(g, 5) - 1);
            lo = lit(g, t, a);
            hi = lit(g, t, b);
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
        unsigned t = int_type(g);
        uint32_t s = new_s(g, S_CASE);
        g->st[s].e = expr(g, t, depth(g), true);
        uint32_t na = 1 + below(g, 3);
        xarm arms[3];
        xlab labs[12];
        uint32_t nl = 0;
        for (uint32_t i = 0; i < na; i++) {
            arms[i].lab = nl;
            arms[i].nlab = 0;
            for (uint32_t k = 0, want = 1 + below(g, 2); k < want; k++) {
                v128 lo = rand_value(g, t);
                v128 hi = chance(g, 30) ? clamp(t, lo + below(g, 6)) : lo;
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
        uint32_t base = g->nlab;
        for (uint32_t i = 0; i < nl; i++) {
            LIMBA_GROW(g->lab, g->nlab, g->caplab);
            g->lab[g->nlab++] = labs[i];
        }
        g->st[s].arm = g->narm;
        g->st[s].narm = na;
        for (uint32_t i = 0; i < na; i++) {
            arms[i].lab += base;
            LIMBA_GROW(g->arm, g->narm, g->caparm);
            g->arm[g->narm++] = arms[i];
        }
        uint32_t mark = g->nscope;
        uint32_t n;
        uint32_t b = block(g, (int)below(g, 3), &n);
        g->nscope = mark;
        g->st[s].alt = b;
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
                if (g->v[p].kind == V_VAR) {
                    int v = pick_var(g, g->v[p].t, true);
                    if (v < 0)
                        ok = false;
                    else
                        a[k] = var_ref(g, (uint32_t)v);
                } else {
                    a[k] = expr(g, g->v[p].t, depth(g), false);
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
        unsigned t = any_type(g);
        uint32_t s = new_s(g, S_VAR);
        uint32_t e = expr(g, t, depth(g), false);
        uint32_t v = new_v(g, t, V_LOCAL);
        g->st[s].var = v;
        g->st[s].e = e;
        show(g, v);
        out[0] = s;
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
            g->st[s].e = expr(g, g->r[g->cur].rt, depth(g), false);
        return s;
    }
    return new_s(g, chance(g, 50) ? S_EXIT : S_CONT);
}

static uint32_t block(G *g, int n, uint32_t *count)
{
    uint32_t *x = NULL, nx = 0, cap = 0;
    g->nesting++;
    for (int i = 0; i < n && g->budget > 0; i++) {
        uint32_t two[2];
        uint32_t k = stmt(g, two);
        for (uint32_t j = 0; j < k; j++) {
            LIMBA_GROW(x, nx, cap);
            x[nx++] = two[j];
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

static void routine(G *g)
{
    LIMBA_GROW(g->r, g->nr, g->capr);
    uint32_t id = g->nr;
    xr *r = &g->r[g->nr++];
    memset(r, 0, sizeof(*r));
    r->func = chance(g, 50);
    r->rt = (uint8_t)any_type(g);
    r->np = (uint8_t)below(g, 4);
    uint32_t mark = g->nscope;
    for (uint32_t k = 0; k < g->r[id].np; k++) {
        unsigned t = any_type(g);
        bool byref = !g->r[id].func && chance(g, 40);
        uint32_t p = new_v(g, t, byref ? V_VAR : V_IN);
        g->r[id].par[k] = p;
        show(g, p);
    }
    g->cur = (int)id;
    uint32_t n;
    uint32_t b = block(g, 2 + (int)below(g, 5), &n);
    if (g->r[id].func) {
        uint32_t s = new_s(g, S_RETURN);
        g->st[s].e = expr(g, g->r[id].rt, depth(g), false);
        uint32_t *x = limba_xmalloc((n + 1) * sizeof(uint32_t));
        memcpy(x, g->ls + b, n * sizeof(uint32_t));
        x[n] = s;
        b = keep_list(g, x, n + 1);
        n++;
        free(x);
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

static void pexpr(G *g, text *o, uint32_t i)
{
    xe *x = &g->e[i];
    switch (x->k) {
    case E_LIT:
        put_value(o, x->t, x->lit);
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
        g->e[i].line = o->line;
        g->e[i].col = o->col;
        put(o, otext[x->op]);
        put(o, " ");
        pexpr(g, o, g->e[i].b);
        put(o, ")");
        break;
    case E_UN:
        put(o, "(");
        g->e[i].line = o->line;
        g->e[i].col = o->col;
        put(o, x->op == O_NEG ? "-(" : x->op == O_ABS ? "abs (" : "not (");
        pexpr(g, o, g->e[i].a);
        put(o, "))");
        break;
    case E_CONV:
        g->e[i].line = o->line;
        g->e[i].col = o->col;
        put(o, tname[x->t]);
        put(o, "(");
        pexpr(g, o, g->e[i].a);
        put(o, ")");
        break;
    case E_CALL:
        g->e[i].line = o->line;
        g->e[i].col = o->col;
        put_rname(o, g, x->fn);
        put(o, "(");
        for (uint32_t k = 0; k < g->e[i].nargs; k++) {
            if (k)
                put(o, ", ");
            pexpr(g, o, g->ls[g->e[i].args + k]);
        }
        put(o, ")");
        break;
    }
}

static void pblock(G *g, text *o, uint32_t b, uint32_t n, int ind);

static void indent(text *o, int ind)
{
    for (int i = 0; i < ind; i++)
        put(o, "  ");
}

static void pstmt(G *g, text *o, uint32_t si, int ind)
{
    xs s = g->st[si];
    indent(o, ind);
    switch (s.k) {
    case S_VAR:
        put(o, "var ");
        put_name(o, g, s.var);
        putf(o, ": %s := ", tname[g->v[s.var].t]);
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_ASSIGN:
        put_name(o, g, s.var);
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
        putf(o, ": %s := ", tname[g->v[s.var].t]);
        pexpr(g, o, s.e);
        put(o, s.down ? " downto " : " to ");
        pexpr(g, o, s.e2);
        put(o, " do\n");
        pblock(g, o, s.blk, s.nblk, ind + 1);
        indent(o, ind);
        put(o, "end;\n");
        break;
    case S_CASE: {
        unsigned t = g->e[s.e].t;
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
        put(o, "writeln(");
        for (uint32_t k = 0; k < s.nargs; k++) {
            if (k)
                put(o, ", ");
            pexpr(g, o, g->ls[s.args + k]);
        }
        put(o, ");\n");
        break;
    case S_CALL:
        put_rname(o, g, s.fn);
        put(o, "(");
        for (uint32_t k = 0; k < s.nargs; k++) {
            if (k)
                put(o, ", ");
            pexpr(g, o, g->ls[s.args + k]);
        }
        put(o, ");\n");
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
    if (nglob) {
        put(o, "\nvar\n");
        for (uint32_t v = 0; v < nglob; v++) {
            put(o, "  ");
            put_name(o, g, v);
            putf(o, ": %s := ", tname[g->v[v].t]);
            pexpr(g, o, g->st[v].e);
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
            putf(o, ": %s", tname[g->v[p].t]);
        }
        put(o, ")");
        if (x->func)
            putf(o, ": %s", tname[x->rt]);
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
    uint32_t *ref;
    text out;
    bool trap, toolong;
    int code;
    uint32_t line, col;
    uint64_t steps;
    v128 ret;
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

static v128 fail(X *x, uint32_t at, int code)
{
    if (!x->trap) {
        x->trap = true;
        x->code = code;
        x->line = x->g->e[at].line;
        x->col = x->g->e[at].col;
    }
    return 0;
}

static int run_block(X *x, uint32_t b, uint32_t n);

static v128 ev(X *x, uint32_t i);

static v128 call(X *x, uint32_t fn, uint32_t args, uint32_t nargs)
{
    G *g = x->g;
    v128 val[4];
    uint32_t target[4];
    for (uint32_t k = 0; k < nargs; k++) {
        uint32_t a = g->ls[args + k];
        if (g->v[g->r[fn].par[k]].kind == V_VAR)
            target[k] = x->ref[g->e[a].var];
        else
            val[k] = ev(x, a);
        if (x->trap)
            return 0;
    }
    for (uint32_t k = 0; k < nargs; k++) {
        uint32_t p = g->r[fn].par[k];
        if (g->v[p].kind == V_VAR) {
            x->ref[p] = target[k];
        } else {
            x->ref[p] = p;
            x->cell[p] = val[k];
        }
    }
    x->ret = 0;
    run_block(x, g->r[fn].blk, g->r[fn].nblk);
    return x->ret;
}

static v128 binary(X *x, uint32_t i)
{
    const xe *e = &x->g->e[i];
    unsigned op = e->op, t = e->t;
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
    const xe *e = &x->g->e[i];
    if (x->trap)
        return 0;
    if (++x->steps > STEP_LIMIT) {
        x->toolong = x->trap = true;
        return 0;
    }
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
            return e->t == T_BOOL ? !v : wrap(e->t, ~v);
        if (e->op == O_NEG && fam(e->t) == 'B')
            return wrap(e->t, -v);
        v = e->op == O_NEG ? -v : v < 0 ? -v : v;
        return fits(e->t, v) ? v : fail(x, i, 6);
    }
    case E_CONV: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        if (fam(e->t) == 'B')
            return wrap(e->t, v);
        return fits(e->t, v) ? v : fail(x, i, 103);
    }
    case E_CALL:
        return call(x, e->fn, e->args, e->nargs);
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
    case S_VAR:
    case S_ASSIGN: {
        v128 v = ev(x, s->e);
        if (x->trap)
            return X_RET;
        x->cell[x->ref[s->var]] = v;
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
        v128 lo = ev(x, s->e);
        v128 hi = ev(x, s->e2);
        if (x->trap)
            return X_RET;
        if (s->down ? lo < hi : lo > hi)
            return X_NEXT;
        for (v128 v = lo;; v += s->down ? -1 : 1) {
            x->cell[s->var] = v;
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
            print_value(&x->out, g->e[a].t, v);
        }
        put(&x->out, "\n");
        return X_NEXT;
    case S_CALL:
        call(x, s->fn, s->args, s->nargs);
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
            if (x->trap)
                return X_RET;
        }
        return X_RET;
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
    free(g->e);
    free(g->st);
    free(g->arm);
    free(g->lab);
    free(g->ls);
    free(g->v);
    free(g->r);
    free(g->scope);
}

static bool attempt(uint64_t seed, limba_lxgen *p)
{
    G g;
    memset(&g, 0, sizeof(g));
    g.s = seed;
    g.cur = -1;
    new_e(&g, E_LIT, 0); /* node 0 stands for none */

    /* the globals: at least one of an integer type and one Boolean; their
       initialisers are the statements of the same numbers */
    uint32_t nglob = 2 + below(&g, 5);
    for (uint32_t k = 0; k < nglob; k++) {
        unsigned t = k == 0 ? int_type(&g) : k == 1 ? T_BOOL : any_type(&g);
        uint32_t v = new_v(&g, t, V_GLOBAL);
        uint32_t s = new_s(&g, S_VAR);
        g.st[s].var = v;
        g.st[s].e = lit(&g, t, rand_value(&g, t));
        show(&g, v);
    }
    g.budget = 30 + (int)below(&g, 60);
    for (uint32_t k = 0, nr = below(&g, 5); k < nr && g.budget > 10; k++)
        routine(&g);
    g.main_blk = block(&g, 4 + (int)below(&g, 12), &g.main_n);
    /* the program ends printing every global */
    {
        uint32_t *x = limba_xmalloc((g.main_n + 1) * sizeof(uint32_t));
        memcpy(x, g.ls + g.main_blk, g.main_n * sizeof(uint32_t));
        uint32_t items[16], k = 0;
        for (uint32_t v = 0; v < nglob; v++) {
            if (v)
                items[k++] = new_e(&g, E_STR, 0);
            items[k++] = var_ref(&g, v);
        }
        uint32_t s = new_s(&g, S_WRITE);
        g.st[s].args = keep_list(&g, items, k);
        g.st[s].nargs = k;
        x[g.main_n] = s;
        g.main_blk = keep_list(&g, x, g.main_n + 1);
        g.main_n++;
        free(x);
    }

    text src = {NULL, 0, 0, 1, 1};
    program(&g, &src, nglob);

    X x;
    memset(&x, 0, sizeof(x));
    x.g = &g;
    x.cell = limba_xcalloc(g.nv ? g.nv : 1, sizeof(v128));
    x.ref = limba_xmalloc((g.nv ? g.nv : 1) * sizeof(uint32_t));
    for (uint32_t v = 0; v < g.nv; v++)
        x.ref[v] = v;
    x.out.line = x.out.col = 1;
    for (uint32_t v = 0; v < nglob; v++)
        x.cell[v] = g.e[g.st[v].e].lit;
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
