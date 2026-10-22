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
 *
 * Exceptions (progetto_ir.md § 11i): a try is a protected region, its
 * blocks all naming the handler, which catches everything, catches one
 * identity and raises the others again, runs a finally that raises again,
 * or raises another; tries nest, also inside handlers and loops. A raise
 * is conditional, now and then of an identity no handler may catch; a
 * helper may be the body of a par_for, writing to the slot of its caller.
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
    uint32_t slot;    /* the value holding the address of slot $0 */
    int calls;        /* calls made by the current function */
    limba_id handler; /* of the blocks made now, LIMBA_NONE for none */
    int trys;         /* tries open */
    limba_id body;    /* the function par_for runs, LIMBA_NONE for none */
    /* a try whose region begins with a loop: its handler, the region
       opened by loop() once the values the loop starts with are made */
    limba_id open_try;
    /* a try whose region ends with the way into a loop: the handler the
       loop has, LIMBA_NONE for none */
    bool close_try;
    limba_id close_to;
    bool main; /* the current function is @main */
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

/* a new block, in the region open now */
static limba_id block(G *g)
{
    limba_id b = limba_block_add(F(g));
    if (g->handler != LIMBA_NONE)
        limba_block_set_handler(F(g), b, g->handler);
    return b;
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
        int64_t bits;
        if (t == LIMBA_T_F32) {
            float f = (float)x;
            uint32_t u;
            memcpy(&u, &f, sizeof(u));
            bits = u; /* an f32 constant is the bits of its float */
        } else {
            memcpy(&bits, &x, sizeof(bits));
        }
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
    uint32_t c = condition(g);
    limba_id bt = block(g), be = block(g);
    limba_id bm = block(g);
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
    /* now and then the way in tests a condition first (a zero-trip guard),
       and in a region the header checks one from outside, which LICM may
       move before the loop only where the same handler catches it */
    uint32_t guard = chance(g, 30) ? condition(g) : UINT32_MAX;
    uint32_t inv = UINT32_MAX;
    if (g->close_try ||
        ((g->handler != LIMBA_NONE || g->open_try != LIMBA_NONE) &&
         chance(g, 40))) { /* from memory: no pass knows it */
        uint32_t k =
            emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, below(g, 8), 0, NULL, 0);
        uint32_t o[2] = {g->slot, k};
        uint32_t a = emit(g, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, 8, 0, o, 2);
        uint32_t x = emit(g, LIMBA_OP_LOAD, LIMBA_T_I64, 0, 0, 0, &a, 1);
        inv = cmp(g, LIMBA_T_I64, x, pick(g, LIMBA_T_I64));
    }
    if (g->open_try != LIMBA_NONE) { /* the try begins here */
        g->handler = g->open_try;
        g->open_try = LIMBA_NONE;
        limba_id bp = block(g);
        br(g, bp, NULL, 0);
        g->cur = bp;
    }
    if (g->close_try) { /* the try ends here: the loop is outside it */
        g->handler = g->close_to;
        g->close_try = false;
    }
    limba_id bh = block(g), bb = block(g);
    limba_id bx = block(g);
    uint32_t hp[3];
    hp[0] = limba_param_add(F(g), bh, LIMBA_T_I64);
    for (uint32_t i = 0; i < nacc; i++)
        hp[1 + i] = limba_param_add(F(g), bh, types[i]);
    if (guard != UINT32_MAX) {
        uint32_t ops[2 + 3 + 3] = {guard, bh, 1 + nacc};
        for (uint32_t i = 0; i < 1 + nacc; i++)
            ops[3 + i] = init[i];
        ops[3 + 1 + nacc] = bx;
        ops[4 + 1 + nacc] = 0;
        emit(g, LIMBA_OP_CBR, LIMBA_T_VOID, 0, 0, 0, ops, 5 + 1 + nacc);
    } else {
        br(g, bh, init, 1 + nacc);
    }

    g->cur = bh;
    if (inv != UINT32_MAX)
        emit(g, LIMBA_OP_CHECK, LIMBA_T_VOID, 0, chance(g, 50) ? 100 : 9, 0,
             &inv, 1);
    uint32_t c[2] = {hp[0], trip};
    uint32_t go = emit(g, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_SLT, 0, 0, c, 2);
    uint32_t ops[5] = {go, bb, 0, bx, 0};
    emit(g, LIMBA_OP_CBR, LIMBA_T_VOID, 0, 0, 0, ops, 5);

    uint32_t before = g->nvals;
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
    /* the header's parameters dominate the exit, unless the guard
       jumps there too */
    g->nvals = guard != UINT32_MAX ? before : outer;
    g->cur = bx;
}

static void switch_stmt(G *g)
{
    uint32_t three = emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, 3, 0, NULL, 0);
    uint32_t sel =
        op2(g, LIMBA_OP_AND, LIMBA_T_I64, pick(g, LIMBA_T_I64), three);
    limba_id t = rand_type(g);
    limba_id cases[3], bd = block(g);
    for (int k = 0; k < 3; k++)
        cases[k] = block(g);
    limba_id bm = block(g);
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
    limba_id callee = below(g, g->fid);
    if (callee == g->body)
        return;
    g->calls++;
    limba_id type = g->m->funcs[callee].type;
    limba_sig s;
    limba_type_sig(g->m, type, &s);
    uint32_t args[1 + 4], v;
    for (uint32_t i = 0; i < s.n; i++)
        args[1 + i] = pick(g, limba_sig_param(&s, i));
    if (chance(g, 25)) { /* through its address */
        args[0] = emit(g, LIMBA_OP_FADDR, LIMBA_T_PTR, 0, callee, 0, NULL, 0);
        v = emit(g, LIMBA_OP_CALLIND, s.ret, 0, type, 0, args, 1 + s.n);
    } else {
        v = emit(g, LIMBA_OP_CALL, s.ret, 0, callee, 0, args + 1, s.n);
    }
    keep(g, s.ret, v);
}

/* ---- exceptions ---- */

/* the identities raised: the table's (below), traps, a family, one the
   table does not name; now and then one no handler may catch */
static int32_t identity(G *g)
{
    static const int32_t pool[] = {1000, 1001, 1002, 1003, 1003, 1004,
                                   100,  6,    108,  900,  902};
    static const int32_t fatal[] = {7, 28, 901};
    if (chance(g, 3))
        return fatal[below(g, 3)];
    return pool[below(g, sizeof(pool) / sizeof(pool[0]))];
}

/* raise an occurrence: a new one, or now and then the identity made at
   run time from a value in scope */
static void raise_new(G *g)
{
    uint32_t o[3];
    if (chance(g, 20)) {
        uint32_t three =
            emit(g, LIMBA_OP_ICONST, LIMBA_T_I32, 0, 3, 0, NULL, 0);
        uint32_t base =
            emit(g, LIMBA_OP_ICONST, LIMBA_T_I32, 0, 1000, 0, NULL, 0);
        uint32_t k =
            op2(g, LIMBA_OP_AND, LIMBA_T_I32, pick(g, LIMBA_T_I32), three);
        o[0] = op2(g, LIMBA_OP_ADD, LIMBA_T_I32, k, base);
    } else {
        o[0] =
            emit(g, LIMBA_OP_ICONST, LIMBA_T_I32, 0, identity(g), 0, NULL, 0);
    }
    /* positions 1 and 2 are in the module, 0 is none */
    o[1] = emit(g, LIMBA_OP_ICONST, LIMBA_T_I32, 0, below(g, 3), 0, NULL, 0);
    static const char *const words[] = {"", "bad digit", "deep", "x"};
    const char *w = words[below(g, 4)];
    o[2] = emit(g, LIMBA_OP_SCONST, LIMBA_T_STR, 0,
                limba_str_intern(g->m, w, strlen(w)), 0, NULL, 0);
    emit(g, LIMBA_OP_RAISE, LIMBA_T_VOID, 0, 0, 0, o, 3);
}

/* if a condition holds, raise */
static void raise_stmt(G *g)
{
    uint32_t c = condition(g);
    limba_id br_ = block(g), bc = block(g);
    uint32_t ops[5] = {c, br_, 0, bc, 0};
    emit(g, LIMBA_OP_CBR, LIMBA_T_VOID, 0, 0, 0, ops, 5);
    uint32_t mark = g->nvals;
    g->cur = br_;
    raise_new(g);
    g->nvals = mark; /* what the raise made dominates nothing */
    g->cur = bc;
}

static void print_i32(G *g, uint32_t v)
{
    v = emit(g, LIMBA_OP_SEXT, LIMBA_T_I64, 0, 0, 0, &v, 1);
    emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_I64, 0, &v, 1);
    emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_NL, 0, NULL, 0);
}

static void print_s(G *g, uint32_t s)
{
    emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_STR, 0, &s, 1);
    emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_NL, 0, NULL, 0);
}

/* try: the region, then its handler, both flowing to a join that takes
   values; the handler sees only what was in scope before the region */
static void try_stmt(G *g)
{
    limba_id outer = g->handler;
    limba_id bh = block(g); /* protected by the outer region, if any */
    uint32_t hp[3];
    hp[0] = limba_param_add(F(g), bh, LIMBA_T_I32);
    hp[1] = limba_param_add(F(g), bh, LIMBA_T_I32);
    hp[2] = limba_param_add(F(g), bh, LIMBA_T_STR);
    limba_id bj = block(g);
    uint32_t nm = below(g, 3);
    limba_id types[2];
    uint32_t params[2];
    for (uint32_t i = 0; i < nm; i++) {
        types[i] = rand_type(g);
        params[i] = limba_param_add(F(g), bj, types[i]);
    }
    uint32_t args[2];

    uint32_t mark = g->nvals;
    g->trys++;
    if (g->depth < 2 && chance(g, 30)) { /* the region begins with a loop */
        g->open_try = bh;
        loop(g);
    } else {
        g->handler = bh;
        limba_id bp = block(g);
        br(g, bp, NULL, 0);
        g->cur = bp;
    }
    if (chance(g, 50)) { /* what a call raises, perhaps once in line, and
                            what fails after it */
        call_stmt(g);
        if (chance(g, 50)) {
            uint32_t c = condition(g);
            emit(g, LIMBA_OP_CHECK, LIMBA_T_VOID, 0, 100, 0, &c, 1);
        }
    }
    statements(g, 1 + (int)below(g, 5));
    /* how the handler ends: 0 and 1 may go on after the try */
    unsigned kind = below(g, 4);
    if (kind >= 2 && g->depth < 2 && chance(g, 40)) {
        /* only the region reaches the join: it may end with the way
           into a loop that is not in it */
        g->close_try = true;
        g->close_to = outer;
        loop(g);
    }
    if (g->handler == bh && chance(g, 15))
        raise_new(g);
    else {
        for (uint32_t i = 0; i < nm; i++)
            args[i] = pick(g, types[i]);
        br(g, bj, args, nm);
    }
    g->nvals = mark;
    g->handler = outer;

    g->depth++;
    g->cur = bh;
    keep(g, LIMBA_T_I32, hp[0]);
    keep(g, LIMBA_T_I32, hp[1]);
    switch (kind) {
    case 0: /* catch them all */
        print_i32(g, hp[0]);
        print_s(g, hp[2]);
        statements(g, (int)below(g, 3));
        for (uint32_t i = 0; i < nm; i++)
            args[i] = pick(g, types[i]);
        br(g, bj, args, nm);
        break;
    case 1: { /* one identity, the others raised again */
        uint32_t k =
            emit(g, LIMBA_OP_ICONST, LIMBA_T_I32, 0, identity(g), 0, NULL, 0);
        uint32_t c[2] = {hp[0], k};
        uint32_t is =
            emit(g, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_EQ, 0, 0, c, 2);
        limba_id bc = block(g), br_ = block(g);
        uint32_t ops[5] = {is, bc, 0, br_, 0};
        emit(g, LIMBA_OP_CBR, LIMBA_T_VOID, 0, 0, 0, ops, 5);
        uint32_t inner = g->nvals;
        g->cur = bc;
        print_s(g, hp[2]);
        statements(g, (int)below(g, 3));
        for (uint32_t i = 0; i < nm; i++)
            args[i] = pick(g, types[i]);
        br(g, bj, args, nm);
        g->nvals = inner;
        g->cur = br_;
        emit(g, LIMBA_OP_RAISE, LIMBA_T_VOID, 0, 0, 0, hp, 3);
        break;
    }
    case 2: /* finally: raised again */
        print_i32(g, hp[1]);
        statements(g, (int)below(g, 3));
        emit(g, LIMBA_OP_RAISE, LIMBA_T_VOID, 0, 0, 0, hp, 3);
        break;
    default: /* another exception */
        statements(g, (int)below(g, 2));
        raise_new(g);
    }
    g->depth--;
    g->trys--;
    g->nvals = mark;
    g->cur = bj;
    for (uint32_t i = 0; i < nm; i++)
        keep(g, types[i], params[i]);
}

/* par_for over a few indices, outside loops and branches like a call; the
   body writes to the slot of this function */
static void par_for_stmt(G *g)
{
    if (g->body == LIMBA_NONE || g->body >= g->fid || g->depth > 0 ||
        g->calls >= 3)
        return;
    g->calls++;
    int64_t lo = below(g, 3), hi = lo + below(g, 4) - 1;
    uint32_t o[4];
    o[0] = emit(g, LIMBA_OP_FADDR, LIMBA_T_PTR, 0, g->body, 0, NULL, 0);
    o[1] = g->slot;
    o[2] = emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, lo, 0, NULL, 0);
    o[3] = emit(g, LIMBA_OP_ICONST, LIMBA_T_I64, 0, hi, 0, NULL, 0);
    emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PAR_FOR, 0, o, 4);
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
    unsigned r = below(g, 111);
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
    } else if (r < 100)
        print_stmt(g);
    else if (r < 105 && g->depth < 3 && g->trys < 2)
        try_stmt(g);
    else if (r < 108 && (g->trys || !g->main || chance(g, 30)))
        raise_stmt(g); /* outside a try in main the end of the program */
    else
        par_for_stmt(g);
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
    /* now and then a helper short enough to go in line, with what it
       raises */
    g->budget = !is_main && chance(g, 40) ? 2 + (int)below(g, 6)
                                          : 15 + (int)below(g, 50);
    g->calls = 0;
    g->handler = LIMBA_NONE;
    g->trys = 0;
    g->main = is_main;
    limba_func *f = F(g);
    limba_sig s;
    limba_type_sig(g->m, f->type, &s);
    g->cur = limba_block_add(f);
    uint32_t first = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        limba_id t = limba_sig_param(&s, i);
        uint32_t v = limba_param_add(F(g), g->cur, t);
        if (i == 0)
            first = v;
        if (t != LIMBA_T_PTR)
            keep(g, t, v);
    }
    if (fid == g->body) { /* the slot of the caller, of 64 bytes */
        g->slot = first;
    } else {
        limba_slot_add(F(g), 64, 8);
        g->slot = emit(g, LIMBA_OP_SLOT, LIMBA_T_PTR, 0, 0, 0, NULL, 0);
    }
    while (g->budget > 0)
        statement(g);
    if (!is_main && chance(g, 40)) /* what its callers may catch */
        raise_stmt(g);
    if (is_main)
        for (int i = 0; i < 3; i++) {
            uint32_t v = pick(g, LIMBA_T_I64);
            emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_I64, 0, &v,
                 1);
            emit(g, LIMBA_OP_CALLRT, LIMBA_T_VOID, 0, LIMBA_RT_PRINT_NL, 0,
                 NULL, 0);
        }
    if (s.ret == LIMBA_T_VOID) {
        emit(g, LIMBA_OP_RET, LIMBA_T_VOID, 0, 0, 0, NULL, 0);
        return;
    }
    uint32_t r = pick(g, s.ret);
    emit(g, LIMBA_OP_RET, LIMBA_T_VOID, 0, 0, 0, &r, 1);
}

limba_module *limba_gen(uint64_t seed)
{
    G g = {.s = seed, .body = LIMBA_NONE, .open_try = LIMBA_NONE};
    g.m = limba_module_new();
    uint32_t helpers = below(&g, 4);
    if (helpers && chance(&g, 50))
        g.body = 0; /* the first helper is the body of par_for */
    /* the exceptions named, often none: then lir_run words them by code */
    if (chance(&g, 60)) {
        static const struct {
            int32_t id;
            const char *name;
            int32_t parent;
        } table[] = {
            {900, "ConstraintError", -1}, {902, "ProgramError", -1},
            {100, "IndexError", 900},     {6, "OverflowError", 900},
            {108, "LengthError", 900},    {1000, "ParseError", -1},
            {1001, "BadDigit", 1000},     {1002, "BadSign", 1000},
            {1003, "Empty", -1},
        };
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            limba_exception_add(
                g.m, table[i].id,
                limba_str_intern(g.m, table[i].name, strlen(table[i].name)),
                table[i].parent);
    }
    limba_id file = limba_str_intern(g.m, "gen.lx", 6);
    limba_pos_add(g.m, file, 3, 7);
    limba_pos_add(g.m, file, 12, 1);
    char name[16];
    for (uint32_t i = 0; i <= helpers; i++) {
        limba_id params[3], ret = LIMBA_T_I64;
        uint32_t n = 0;
        if (i == g.body) {
            params[0] = LIMBA_T_PTR;
            params[1] = LIMBA_T_I64;
            n = 2;
            ret = LIMBA_T_VOID;
            snprintf(name, sizeof(name), "f%u", i);
        } else if (i < helpers) {
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
