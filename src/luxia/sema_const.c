/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_const.c - the constants of Luxia 0 (see sema.h): computed exactly,
 * as in Ada, and checked against a type only when they take one.
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

uint32_t lxs_value_new(limba_lxs *S, uint8_t kind)
{
    LIMBA_GROW(S->v, S->nv, S->capv);
    limba_lxs_value *v = &S->v[S->nv];
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    limba_rat_init(&v->num);
    return S->nv++;
}

static void big_i128(limba_big *r, __int128 v)
{
    unsigned __int128 m = v < 0 ? -(unsigned __int128)v : (unsigned __int128)v;
    limba_big hi;
    limba_big_init(&hi);
    limba_big_set_u64(&hi, (uint64_t)(m >> 64));
    limba_big_shl(&hi, &hi, 64);
    limba_big_set_u64(r, (uint64_t)m);
    limba_big_add(r, r, &hi);
    limba_big_free(&hi);
    if (v < 0)
        limba_big_neg(r, r);
}

uint32_t lxs_value_int(limba_lxs *S, __int128 v)
{
    uint32_t id = lxs_value_new(S, LXV_NUM);
    big_i128(&S->v[id].num.num, v);
    return id;
}

static uint32_t value_rat(limba_lxs *S, const limba_rat *r)
{
    uint32_t id = lxs_value_new(S, LXV_NUM);
    limba_rat_copy(&S->v[id].num, r);
    return id;
}

bool lxs_value_to_int(const limba_lxs *S, uint32_t v, __int128 *out)
{
    const limba_lxs_value *x = &S->v[v];
    if (x->kind != LXV_NUM || !limba_rat_is_int(&x->num))
        return false;
    const limba_big *b = &x->num.num;
    if (b->n > 4 || (b->n == 4 && (b->w[3] & 0x80000000u)))
        return false;
    unsigned __int128 m = 0;
    for (uint32_t i = b->n; i-- > 0;)
        m = m << 32 | b->w[i];
    *out = b->neg ? -(__int128)m : (__int128)m;
    return true;
}

int lxs_value_cmp(const limba_lxs *S, uint32_t a, uint32_t b)
{
    const limba_lxs_value *x = &S->v[a], *y = &S->v[b];
    if (x->kind == LXV_STR && y->kind == LXV_STR) {
        size_t n, m;
        const char *s = limba_strtab_get(S->lx->strings, x->str, &n);
        const char *t = limba_strtab_get(S->lx->strings, y->str, &m);
        int c = memcmp(s, t, n < m ? n : m);
        return c ? (c < 0 ? -1 : 1) : n < m ? -1 : n > m ? 1 : 0;
    }
    if (x->kind != LXV_NUM || y->kind != LXV_NUM)
        return x->kind == y->kind ? 0 : 1;
    return limba_rat_cmp(&x->num, &y->num);
}

/* the source text of a literal: digits, '_', '.', exponent, prefix */
uint32_t lxs_literal(limba_lxs *S, uint32_t node)
{
    limba_lx_node *x = lxs_node(S, node);
    if (x->kind == LXN_INT && !(x->flags & LXN_F_BIG))
        return lxs_value_int(S, (__int128)S->lx->ints[x->a]);
    limba_where w;
    if (!limba_source_where(S->src, x->loc, &w))
        return 0;
    const char *s = S->src->file[w.file].text + w.off;
    uint32_t id = lxs_value_new(S, LXV_NUM);
    bool ok;
    if (x->kind == LXN_INT) {
        unsigned base = 10;
        size_t start = 0, n;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'o' || s[1] == 'b')) {
            base = s[1] == 'x' ? 16 : s[1] == 'o' ? 8 : 2;
            start = 2;
        }
        for (n = start;
             (s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f') ||
             (s[n] >= 'A' && s[n] <= 'F') || s[n] == '_';
             n++)
            ;
        ok = limba_big_parse(&S->v[id].num.num, s + start, n - start, base);
    } else {
        size_t n = 0;
        while ((s[n] >= '0' && s[n] <= '9') || s[n] == '_' ||
               (s[n] == '.' && s[n + 1] >= '0' && s[n + 1] <= '9'))
            n++;
        if (s[n] == 'e') {
            n++;
            if (s[n] == '+' || s[n] == '-')
                n++;
            while ((s[n] >= '0' && s[n] <= '9') || s[n] == '_')
                n++;
        }
        ok = limba_rat_parse(&S->v[id].num, s, n);
    }
    if (!ok) {
        lxs_error(S, LXE_CONST_OVERFLOW, node, "this number is past %d bits",
                  LIMBA_BIG_MAXBITS);
        return 0;
    }
    return id;
}

static bool is_float(const limba_lxs *S, limba_type t)
{
    return t && (lxs_ty(S, t)->kind == LIMBA_TK_FLOAT || t == S->ts.ureal);
}

/* a - b * floor(a / b) or a - b * trunc(a / b), integers */
static bool int_divmod(limba_rat *q, limba_rat *m, const limba_rat *a,
                       const limba_rat *b, bool floor_div)
{
    limba_big bq, bm;
    limba_big_init(&bq);
    limba_big_init(&bm);
    limba_big_divmod(&bq, &bm, &a->num, &b->num);
    if (floor_div && !limba_big_is_zero(&bm) && bm.neg != b->num.neg) {
        limba_big one;
        limba_big_init(&one);
        limba_big_set_u64(&one, 1);
        limba_big_sub(&bq, &bq, &one);
        limba_big_add(&bm, &bm, &b->num);
        limba_big_free(&one);
    }
    if (q)
        limba_rat_set_big(q, &bq);
    if (m)
        limba_rat_set_big(m, &bm);
    limba_big_free(&bq);
    limba_big_free(&bm);
    return true;
}

static bool bits_of(const limba_lxs *S, uint32_t v, uint64_t *out)
{
    __int128 i;
    if (!lxs_value_to_int(S, v, &i) || i < 0 || i > (__int128)UINT64_MAX)
        return false;
    *out = (uint64_t)i;
    return true;
}

uint32_t lxs_fold_unary(limba_lxs *S, uint32_t node, unsigned op, uint32_t a,
                        limba_type type)
{
    if (!a || S->v[a].kind != LXV_NUM)
        return 0;
    limba_rat r;
    limba_rat_init(&r);
    uint32_t out = 0;
    switch (op) {
    case LX_MINUS:
        limba_rat_neg(&r, &S->v[a].num);
        out = value_rat(S, &r);
        break;
    case LX_PLUS:
        out = a;
        break;
    case LX_KW_ABS:
        limba_rat_copy(&r, &S->v[a].num);
        if (limba_big_sign(&r.num) < 0)
            limba_rat_neg(&r, &r);
        out = value_rat(S, &r);
        break;
    case LX_KW_NOT:
        if (type == S->ts.bool_ || lxs_base(S, type) == S->ts.bool_) {
            __int128 v;
            lxs_value_to_int(S, a, &v);
            out = lxs_value_int(S, !v);
        } else {
            uint64_t v;
            unsigned bits = lxs_ty(S, type)->bits;
            if (bits_of(S, a, &v)) {
                uint64_t mask = bits == 64 ? UINT64_MAX : (1ull << bits) - 1;
                out = lxs_value_int(S, (__int128)(~v & mask));
            }
        }
        break;
    }
    limba_rat_free(&r);
    if (out && type && !lxs_fit(S, node, &out, type))
        return 0;
    return out;
}

uint32_t lxs_fold_binary(limba_lxs *S, uint32_t node, unsigned op, uint32_t a,
                         uint32_t b, limba_type type)
{
    if (!a || !b)
        return 0;
    const limba_lxs_value *x = &S->v[a], *y = &S->v[b];
    int c;
    switch (op) {
    case LX_EQ:
    case LX_NE:
    case LX_LT:
    case LX_LE:
    case LX_GT:
    case LX_GE:
        c = lxs_value_cmp(S, a, b);
        return lxs_value_int(S, op == LX_EQ   ? c == 0
                                : op == LX_NE ? c != 0
                                : op == LX_LT ? c < 0
                                : op == LX_LE ? c <= 0
                                : op == LX_GT ? c > 0
                                              : c >= 0);
    case LX_AMP: {
        /* strings and characters */
        char *buf = NULL;
        size_t n = 0, cap = 0;
        const limba_lxs_value *parts[2] = {x, y};
        for (int i = 0; i < 2; i++) {
            char tmp[4];
            const char *s;
            size_t len;
            if (parts[i]->kind == LXV_STR) {
                s = limba_strtab_get(S->lx->strings, parts[i]->str, &len);
            } else {
                __int128 cp;
                lxs_value_to_int(S, i ? b : a, &cp);
                uint32_t u = (uint32_t)cp;
                len = u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;
                if (len == 1) {
                    tmp[0] = (char)u;
                } else {
                    for (size_t k = len; k-- > 1;) {
                        tmp[k] = (char)(0x80 | (u & 0x3f));
                        u >>= 6;
                    }
                    tmp[0] = (char)((0xf00 >> len) | u);
                }
                s = tmp;
            }
            while (n + len > cap) {
                cap = cap ? 2 * cap : 64;
                buf = limba_xrealloc(buf, cap, 1);
            }
            memcpy(buf + n, s, len);
            n += len;
        }
        uint32_t id = lxs_value_new(S, LXV_STR);
        S->v[id].str = limba_strtab_intern(S->lx->strings, buf ? buf : "", n);
        free(buf);
        return id;
    }
    }
    if (x->kind != LXV_NUM || y->kind != LXV_NUM)
        return 0;
    limba_rat r;
    limba_rat_init(&r);
    bool ok = true;
    uint32_t out = 0;
    x = &S->v[a];
    y = &S->v[b];
    switch (op) {
    case LX_PLUS:
        ok = limba_rat_add(&r, &x->num, &y->num);
        break;
    case LX_MINUS:
        ok = limba_rat_sub(&r, &x->num, &y->num);
        break;
    case LX_STAR:
        ok = limba_rat_mul(&r, &x->num, &y->num);
        break;
    case LX_SLASH:
    case LX_KW_DIV:
    case LX_KW_MOD:
    case LX_KW_REM:
        if (limba_big_is_zero(&y->num.num)) {
            lxs_error(S, LXE_DIV_ZERO, node, "a constant divided by zero");
            limba_rat_free(&r);
            return 0;
        }
        if (op == LX_SLASH)
            ok = limba_rat_div(&r, &x->num, &y->num);
        else if (op == LX_KW_DIV)
            int_divmod(&r, NULL, &x->num, &y->num, false);
        else
            int_divmod(NULL, &r, &x->num, &y->num, op == LX_KW_MOD);
        break;
    case LX_POWER: {
        __int128 e;
        if (!lxs_value_to_int(S, b, &e) || e > INT64_MAX || e < -INT64_MAX) {
            ok = false;
            break;
        }
        if (e < 0 && !is_float(S, type)) {
            lxs_error(S, LXE_CONST_RANGE, node,
                      "a negative exponent on an integer");
            limba_rat_free(&r);
            return 0;
        }
        if (e < 0 && limba_big_is_zero(&x->num.num)) {
            lxs_error(S, LXE_DIV_ZERO, node, "zero to a negative power");
            limba_rat_free(&r);
            return 0;
        }
        ok = limba_rat_pow(&r, &x->num, (int64_t)e);
        break;
    }
    case LX_KW_SHL:
    case LX_KW_SHR:
    case LX_KW_AND:
    case LX_KW_OR:
    case LX_KW_XOR: {
        uint64_t u, v;
        if (!bits_of(S, a, &u) || !bits_of(S, b, &v)) {
            limba_rat_free(&r);
            return 0;
        }
        unsigned bits = type ? lxs_ty(S, type)->bits : 64;
        if (type == S->ts.bool_ || lxs_base(S, type) == S->ts.bool_)
            bits = 1;
        uint64_t mask = bits >= 64 ? UINT64_MAX : (1ull << bits) - 1, res;
        if (op == LX_KW_SHL || op == LX_KW_SHR) {
            if (v >= bits) {
                lxs_error(S, LXE_CONST_RANGE, node,
                          "a shift by %llu of a value of %u bits",
                          (unsigned long long)v, bits);
                limba_rat_free(&r);
                return 0;
            }
            res = op == LX_KW_SHL ? (u << v) & mask : u >> v;
        } else {
            res = op == LX_KW_AND ? u & v : op == LX_KW_OR ? u | v : u ^ v;
        }
        limba_rat_free(&r);
        out = lxs_value_int(S, (__int128)res);
        return type && !lxs_fit(S, node, &out, type) ? 0 : out;
    }
    default:
        limba_rat_free(&r);
        return 0;
    }
    if (!ok) {
        lxs_error(S, LXE_CONST_OVERFLOW, node, "this constant is past %d bits",
                  LIMBA_BIG_MAXBITS);
        limba_rat_free(&r);
        return 0;
    }
    out = value_rat(S, &r);
    limba_rat_free(&r);
    if (type && !lxs_fit(S, node, &out, type))
        return 0;
    return out;
}

bool lxs_fit(limba_lxs *S, uint32_t node, uint32_t *v, limba_type type)
{
    if (!*v || !type || type == S->ts.uint || type == S->ts.ureal)
        return true;
    const limba_typeinfo *t = lxs_ty(S, type);
    char tb[128], vb[64];
    if (t->kind == LIMBA_TK_FLOAT) {
        double d;
        float f;
        bool ok = t->bits == 32 ? limba_rat_to_f32(&S->v[*v].num, &f)
                                : limba_rat_to_f64(&S->v[*v].num, &d);
        if (!ok) {
            lxs_error(S, LXE_CONST_RANGE, node,
                      "the constant is too large "
                      "for %s",
                      lxs_tname(S, type, tb));
            return false;
        }
        return true;
    }
    if (!limba_types_is_discrete(&S->ts, type))
        return true;
    if (S->v[*v].kind != LXV_NUM || !limba_rat_is_int(&S->v[*v].num))
        return true; /* the type mismatch is reported by the caller */
    if (t->flags & LIMBA_TF_MODULAR && !(t->flags & LIMBA_TF_RANGE)) {
        /* wrap modulo 2^bits */
        limba_rat m, q;
        limba_rat_init(&m);
        limba_rat_init(&q);
        limba_big_set_u64(&m.num, 1);
        limba_big_shl(&m.num, &m.num, t->bits);
        int_divmod(NULL, &q, &S->v[*v].num, &m, true);
        *v = value_rat(S, &q);
        limba_rat_free(&m);
        limba_rat_free(&q);
        return true;
    }
    __int128 i;
    bool in = lxs_value_to_int(S, *v, &i) && i >= t->lo && i <= t->hi;
    if (!in) {
        limba_big_str(&S->v[*v].num.num, vb, sizeof(vb));
        lxs_error(S, LXE_CONST_RANGE, node, "%s is out of the range of %s", vb,
                  lxs_tname(S, type, tb));
    }
    return in;
}
