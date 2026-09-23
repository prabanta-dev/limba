/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gen.c - random IR programs (see gen.h).
 *
 * A function is grown statement by statement from the values in scope: a
 * value is in scope where its definition dominates, so a value made inside
 * a branch or a loop body is forgotten when the region closes, and what
 * flows out of it flows through block parameters. Loops count up to a
 * small bound; a function calls only the functions made before it. The
 * weights favour what the passes look for: constants, x - x, x * 1,
 * repeated expressions, branches on constants, values nobody uses.
 */
#include "gen.h"

#include "common/xalloc.h"
#include "ir/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    limba_id t;
    uint32_t v;
} gval;

typedef struct {
    uint64_t s;
    limba_module *m;
    limba_id fid;
    limba_id cur;
    gval *vals;
    uint32_t nvals, capvals;
    int depth;
    int budget;
    uint32_t slot; /* the value holding the address of slot $0 */
    int calls;     /* calls made by the current function */
} G;

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

static limba_func *F(G *g)
{
    return &g->m->funcs[g->fid];
}

static uint32_t emit(G *g, unsigned op, limba_id t, unsigned cc, int64_t imm,
                     int64_t imm2, const uint32_t *ops, uint32_t n)
{
    return limba_inst_add(F(g), g->cur, op, t, cc, imm, imm2, ops, n);
}

static void keep(G *g, limba_id t, uint32_t v)
{
    LIMBA_GROW(g->vals, g->nvals, g->capvals);
    g->vals[g->nvals++] = (gval){t, v};
}

static limba_id rand_type(G *g)
{
    static const limba_id types[] = {
        LIMBA_T_I64, LIMBA_T_I64, LIMBA_T_I64, LIMBA_T_I64,
        LIMBA_T_I64, LIMBA_T_I32, LIMBA_T_I32, LIMBA_T_I32,
        LIMBA_T_I8,  LIMBA_T_I8,  LIMBA_T_I1,  LIMBA_T_F64,
        LIMBA_T_F64, LIMBA_T_F64, LIMBA_T_F32, LIMBA_T_F32,
    };
    return types[below(g, sizeof(types) / sizeof(types[0]))];
}

static limba_id rand_int_type(G *g)
{
    limba_id t;
    do
        t = rand_type(g);
    while (!limba_type_is_int(t));
    return t;
}

/* ---- values ---- */

static uint32_t constant(G *g, limba_id t)
{
    uint32_t v;
    if (limba_type_is_int(t)) {
        static const int64_t pool[] = {0, 1, -1, 2, 3, 7, 100, 255, -128};
        unsigned bits = limba_type_bits(t);
        int64_t k;
        switch (below(g, 6)) {
        case 0:
            k = bits >= 64 ? INT64_MAX : ((int64_t)1 << (bits - 1)) - 1;
            break;
        case 1:
            k = bits >= 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));
            break;
        case 2:
            k = (int64_t)rnd(g);
            break;
        default:
            k = pool[below(g, sizeof(pool) / sizeof(pool[0]))];
        }
        v = emit(g, LIMBA_OP_ICONST, t, 0, limba_int_norm(k, t), 0, NULL, 0);
    } else {
        static const double pool[] = {0.0, -0.0,  1.0,    -1.5, 0.1,
                                      2.5, 1e300, 3e-310, 1e9};
        double x;
        switch (below(g, 12)) {
        case 0:
            x = INFINITY;
            break;
        case 1:
            x = NAN;
            break;
        case 2:
            x = (double)(int32_t)rnd(g) / 1024.0;
            break;
        default:
            x = pool[below(g, sizeof(pool) / sizeof(pool[0]))];
        }
        if (t == LIMBA_T_F32)
            x = (float)x;
        int64_t bits;
        memcpy(&bits, &x, sizeof(bits));
        v = emit(g, LIMBA_OP_FCONST, t, 0, bits, 0, NULL, 0);
    }
    keep(g, t, v);
    return v;
}

/* a value of type t in scope, or a new constant */
static uint32_t pick(G *g, limba_id t)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->nvals; i++)
        n += g->vals[i].t == t;
    if (!n || chance(g, 12))
        return constant(g, t);
    uint32_t k = below(g, n);
    for (uint32_t i = 0; i < g->nvals; i++)
        if (g->vals[i].t == t && k-- == 0)
            return g->vals[i].v;
    return constant(g, t);
}

static uint32_t op2(G *g, unsigned op, limba_id t, uint32_t a, uint32_t b)
{
    uint32_t ops[2] = {a, b};
    return emit(g, op, t, 0, 0, 0, ops, 2);
}

static uint32_t cmp(G *g, limba_id t, uint32_t a, uint32_t b)
{
    uint32_t ops[2] = {a, b};
    unsigned cc =
        limba_type_is_float(t) ? LIMBA_CC_OEQ + below(g, 14) : below(g, 10);
    unsigned op = limba_type_is_float(t) ? LIMBA_OP_FCMP : LIMBA_OP_ICMP;
    uint32_t v = emit(g, op, LIMBA_T_I1, cc, 0, 0, ops, 2);
    keep(g, LIMBA_T_I1, v);
    return v;
}

/* an i1: a comparison in scope, a new one, or now and then a constant */
static uint32_t condition(G *g)
{
    if (chance(g, 20))
        return constant(g, LIMBA_T_I1);
    limba_id t = rand_type(g);
    if (t == LIMBA_T_I1 || chance(g, 30))
        return pick(g, LIMBA_T_I1);
    return cmp(g, t, pick(g, t), pick(g, t));
}

/* ---- statements ---- */

static void statements(G *g, int n);

static void int_op(G *g)
{
    static const unsigned ops[] = {
        LIMBA_OP_ADD,  LIMBA_OP_ADD,  LIMBA_OP_SUB,  LIMBA_OP_SUB,
        LIMBA_OP_MUL,  LIMBA_OP_AND,  LIMBA_OP_OR,   LIMBA_OP_XOR,
        LIMBA_OP_SHL,  LIMBA_OP_LSHR, LIMBA_OP_ASHR, LIMBA_OP_SDIV,
        LIMBA_OP_UDIV, LIMBA_OP_SREM, LIMBA_OP_UREM, LIMBA_OP_NEG,
        LIMBA_OP_NOT,
    };
    /* rare: they end most programs they appear in */
    static const unsigned ovs[] = {LIMBA_OP_ADDOV, LIMBA_OP_SUBOV,
                                   LIMBA_OP_MULOV};
    limba_id t = rand_int_type(g);
    unsigned op = chance(g, 3) ? ovs[below(g, 3)]
                               : ops[below(g, sizeof(ops) / sizeof(ops[0]))];
    uint32_t a = pick(g, t), v;
    if (op == LIMBA_OP_NEG || op == LIMBA_OP_NOT) {
        v = emit(g, op, t, 0, 0, 0, &a, 1);
    } else {
        /* the second operand: the same value (x - x), an identity
           constant (x * 1), or another value */
        uint32_t b;
        unsigned how = below(g, 10);
        if (how == 0)
            b = a;
        else if (how == 1) {
            int64_t k = chance(g, 50) ? 0 : 1;
            b = emit(g, LIMBA_OP_ICONST, t, 0, k, 0, NULL, 0);
        } else
            b = pick(g, t);
        bool div = op == LIMBA_OP_SDIV || op == LIMBA_OP_UDIV ||
                   op == LIMBA_OP_SREM || op == LIMBA_OP_UREM;
        if (div && t != LIMBA_T_I1 && chance(g, 90)) { /* mostly safe */
            uint32_t one = emit(g, LIMBA_OP_ICONST, t, 0, 1, 0, NULL, 0);
            b = op2(g, LIMBA_OP_OR, t, b, one);
        }
        v = op2(g, op, t, a, b);
    }
    keep(g, t, v);
}

static void float_op(G *g)
{
    static const unsigned ops[] = {
        LIMBA_OP_FADD, LIMBA_OP_FSUB, LIMBA_OP_FMUL,   LIMBA_OP_FDIV,
        LIMBA_OP_FNEG, LIMBA_OP_FMA,  LIMBA_OP_FROUND, LIMBA_OP_FROUNDA};
    limba_id t = chance(g, 60) ? LIMBA_T_F64 : LIMBA_T_F32;
    unsigned op = ops[below(g, sizeof(ops) / sizeof(ops[0]))];
    uint32_t o[3] = {pick(g, t), pick(g, t), pick(g, t)};
    uint32_t n =
        op == LIMBA_OP_FNEG || op == LIMBA_OP_FROUND || op == LIMBA_OP_FROUNDA
            ? 1
        : op == LIMBA_OP_FMA ? 3
                             : 2;
    keep(g, t, emit(g, op, t, 0, 0, 0, o, n));
}

static void select_op(G *g)
{
    uint32_t c = condition(g);
    limba_id t = rand_type(g);
    uint32_t o[3] = {c, pick(g, t), pick(g, t)};
    if (chance(g, 15))
        o[2] = o[1];
    keep(g, t, emit(g, LIMBA_OP_SELECT, t, 0, 0, 0, o, 3));
}

static void convert(G *g)
{
    limba_id from = rand_type(g), to = rand_type(g);
    unsigned bf = limba_type_bits(from), bt = limba_type_bits(to);
    unsigned op;
    if (bf && bt) {
        if (bf == bt)
            return;
        op = bf > bt ? LIMBA_OP_TRUNC
                     : (chance(g, 50) ? LIMBA_OP_ZEXT : LIMBA_OP_SEXT);
    } else if (bf) {
        op = chance(g, 50) ? LIMBA_OP_SITOFP : LIMBA_OP_UITOFP;
    } else if (bt) {
        if (bt == 1)
            return;
        op = chance(g, 50) ? LIMBA_OP_FPTOSI : LIMBA_OP_FPTOUI;
    } else if (from != to) {
        op = from == LIMBA_T_F64 ? LIMBA_OP_FPTRUNC : LIMBA_OP_FPEXT;
    } else if (from == LIMBA_T_F64 || chance(g, 50)) {
        from = from == LIMBA_T_F64 ? LIMBA_T_F64 : LIMBA_T_F32;
        to = from == LIMBA_T_F64 ? LIMBA_T_I64 : LIMBA_T_I32;
        op = LIMBA_OP_BITCAST;
    } else {
        return;
    }
    uint32_t a = pick(g, from);
    keep(g, to, emit(g, op, to, 0, 0, 0, &a, 1));
}

/* a jump to block b with the values args */
static void br(G *g, limba_id b, const uint32_t *args, uint32_t n)
{
    uint32_t ops[2 + 4];
    ops[0] = b;
    ops[1] = n;
    for (uint32_t i = 0; i < n; i++)
        ops[2 + i] = args[i];
    emit(g, LIMBA_OP_BR, LIMBA_T_VOID, 0, 0, 0, ops, 2 + n);
}

static void diamond(G *g)
{
    limba_func *f = F(g);
    uint32_t c = condition(g);
    limba_id bt = limba_block_add(f), be = limba_block_add(f);
    limba_id bm = limba_block_add(f);
    uint32_t nm = 1 + below(g, 2);
    limba_id types[2];
    uint32_t params[2];
    for (uint32_t i = 0; i < nm; i++) {
        types[i] = rand_type(g);
        params[i] = limba_param_add(F(g), bm, types[i]);
    }
    uint32_t ops[5] = {c, bt, 0, be, 0};
    emit(g, LIMBA_OP_CBR, LIMBA_T_VOID, 0, 0, 0, ops, 5);

    uint32_t mark = g->nvals;
    g->depth++;
    for (int side = 0; side < 2; side++) {
        g->cur = side ? be : bt;
        statements(g, 1 + (int)below(g, 4));
        uint32_t args[2];
        for (uint32_t i = 0; i < nm; i++)
            args[i] = pick(g, types[i]);
        br(g, bm, args, nm);
        g->nvals = mark;
    }
    g->depth--;
    g->cur = bm;
    for (uint32_t i = 0; i < nm; i++)
        keep(g, types[i], params[i]);
}

static void loop(G *g)
{
    limba_func *f = F(g);
    uint32_t trip;
    if (chance(g, 60)) {
        trip =
            emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, below(g, 11), 0, NULL, 0);
    } else { /* a trip count only known at run time, 0 to 7 */
        uint32_t seven =
            emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, 7, 0, NULL, 0);
        trip = op2(g, LIMBA_OP_AND, LIMBA_T_I64, pick(g, LIMBA_T_I64), seven);
    }
    uint32_t nacc = 1 + below(g, 2);
    limba_id types[2];
    uint32_t init[3];
    init[0] = emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, 0, 0, NULL, 0);
    for (uint32_t i = 0; i < nacc; i++) {
        types[i] = rand_type(g);
        init[1 + i] = pick(g, types[i]);
    }
    limba_id bh = limba_block_add(f), bb = limba_block_add(f);
    limba_id bx = limba_block_add(f);
    uint32_t hp[3];
    hp[0] = limba_param_add(F(g), bh, LIMBA_T_I64);
    for (uint32_t i = 0; i < nacc; i++)
        hp[1 + i] = limba_param_add(F(g), bh, types[i]);
    br(g, bh, init, 1 + nacc);

    g->cur = bh;
    uint32_t c[2] = {hp[0], trip};
    uint32_t go = emit(g, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_SLT, 0, 0, c, 2);
    uint32_t ops[5] = {go, bb, 0, bx, 0};
    emit(g, LIMBA_OP_CBR, LIMBA_T_VOID, 0, 0, 0, ops, 5);

    for (uint32_t i = 0; i < 1 + nacc; i++)
        keep(g, i ? types[i - 1] : LIMBA_T_I64, hp[i]);
    uint32_t outer = g->nvals;
    g->depth++;
    g->cur = bb;
    statements(g, 1 + (int)below(g, 5));
    uint32_t next[3];
    uint32_t one = emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, 1, 0, NULL, 0);
    next[0] = op2(g, LIMBA_OP_ADD, LIMBA_T_I64, hp[0], one);
    for (uint32_t i = 0; i < nacc; i++)
        next[1 + i] = pick(g, types[i]);
    br(g, bh, next, 1 + nacc);
    g->depth--;
    g->nvals = outer; /* the header's parameters dominate the exit */
    g->cur = bx;
}

static void switch_stmt(G *g)
{
    limba_func *f = F(g);
    uint32_t three = emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, 3, 0, NULL, 0);
    uint32_t sel =
        op2(g, LIMBA_OP_AND, LIMBA_T_I64, pick(g, LIMBA_T_I64), three);
    limba_id t = rand_type(g);
    limba_id cases[3], bd = limba_block_add(f);
    for (int k = 0; k < 3; k++)
        cases[k] = limba_block_add(F(g));
    limba_id bm = limba_block_add(F(g));
    uint32_t param = limba_param_add(F(g), bm, t);
    /* values 0, 1 and 5: 2 and 3 go to the default, 5 never matches */
    static const int64_t values[3] = {0, 1, 5};
    uint32_t ops[3 + 9] = {sel, bd, 3};
    for (int k = 0; k < 3; k++) {
        ops[3 + 3 * k] = (uint32_t)(uint64_t)values[k];
        ops[4 + 3 * k] = (uint32_t)((uint64_t)values[k] >> 32);
        ops[5 + 3 * k] = cases[k];
    }
    emit(g, LIMBA_OP_SWITCH, LIMBA_T_VOID, 0, 0, 0, ops, 12);
    uint32_t mark = g->nvals;
    g->depth++;
    for (int k = 0; k < 4; k++) {
        g->cur = k < 3 ? cases[k] : bd;
        statements(g, (int)below(g, 3));
        uint32_t v = pick(g, t);
        br(g, bm, &v, 1);
        g->nvals = mark;
    }
    g->depth--;
    g->cur = bm;
    keep(g, t, param);
}

/* Calls only outside loops and branches, three per function at most:
   calls inside loops would multiply down the chain of functions into
   hundreds of millions of steps. */
static void call_stmt(G *g)
{
    if (g->fid == 0 || g->depth > 0 || g->calls >= 3)
        return;
    g->calls++;
    limba_id callee = below(g, g->fid);
    limba_sig s;
    limba_type_sig(g->m, g->m->funcs[callee].type, &s);
    uint32_t args[4];
    for (uint32_t i = 0; i < s.n; i++)
        args[i] = pick(g, limba_sig_param(&s, i));
    uint32_t v = emit(g, LIMBA_OP_CALL, s.ret, 0, callee, 0, args, s.n);
    keep(g, s.ret, v);
}

static void print_stmt(G *g)
{
    limba_id t = rand_type(g);
    uint32_t v = pick(g, t);
    if (limba_type_is_float(t)) {
        if (t == LIMBA_T_F32)
            v = emit(g, LIMBA_OP_FPEXT, LIMBA_T_F64, 0, 0, 0, &v, 1);
        emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_F64, 0, &v, 1);
    } else {
        if (t != LIMBA_T_I64)
            v = emit(g, LIMBA_OP_SEXT, LIMBA_T_I64, 0, 0, 0, &v, 1);
        emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_I64, 0, &v, 1);
    }
    emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_NL, 0, NULL, 0);
}

static void memory_stmt(G *g)
{
    uint32_t k =
        emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, below(g, 8), 0, NULL, 0);
    uint32_t o[2] = {g->slot, k};
    uint32_t a = emit(g, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, 8, 0, o, 2);
    limba_id t = rand_type(g);
    if (chance(g, 50)) {
        uint32_t s[2] = {pick(g, t), a};
        emit(g, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, s, 2);
    } else {
        keep(g, t, emit(g, LIMBA_OP_LOAD, t, 0, 0, 0, &a, 1));
    }
}

static void statement(G *g)
{
    g->budget--;
    unsigned r = below(g, 100);
    if (r < 25)
        int_op(g);
    else if (r < 37)
        float_op(g);
    else if (r < 44)
        select_op(g);
    else if (r < 52)
        convert(g);
    else if (r < 57)
        constant(g, rand_type(g));
    else if (r < 65)
        cmp(g, LIMBA_T_I64, pick(g, LIMBA_T_I64), pick(g, LIMBA_T_I64));
    else if (r < 72 && g->depth < 3)
        diamond(g);
    else if (r < 78 && g->depth < 2)
        loop(g);
    else if (r < 82 && g->depth < 3)
        switch_stmt(g);
    else if (r < 87)
        call_stmt(g);
    else if (r < 93)
        print_stmt(g);
    else if (r < 98)
        memory_stmt(g);
    else if (chance(g, 25)) { /* a failed check ends the program */
        uint32_t c = condition(g);
        emit(g, LIMBA_OP_CHECK, LIMBA_T_VOID, 0, 9, 0, &c, 1);
    } else
        print_stmt(g);
}

static void statements(G *g, int n)
{
    for (int i = 0; i < n && g->budget > 0; i++)
        statement(g);
}

/* ---- functions ---- */

static void body(G *g, limba_id fid, bool is_main)
{
    g->fid = fid;
    g->nvals = 0;
    g->depth = 0;
    g->budget = 15 + (int)below(g, 50);
    g->calls = 0;
    limba_func *f = F(g);
    limba_sig s;
    limba_type_sig(g->m, f->type, &s);
    g->cur = limba_block_add(f);
    for (uint32_t i = 0; i < s.n; i++) {
        limba_id t = limba_sig_param(&s, i);
        keep(g, t, limba_param_add(F(g), g->cur, t));
    }
    limba_slot_add(F(g), 64, 8);
    g->slot = emit(g, LIMBA_OP_SLOT, LIMBA_T_PTR, 0, 0, 0, NULL, 0);
    while (g->budget > 0)
        statement(g);
    if (is_main)
        for (int i = 0; i < 3; i++) {
            uint32_t v = pick(g, LIMBA_T_I64);
            emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_I64, 0, &v,
                 1);
            emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_NL, 0,
                 NULL, 0);
        }
    uint32_t r = pick(g, s.ret);
    emit(g, LIMBA_OP_RET, LIMBA_T_VOID, 0, 0, 0, &r, 1);
}

limba_module *limba_gen(uint64_t seed)
{
    G g = {.s = seed};
    g.m = limba_module_new();
    uint32_t helpers = below(&g, 4);
    char name[16];
    for (uint32_t i = 0; i <= helpers; i++) {
        limba_id params[3], ret = LIMBA_T_I64;
        uint32_t n = 0;
        if (i < helpers) {
            n = below(&g, 4);
            for (uint32_t k = 0; k < n; k++)
                params[k] = rand_type(&g);
            ret = rand_type(&g);
            snprintf(name, sizeof(name), "f%u", i);
        } else {
            snprintf(name, sizeof(name), "main");
        }
        limba_id t = limba_type_func(g.m, ret, params, n, false);
        limba_func_add(g.m, limba_str_intern(g.m, name, strlen(name)), t,
                       i == helpers ? LIMBA_SYM_EXPORT : 0);
    }
    for (uint32_t i = 0; i <= helpers; i++)
        body(&g, i, i == helpers);
    free(g.vals);
    return g.m;
}
