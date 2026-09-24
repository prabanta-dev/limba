/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_call.c - calls in the IR (see lower.h): conversions T(x), the
 * routines of the program, the routines of the language.
 *
 * A parameter without a mode passes a scalar by value and a record or an
 * array by address (it is read-only: the callee cannot tell); var and out
 * pass the address; an open array passes its address and its length.
 */
#include "lower.h"

#include "common/xalloc.h"

#include <stdlib.h>

static const limba_typeinfo *ti(const lxl *L, limba_ltype t)
{
    return &L->S->ts.t[t];
}

static uint32_t arg(const lxl *L, uint32_t node, uint32_t i)
{
    return limba_lx_list_at(L->S->t, L->S->t->node[node].b, i);
}

static uint32_t nargs(const lxl *L, uint32_t node)
{
    return L->S->t->node[L->S->t->node[node].b].b;
}

static limba_id un(lxl *L, unsigned op, limba_id type, limba_id a)
{
    return lxl_emit(L, op, type, 0, 0, 0, &a, 1);
}

static limba_id bin(lxl *L, unsigned op, limba_id type, limba_id a, limba_id b)
{
    uint32_t o[2] = {a, b};
    return lxl_emit(L, op, type, 0, 0, 0, o, 2);
}

static limba_id icmp(lxl *L, unsigned cc, limba_id a, limba_id b)
{
    uint32_t o[2] = {a, b};
    return lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, cc, 0, 0, o, 2);
}

/* an array argument for an open parameter: address and length */
static void open_arg(lxl *L, uint32_t a, limba_id *p, limba_id *len)
{
    limba_ltype t = L->S->type[a];
    if (ti(L, t)->kind == LIMBA_LTK_POINTER)
        t = ti(L, t)->elem;
    limba_id lo, hi;
    lxl_array_parts(L, a, p, len, &lo, &hi);
    if (*len != LIMBA_NONE)
        return;
    const limba_typeinfo *at = ti(L, t);
    if (at->flags & LIMBA_TF_DYNAMIC) {
        limba_id l = lxl_to_i64(L, lo, at->index),
                 h = lxl_to_i64(L, hi, at->index);
        *len = bin(L, LIMBA_OP_ADD, LIMBA_T_I64,
                   bin(L, LIMBA_OP_SUB, LIMBA_T_I64, h, l),
                   lxl_iconst(L, LIMBA_T_I64, 1));
        return;
    }
    const limba_typeinfo *ix = ti(L, at->index);
    *len = lxl_iconst(L, LIMBA_T_I64, (int64_t)(ix->hi - ix->lo + 1));
}

static void routine(lxl *L, uint32_t node, limba_sym s, limba_id *result)
{
    limba_lxs *S = L->S;
    const limba_typeinfo *sig = ti(L, S->st.sym[s].type);
    uint32_t first = sig->first, count = sig->count;
    uint32_t *ops = NULL, n = 0, cap = 0;
    for (uint32_t i = 0; i < count && i < nargs(L, node); i++) {
        limba_param p = S->ts.param[first + i];
        uint32_t a = arg(L, node, i);
        limba_id v[2];
        uint32_t k = 1;
        if (ti(L, p.type)->kind == LIMBA_LTK_OPEN) {
            open_arg(L, a, &v[0], &v[1]);
            k = 2;
        } else if (p.mode != LXS_IN || !lxl_scalar(L, p.type)) {
            v[0] = lxl_addr(L, a);
        } else {
            v[0] = lxl_coerce(L, lxl_value(L, a), S->type[a], p.type);
        }
        for (uint32_t j = 0; j < k; j++) {
            LIMBA_GROW(ops, n, cap);
            ops[n++] = v[j];
        }
    }
    limba_id rt =
        sig->elem == S->ts.void_ ? LIMBA_T_VOID : lxl_type(L, sig->elem);
    limba_id r = lxl_emit(L, LIMBA_OP_CALL, rt, 0, L->func_of[s], 0, ops, n);
    free(ops);
    *result = rt == LIMBA_T_VOID ? LIMBA_NONE : r;
}

/* a value as a string, for write with a width and for str */
static limba_id to_string(lxl *L, limba_id v, limba_ltype t, limba_id dec)
{
    const limba_typeinfo *x = ti(L, lxs_base(L->S, t));
    switch (x->kind) {
    case LIMBA_LTK_INT:
        v = lxl_to_i64(L, v, t);
        return lxl_rt(
            L, lxl_signed(L, t) ? LIMBA_RT_STR_FROM_I64 : LIMBA_RT_STR_FROM_U64,
            LIMBA_T_STR, &v, 1);
    case LIMBA_LTK_FLOAT: {
        if (x->bits == 32)
            v = un(L, LIMBA_OP_FPEXT, LIMBA_T_F64, v);
        if (dec != LIMBA_NONE) {
            uint32_t a[2] = {v, dec};
            return lxl_rt(L, LIMBA_RT_STR_FROM_F64_FIXED, LIMBA_T_STR, a, 2);
        }
        return lxl_rt(L, LIMBA_RT_STR_FROM_F64, LIMBA_T_STR, &v, 1);
    }
    case LIMBA_LTK_BOOL:
        return lxl_rt(L, LIMBA_RT_STR_FROM_BOOL, LIMBA_T_STR, &v, 1);
    case LIMBA_LTK_CHAR:
        return lxl_rt(L, LIMBA_RT_STR_FROM_CHAR, LIMBA_T_STR, &v, 1);
    }
    return v; /* a String */
}

static void lx_print(lxl *L, limba_id v, limba_ltype t)
{
    const limba_typeinfo *x = ti(L, lxs_base(L->S, t));
    unsigned rt = LIMBA_RT_PRINT_STR;
    switch (x->kind) {
    case LIMBA_LTK_INT:
        v = lxl_to_i64(L, v, t);
        rt = lxl_signed(L, t) ? LIMBA_RT_PRINT_I64 : LIMBA_RT_PRINT_U64;
        break;
    case LIMBA_LTK_FLOAT:
        if (x->bits == 32)
            v = un(L, LIMBA_OP_FPEXT, LIMBA_T_F64, v);
        rt = LIMBA_RT_PRINT_F64;
        break;
    case LIMBA_LTK_BOOL:
        rt = LIMBA_RT_PRINT_BOOL;
        break;
    case LIMBA_LTK_CHAR:
        rt = LIMBA_RT_PRINT_CHAR;
        break;
    }
    lxl_rt(L, rt, LIMBA_T_VOID, &v, 1);
}

static void lx_write(lxl *L, uint32_t node, bool line)
{
    for (uint32_t i = 0; i < nargs(L, node); i++) {
        uint32_t a = arg(L, node, i);
        const limba_lx_node *x = &L->S->t->node[a];
        if (x->kind != LXN_FMT) {
            lx_print(L, lxl_value(L, a), L->S->type[a]);
            continue;
        }
        uint32_t vn = x->a, wn = x->b, dn = x->c;
        limba_ltype t = L->S->type[vn];
        limba_id v = lxl_value(L, vn), w = lxl_value(L, wn);
        limba_id d = dn ? lxl_value(L, dn) : LIMBA_NONE;
        uint32_t args[2] = {to_string(L, v, t, d), w};
        lxl_rt(L, LIMBA_RT_PRINT_STR_W, LIMBA_T_VOID, args, 2);
    }
    if (line)
        lxl_rt(L, LIMBA_RT_PRINT_NL, LIMBA_T_VOID, NULL, 0);
}

/* a real function of the library on Float32 or Float64 */
static limba_id real_fn(lxl *L, unsigned rt, limba_id v, limba_ltype t)
{
    bool f32 = ti(L, lxs_base(L->S, t))->bits == 32;
    if (f32)
        v = un(L, LIMBA_OP_FPEXT, LIMBA_T_F64, v);
    limba_id r = lxl_rt(L, rt, LIMBA_T_F64, &v, 1);
    return f32 ? un(L, LIMBA_OP_FPTRUNC, LIMBA_T_F32, r) : r;
}

/* a temporary of 8 bytes in memory, for the runtime to write */
static limba_id temp(lxl *L)
{
    limba_func *f = limba_ssa_func(L->ssa);
    limba_id slot = limba_slot_add(f, 8, 8);
    return lxl_emit(L, LIMBA_OP_SLOT, LIMBA_T_PTR, 0, slot, 0, NULL, 0);
}

static void builtin(lxl *L, uint32_t node, unsigned id, limba_id *result)
{
    limba_lxs *S = L->S;
    limba_ltype rt = S->type[node];
    uint32_t a0 = nargs(L, node) ? arg(L, node, 0) : 0;
    limba_ltype t0 = a0 ? S->type[a0] : 0;
    limba_id v;
    *result = LIMBA_NONE;
    switch (id) {
    case LXB_WRITE:
    case LXB_WRITELN:
        lx_write(L, node, id == LXB_WRITELN);
        return;
    case LXB_WRITEBYTE:
        v = lxl_value(L, a0);
        lxl_rt(L, LIMBA_RT_PRINT_BYTE, LIMBA_T_VOID, &v, 1);
        return;
    case LXB_READLINE:
        v = lxl_addr(L, a0);
        *result = lxl_rt(L, LIMBA_RT_READ_LINE, LIMBA_T_I1, &v, 1);
        return;
    case LXB_LENGTH:
    case LXB_LOW:
    case LXB_HIGH: {
        const limba_typeinfo *x = ti(L, t0);
        if (x->kind == LIMBA_LTK_STRING) {
            v = lxl_value(L, a0);
            *result = id == LXB_LOW
                          ? lxl_iconst(L, LIMBA_T_I64, 1)
                          : lxl_rt(L, LIMBA_RT_STR_LEN, LIMBA_T_I64, &v, 1);
            return;
        }
        limba_id p, len, lo, hi;
        if (x->kind == LIMBA_LTK_POINTER)
            x = ti(L, x->elem);
        lxl_array_parts(L, a0, &p, &len, &lo, &hi);
        if (x->kind == LIMBA_LTK_OPEN) {
            *result = id == LXB_LENGTH ? len
                      : id == LXB_LOW  ? lxl_iconst(L, LIMBA_T_I64, 0)
                                       : bin(L, LIMBA_OP_SUB, LIMBA_T_I64, len,
                                             lxl_iconst(L, LIMBA_T_I64, 1));
            return;
        }
        /* a computed array (a static one is a constant) */
        if (id == LXB_LOW) {
            *result = lo;
        } else if (id == LXB_HIGH) {
            *result = hi;
        } else {
            limba_id l = lxl_to_i64(L, lo, x->index),
                     h = lxl_to_i64(L, hi, x->index);
            limba_id n = bin(L, LIMBA_OP_ADD, LIMBA_T_I64,
                             bin(L, LIMBA_OP_SUB, LIMBA_T_I64, h, l),
                             lxl_iconst(L, LIMBA_T_I64, 1));
            *result = lxl_conv(L, n, S->ty_int[3], rt);
        }
        return;
    }
    case LXB_COPY: {
        uint32_t a[3] = {lxl_value(L, a0), 0, lxl_value(L, arg(L, node, 2))};
        a[1] = bin(L, LIMBA_OP_SUB, LIMBA_T_I64, lxl_value(L, arg(L, node, 1)),
                   lxl_iconst(L, LIMBA_T_I64, 1));
        *result = lxl_rt(L, LIMBA_RT_STR_MID, LIMBA_T_STR, a, 3);
        return;
    }
    case LXB_CHR: {
        limba_id w = lxl_to_i64(L, lxl_value(L, a0), t0);
        limba_id ok =
            icmp(L, LIMBA_CC_ULE, w, lxl_iconst(L, LIMBA_T_I64, 0x10ffff));
        limba_id sur =
            bin(L, LIMBA_OP_AND, LIMBA_T_I1,
                icmp(L, LIMBA_CC_UGE, w, lxl_iconst(L, LIMBA_T_I64, 0xd800)),
                icmp(L, LIMBA_CC_ULE, w, lxl_iconst(L, LIMBA_T_I64, 0xdfff)));
        ok = bin(L, LIMBA_OP_AND, LIMBA_T_I1, ok,
                 un(L, LIMBA_OP_NOT, LIMBA_T_I1, sur));
        lxl_check(L, ok, LXR_CONVERSION);
        *result = un(L, LIMBA_OP_TRUNC, LIMBA_T_I32, w);
        return;
    }
    case LXB_ORD: {
        v = lxl_value(L, a0);
        limba_id it = lxl_type(L, t0);
        *result = it == LIMBA_T_I32 ? v
                  : limba_type_bits(it) > 32
                      ? un(L, LIMBA_OP_TRUNC, LIMBA_T_I32, v)
                      : un(L, LIMBA_OP_ZEXT, LIMBA_T_I32, v);
        return;
    }
    case LXB_SUCC:
    case LXB_PRED: {
        limba_ltype base = lxs_base(S, t0);
        const limba_typeinfo *x = ti(L, base);
        limba_id w = lxl_to_i64(L, lxl_value(L, a0), t0);
        w = bin(L, id == LXB_SUCC ? LIMBA_OP_ADD : LIMBA_OP_SUB, LIMBA_T_I64, w,
                lxl_iconst(L, LIMBA_T_I64, 1));
        bool sg = lxl_signed(L, base);
        limba_id c = id == LXB_SUCC
                         ? icmp(L, sg ? LIMBA_CC_SLE : LIMBA_CC_ULE, w,
                                lxl_iconst(L, LIMBA_T_I64, (int64_t)x->hi))
                         : icmp(L, sg ? LIMBA_CC_SGE : LIMBA_CC_UGE, w,
                                lxl_iconst(L, LIMBA_T_I64, (int64_t)x->lo));
        lxl_check(L, c, LXR_RANGE);
        limba_id it = lxl_type(L, base);
        *result = it == LIMBA_T_I64 ? w : un(L, LIMBA_OP_TRUNC, it, w);
        return;
    }
    case LXB_STR:
        *result = to_string(L, lxl_value(L, a0), t0, LIMBA_NONE);
        return;
    case LXB_VAL: {
        uint32_t xn = arg(L, node, 1);
        limba_ltype xt = S->type[xn];
        bool real = ti(L, lxs_base(S, xt))->kind == LIMBA_LTK_FLOAT;
        limba_id tmp = temp(L);
        uint32_t a[2] = {lxl_value(L, a0), tmp};
        limba_id ok =
            lxl_rt(L, real ? LIMBA_RT_STR_TO_F64 : LIMBA_RT_STR_TO_I64,
                   LIMBA_T_I1, a, 2);
        /* the variable changes only when the text is a number */
        limba_id yes = limba_ssa_block(L->ssa), done = limba_ssa_block(L->ssa);
        limba_ssa_cbr(L->ssa, L->cur, ok, yes, done);
        limba_ssa_seal(L->ssa, yes);
        L->cur = yes;
        limba_id raw =
            un(L, LIMBA_OP_LOAD, real ? LIMBA_T_F64 : LIMBA_T_I64, tmp);
        limba_id conv = lxl_conv(L, raw, real ? S->ty_f64 : S->ty_int[3], xt);
        uint32_t so[2] = {conv, lxl_addr(L, xn)};
        lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, so, 2);
        limba_ssa_br(L->ssa, L->cur, done);
        limba_ssa_seal(L->ssa, done);
        L->cur = done;
        *result = ok;
        return;
    }
    case LXB_SQRT:
        *result = real_fn(L, LIMBA_RT_MATH_SQRT, lxl_value(L, a0), t0);
        return;
    case LXB_SIN:
        *result = real_fn(L, LIMBA_RT_MATH_SIN, lxl_value(L, a0), t0);
        return;
    case LXB_COS:
        *result = real_fn(L, LIMBA_RT_MATH_COS, lxl_value(L, a0), t0);
        return;
    case LXB_TAN:
        *result = real_fn(L, LIMBA_RT_MATH_TAN, lxl_value(L, a0), t0);
        return;
    case LXB_ARCTAN:
        *result = real_fn(L, LIMBA_RT_MATH_ATAN, lxl_value(L, a0), t0);
        return;
    case LXB_EXP:
        *result = real_fn(L, LIMBA_RT_MATH_EXP, lxl_value(L, a0), t0);
        return;
    case LXB_LN:
        *result = real_fn(L, LIMBA_RT_MATH_LN, lxl_value(L, a0), t0);
        return;
    case LXB_TRUNC:
        *result = real_fn(L, LIMBA_RT_MATH_TRUNC, lxl_value(L, a0), t0);
        return;
    case LXB_FLOOR:
        *result = real_fn(L, LIMBA_RT_MATH_FLOOR, lxl_value(L, a0), t0);
        return;
    case LXB_CEIL:
        *result = real_fn(L, LIMBA_RT_MATH_CEIL, lxl_value(L, a0), t0);
        return;
    case LXB_ROUND:
        v = lxl_value(L, a0);
        *result = un(L, LIMBA_OP_FROUND, lxl_type(L, t0), v);
        return;
    case LXB_DISPOSE:
        v = lxl_value(L, a0);
        lxl_rt(L, LIMBA_RT_MEM_FREE, LIMBA_T_VOID, &v, 1);
        return;
    case LXB_ARGCOUNT:
        *result = lxl_rt(L, LIMBA_RT_ARG_COUNT, LIMBA_T_I32, NULL, 0);
        return;
    case LXB_ARG:
        v = lxl_value(L, a0);
        *result = lxl_rt(L, LIMBA_RT_ARG, LIMBA_T_STR, &v, 1);
        return;
    case LXB_HALT:
        v = lxl_value(L, a0);
        lxl_rt(L, LIMBA_RT_HALT, LIMBA_T_VOID, &v, 1);
        limba_ssa_unreachable(L->ssa, L->cur);
        {
            limba_id after = limba_ssa_block(L->ssa);
            limba_ssa_seal(L->ssa, after);
            L->cur = after;
        }
        return;
    }
}

void lxl_call(lxl *L, uint32_t node, limba_id *result)
{
    limba_lxs *S = L->S;
    uint32_t callee = S->t->node[node].a;
    limba_sym s = S->sym[callee];
    *result = LIMBA_NONE;
    if (!s)
        return;
    const limba_symbol *y = &S->st.sym[s];
    switch (y->kind) {
    case LIMBA_LSYM_TYPE: {
        uint32_t a = arg(L, node, 0);
        *result = lxl_conv(L, lxl_value(L, a), S->type[a], S->type[node]);
        return;
    }
    case LIMBA_LSYM_ROUTINE:
        routine(L, node, s, result);
        return;
    case LIMBA_LSYM_BUILTIN:
        builtin(L, node, y->value, result);
        return;
    }
}
