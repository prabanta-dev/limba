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

/* the length of the bounds l..h, i64 values: 0 when the range is empty */
static limba_id length64(lxl *L, limba_id l, limba_id h)
{
    limba_id n = bin(L, LIMBA_OP_ADD, LIMBA_T_I64,
                     bin(L, LIMBA_OP_SUB, LIMBA_T_I64, h, l),
                     lxl_iconst(L, LIMBA_T_I64, 1));
    limba_id zero = lxl_iconst(L, LIMBA_T_I64, 0);
    uint32_t sel[3] = {icmp(L, LIMBA_CC_SLT, n, zero), zero, n};
    return lxl_emit(L, LIMBA_OP_SELECT, LIMBA_T_I64, 0, 0, 0, sel, 3);
}

/* array node a of move: the address of its elements and its bounds as
   i64 values, whatever it is (static, computed, an open parameter, made
   by new); the dangling pointer it may be reached by stays on the address
   (lxl_via) */
static void span(lxl *L, uint32_t a, uint32_t at, limba_id *base, limba_id *lo,
                 limba_id *hi)
{
    const limba_typeinfo *x = ti(L, L->S->type[a]);
    limba_id p, len, l, h;
    lxl_array_parts(L, a, at, &p, &len, &l, &h);
    int64_t disp = L->heap_disp;
    if (x->kind == LIMBA_LTK_OPEN) {
        *lo = l;
        *hi = h;
    } else if (x->flags & LIMBA_TF_DYNAMIC) {
        *lo = lxl_to_i64(L, l, x->index);
        *hi = lxl_to_i64(L, h, x->index);
    } else {
        const limba_typeinfo *ix = ti(L, x->index);
        *lo = lxl_iconst(L, LIMBA_T_I64, (int64_t)ix->lo);
        *hi = lxl_iconst(L, LIMBA_T_I64, (int64_t)(uint64_t)ix->hi);
    }
    *base = p;
    if (disp) {
        uint32_t o[2] = {p, lxl_iconst(L, LIMBA_T_I64, 0)};
        *base = lxl_emit(L, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, 0, disp, o, 2);
        if (lxl_via(L, p) != LIMBA_NONE)
            lxl_via_set(L, *base, lxl_via(L, p));
    }
}

/* the address of element i (an i64) of an array whose elements start at
   base and whose low bound is lo */
static limba_id element_at(lxl *L, limba_id base, limba_id lo, limba_id i,
                           uint64_t size)
{
    uint32_t o[2] = {base, bin(L, LIMBA_OP_SUB, LIMBA_T_I64, i, lo)};
    limba_id e =
        lxl_emit(L, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, (int64_t)size, 0, o, 2);
    if (lxl_via(L, base) != LIMBA_NONE)
        lxl_via_set(L, e, lxl_via(L, base));
    return e;
}

/* count elements from index i of an array lo..hi are inside it: i in
   lo..hi and count - 1 <= hi - i (no sum that could overflow) */
static limba_id inside(lxl *L, bool sg, limba_id i, limba_id lo, limba_id hi,
                       limba_id count)
{
    limba_id a = icmp(L, sg ? LIMBA_CC_SGE : LIMBA_CC_UGE, i, lo);
    limba_id b = icmp(L, sg ? LIMBA_CC_SLE : LIMBA_CC_ULE, i, hi);
    limba_id c = icmp(
        L, LIMBA_CC_ULE,
        bin(L, LIMBA_OP_SUB, LIMBA_T_I64, count, lxl_iconst(L, LIMBA_T_I64, 1)),
        bin(L, LIMBA_OP_SUB, LIMBA_T_I64, hi, i));
    return bin(L, LIMBA_OP_AND, LIMBA_T_I1,
               bin(L, LIMBA_OP_AND, LIMBA_T_I1, a, b), c);
}

/* the checks of a tract of count elements from index from, in an array
   lo..hi (§ 9.5, as move): count below 0 a range error, then, unless
   count is 0, the tract inside the array (index error) */
static void tract_checks(lxl *L, bool sg, limba_id from, limba_id count,
                         limba_id lo, limba_id hi)
{
    limba_id zero = lxl_iconst(L, LIMBA_T_I64, 0);
    if (sg)
        lxl_check(L, icmp(L, LIMBA_CC_SGE, count, zero), LXR_RANGE);
    lxl_check(L,
              bin(L, LIMBA_OP_OR, LIMBA_T_I1, icmp(L, LIMBA_CC_EQ, count, zero),
                  inside(L, sg, from, lo, hi, count)),
              LXR_INDEX);
}

/* node a of translate, occurrences, readbytes or writebytes, an array of
   Bytes or a String: the address of its first byte and its bounds as i64
   values (a String from 1, its bytes valid while the String has a
   reference: § 11c) */
static void bytes_span(lxl *L, uint32_t a, uint32_t at, limba_id *base,
                       limba_id *lo, limba_id *hi)
{
    if (ti(L, L->S->type[a])->kind != LIMBA_LTK_STRING) {
        span(L, a, at, base, lo, hi);
        return;
    }
    limba_id s = lxl_value(L, a);
    *base = lxl_rt(L, LIMBA_RT_STR_PTR, LIMBA_T_PTR, &s, 1);
    *lo = lxl_iconst(L, LIMBA_T_I64, 1);
    *hi = lxl_rt(L, LIMBA_RT_STR_LEN, LIMBA_T_I64, &s, 1);
}

/* the length of a computed array, whose bounds are values of type it */
static limba_id dyn_length(lxl *L, limba_id lo, limba_id hi, limba_ltype it)
{
    return length64(L, lxl_to_i64(L, lo, it), lxl_to_i64(L, hi, it));
}

/* an i64 value as a value of the integer type t */
static limba_id from_i64(lxl *L, limba_id v, limba_ltype t)
{
    limba_id it = lxl_type(L, t);
    return limba_type_bits(it) < 64 ? un(L, LIMBA_OP_TRUNC, it, v) : v;
}

/* an array argument for an open parameter of type pt, in the call node:
   its address and its bounds as i64 values (§ 4.5) */
static void open_arg(lxl *L, uint32_t node, uint32_t a, limba_ltype pt,
                     limba_id *p, limba_id *lo, limba_id *hi)
{
    limba_ltype t = L->S->type[a];
    if (ti(L, t)->kind == LIMBA_LTK_POINTER)
        t = ti(L, t)->elem;
    limba_id len, l, h;
    lxl_array_parts(L, a, node, p, &len, &l, &h);
    const limba_typeinfo *at = ti(L, t);
    if (at->kind == LIMBA_LTK_OPEN) {
        *lo = l;
        *hi = h;
    } else if (at->flags & LIMBA_TF_DYNAMIC) {
        *lo = lxl_to_i64(L, l, at->index);
        *hi = lxl_to_i64(L, h, at->index);
    } else {
        /* bounds known now, already checked against the parameter */
        const limba_typeinfo *ix = ti(L, at->index);
        *lo = lxl_iconst(L, LIMBA_T_I64, (int64_t)ix->lo);
        *hi = lxl_iconst(L, LIMBA_T_I64, (int64_t)(uint64_t)ix->hi);
        return;
    }
    /* the index of the parameter may be a range: the bounds of a
       non-empty argument belong to it */
    limba_ltype pix = ti(L, pt)->index;
    const limba_typeinfo *pi = ti(L, pix);
    if (!(pi->flags & LIMBA_TF_RANGE))
        return;
    bool sg = lxl_signed(L, pix);
    limba_id empty = icmp(L, sg ? LIMBA_CC_SGT : LIMBA_CC_UGT, *lo, *hi);
    limba_id in_lo = icmp(L, sg ? LIMBA_CC_SGE : LIMBA_CC_UGE, *lo,
                          lxl_iconst(L, LIMBA_T_I64, (int64_t)pi->lo));
    limba_id in_hi =
        icmp(L, sg ? LIMBA_CC_SLE : LIMBA_CC_ULE, *hi,
             lxl_iconst(L, LIMBA_T_I64, (int64_t)(uint64_t)pi->hi));
    lxl_at(L, node);
    lxl_check(L,
              bin(L, LIMBA_OP_OR, LIMBA_T_I1, empty,
                  bin(L, LIMBA_OP_AND, LIMBA_T_I1, in_lo, in_hi)),
              LXR_RANGE);
}

static void routine(lxl *L, uint32_t node, limba_sym s, limba_id *result)
{
    limba_lxs *S = L->S;
    const limba_typeinfo *sig = ti(L, S->st.sym[s].type);
    uint32_t first = sig->first, count = sig->count;
    uint32_t *ops = NULL, n = 0, cap = 0;
    /* a routine of C (§ 8.5): call.ext, an array its address alone, a
       record by value the address of its bytes (IR § 11e) */
    bool ext = lxl_external(L, s);
    bool agg = sig->elem != S->ts.void_ && !lxl_scalar(L, sig->elem);
    limba_id slot = LIMBA_NONE;
    if (agg) {
        /* a record or an array result: a slot of the caller, first */
        slot = lxl_temp(L, sig->elem, node);
        LIMBA_GROW(ops, n, cap);
        ops[n++] = slot;
    }
    /* scalar out arguments reached through a pointer: where each goes
       back after the call, the slot the routine writes, its type */
    limba_id *backs = NULL;
    uint32_t nbacks = 0, capbacks = 0;
    for (uint32_t i = 0; i < count && i < nargs(L, node); i++) {
        limba_param p = S->ts.param[first + i];
        uint32_t a = arg(L, node, i);
        limba_ltype at = S->type[a];
        if (ti(L, at)->kind == LIMBA_LTK_POINTER)
            at = ti(L, at)->elem;
        limba_id v[3];
        uint32_t k = 1;
        bool by_addr = true;
        if (ti(L, p.type)->kind == LIMBA_LTK_OPEN) {
            open_arg(L, node, a, p.type, &v[0], &v[1], &v[2]);
            k = ext ? 1 : 3;
        } else if (p.mode != LXS_IN || !lxl_scalar(L, p.type)) {
            v[0] = lxl_addr(L, a);
        } else {
            by_addr = false;
            v[0] = lxl_value(L, a);
            lxl_at(L, node); /* a range is checked at the call */
            v[0] = lxl_coerce(L, v[0], S->type[a], p.type);
        }
        /* a record or an array reached through a pointer is no argument
           (§ 3.10, sema): only a scalar out one comes here */
        bool through = by_addr && lxl_via(L, v[0]) != LIMBA_NONE;
        if (through && p.mode == LXS_OUT) {
            /* a scalar (a var argument, an aggregate out one, cannot be
               reached through a pointer: § 3.10): copy-out goes back
               through the pointer after the call, checked there */
            limba_id tmp = lxl_temp(L, at, node);
            limba_id back[3] = {v[0], tmp, lxl_type(L, at)};
            for (unsigned j = 0; j < 3; j++) {
                LIMBA_GROW(backs, nbacks, capbacks);
                backs[nbacks++] = back[j];
            }
            v[0] = tmp;
        }
        for (uint32_t j = 0; j < k; j++) {
            LIMBA_GROW(ops, n, cap);
            ops[n++] = v[j];
        }
    }
    lxl_at(L, node);
    limba_id rt =
        sig->elem == S->ts.void_ || agg ? LIMBA_T_VOID : lxl_type(L, sig->elem);
    limba_id r = lxl_emit(L, ext ? LIMBA_OP_CALLEXT : LIMBA_OP_CALL, rt, 0,
                          L->func_of[s], 0, ops, n);
    free(ops);
    for (uint32_t b = 0; b < nbacks; b += 3) {
        limba_id x = un(L, LIMBA_OP_LOAD, backs[b + 2], backs[b + 1]);
        lxl_live(L, backs[b]);
        uint32_t so[2] = {x, backs[b]};
        lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, so, 2);
    }
    free(backs);
    *result = agg ? slot : rt == LIMBA_T_VOID ? LIMBA_NONE : r;
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
        if (dec == LIMBA_NONE && x->bits == 32) /* its own shortest form */
            return lxl_rt(L, LIMBA_RT_STR_FROM_F32, LIMBA_T_STR, &v, 1);
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
    case LIMBA_LTK_BIGINT:
        return lxl_rt(L, LIMBA_RT_STR_FROM_BIG, LIMBA_T_STR, &v, 1);
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
        rt = x->bits == 32 ? LIMBA_RT_PRINT_F32 : LIMBA_RT_PRINT_F64;
        break;
    case LIMBA_LTK_BOOL:
        rt = LIMBA_RT_PRINT_BOOL;
        break;
    case LIMBA_LTK_CHAR:
        rt = LIMBA_RT_PRINT_CHAR;
        break;
    case LIMBA_LTK_BIGINT:
        rt = LIMBA_RT_PRINT_BIG;
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
        /* a width from 0, decimals from 0 to 100, each checked where it
           is written, before the next is computed (§ 9) */
        limba_id v = lxl_value(L, vn), w = lxl_value(L, wn);
        limba_id zero = lxl_iconst(L, LIMBA_T_I32, 0);
        lxl_at(L, wn);
        lxl_check(L, icmp(L, LIMBA_CC_SGE, w, zero), LXR_RANGE);
        limba_id d = LIMBA_NONE;
        if (dn) {
            d = lxl_value(L, dn);
            lxl_at(L, dn);
            lxl_check(L,
                      icmp(L, LIMBA_CC_ULE, d, lxl_iconst(L, LIMBA_T_I32, 100)),
                      LXR_RANGE);
        }
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
        lxl_live(L, v);
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
        lxl_array_parts(L, a0, node, &p, &len, &lo, &hi);
        if (x->kind == LIMBA_LTK_OPEN) {
            /* the bounds of the argument, i64 values */
            *result = id == LXB_LOW ? from_i64(L, lo, rt)
                      : id == LXB_HIGH
                          ? from_i64(L, hi, rt)
                          : lxl_conv(L, length64(L, lo, hi), S->ty_int[3], rt);
            return;
        }
        /* a computed array (a static one is a constant) */
        if (id == LXB_LOW) {
            *result = lo;
        } else if (id == LXB_HIGH) {
            *result = hi;
        } else {
            limba_id n = dyn_length(L, lo, hi, x->index);
            *result = lxl_conv(L, n, S->ty_int[3], rt);
        }
        return;
    }
    case LXB_COPY: {
        /* from left to right; from 1 on and a count from 0, past the end
           cut short (§ 4.5) */
        limba_id s = lxl_value(L, a0);
        limba_id from = lxl_value(L, arg(L, node, 1));
        limba_id n = lxl_value(L, arg(L, node, 2));
        lxl_at(L, node);
        limba_id ok =
            bin(L, LIMBA_OP_AND, LIMBA_T_I1,
                icmp(L, LIMBA_CC_SGE, from, lxl_iconst(L, LIMBA_T_I64, 1)),
                icmp(L, LIMBA_CC_SGE, n, lxl_iconst(L, LIMBA_T_I64, 0)));
        lxl_check(L, ok, LXR_RANGE);
        uint32_t a[3] = {s,
                         bin(L, LIMBA_OP_SUB, LIMBA_T_I64, from,
                             lxl_iconst(L, LIMBA_T_I64, 1)),
                         n};
        *result = lxl_rt(L, LIMBA_RT_STR_MID, LIMBA_T_STR, a, 3);
        return;
    }
    case LXB_CHR: {
        limba_id w = lxl_to_i64(L, lxl_value(L, a0), t0);
        lxl_at(L, node); /* checked at its name */
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
        limba_id v = lxl_to_i64(L, lxl_value(L, a0), t0);
        /* the value before the step: pred of the first and succ of the
           last are errors (the step itself could wrap) */
        bool sg = lxl_signed(L, base);
        limba_id c =
            id == LXB_SUCC
                ? icmp(L, sg ? LIMBA_CC_SLT : LIMBA_CC_ULT, v,
                       lxl_iconst(L, LIMBA_T_I64, (int64_t)(uint64_t)x->hi))
                : icmp(L, sg ? LIMBA_CC_SGT : LIMBA_CC_UGT, v,
                       lxl_iconst(L, LIMBA_T_I64, (int64_t)x->lo));
        lxl_at(L, node);
        lxl_check(L, c, LXR_RANGE);
        limba_id w = bin(L, id == LXB_SUCC ? LIMBA_OP_ADD : LIMBA_OP_SUB,
                         LIMBA_T_I64, v, lxl_iconst(L, LIMBA_T_I64, 1));
        limba_id it = lxl_type(L, base);
        *result = it == LIMBA_T_I64 ? w : un(L, LIMBA_OP_TRUNC, it, w);
        return;
    }
    case LXB_STR:
        *result = to_string(L, lxl_value(L, a0), t0, LIMBA_NONE);
        return;
    case LXB_VAL: {
        /* the runtime reads a number of the type of the variable, its
           range too: a text beyond it is no such number (§ 9) */
        uint32_t xn = arg(L, node, 1);
        limba_ltype xt = S->type[xn];
        const limba_typeinfo *x = ti(L, xt);
        if (x->kind == LIMBA_LTK_BIGINT) {
            /* any integer: through a typed slot, counted */
            limba_id tmpb = lxl_temp(L, xt, node);
            uint32_t ab[2] = {lxl_value(L, a0), tmpb};
            limba_id okb = lxl_rt(L, LIMBA_RT_STR_TO_BIG, LIMBA_T_I1, ab, 2);
            limba_id yes = limba_ssa_block(L->ssa),
                     done = limba_ssa_block(L->ssa);
            limba_ssa_cbr(L->ssa, L->cur, okb, yes, done);
            limba_ssa_seal(L->ssa, yes);
            L->cur = yes;
            uint32_t so[2] = {un(L, LIMBA_OP_LOAD, LIMBA_T_REF, tmpb),
                              lxl_addr(L, xn)};
            lxl_live(L, so[1]);
            lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, so, 2);
            limba_ssa_br(L->ssa, L->cur, done);
            limba_ssa_seal(L->ssa, done);
            L->cur = done;
            *result = okb;
            return;
        }
        bool real = ti(L, lxs_base(S, xt))->kind == LIMBA_LTK_FLOAT;
        limba_id it = lxl_type(L, xt), tmp = temp(L);
        uint32_t a[4] = {lxl_value(L, a0), tmp,
                         lxl_iconst(L, LIMBA_T_I64, (int64_t)x->lo),
                         lxl_iconst(L, LIMBA_T_I64, (int64_t)(uint64_t)x->hi)};
        unsigned rt = real && it == LIMBA_T_F32 ? LIMBA_RT_STR_TO_F32
                      : real                    ? LIMBA_RT_STR_TO_F64
                      : lxl_signed(L, xt)       ? LIMBA_RT_STR_TO_I64
                                                : LIMBA_RT_STR_TO_U64;
        limba_id ok = lxl_rt(L, rt, LIMBA_T_I1, a, real ? 2 : 4);
        /* the variable changes only when the text is a number */
        limba_id yes = limba_ssa_block(L->ssa), done = limba_ssa_block(L->ssa);
        limba_ssa_cbr(L->ssa, L->cur, ok, yes, done);
        limba_ssa_seal(L->ssa, yes);
        L->cur = yes;
        limba_id v = un(L, LIMBA_OP_LOAD, real ? it : LIMBA_T_I64, tmp);
        if (!real && it != LIMBA_T_I64)
            v = un(L, LIMBA_OP_TRUNC, it, v);
        uint32_t so[2] = {v, lxl_addr(L, xn)};
        lxl_live(L, so[1]);
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
    case LXB_MOVE: {
        /* move(src, from, dst, to, count), from the left (§ 9.5): count
           below 0 a range error; otherwise, unless count is 0, both
           ranges inside their arrays (index error), checked before any
           byte moves; the dangling checks at the copy; then the Strings
           counted and one memcpy, which may overlap (memmove) */
        uint32_t src = a0, dst = arg(L, node, 2);
        limba_ltype st = S->type[src];
        limba_ltype el = ti(L, st)->elem;
        limba_ltype ib = lxs_base(S, ti(L, st)->index);
        bool sg = lxl_signed(L, ib);
        limba_id sb, slo, shi, db, dlo, dhi;
        span(L, src, node, &sb, &slo, &shi);
        limba_id from = lxl_to_i64(L, lxl_value(L, arg(L, node, 1)), ib);
        span(L, dst, node, &db, &dlo, &dhi);
        limba_id to = lxl_to_i64(L, lxl_value(L, arg(L, node, 3)), ib);
        limba_id count = lxl_to_i64(L, lxl_value(L, arg(L, node, 4)), ib);
        lxl_at(L, node);
        limba_id zero = lxl_iconst(L, LIMBA_T_I64, 0);
        if (sg)
            lxl_check(L, icmp(L, LIMBA_CC_SGE, count, zero), LXR_RANGE);
        lxl_check(L,
                  bin(L, LIMBA_OP_OR, LIMBA_T_I1,
                      icmp(L, LIMBA_CC_EQ, count, zero),
                      bin(L, LIMBA_OP_AND, LIMBA_T_I1,
                          inside(L, sg, from, slo, shi, count),
                          inside(L, sg, to, dlo, dhi, count))),
                  LXR_INDEX);
        uint64_t esize = ti(L, el)->size;
        limba_id sa = element_at(L, sb, slo, from, esize);
        limba_id da = element_at(L, db, dlo, to, esize);
        lxl_live(L, sa);
        lxl_live(L, da);
        lxl_rc(L, LIMBA_OP_RETAIN, sa, el, count);
        lxl_rc(L, LIMBA_OP_RELEASE, da, el, count);
        uint32_t m[3] = {da, sa,
                         bin(L, LIMBA_OP_MUL, LIMBA_T_I64, count,
                             lxl_iconst(L, LIMBA_T_I64, (int64_t)esize))};
        lxl_emit(L, LIMBA_OP_MEMCPY, LIMBA_T_VOID, 0, 0, 0, m, 3);
        return;
    }
    case LXB_TRANSLATE: {
        /* translate(a, from, count, table), from the left (§ 9.5): the
           checks of move, then the bytes through the table, read whole
           first (mem_translate) */
        limba_ltype ib = lxs_base(S, ti(L, t0)->index);
        limba_id b, lo, hi, tb, tlo, thi;
        span(L, a0, node, &b, &lo, &hi);
        limba_id from = lxl_to_i64(L, lxl_value(L, arg(L, node, 1)), ib);
        limba_id count = lxl_to_i64(L, lxl_value(L, arg(L, node, 2)), ib);
        span(L, arg(L, node, 3), node, &tb, &tlo, &thi);
        lxl_at(L, node);
        tract_checks(L, lxl_signed(L, ib), from, count, lo, hi);
        limba_id pa = element_at(L, b, lo, from, 1);
        lxl_live(L, pa);
        lxl_live(L, tb);
        uint32_t args[3] = {pa, count, tb};
        lxl_rt(L, LIMBA_RT_MEM_TRANSLATE, LIMBA_T_VOID, args, 3);
        return;
    }
    case LXB_REVERSE: {
        /* reverse(a, from, count): a permutation, no String counted */
        limba_ltype ib = lxs_base(S, ti(L, t0)->index);
        limba_id b, lo, hi;
        span(L, a0, node, &b, &lo, &hi);
        limba_id from = lxl_to_i64(L, lxl_value(L, arg(L, node, 1)), ib);
        limba_id count = lxl_to_i64(L, lxl_value(L, arg(L, node, 2)), ib);
        lxl_at(L, node);
        tract_checks(L, lxl_signed(L, ib), from, count, lo, hi);
        uint64_t esize = ti(L, ti(L, t0)->elem)->size;
        limba_id pa = element_at(L, b, lo, from, esize);
        lxl_live(L, pa);
        uint32_t args[3] = {pa, count,
                            lxl_iconst(L, LIMBA_T_I64, (int64_t)esize)};
        lxl_rt(L, LIMBA_RT_MEM_REVERSE, LIMBA_T_VOID, args, 3);
        return;
    }
    case LXB_OCCURRENCES: {
        /* occurrences(a, from, count, pattern): the checks of move, then
           an empty pattern a range error, whatever count is */
        bool str = ti(L, t0)->kind == LIMBA_LTK_STRING;
        limba_ltype ib = str ? S->ty_int[3] : lxs_base(S, ti(L, t0)->index);
        limba_id b, lo, hi, qb, qlo, qhi;
        bytes_span(L, a0, node, &b, &lo, &hi);
        limba_id from = lxl_to_i64(L, lxl_value(L, arg(L, node, 1)), ib);
        limba_id count = lxl_to_i64(L, lxl_value(L, arg(L, node, 2)), ib);
        bytes_span(L, arg(L, node, 3), node, &qb, &qlo, &qhi);
        lxl_at(L, node);
        tract_checks(L, lxl_signed(L, ib), from, count, lo, hi);
        limba_id m = length64(L, qlo, qhi);
        lxl_check(L, icmp(L, LIMBA_CC_SGT, m, lxl_iconst(L, LIMBA_T_I64, 0)),
                  LXR_RANGE);
        limba_id pa = element_at(L, b, lo, from, 1);
        limba_id pq = element_at(L, qb, qlo, qlo, 1);
        lxl_live(L, pa);
        lxl_live(L, pq);
        uint32_t args[4] = {pa, count, pq, m};
        *result = from_i64(
            L, lxl_rt(L, LIMBA_RT_MEM_COUNT, LIMBA_T_I64, args, 4), ib);
        return;
    }
    case LXB_READBYTES:
    case LXB_WRITEBYTES: {
        /* readbytes(a, from, count), writebytes(a, from, count): the
           checks of move, then one read or write of the tract (§ 9.1,
           § 9.2) */
        bool rd = id == LXB_READBYTES;
        bool str = ti(L, t0)->kind == LIMBA_LTK_STRING;
        limba_ltype ib = str ? S->ty_int[3] : lxs_base(S, ti(L, t0)->index);
        limba_id b, lo, hi;
        bytes_span(L, a0, node, &b, &lo, &hi);
        limba_id from = lxl_to_i64(L, lxl_value(L, arg(L, node, 1)), ib);
        limba_id count = lxl_to_i64(L, lxl_value(L, arg(L, node, 2)), ib);
        lxl_at(L, node);
        tract_checks(L, lxl_signed(L, ib), from, count, lo, hi);
        limba_id pa = element_at(L, b, lo, from, 1);
        lxl_live(L, pa);
        uint32_t args[2] = {pa, count};
        if (rd)
            *result = from_i64(
                L, lxl_rt(L, LIMBA_RT_IO_READ, LIMBA_T_I64, args, 2), ib);
        else
            lxl_rt(L, LIMBA_RT_IO_WRITE, LIMBA_T_VOID, args, 2);
        return;
    }
    case LXB_NEWCSTRING:
        v = lxl_value(L, a0);
        lxl_at(L, node); /* a 0 inside: the range error, here */
        *result = lxl_rt(L, LIMBA_RT_CSTR_NEW, LIMBA_T_PTR, &v, 1);
        return;
    case LXB_CVALUE: {
        const limba_typeinfo *x = ti(L, t0);
        if (x->kind == LIMBA_LTK_ARRAY || x->kind == LIMBA_LTK_OPEN) {
            /* a buffer C filled: its first n elements, n within it */
            limba_id p, lo, hi;
            open_arg(L, node, a0, t0, &p, &lo, &hi);
            limba_id len = bin(L, LIMBA_OP_ADD, LIMBA_T_I64,
                               bin(L, LIMBA_OP_SUB, LIMBA_T_I64, hi, lo),
                               lxl_iconst(L, LIMBA_T_I64, 1));
            limba_id n = lxl_value(L, arg(L, node, 1));
            lxl_at(L, node);
            /* 0 <= n <= length, without a sign: an empty array has
               length 0 or less */
            limba_id zero = lxl_iconst(L, LIMBA_T_I64, 0);
            limba_id pos = icmp(L, LIMBA_CC_SGT, len, zero);
            uint32_t so[3] = {pos, len, zero};
            len = lxl_emit(L, LIMBA_OP_SELECT, LIMBA_T_I64, 0, 0, 0, so, 3);
            lxl_check(L, icmp(L, LIMBA_CC_ULE, n, len), LXR_INDEX);
            uint32_t args[2] = {p, n};
            *result = lxl_rt(L, LIMBA_RT_CSTR_VALUE_N, LIMBA_T_STR, args, 2);
            return;
        }
        v = lxl_value(L, a0);
        limba_id n =
            nargs(L, node) == 2 ? lxl_value(L, arg(L, node, 1)) : LIMBA_NONE;
        lxl_at(L, node);
        limba_id null =
            lxl_emit(L, LIMBA_OP_NULLV, LIMBA_T_PTR, 0, 0, 0, NULL, 0);
        lxl_check(L, icmp(L, LIMBA_CC_NE, v, null), LXR_NIL);
        if (n == LIMBA_NONE) {
            *result = lxl_rt(L, LIMBA_RT_CSTR_VALUE, LIMBA_T_STR, &v, 1);
            return;
        }
        lxl_check(L, icmp(L, LIMBA_CC_SGE, n, lxl_iconst(L, LIMBA_T_I64, 0)),
                  LXR_RANGE);
        uint32_t args[2] = {v, n};
        *result = lxl_rt(L, LIMBA_RT_CSTR_VALUE_N, LIMBA_T_STR, args, 2);
        return;
    }
    case LXB_FREECSTRING: {
        limba_id at = lxl_addr(L, a0);
        lxl_at(L, node);
        lxl_live(L, at);
        v = un(L, LIMBA_OP_LOAD, LIMBA_T_PTR, at);
        lxl_rt(L, LIMBA_RT_CSTR_FREE, LIMBA_T_VOID, &v, 1);
        uint32_t so[2] = {
            lxl_emit(L, LIMBA_OP_NULLV, LIMBA_T_PTR, 0, 0, 0, NULL, 0), at};
        lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, so, 2);
        return;
    }
    case LXB_DISPOSE: {
        v = lxl_value(L, a0);
        limba_ltype target = ti(L, t0)->elem;
        /* an array made by new: its elements, from the bounds in its
           block */
        bool heap = ti(L, target)->kind == LIMBA_LTK_OPEN;
        limba_ltype counted = heap ? ti(L, target)->elem : target;
        if (!lxl_holds_str(L, counted)) {
            /* nil: nothing; freed already: invalid dispose, in mem_free */
            lxl_rt(L, LIMBA_RT_MEM_FREE, LIMBA_T_VOID, &v, 1);
            return;
        }
        /* its Strings released first, but only in a live object: a
           second dispose is still the error of mem_free */
        limba_id check = limba_ssa_block(L->ssa), rel = limba_ssa_block(L->ssa),
                 fr = limba_ssa_block(L->ssa), done = limba_ssa_block(L->ssa);
        limba_id null =
            lxl_emit(L, LIMBA_OP_NULLV, LIMBA_T_PTR, 0, 0, 0, NULL, 0);
        limba_ssa_cbr(L->ssa, L->cur, icmp(L, LIMBA_CC_NE, v, null), check,
                      done);
        limba_ssa_seal(L->ssa, check);
        L->cur = check;
        limba_id live = lxl_rt(L, LIMBA_RT_PTR_LIVE, LIMBA_T_I1, &v, 1);
        limba_ssa_cbr(L->ssa, L->cur, live, rel, fr);
        limba_ssa_seal(L->ssa, rel);
        L->cur = rel;
        if (heap) {
            limba_id zero = lxl_iconst(L, LIMBA_T_I64, 0);
            uint32_t a8[2] = {v, zero}, a16[2] = {v, zero};
            limba_id lo = un(L, LIMBA_OP_LOADINV, LIMBA_T_I64, v);
            limba_id hi =
                un(L, LIMBA_OP_LOADINV, LIMBA_T_I64,
                   lxl_emit(L, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, 0, 8, a8, 2));
            limba_id n = length64(L, lo, hi);
            lxl_rc(L, LIMBA_OP_RELEASE,
                   lxl_emit(L, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, 0, 16, a16, 2),
                   counted, n);
        } else {
            lxl_rc(L, LIMBA_OP_RELEASE, v, target,
                   lxl_iconst(L, LIMBA_T_I64, 1));
        }
        limba_ssa_br(L->ssa, L->cur, fr);
        limba_ssa_seal(L->ssa, fr);
        L->cur = fr;
        lxl_rt(L, LIMBA_RT_MEM_FREE, LIMBA_T_VOID, &v, 1);
        limba_ssa_br(L->ssa, L->cur, done);
        limba_ssa_seal(L->ssa, done);
        L->cur = done;
        return;
    }
    case LXB_ARGCOUNT:
        *result = lxl_rt(L, LIMBA_RT_ARG_COUNT, LIMBA_T_I32, NULL, 0);
        return;
    case LXB_ARG: {
        /* from 1 to argcount(), as an index (§ 9) */
        v = lxl_value(L, a0);
        limba_id n = lxl_rt(L, LIMBA_RT_ARG_COUNT, LIMBA_T_I32, NULL, 0);
        limba_id k =
            bin(L, LIMBA_OP_SUB, LIMBA_T_I32, v, lxl_iconst(L, LIMBA_T_I32, 1));
        lxl_at(L, node);
        lxl_check(L, icmp(L, LIMBA_CC_ULT, k, n), LXR_RANGE);
        *result = lxl_rt(L, LIMBA_RT_ARG, LIMBA_T_STR, &v, 1);
        return;
    }
    case LXB_HALT: {
        v = lxl_value(L, a0);
        /* 0 or 2..255 but 141: 1 is the status of the errors at run
           time, 141 that of a closed output */
        limba_id zero = lxl_iconst(L, LIMBA_T_I32, 0);
        limba_id past =
            bin(L, LIMBA_OP_SUB, LIMBA_T_I32, v, lxl_iconst(L, LIMBA_T_I32, 2));
        limba_id ok = bin(
            L, LIMBA_OP_AND, LIMBA_T_I1,
            bin(L, LIMBA_OP_OR, LIMBA_T_I1, icmp(L, LIMBA_CC_EQ, v, zero),
                icmp(L, LIMBA_CC_ULE, past, lxl_iconst(L, LIMBA_T_I32, 253))),
            icmp(L, LIMBA_CC_NE, v, lxl_iconst(L, LIMBA_T_I32, 141)));
        lxl_at(L, node);
        lxl_check(L, ok, LXR_RANGE);
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
        limba_id v = lxl_value(L, a);
        lxl_at(L, node);
        *result = lxl_conv_at(L, v, S->type[a], S->type[node], node);
        return;
    }
    case LIMBA_LSYM_ROUTINE:
        routine(L, node, s, result);
        return;
    case LIMBA_LSYM_BUILTIN:
        lxl_at(L, node);
        builtin(L, node, y->value, result);
        return;
    }
}
