/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fold.c - the "fold" pass: operations on constants become constants, and
 * integer identities (x + 0, x * 1, x - x, ...) become the value they are.
 *
 * Folding must give exactly what the machine would: nothing that may trap
 * is folded when it would trap (a division by zero, INT_MIN / -1, an
 * overflowing add.ov), shifts by the width or more are left alone, floats
 * are computed in their own precision; float to integer conversions
 * saturate, as the IR defines them. No float identities: x + 0.0 is not x when
 * x is -0.0, and a NaN has no identity at all.
 *
 * Blocks are visited in dominator-tree order, so an operand is folded
 * before its uses; the pipeline repeats for what is left.
 */
#define _GNU_SOURCE /* roundeven */
#include "pass.h"

#include "common/xalloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    limba_func *f;
    limba_edit e;
} fctx;

static const limba_inst *def(fctx *c, uint32_t x)
{
    return &c->f->insts[limba_edit_resolve(&c->e, x)];
}

static bool ival(fctx *c, uint32_t x, int64_t *v)
{
    const limba_inst *d = def(c, x);
    if (d->op != LIMBA_OP_ICONST)
        return false;
    *v = limba_int_norm(d->imm, d->type);
    return true;
}

static bool fval(fctx *c, uint32_t x, double *v)
{
    const limba_inst *d = def(c, x);
    if (d->op != LIMBA_OP_FCONST)
        return false;
    memcpy(v, &d->imm, sizeof(*v));
    return true;
}

static uint64_t mask(unsigned bits)
{
    return bits >= 64 ? ~0ull : (1ull << bits) - 1;
}

/* the value as unsigned in its width */
static uint64_t uval(int64_t v, limba_id t)
{
    return (uint64_t)v & mask(limba_type_bits(t));
}

/* the smallest signed value of t */
static int64_t smin(limba_id t)
{
    unsigned bits = limba_type_bits(t);
    return bits >= 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));
}

static bool fits_signed(__int128 v, limba_id t)
{
    unsigned bits = limba_type_bits(t);
    __int128 lo = -((__int128)1 << (bits - 1));
    __int128 hi = ((__int128)1 << (bits - 1)) - 1;
    return v >= lo && v <= hi;
}

static int64_t fbits(double x, limba_id t)
{
    if (t == LIMBA_T_F32)
        x = (float)x;
    int64_t b;
    memcpy(&b, &x, sizeof(b));
    return b;
}

/* ---- both operands constant ---- */

static bool fold_int_bin(const limba_inst *in, int64_t a, int64_t b, int64_t *r)
{
    limba_id t = in->type;
    unsigned bits = limba_type_bits(t);
    uint64_t ua = uval(a, t), ub = uval(b, t);
    switch (in->op) {
    case LIMBA_OP_ADD:
        *r = (int64_t)(ua + ub);
        return true;
    case LIMBA_OP_SUB:
        *r = (int64_t)(ua - ub);
        return true;
    case LIMBA_OP_MUL:
        *r = (int64_t)(ua * ub);
        return true;
    case LIMBA_OP_AND:
        *r = a & b;
        return true;
    case LIMBA_OP_OR:
        *r = a | b;
        return true;
    case LIMBA_OP_XOR:
        *r = a ^ b;
        return true;
    case LIMBA_OP_SHL:
        if (ub >= bits)
            return false;
        *r = (int64_t)(ua << ub);
        return true;
    case LIMBA_OP_LSHR:
        if (ub >= bits)
            return false;
        *r = (int64_t)(ua >> ub);
        return true;
    case LIMBA_OP_ASHR:
        if (ub >= bits)
            return false;
        *r = a >> ub; /* a is sign-extended: arithmetic in gcc and clang */
        return true;
    case LIMBA_OP_UDIV:
    case LIMBA_OP_UREM:
        if (ub == 0)
            return false;
        *r = (int64_t)(in->op == LIMBA_OP_UDIV ? ua / ub : ua % ub);
        return true;
    case LIMBA_OP_SDIV:
    case LIMBA_OP_SREM:
        if (b == 0 || (a == smin(t) && b == -1))
            return false;
        *r = in->op == LIMBA_OP_SDIV ? a / b : a % b;
        return true;
    case LIMBA_OP_ADDOV:
    case LIMBA_OP_SUBOV:
    case LIMBA_OP_MULOV: {
        __int128 x = a, y = b;
        __int128 v = in->op == LIMBA_OP_ADDOV   ? x + y
                     : in->op == LIMBA_OP_SUBOV ? x - y
                                                : x * y;
        if (!fits_signed(v, t))
            return false; /* it traps: leave it to run time */
        *r = (int64_t)v;
        return true;
    }
    }
    return false;
}

static bool fold_icmp(unsigned cc, limba_id t, int64_t a, int64_t b, bool *r)
{
    uint64_t ua = uval(a, t), ub = uval(b, t);
    switch (cc) {
    case LIMBA_CC_EQ:
        *r = a == b;
        return true;
    case LIMBA_CC_NE:
        *r = a != b;
        return true;
    case LIMBA_CC_SLT:
        *r = a < b;
        return true;
    case LIMBA_CC_SLE:
        *r = a <= b;
        return true;
    case LIMBA_CC_SGT:
        *r = a > b;
        return true;
    case LIMBA_CC_SGE:
        *r = a >= b;
        return true;
    case LIMBA_CC_ULT:
        *r = ua < ub;
        return true;
    case LIMBA_CC_ULE:
        *r = ua <= ub;
        return true;
    case LIMBA_CC_UGT:
        *r = ua > ub;
        return true;
    case LIMBA_CC_UGE:
        *r = ua >= ub;
        return true;
    }
    return false;
}

static bool fold_fcmp(unsigned cc, double a, double b, bool *r)
{
    bool uno = isnan(a) || isnan(b);
    switch (cc) {
    case LIMBA_CC_OEQ:
        *r = !uno && a == b;
        return true;
    case LIMBA_CC_ONE:
        *r = !uno && a != b;
        return true;
    case LIMBA_CC_OLT:
        *r = !uno && a < b;
        return true;
    case LIMBA_CC_OLE:
        *r = !uno && a <= b;
        return true;
    case LIMBA_CC_OGT:
        *r = !uno && a > b;
        return true;
    case LIMBA_CC_OGE:
        *r = !uno && a >= b;
        return true;
    case LIMBA_CC_ORD:
        *r = !uno;
        return true;
    case LIMBA_CC_UNO:
        *r = uno;
        return true;
    case LIMBA_CC_UEQ:
        *r = uno || a == b;
        return true;
    case LIMBA_CC_UNE:
        *r = uno || a != b;
        return true;
    case LIMBA_CC_FULT:
        *r = uno || a < b;
        return true;
    case LIMBA_CC_FULE:
        *r = uno || a <= b;
        return true;
    case LIMBA_CC_FUGT:
        *r = uno || a > b;
        return true;
    case LIMBA_CC_FUGE:
        *r = uno || a >= b;
        return true;
    }
    return false;
}

/* a float operation in the precision of t */
static bool fold_float(const limba_inst *in, const double *v, int64_t *r)
{
    limba_id t = in->type;
    double x;
    if (t == LIMBA_T_F32) {
        float a = (float)v[0], b = (float)v[1], c = (float)v[2];
        switch (in->op) {
        case LIMBA_OP_FADD:
            x = a + b;
            break;
        case LIMBA_OP_FSUB:
            x = a - b;
            break;
        case LIMBA_OP_FMUL:
            x = a * b;
            break;
        case LIMBA_OP_FDIV:
            x = a / b;
            break;
        case LIMBA_OP_FNEG:
            x = -a;
            break;
        case LIMBA_OP_FROUND:
            x = roundevenf(a);
            break;
        case LIMBA_OP_FROUNDA:
            x = roundf(a); /* C's round: halfway away from zero */
            break;
        case LIMBA_OP_FMA:
            x = fmaf(a, b, c);
            break;
        default:
            return false;
        }
    } else {
        switch (in->op) {
        case LIMBA_OP_FADD:
            x = v[0] + v[1];
            break;
        case LIMBA_OP_FSUB:
            x = v[0] - v[1];
            break;
        case LIMBA_OP_FMUL:
            x = v[0] * v[1];
            break;
        case LIMBA_OP_FDIV:
            x = v[0] / v[1];
            break;
        case LIMBA_OP_FNEG:
            x = -v[0];
            break;
        case LIMBA_OP_FROUND:
            x = roundeven(v[0]);
            break;
        case LIMBA_OP_FROUNDA:
            x = round(v[0]);
            break;
        case LIMBA_OP_FMA:
            x = fma(v[0], v[1], v[2]);
            break;
        default:
            return false;
        }
    }
    /* A NaN made by arithmetic has no fixed sign or payload (IEEE 754
       6.3): which one comes out depends on the machine and the order of
       the operands. Leave it to run time, so that optimising never changes
       the bits a program sees. Negation only flips a bit: it may fold. */
    if (isnan(x) && in->op != LIMBA_OP_FNEG)
        return false;
    *r = fbits(x, t);
    return true;
}

/* a conversion of a constant; false when it stays for run time */
static bool fold_conv(fctx *c, limba_inst *in, uint32_t x)
{
    const limba_inst *d = def(c, x);
    limba_id from = d->type, to = in->type;
    int64_t a;
    double fa;
    if (ival(c, x, &a)) {
        uint64_t ua = uval(a, from);
        switch (in->op) {
        case LIMBA_OP_TRUNC:
            limba_inst_set_iconst(in, a);
            return true;
        case LIMBA_OP_ZEXT:
            limba_inst_set_iconst(in, (int64_t)ua);
            return true;
        case LIMBA_OP_SEXT:
            limba_inst_set_iconst(in, from == LIMBA_T_I1 ? -a : a);
            return true;
        case LIMBA_OP_SITOFP:
            limba_inst_set_fconst(
                in, fbits(to == LIMBA_T_F32 ? (float)a : (double)a, to));
            return true;
        case LIMBA_OP_UITOFP:
            limba_inst_set_fconst(
                in, fbits(to == LIMBA_T_F32 ? (float)ua : (double)ua, to));
            return true;
        case LIMBA_OP_BITCAST:
            if (from == LIMBA_T_I64) {
                limba_inst_set_fconst(in, a);
            } else { /* i32 to f32 */
                uint32_t u = (uint32_t)ua;
                float fl;
                memcpy(&fl, &u, sizeof(fl));
                limba_inst_set_fconst(in, fbits(fl, LIMBA_T_F32));
            }
            return true;
        }
        return false;
    }
    if (!fval(c, x, &fa))
        return false;
    /* converting a NaN between widths may change its bits: run time */
    if (isnan(fa) && (in->op == LIMBA_OP_FPTRUNC || in->op == LIMBA_OP_FPEXT))
        return false;
    switch (in->op) {
    case LIMBA_OP_FPTRUNC:
        limba_inst_set_fconst(in, fbits((float)fa, LIMBA_T_F32));
        return true;
    case LIMBA_OP_FPEXT:
        limba_inst_set_fconst(in, fbits(fa, LIMBA_T_F64));
        return true;
    case LIMBA_OP_FPTOSI:
    case LIMBA_OP_FPTOUI: {
        /* saturating, as the IR defines them: NaN gives 0, out of range
           the nearest end of the type */
        unsigned bits = limba_type_bits(to);
        double tr = trunc(fa);
        uint64_t v;
        if (in->op == LIMBA_OP_FPTOSI) {
            double lo = -ldexp(1.0, (int)bits - 1);
            if (isnan(fa))
                v = 0;
            else if (tr < lo)
                v = (uint64_t)smin(to);
            else if (tr >= -lo)
                v = (uint64_t)smin(to) - 1; /* the maximum */
            else
                v = (uint64_t)(int64_t)tr;
        } else {
            if (isnan(fa) || tr <= 0)
                v = 0;
            else if (tr >= ldexp(1.0, (int)bits))
                v = mask(bits);
            else
                v = (uint64_t)tr;
        }
        limba_inst_set_iconst(in, (int64_t)v);
        return true;
    }
    case LIMBA_OP_BITCAST:
        if (from == LIMBA_T_F64) {
            int64_t b;
            memcpy(&b, &fa, sizeof(b));
            limba_inst_set_iconst(in, b);
        } else {
            float fl = (float)fa;
            uint32_t u;
            memcpy(&u, &fl, sizeof(u));
            limba_inst_set_iconst(in, (int32_t)u);
        }
        return true;
    }
    return false;
}

/* ---- identities with one constant or twice the same operand ---- */

static bool identity(fctx *c, limba_inst *in, uint32_t id, uint32_t x,
                     uint32_t y)
{
    limba_id t = in->type;
    int64_t k;
    bool kx = ival(c, x, &k), ky = false;
    int64_t ones = limba_int_norm(-1, t);
    if (!kx)
        ky = ival(c, y, &k);
    bool same = limba_edit_resolve(&c->e, x) == limba_edit_resolve(&c->e, y);
    uint32_t other = kx ? y : x; /* the operand that is not constant */
    bool comm = limba_ops[in->op].flags & LIMBA_OPF_COMMUTATIVE;

    switch (in->op) {
    case LIMBA_OP_ADD:
    case LIMBA_OP_OR:
    case LIMBA_OP_XOR:
        if ((ky || (kx && comm)) && k == 0) {
            limba_edit_replace(&c->e, id, other);
            return true;
        }
        break;
    case LIMBA_OP_SUB:
    case LIMBA_OP_SHL:
    case LIMBA_OP_LSHR:
    case LIMBA_OP_ASHR:
        if (ky && k == 0) {
            limba_edit_replace(&c->e, id, x);
            return true;
        }
        break;
    case LIMBA_OP_MUL:
        if ((kx || ky) && k == 1) {
            limba_edit_replace(&c->e, id, other);
            return true;
        }
        if ((kx || ky) && k == 0) {
            limba_inst_set_iconst(in, 0);
            return true;
        }
        break;
    case LIMBA_OP_AND:
        if ((kx || ky) && k == 0) {
            limba_inst_set_iconst(in, 0);
            return true;
        }
        if ((kx || ky) && k == ones) {
            limba_edit_replace(&c->e, id, other);
            return true;
        }
        break;
    case LIMBA_OP_UDIV:
    case LIMBA_OP_SDIV:
        if (ky && k == 1) {
            limba_edit_replace(&c->e, id, x);
            return true;
        }
        break;
    }
    if (kx && (in->op == LIMBA_OP_OR) && k == ones) {
        limba_inst_set_iconst(in, ones);
        return true;
    }
    if (ky && (in->op == LIMBA_OP_OR) && k == ones) {
        limba_inst_set_iconst(in, ones);
        return true;
    }
    if (!same)
        return false;
    switch (in->op) {
    case LIMBA_OP_SUB:
    case LIMBA_OP_XOR:
        limba_inst_set_iconst(in, 0);
        return true;
    case LIMBA_OP_AND:
    case LIMBA_OP_OR:
        limba_edit_replace(&c->e, id, x);
        return true;
    }
    return false;
}

static bool fold_inst(fctx *c, uint32_t id)
{
    limba_inst *in = &c->f->insts[id];
    const uint32_t *o = c->f->operands + in->first;
    const limba_op_info *op = &limba_ops[in->op];
    int64_t a, b, r;
    double v[3] = {0, 0, 0};

    switch (op->format) {
    case LIMBA_F_UN:
        if (in->op == LIMBA_OP_FNEG || in->op == LIMBA_OP_FROUND ||
            in->op == LIMBA_OP_FROUNDA) {
            if (!fval(c, o[0], &v[0]) || !fold_float(in, v, &r))
                return false;
            limba_inst_set_fconst(in, r);
            return true;
        }
        if (!ival(c, o[0], &a))
            return false;
        limba_inst_set_iconst(
            in, in->op == LIMBA_OP_NEG ? (int64_t)(0 - uval(a, in->type)) : ~a);
        return true;
    case LIMBA_F_BIN:
        if (limba_type_is_float(in->type)) {
            if (!fval(c, o[0], &v[0]) || !fval(c, o[1], &v[1]) ||
                !fold_float(in, v, &r))
                return false;
            limba_inst_set_fconst(in, r);
            return true;
        }
        if (ival(c, o[0], &a) && ival(c, o[1], &b)) {
            if (!fold_int_bin(in, a, b, &r))
                return false;
            limba_inst_set_iconst(in, r);
            return true;
        }
        return identity(c, in, id, o[0], o[1]);
    case LIMBA_F_CMP: {
        bool res;
        const limba_inst *d = def(c, o[0]);
        if (in->op == LIMBA_OP_ICMP) {
            if (ival(c, o[0], &a) && ival(c, o[1], &b) &&
                fold_icmp(in->cc, d->type, a, b, &res)) {
                limba_inst_set_iconst(in, res);
                return true;
            }
            /* x == x, x <= x ...: integers have no NaN */
            if (limba_edit_resolve(&c->e, o[0]) ==
                limba_edit_resolve(&c->e, o[1])) {
                bool eq = in->cc == LIMBA_CC_EQ || in->cc == LIMBA_CC_SLE ||
                          in->cc == LIMBA_CC_SGE || in->cc == LIMBA_CC_ULE ||
                          in->cc == LIMBA_CC_UGE;
                limba_inst_set_iconst(in, eq);
                return true;
            }
            return false;
        }
        if (fval(c, o[0], &v[0]) && fval(c, o[1], &v[1]) &&
            fold_fcmp(in->cc, v[0], v[1], &res)) {
            limba_inst_set_iconst(in, res);
            return true;
        }
        return false;
    }
    case LIMBA_F_TERN:
        if (in->op == LIMBA_OP_SELECT) {
            if (ival(c, o[0], &a)) {
                limba_edit_replace(&c->e, id, a ? o[1] : o[2]);
                return true;
            }
            if (limba_edit_resolve(&c->e, o[1]) ==
                limba_edit_resolve(&c->e, o[2])) {
                limba_edit_replace(&c->e, id, o[1]);
                return true;
            }
            return false;
        }
        if (!fval(c, o[0], &v[0]) || !fval(c, o[1], &v[1]) ||
            !fval(c, o[2], &v[2]) || !fold_float(in, v, &r))
            return false;
        limba_inst_set_fconst(in, r);
        return true;
    case LIMBA_F_CONV:
        return fold_conv(c, in, o[0]);
    }
    return false;
}

uint32_t limba_pass_fold(limba_pass_ctx *x, limba_func *f)
{
    fctx c = {.f = f};
    const limba_cfg *cfg = limba_pass_cfg_of(x, f); /* no branch changes */
    /* blocks in dominator-tree pre-order, definitions before uses: the
       pre numbers are distinct and below 2 * nblocks, so a table sorts */
    uint32_t *order = limba_xmalloc(((size_t)f->nblocks + 1) * sizeof(*order));
    uint32_t *at = limba_xmalloc((2 * (size_t)f->nblocks + 2) * sizeof(*at));
    uint32_t n = 0;
    for (uint32_t k = 0; k < 2 * f->nblocks + 2; k++)
        at[k] = LIMBA_NONE;
    for (uint32_t b = 0; b < f->nblocks; b++)
        if (limba_cfg_reachable(cfg, b))
            at[cfg->pre[b]] = b;
    for (uint32_t k = 0; k < 2 * f->nblocks + 2; k++)
        if (at[k] != LIMBA_NONE)
            order[n++] = at[k];
    free(at);

    limba_edit_begin(&c.e, f);
    uint32_t changes = 0;
    for (uint32_t i = 0; i < n; i++) {
        const limba_block *bl = &f->blocks[order[i]];
        for (uint32_t k = bl->nparams; k < bl->ninsts; k++)
            changes += fold_inst(&c, bl->insts[k]);
    }
    if (changes)
        limba_edit_end(&c.e);
    else
        limba_edit_cancel(&c.e);
    free(order);
    return changes;
}
