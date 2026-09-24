/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_expr.c - the expressions of Luxia 0 in the IR (see lower.h), with
 * the checks the language promises at run time.
 */
#include "lower.h"

#include <math.h>
#include <string.h>

static const limba_typeinfo *ti(const lxl *L, limba_ltype t)
{
    return &L->S->ts.t[t];
}

static limba_ltype ntype(const lxl *L, uint32_t node)
{
    return L->S->type[node];
}

static limba_lx_node *nd(const lxl *L, uint32_t node)
{
    return &L->S->t->node[node];
}

bool lxl_signed(const lxl *L, limba_ltype t)
{
    const limba_typeinfo *x = ti(L, t);
    return (x->kind == LIMBA_LTK_INT && (x->flags & LIMBA_TF_SIGNED) &&
            !(x->flags & LIMBA_TF_MODULAR)) ||
           x->kind == LIMBA_LTK_FLOAT;
}

static bool is_modular(const lxl *L, limba_ltype t)
{
    return ti(L, t)->kind == LIMBA_LTK_INT &&
           (ti(L, t)->flags & LIMBA_TF_MODULAR);
}

static bool is_float(const lxl *L, limba_ltype t)
{
    return ti(L, t)->kind == LIMBA_LTK_FLOAT;
}

static limba_id bin(lxl *L, unsigned op, limba_id type, limba_id a, limba_id b)
{
    uint32_t o[2] = {a, b};
    return lxl_emit(L, op, type, 0, 0, 0, o, 2);
}

static limba_id un(lxl *L, unsigned op, limba_id type, limba_id a)
{
    return lxl_emit(L, op, type, 0, 0, 0, &a, 1);
}

static limba_id cmp(lxl *L, bool fl, unsigned cc, limba_id a, limba_id b)
{
    uint32_t o[2] = {a, b};
    return lxl_emit(L, fl ? LIMBA_OP_FCMP : LIMBA_OP_ICMP, LIMBA_T_I1, cc, 0, 0,
                    o, 2);
}

static limba_id fconst(lxl *L, limba_id type, double d)
{
    int64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return lxl_emit(L, LIMBA_OP_FCONST, type, 0, bits, 0, NULL, 0);
}

static limba_id load(lxl *L, limba_ltype t, limba_id addr)
{
    return un(L, LIMBA_OP_LOAD, lxl_type(L, t), addr);
}

static void store(lxl *L, limba_id value, limba_id addr)
{
    uint32_t o[2] = {value, addr};
    lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, o, 2);
}

/* base + index * scale + disp */
static limba_id addr(lxl *L, limba_id base, limba_id index, int64_t scale,
                     int64_t disp)
{
    uint32_t o[2] = {base, index};
    return lxl_emit(L, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, scale, disp, o, 2);
}

/* an integer value widened to i64, by the sign of its type */
limba_id lxl_to_i64(lxl *L, limba_id v, limba_ltype t)
{
    limba_id it = lxl_type(L, t);
    if (it == LIMBA_T_I64)
        return v;
    return un(L, lxl_signed(L, t) ? LIMBA_OP_SEXT : LIMBA_OP_ZEXT, LIMBA_T_I64,
              v);
}

/* ---- constants ---- */

static limba_id constant(lxl *L, uint32_t node)
{
    limba_lxs *S = L->S;
    limba_ltype t = ntype(L, node);
    const limba_lxs_value *v = &S->v[S->val[node]];
    limba_id it = lxl_type(L, t);
    if (v->kind == LXV_STR) {
        size_t n;
        const char *s = limba_strtab_get(S->lx->strings, v->str, &n);
        return lxl_emit(L, LIMBA_OP_SCONST, LIMBA_T_STR, 0,
                        limba_str_intern(L->m, s, n), 0, NULL, 0);
    }
    if (v->kind == LXV_NIL)
        return lxl_emit(L, LIMBA_OP_NULLV, LIMBA_T_PTR, 0, 0, 0, NULL, 0);
    if (is_float(L, t)) {
        double d;
        float f;
        if (ti(L, t)->bits == 32) {
            limba_rat_to_f32(&v->num, &f);
            d = f;
        } else {
            limba_rat_to_f64(&v->num, &d);
        }
        return fconst(L, it, d);
    }
    __int128 i = 0;
    lxs_value_to_int(S, S->val[node], &i);
    return lxl_iconst(L, it, (int64_t)(uint64_t)(unsigned __int128)i);
}

/* ---- checks ---- */

/* lo <= v <= hi for a value of type t (signed or not by t) */
static void check_range(lxl *L, limba_id v, limba_ltype t, __int128 lo,
                        __int128 hi, int64_t code)
{
    limba_id it = lxl_type(L, t);
    bool sg = lxl_signed(L, t);
    const limba_typeinfo *x = ti(L, lxs_base(L->S, t));
    limba_id ok = LIMBA_NONE;
    if (lo > x->lo) {
        ok = cmp(L, false, sg ? LIMBA_CC_SGE : LIMBA_CC_UGE, v,
                 lxl_iconst(L, it, (int64_t)lo));
    }
    if (hi < x->hi) {
        limba_id c = cmp(L, false, sg ? LIMBA_CC_SLE : LIMBA_CC_ULE, v,
                         lxl_iconst(L, it, (int64_t)(uint64_t)hi));
        ok = ok == LIMBA_NONE ? c : bin(L, LIMBA_OP_AND, LIMBA_T_I1, ok, c);
    }
    if (ok != LIMBA_NONE)
        lxl_check(L, ok, code);
}

limba_id lxl_coerce(lxl *L, limba_id v, limba_ltype from, limba_ltype to)
{
    if (!from || !to || from == to || !lxl_scalar(L, to))
        return v;
    const limba_typeinfo *x = ti(L, to);
    if ((x->flags & LIMBA_TF_RANGE) && limba_types_is_discrete(&L->S->ts, to)) {
        const limba_typeinfo *y = ti(L, from);
        if (!(y->lo >= x->lo && y->hi <= x->hi))
            check_range(L, v, to, x->lo, x->hi, LXR_RANGE);
    }
    return v;
}

/* ---- conversions T(x) ---- */

limba_id lxl_conv(lxl *L, limba_id v, limba_ltype from, limba_ltype to)
{
    limba_id fi = lxl_type(L, from), toi = lxl_type(L, to);
    const limba_typeinfo *tx = ti(L, to);
    bool ff = is_float(L, from), tf = is_float(L, to);
    if (ff && tf) {
        if (fi == toi)
            return v;
        return un(L, toi == LIMBA_T_F64 ? LIMBA_OP_FPEXT : LIMBA_OP_FPTRUNC,
                  toi, v);
    }
    if (!ff && tf)
        return un(L, lxl_signed(L, from) ? LIMBA_OP_SITOFP : LIMBA_OP_UITOFP,
                  toi, v);
    if (ff && !tf) {
        /* round half away from zero (Ada), then it must fit */
        v = un(L, LIMBA_OP_FROUNDA, fi, v);
        double lo = (double)tx->lo, hi_excl = (double)(tx->hi) + 1.0;
        if (!(tx->flags & LIMBA_TF_RANGE)) {
            unsigned bits = ti(L, lxs_base(L->S, to))->bits;
            bool sg = tx->flags & LIMBA_TF_SIGNED;
            lo = sg ? -ldexp(1.0, (int)bits - 1) : 0.0;
            hi_excl = ldexp(1.0, sg ? (int)bits - 1 : (int)bits);
        }
        limba_id a = cmp(L, true, LIMBA_CC_OGE, v, fconst(L, fi, lo));
        limba_id b = cmp(L, true, LIMBA_CC_OLT, v, fconst(L, fi, hi_excl));
        lxl_check(L, bin(L, LIMBA_OP_AND, LIMBA_T_I1, a, b), LXR_CONVERSION);
        return un(L,
                  lxl_signed(L, to) || tx->lo < 0 ? LIMBA_OP_FPTOSI
                                                  : LIMBA_OP_FPTOUI,
                  toi, v);
    }
    if (tx->kind != LIMBA_LTK_INT || ti(L, from)->kind != LIMBA_LTK_INT) {
        /* same kind, compatible: a range may narrow */
        return lxl_coerce(L, v, from, to);
    }
    /* integer to integer: a Bits type takes the low bits; any other must
       hold the value */
    if (!(is_modular(L, to) && !(tx->flags & LIMBA_TF_RANGE))) {
        limba_id w = lxl_to_i64(L, v, from);
        bool fsg = lxl_signed(L, from);
        __int128 lo = tx->lo, hi = tx->hi;
        limba_id ok = LIMBA_NONE;
        if (fsg) {
            if (lo > INT64_MIN)
                ok = cmp(L, false, LIMBA_CC_SGE, w,
                         lxl_iconst(L, LIMBA_T_I64, (int64_t)lo));
            if (hi < INT64_MAX) {
                limba_id c = cmp(L, false, LIMBA_CC_SLE, w,
                                 lxl_iconst(L, LIMBA_T_I64, (int64_t)hi));
                ok = ok == LIMBA_NONE ? c
                                      : bin(L, LIMBA_OP_AND, LIMBA_T_I1, ok, c);
            }
        } else {
            if (lo > 0)
                ok = cmp(L, false, LIMBA_CC_UGE, w,
                         lxl_iconst(L, LIMBA_T_I64, (int64_t)lo));
            if (hi < (__int128)UINT64_MAX) {
                limba_id c =
                    cmp(L, false, LIMBA_CC_ULE, w,
                        lxl_iconst(L, LIMBA_T_I64, (int64_t)(uint64_t)hi));
                ok = ok == LIMBA_NONE ? c
                                      : bin(L, LIMBA_OP_AND, LIMBA_T_I1, ok, c);
            }
        }
        if (ok != LIMBA_NONE)
            lxl_check(L, ok, LXR_CONVERSION);
    }
    unsigned fb = limba_type_bits(fi), tb = limba_type_bits(toi);
    if (fb == tb)
        return v;
    if (fb > tb)
        return un(L, LIMBA_OP_TRUNC, toi, v);
    return un(L, lxl_signed(L, from) ? LIMBA_OP_SEXT : LIMBA_OP_ZEXT, toi, v);
}

/* ---- arithmetic ---- */

static limba_id arith(lxl *L, uint32_t node, unsigned op, limba_ltype t,
                      limba_id a, limba_id b)
{
    limba_id it = lxl_type(L, t);
    (void)node;
    if (is_float(L, t)) {
        unsigned fop = op == LX_PLUS    ? LIMBA_OP_FADD
                       : op == LX_MINUS ? LIMBA_OP_FSUB
                       : op == LX_STAR  ? LIMBA_OP_FMUL
                                        : LIMBA_OP_FDIV;
        return bin(L, fop, it, a, b);
    }
    bool sg = lxl_signed(L, t), mod = is_modular(L, t);
    unsigned bits = limba_type_bits(it);
    switch (op) {
    case LX_PLUS:
    case LX_MINUS:
    case LX_STAR:
        if (mod)
            return bin(L,
                       op == LX_PLUS    ? LIMBA_OP_ADD
                       : op == LX_MINUS ? LIMBA_OP_SUB
                                        : LIMBA_OP_MUL,
                       it, a, b);
        if (sg)
            return bin(L,
                       op == LX_PLUS    ? LIMBA_OP_ADDOV
                       : op == LX_MINUS ? LIMBA_OP_SUBOV
                                        : LIMBA_OP_MULOV,
                       it, a, b);
        /* a number without a sign: its own checks */
        if (op == LX_PLUS) {
            limba_id r = bin(L, LIMBA_OP_ADD, it, a, b);
            lxl_check(L, cmp(L, false, LIMBA_CC_UGE, r, a), LXR_OVERFLOW);
            return r;
        }
        if (op == LX_MINUS) {
            lxl_check(L, cmp(L, false, LIMBA_CC_UGE, a, b), LXR_OVERFLOW);
            return bin(L, LIMBA_OP_SUB, it, a, b);
        }
        if (bits < 64) {
            limba_id wa = un(L, LIMBA_OP_ZEXT, LIMBA_T_I64, a);
            limba_id wb = un(L, LIMBA_OP_ZEXT, LIMBA_T_I64, b);
            limba_id p = bin(L, LIMBA_OP_MUL, LIMBA_T_I64, wa, wb);
            lxl_check(
                L,
                cmp(L, false, LIMBA_CC_ULE, p,
                    lxl_iconst(L, LIMBA_T_I64, (int64_t)((1ull << bits) - 1))),
                LXR_OVERFLOW);
            return un(L, LIMBA_OP_TRUNC, it, p);
        } else {
            limba_id r = bin(L, LIMBA_OP_MUL, it, a, b);
            limba_id zero = lxl_iconst(L, it, 0), one = lxl_iconst(L, it, 1);
            limba_id bz = cmp(L, false, LIMBA_CC_EQ, b, zero);
            uint32_t so[3] = {bz, one, b};
            limba_id d = lxl_emit(L, LIMBA_OP_SELECT, it, 0, 0, 0, so, 3);
            limba_id q = bin(L, LIMBA_OP_UDIV, it, r, d);
            limba_id ok = bin(L, LIMBA_OP_OR, LIMBA_T_I1, bz,
                              cmp(L, false, LIMBA_CC_EQ, q, a));
            lxl_check(L, ok, LXR_OVERFLOW);
            return r;
        }
    case LX_KW_DIV:
    case LX_KW_MOD:
    case LX_KW_REM: {
        limba_id zero = lxl_iconst(L, it, 0);
        lxl_check(L, cmp(L, false, LIMBA_CC_NE, b, zero), LXR_DIVZERO);
        if (!sg)
            return bin(L, op == LX_KW_DIV ? LIMBA_OP_UDIV : LIMBA_OP_UREM, it,
                       a, b);
        int64_t min = bits == 64 ? INT64_MIN : -(int64_t)(1ull << (bits - 1));
        limba_id amin = cmp(L, false, LIMBA_CC_EQ, a, lxl_iconst(L, it, min));
        limba_id bm1 = cmp(L, false, LIMBA_CC_EQ, b, lxl_iconst(L, it, -1));
        limba_id bad = bin(L, LIMBA_OP_AND, LIMBA_T_I1, amin, bm1);
        if (op == LX_KW_DIV) {
            lxl_check(L, un(L, LIMBA_OP_NOT, LIMBA_T_I1, bad), LXR_OVERFLOW);
            return bin(L, LIMBA_OP_SDIV, it, a, b);
        }
        /* MIN rem -1 is 0: divide by 1 instead, the IR would trap */
        uint32_t so[3] = {bad, lxl_iconst(L, it, 1), b};
        limba_id d = lxl_emit(L, LIMBA_OP_SELECT, it, 0, 0, 0, so, 3);
        limba_id r = bin(L, LIMBA_OP_SREM, it, a, d);
        if (op == LX_KW_REM)
            return r;
        /* mod takes the sign of the divisor */
        limba_id nz = cmp(L, false, LIMBA_CC_NE, r, zero);
        limba_id diff =
            cmp(L, false, LIMBA_CC_SLT, bin(L, LIMBA_OP_XOR, it, r, b), zero);
        limba_id fix = bin(L, LIMBA_OP_AND, LIMBA_T_I1, nz, diff);
        uint32_t so2[3] = {fix, bin(L, LIMBA_OP_ADD, it, r, b), r};
        return lxl_emit(L, LIMBA_OP_SELECT, it, 0, 0, 0, so2, 3);
    }
    }
    return a;
}

static limba_id power(lxl *L, uint32_t node, limba_ltype t, limba_id a,
                      limba_id e, limba_ltype et)
{
    limba_id it = lxl_type(L, t);
    if (is_float(L, t)) {
        limba_id fa =
            it == LIMBA_T_F64 ? a : un(L, LIMBA_OP_FPEXT, LIMBA_T_F64, a);
        limba_id fe =
            un(L, lxl_signed(L, et) ? LIMBA_OP_SITOFP : LIMBA_OP_UITOFP,
               LIMBA_T_F64, e);
        uint32_t args[2] = {fa, fe};
        limba_id r = lxl_rt(L, LIMBA_RT_MATH_POW, LIMBA_T_F64, args, 2);
        return it == LIMBA_T_F64 ? r : un(L, LIMBA_OP_FPTRUNC, it, r);
    }
    if (!lxl_signed(L, t)) {
        lxs_error(L->S, LXE_UNSUPPORTED, node,
                  "** on a type without a sign is not translated yet");
        return a;
    }
    uint32_t args[2] = {lxl_to_i64(L, a, t), lxl_to_i64(L, e, et)};
    limba_id r = lxl_rt(L, LIMBA_RT_INT_POW, LIMBA_T_I64, args, 2);
    if (it == LIMBA_T_I64)
        return r;
    return lxl_conv(L, r, L->S->ty_int[3], lxs_base(L->S, t));
}

static limba_id shift(lxl *L, unsigned op, limba_ltype t, limba_id a,
                      limba_id n, limba_ltype nt)
{
    limba_id it = lxl_type(L, t);
    unsigned bits = limba_type_bits(it);
    limba_id w = lxl_to_i64(L, n, nt);
    lxl_check(L,
              cmp(L, false, LIMBA_CC_ULT, w,
                  lxl_iconst(L, LIMBA_T_I64, (int64_t)bits)),
              LXR_SHIFT);
    limba_id c = bits == 64 ? w : un(L, LIMBA_OP_TRUNC, it, w);
    return bin(L, op == LX_KW_SHL ? LIMBA_OP_SHL : LIMBA_OP_LSHR, it, a, c);
}

static unsigned int_cc(unsigned op, bool sg)
{
    switch (op) {
    case LX_EQ:
        return LIMBA_CC_EQ;
    case LX_NE:
        return LIMBA_CC_NE;
    case LX_LT:
        return sg ? LIMBA_CC_SLT : LIMBA_CC_ULT;
    case LX_LE:
        return sg ? LIMBA_CC_SLE : LIMBA_CC_ULE;
    case LX_GT:
        return sg ? LIMBA_CC_SGT : LIMBA_CC_UGT;
    default:
        return sg ? LIMBA_CC_SGE : LIMBA_CC_UGE;
    }
}

static unsigned float_cc(unsigned op)
{
    switch (op) {
    case LX_EQ:
        return LIMBA_CC_OEQ;
    case LX_NE:
        return LIMBA_CC_UNE;
    case LX_LT:
        return LIMBA_CC_OLT;
    case LX_LE:
        return LIMBA_CC_OLE;
    case LX_GT:
        return LIMBA_CC_OGT;
    default:
        return LIMBA_CC_OGE;
    }
}

static limba_id compare(lxl *L, unsigned op, limba_ltype t, limba_id a,
                        limba_id b)
{
    const limba_typeinfo *x = ti(L, t);
    if (x->kind == LIMBA_LTK_FLOAT)
        return cmp(L, true, float_cc(op), a, b);
    if (x->kind == LIMBA_LTK_STRING) {
        uint32_t args[2] = {a, b};
        limba_id c = lxl_rt(L, LIMBA_RT_STR_CMP, LIMBA_T_I32, args, 2);
        return cmp(L, false, int_cc(op, true), c,
                   lxl_iconst(L, LIMBA_T_I32, 0));
    }
    return cmp(L, false, int_cc(op, lxl_signed(L, t)), a, b);
}

/* a string from a String or a Char */
static limba_id as_string(lxl *L, limba_id v, limba_ltype t)
{
    if (ti(L, t)->kind == LIMBA_LTK_STRING)
        return v;
    return lxl_rt(L, LIMBA_RT_STR_FROM_CHAR, LIMBA_T_STR, &v, 1);
}

/* a Boolean made of jumps, as a value */
static limba_id bool_value(lxl *L, uint32_t node)
{
    uint32_t var = limba_ssa_var(L->ssa, LIMBA_T_I1);
    limba_id t = limba_ssa_block(L->ssa), f = limba_ssa_block(L->ssa),
             join = limba_ssa_block(L->ssa);
    lxl_branch(L, node, t, f);
    limba_ssa_seal(L->ssa, t);
    limba_ssa_seal(L->ssa, f);
    L->cur = t;
    limba_ssa_def(L->ssa, var, t, lxl_iconst(L, LIMBA_T_I1, 1));
    limba_ssa_br(L->ssa, t, join);
    L->cur = f;
    limba_ssa_def(L->ssa, var, f, lxl_iconst(L, LIMBA_T_I1, 0));
    limba_ssa_br(L->ssa, f, join);
    limba_ssa_seal(L->ssa, join);
    L->cur = join;
    return limba_ssa_use(L->ssa, var, join, UINT32_MAX);
}

static bool is_logic(const lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    if (x->kind == LXN_BINARY && (x->op == LX_KW_AND || x->op == LX_KW_OR))
        return ti(L, ntype(L, node))->kind == LIMBA_LTK_BOOL;
    if (x->kind == LXN_UNARY && x->op == LX_KW_NOT)
        return ti(L, ntype(L, node))->kind == LIMBA_LTK_BOOL;
    return false;
}

void lxl_branch(lxl *L, uint32_t node, limba_id t, limba_id f)
{
    limba_lx_node *x = nd(L, node);
    if (x->kind == LXN_BINARY && is_logic(L, node)) {
        limba_id mid = limba_ssa_block(L->ssa);
        if (x->op == LX_KW_AND)
            lxl_branch(L, x->a, mid, f);
        else
            lxl_branch(L, x->a, t, mid);
        limba_ssa_seal(L->ssa, mid);
        L->cur = mid;
        lxl_branch(L, nd(L, node)->b, t, f);
        return;
    }
    if (x->kind == LXN_UNARY && is_logic(L, node)) {
        lxl_branch(L, x->a, f, t);
        return;
    }
    limba_id c = lxl_value(L, node);
    limba_ssa_cbr(L->ssa, L->cur, c, t, f);
}

/* ---- designators ---- */

limba_id lxl_var_addr(lxl *L, limba_sym s)
{
    const lxl_store *st = &L->store[s];
    if (st->kind == LXL_GLOBAL)
        return lxl_emit(L, LIMBA_OP_GADDR, LIMBA_T_PTR, 0, st->global, 0, NULL,
                        0);
    return st->addr;
}

static limba_id nil_checked(lxl *L, limba_id p)
{
    limba_id null = lxl_emit(L, LIMBA_OP_NULLV, LIMBA_T_PTR, 0, 0, 0, NULL, 0);
    lxl_check(L, cmp(L, false, LIMBA_CC_NE, p, null), LXR_NIL);
    return p;
}

/* the address of what a pointer or an aggregate designator names */
static limba_id base_addr(lxl *L, uint32_t base)
{
    limba_ltype bt = ntype(L, base);
    if (ti(L, bt)->kind == LIMBA_LTK_POINTER)
        return nil_checked(L, lxl_value(L, base));
    return lxl_addr(L, base);
}

/* the storage of an array: its address, and for an open or computed one
   the length or the bounds */
void lxl_array_parts(lxl *L, uint32_t base, limba_id *p, limba_id *len,
                     limba_id *lo, limba_id *hi)
{
    *len = *lo = *hi = LIMBA_NONE;
    limba_lx_node *x = nd(L, base);
    limba_ltype bt = ntype(L, base);
    if (x->kind == LXN_REF && L->S->sym[base]) {
        const lxl_store *st = &L->store[L->S->sym[base]];
        if (st->kind == LXL_OPEN) {
            *p = st->addr;
            *len = st->len;
            return;
        }
        if (st->kind == LXL_DYN) {
            *p = st->addr;
            *lo = st->lo;
            *hi = st->hi;
            return;
        }
    }
    *p = ti(L, bt)->kind == LIMBA_LTK_POINTER ? base_addr(L, base)
                                              : lxl_addr(L, base);
}

static limba_id index_addr(lxl *L, uint32_t node)
{
    limba_lx_node *x = nd(L, node);
    uint32_t base = x->a, idx = x->b;
    limba_ltype bt = ntype(L, base);
    if (ti(L, bt)->kind == LIMBA_LTK_POINTER)
        bt = ti(L, bt)->elem;
    const limba_typeinfo *at = ti(L, bt);
    limba_id p, len, lo, hi;
    lxl_array_parts(L, base, &p, &len, &lo, &hi);
    limba_id i = lxl_value(L, idx);
    limba_ltype it = ntype(L, idx);
    limba_id i64 = lxl_to_i64(L, i, it);
    uint64_t esize = ti(L, at->elem)->size;
    if (at->kind == LIMBA_LTK_OPEN) {
        lxl_check(L, cmp(L, false, LIMBA_CC_ULT, i64, len), LXR_INDEX);
        return addr(L, p, i64, (int64_t)esize, 0);
    }
    const limba_typeinfo *ix = ti(L, at->index);
    bool sg = lxl_signed(L, at->index);
    limba_id off;
    if (at->flags & LIMBA_TF_DYNAMIC) {
        limba_id lo64 = lxl_to_i64(L, lo, at->index),
                 hi64 = lxl_to_i64(L, hi, at->index);
        limba_id a = cmp(L, false, sg ? LIMBA_CC_SGE : LIMBA_CC_UGE, i64, lo64);
        limba_id b = cmp(L, false, sg ? LIMBA_CC_SLE : LIMBA_CC_ULE, i64, hi64);
        lxl_check(L, bin(L, LIMBA_OP_AND, LIMBA_T_I1, a, b), LXR_INDEX);
        off = bin(L, LIMBA_OP_SUB, LIMBA_T_I64, i64, lo64);
    } else {
        check_range(L, i, it, ix->lo, ix->hi, LXR_INDEX);
        off = bin(L, LIMBA_OP_SUB, LIMBA_T_I64, i64,
                  lxl_iconst(L, LIMBA_T_I64, (int64_t)ix->lo));
    }
    return addr(L, p, off, (int64_t)esize, 0);
}

limba_id lxl_addr(lxl *L, uint32_t node)
{
    limba_lx_node *x = nd(L, node);
    switch (x->kind) {
    case LXN_REF: {
        limba_sym s = L->S->sym[node];
        return lxl_var_addr(L, s);
    }
    case LXN_SEL: {
        uint32_t base = x->a, fname = x->b;
        limba_ltype bt = ntype(L, base);
        if (ti(L, bt)->kind == LIMBA_LTK_POINTER)
            bt = ti(L, bt)->elem;
        const limba_typeinfo *r = ti(L, bt);
        uint64_t off = 0;
        for (uint32_t i = 0; i < r->count; i++)
            if (L->S->ts.field[r->first + i].name == fname)
                off = L->S->ts.field[r->first + i].offset;
        limba_id b = base_addr(L, base);
        return addr(L, b, lxl_iconst(L, LIMBA_T_I64, 0), 0, (int64_t)off);
    }
    case LXN_INDEX:
        return index_addr(L, node);
    case LXN_DEREF:
        return nil_checked(L, lxl_value(L, x->a));
    case LXN_CALL: {
        /* a function result in memory: not in Luxia 0 */
        limba_id r;
        lxl_call(L, node, &r);
        return r;
    }
    }
    return lxl_emit(L, LIMBA_OP_NULLV, LIMBA_T_PTR, 0, 0, 0, NULL, 0);
}

/* ---- values ---- */

static limba_id binary(lxl *L, uint32_t node)
{
    limba_lx_node *x = nd(L, node);
    unsigned op = x->op;
    uint32_t l = x->a, r = x->b;
    limba_ltype t = ntype(L, node), lt = ntype(L, l), rt = ntype(L, r);
    if (is_logic(L, node))
        return bool_value(L, node);
    limba_id a = lxl_value(L, l), b = lxl_value(L, nd(L, node)->b);
    switch (op) {
    case LX_EQ:
    case LX_NE:
    case LX_LT:
    case LX_LE:
    case LX_GT:
    case LX_GE:
        return compare(L, op, lt, a, b);
    case LX_AMP: {
        uint32_t args[2] = {as_string(L, a, lt), as_string(L, b, rt)};
        return lxl_rt(L, LIMBA_RT_STR_CONCAT, LIMBA_T_STR, args, 2);
    }
    case LX_POWER:
        return power(L, node, t, a, b, rt);
    case LX_KW_SHL:
    case LX_KW_SHR:
        return shift(L, op, t, a, b, rt);
    case LX_KW_AND:
    case LX_KW_OR:
    case LX_KW_XOR:
        return bin(L,
                   op == LX_KW_AND  ? LIMBA_OP_AND
                   : op == LX_KW_OR ? LIMBA_OP_OR
                                    : LIMBA_OP_XOR,
                   lxl_type(L, t), a, b);
    }
    return arith(L, node, op, t, a, b);
}

static limba_id unary(lxl *L, uint32_t node)
{
    limba_lx_node *x = nd(L, node);
    unsigned op = x->op;
    limba_ltype t = ntype(L, node);
    if (is_logic(L, node))
        return bool_value(L, node);
    limba_id v = lxl_value(L, x->a), it = lxl_type(L, t);
    switch (op) {
    case LX_PLUS:
        return v;
    case LX_MINUS:
        if (is_float(L, t))
            return un(L, LIMBA_OP_FNEG, it, v);
        if (is_modular(L, t))
            return un(L, LIMBA_OP_NEG, it, v);
        return bin(L, LIMBA_OP_SUBOV, it, lxl_iconst(L, it, 0), v);
    case LX_KW_NOT:
        return un(L, LIMBA_OP_NOT, it, v);
    case LX_KW_ABS: {
        limba_id neg, lt0;
        if (is_float(L, t)) {
            neg = un(L, LIMBA_OP_FNEG, it, v);
            lt0 = cmp(L, true, LIMBA_CC_OLT, v, fconst(L, it, 0.0));
        } else {
            neg = bin(L, LIMBA_OP_SUBOV, it, lxl_iconst(L, it, 0), v);
            lt0 = cmp(L, false, LIMBA_CC_SLT, v, lxl_iconst(L, it, 0));
        }
        uint32_t so[3] = {lt0, neg, v};
        return lxl_emit(L, LIMBA_OP_SELECT, it, 0, 0, 0, so, 3);
    }
    }
    return v;
}

static limba_id membership(lxl *L, uint32_t node)
{
    limba_lx_node *x = nd(L, node);
    uint32_t vn = x->a, lon = x->b, hin = x->c;
    limba_ltype t = ntype(L, vn);
    limba_id v = lxl_value(L, vn), a, b;
    bool sg = lxl_signed(L, t);
    if (!hin) {
        /* x in T */
        limba_ltype rt = L->S->st.sym[L->S->sym[lon]].type;
        const limba_typeinfo *r = ti(L, rt);
        limba_id it = lxl_type(L, t);
        a = cmp(L, false, sg ? LIMBA_CC_SGE : LIMBA_CC_UGE, v,
                lxl_iconst(L, it, (int64_t)r->lo));
        b = cmp(L, false, sg ? LIMBA_CC_SLE : LIMBA_CC_ULE, v,
                lxl_iconst(L, it, (int64_t)(uint64_t)r->hi));
    } else {
        a = compare(L, LX_GE, t, v, lxl_value(L, lon));
        b = compare(L, LX_LE, t, v, lxl_value(L, hin));
    }
    return bin(L, LIMBA_OP_AND, LIMBA_T_I1, a, b);
}

limba_id lxl_value(lxl *L, uint32_t node)
{
    limba_lxs *S = L->S;
    limba_ltype t = ntype(L, node);
    if (S->val[node] && t && lxl_scalar(L, t))
        return constant(L, node);
    limba_lx_node *x = nd(L, node);
    switch (x->kind) {
    case LXN_REF: {
        limba_sym s = S->sym[node];
        const lxl_store *st = &L->store[s];
        if (st->kind == LXL_SSA)
            return limba_ssa_use(L->ssa, st->var, L->cur, node);
        return load(L, t, lxl_var_addr(L, s));
    }
    case LXN_SEL:
    case LXN_INDEX:
    case LXN_DEREF: {
        limba_ltype bt = ntype(L, x->a);
        if (x->kind == LXN_INDEX && ti(L, bt)->kind == LIMBA_LTK_STRING) {
            /* s[i]: a byte, from 1 to length(s) */
            limba_id s = lxl_value(L, x->a);
            limba_id i = lxl_to_i64(L, lxl_value(L, nd(L, node)->b),
                                    ntype(L, nd(L, node)->b));
            limba_id len = lxl_rt(L, LIMBA_RT_STR_LEN, LIMBA_T_I64, &s, 1);
            limba_id a =
                cmp(L, false, LIMBA_CC_SGE, i, lxl_iconst(L, LIMBA_T_I64, 1));
            limba_id b = cmp(L, false, LIMBA_CC_SLE, i, len);
            lxl_check(L, bin(L, LIMBA_OP_AND, LIMBA_T_I1, a, b), LXR_INDEX);
            limba_id p = lxl_rt(L, LIMBA_RT_STR_PTR, LIMBA_T_PTR, &s, 1);
            return load(L, t, addr(L, p, i, 1, -1));
        }
        return load(L, t, lxl_addr(L, node));
    }
    case LXN_BINARY:
        return binary(L, node);
    case LXN_UNARY:
        return unary(L, node);
    case LXN_IN:
        return membership(L, node);
    case LXN_CALL: {
        limba_id r = LIMBA_NONE;
        lxl_call(L, node, &r);
        return r;
    }
    case LXN_NEW: {
        limba_ltype target = ti(L, t)->elem;
        uint64_t size = ti(L, target)->size;
        limba_id n = lxl_iconst(L, LIMBA_T_I64, (int64_t)(size ? size : 1));
        limba_id p = lxl_rt(L, LIMBA_RT_MEM_ALLOC, LIMBA_T_PTR, &n, 1);
        uint32_t o[3] = {p, lxl_iconst(L, LIMBA_T_I8, 0), n};
        lxl_emit(L, LIMBA_OP_MEMSET, LIMBA_T_VOID, 0, 0, 0, o, 3);
        return p;
    }
    }
    return lxl_iconst(L, lxl_type(L, t), 0);
}

void lxl_assign(lxl *L, uint32_t target, uint32_t value)
{
    limba_lxs *S = L->S;
    limba_ltype t = ntype(L, target);
    if (!lxl_scalar(L, t)) {
        uint64_t size = ti(L, t)->size;
        limba_id src = lxl_addr(L, value);
        limba_id dst = lxl_addr(L, target);
        uint32_t o[3] = {dst, src, lxl_iconst(L, LIMBA_T_I64, (int64_t)size)};
        lxl_emit(L, LIMBA_OP_MEMCPY, LIMBA_T_VOID, 0, 0, 0, o, 3);
        return;
    }
    limba_id v = lxl_coerce(L, lxl_value(L, value), ntype(L, value), t);
    limba_lx_node *x = nd(L, target);
    if (x->kind == LXN_REF) {
        limba_sym s = S->sym[target];
        const lxl_store *st = &L->store[s];
        if (st->kind == LXL_SSA) {
            limba_ssa_def(L->ssa, st->var, L->cur, v);
            return;
        }
    }
    store(L, v, lxl_addr(L, target));
}
