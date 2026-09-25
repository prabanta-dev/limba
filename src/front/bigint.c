/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * bigint.c - integers and rationals of any size (see bigint.h). Plain
 * algorithms: schoolbook product, bit-by-bit division. The numbers of a
 * program's constants are small, and the size limit bounds the rest.
 */
#include "bigint.h"

#include "common/xalloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXWORDS (LIMBA_BIG_MAXBITS / 32 + 2)

void limba_big_init(limba_big *a)
{
    memset(a, 0, sizeof(*a));
}

void limba_big_free(limba_big *a)
{
    free(a->w);
    limba_big_init(a);
}

static void reserve(limba_big *a, uint32_t n)
{
    if (n > a->cap) {
        a->w = limba_xrealloc(a->w, n, sizeof(*a->w));
        a->cap = n;
    }
}

static void trim(limba_big *a)
{
    while (a->n && a->w[a->n - 1] == 0)
        a->n--;
    if (a->n == 0)
        a->neg = false;
}

/* r takes the storage of t */
static void move(limba_big *r, limba_big *t)
{
    if (r == t)
        return;
    free(r->w);
    *r = *t;
    limba_big_init(t);
}

void limba_big_copy(limba_big *r, const limba_big *a)
{
    if (r == a)
        return;
    reserve(r, a->n);
    if (a->n)
        memcpy(r->w, a->w, a->n * sizeof(*a->w));
    r->n = a->n;
    r->neg = a->neg;
}

void limba_big_set_u64(limba_big *r, uint64_t v)
{
    reserve(r, 2);
    r->w[0] = (uint32_t)v;
    r->w[1] = (uint32_t)(v >> 32);
    r->n = 2;
    r->neg = false;
    trim(r);
}

static void set_u128(limba_big *r, unsigned __int128 v)
{
    reserve(r, 4);
    for (int k = 0; k < 4; k++)
        r->w[k] = (uint32_t)(v >> (32 * k));
    r->n = 4;
    r->neg = false;
    trim(r);
}

void limba_big_set_i64(limba_big *r, int64_t v)
{
    uint64_t m = v < 0 ? -(uint64_t)v : (uint64_t)v;
    limba_big_set_u64(r, m);
    r->neg = v < 0 && m != 0;
}

uint32_t limba_big_bits(const limba_big *a)
{
    if (a->n == 0)
        return 0;
    return (a->n - 1) * 32 + (32 - (uint32_t)__builtin_clz(a->w[a->n - 1]));
}

int limba_big_sign(const limba_big *a)
{
    return a->n == 0 ? 0 : a->neg ? -1 : 1;
}

static int mag_cmp(const limba_big *a, const limba_big *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (uint32_t i = a->n; i-- > 0;)
        if (a->w[i] != b->w[i])
            return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

int limba_big_cmp(const limba_big *a, const limba_big *b)
{
    int sa = limba_big_sign(a), sb = limba_big_sign(b);
    if (sa != sb)
        return sa < sb ? -1 : 1;
    int m = mag_cmp(a, b);
    return sa < 0 ? -m : m;
}

/* t = |a| + |b| */
static void mag_add(limba_big *t, const limba_big *a, const limba_big *b)
{
    if (a->n < b->n) {
        const limba_big *x = a;
        a = b;
        b = x;
    }
    reserve(t, a->n + 1);
    uint64_t carry = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        carry += (uint64_t)a->w[i] + (i < b->n ? b->w[i] : 0);
        t->w[i] = (uint32_t)carry;
        carry >>= 32;
    }
    t->w[a->n] = (uint32_t)carry;
    t->n = a->n + 1;
    t->neg = false;
    trim(t);
}

/* t = |a| - |b|, |a| >= |b| */
static void mag_sub(limba_big *t, const limba_big *a, const limba_big *b)
{
    reserve(t, a->n);
    int64_t borrow = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        int64_t d = (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0) - borrow;
        borrow = d < 0;
        t->w[i] = (uint32_t)(d + (borrow ? (int64_t)1 << 32 : 0));
    }
    t->n = a->n;
    t->neg = false;
    trim(t);
}

static bool fits(const limba_big *a)
{
    return limba_big_bits(a) <= LIMBA_BIG_MAXBITS;
}

static bool addsub(limba_big *r, const limba_big *a, const limba_big *b,
                   bool bneg)
{
    limba_big t;
    limba_big_init(&t);
    if (a->neg == bneg) {
        mag_add(&t, a, b);
        t.neg = a->neg;
    } else if (mag_cmp(a, b) >= 0) {
        mag_sub(&t, a, b);
        t.neg = a->neg;
    } else {
        mag_sub(&t, b, a);
        t.neg = bneg;
    }
    trim(&t);
    bool ok = fits(&t);
    move(r, &t);
    return ok;
}

bool limba_big_add(limba_big *r, const limba_big *a, const limba_big *b)
{
    return addsub(r, a, b, b->neg);
}

bool limba_big_sub(limba_big *r, const limba_big *a, const limba_big *b)
{
    return addsub(r, a, b, b->n ? !b->neg : false);
}

void limba_big_neg(limba_big *r, const limba_big *a)
{
    limba_big_copy(r, a);
    r->neg = r->n ? !a->neg : false;
}

void limba_big_abs(limba_big *r, const limba_big *a)
{
    limba_big_copy(r, a);
    r->neg = false;
}

bool limba_big_mul(limba_big *r, const limba_big *a, const limba_big *b)
{
    if (limba_big_bits(a) + limba_big_bits(b) > LIMBA_BIG_MAXBITS + 1)
        return false;
    limba_big t;
    limba_big_init(&t);
    uint32_t n = a->n + b->n;
    reserve(&t, n ? n : 1);
    memset(t.w, 0, (n ? n : 1) * sizeof(*t.w));
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < b->n; j++) {
            carry += (uint64_t)a->w[i] * b->w[j] + t.w[i + j];
            t.w[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        t.w[i + b->n] = (uint32_t)carry;
    }
    t.n = n;
    t.neg = a->neg != b->neg;
    trim(&t);
    bool ok = fits(&t);
    move(r, &t);
    return ok;
}

/* r = a << bits, without the size limit (for the rounding, which shifts
   past it for a moment) */
static void shl_raw(limba_big *r, const limba_big *a, uint32_t bits)
{
    if (a->n == 0) {
        limba_big_set_u64(r, 0);
        return;
    }
    limba_big t;
    limba_big_init(&t);
    uint32_t ws = bits / 32, bs = bits % 32;
    reserve(&t, a->n + ws + 1);
    memset(t.w, 0, (a->n + ws + 1) * sizeof(*t.w));
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t v = (uint64_t)a->w[i] << bs;
        t.w[i + ws] |= (uint32_t)v;
        t.w[i + ws + 1] |= (uint32_t)(v >> 32);
    }
    t.n = a->n + ws + 1;
    t.neg = a->neg;
    trim(&t);
    move(r, &t);
}

bool limba_big_shl(limba_big *r, const limba_big *a, uint32_t bits)
{
    if (limba_big_bits(a) + (uint64_t)bits > LIMBA_BIG_MAXBITS)
        return false;
    shl_raw(r, a, bits);
    return true;
}

static bool bit(const limba_big *a, uint32_t i)
{
    return i / 32 < a->n && (a->w[i / 32] >> (i % 32) & 1);
}

void limba_big_divmod(limba_big *q, limba_big *m, const limba_big *a,
                      const limba_big *b)
{
    limba_big tq, tr;
    limba_big_init(&tq);
    limba_big_init(&tr);
    if (b->n == 1) {
        /* one word: the common case, done a word at a time */
        uint64_t rem = 0, d = b->w[0];
        reserve(&tq, a->n ? a->n : 1);
        for (uint32_t i = a->n; i-- > 0;) {
            uint64_t cur = rem << 32 | a->w[i];
            tq.w[i] = (uint32_t)(cur / d);
            rem = cur % d;
        }
        tq.n = a->n;
        trim(&tq);
        limba_big_set_u64(&tr, rem);
    } else {
        limba_big mb;
        limba_big_init(&mb);
        limba_big_abs(&mb, b);
        uint32_t nb = limba_big_bits(a);
        reserve(&tq, a->n ? a->n : 1);
        memset(tq.w, 0, (a->n ? a->n : 1) * sizeof(*tq.w));
        tq.n = a->n;
        for (uint32_t i = nb; i-- > 0;) {
            limba_big_shl(&tr, &tr, 1);
            if (bit(a, i)) {
                if (tr.n == 0) {
                    reserve(&tr, 1);
                    tr.w[0] = 0;
                    tr.n = 1;
                }
                tr.w[0] |= 1;
            }
            if (mag_cmp(&tr, &mb) >= 0) {
                limba_big t;
                limba_big_init(&t);
                mag_sub(&t, &tr, &mb);
                move(&tr, &t);
                tq.w[i / 32] |= 1u << (i % 32);
            }
        }
        trim(&tq);
        limba_big_free(&mb);
    }
    tq.neg = tq.n && a->neg != b->neg;
    tr.neg = tr.n && a->neg;
    if (q)
        move(q, &tq);
    if (m)
        move(m, &tr);
    limba_big_free(&tq);
    limba_big_free(&tr);
}

bool limba_big_pow(limba_big *r, const limba_big *a, uint64_t e)
{
    uint32_t bits = limba_big_bits(a);
    if (bits > 1 && e > 0 &&
        (e > LIMBA_BIG_MAXBITS || (uint64_t)(bits - 1) * e > LIMBA_BIG_MAXBITS))
        return false;
    limba_big result, base;
    limba_big_init(&result);
    limba_big_init(&base);
    limba_big_set_u64(&result, 1);
    limba_big_copy(&base, a);
    bool ok = true;
    while (e && ok) {
        if (e & 1)
            ok = limba_big_mul(&result, &result, &base);
        e >>= 1;
        if (e && ok)
            ok = limba_big_mul(&base, &base, &base);
    }
    move(r, &result);
    limba_big_free(&base);
    return ok;
}

void limba_big_gcd(limba_big *r, const limba_big *a, const limba_big *b)
{
    limba_big x, y, t;
    limba_big_init(&x);
    limba_big_init(&y);
    limba_big_init(&t);
    limba_big_abs(&x, a);
    limba_big_abs(&y, b);
    while (y.n) {
        limba_big_divmod(NULL, &t, &x, &y);
        move(&x, &y);
        move(&y, &t);
    }
    move(r, &x);
    limba_big_free(&y);
    limba_big_free(&t);
}

bool limba_big_to_u64(const limba_big *a, uint64_t *v)
{
    if (a->neg || a->n > 2)
        return false;
    *v = a->n == 0   ? 0
         : a->n == 1 ? a->w[0]
                     : (uint64_t)a->w[1] << 32 | a->w[0];
    return true;
}

bool limba_big_to_i64(const limba_big *a, int64_t *v)
{
    uint64_t m;
    limba_big t = *a;
    t.neg = false;
    if (!limba_big_to_u64(&t, &m))
        return false;
    if (a->neg) {
        if (m > (uint64_t)1 << 63)
            return false;
        *v = (int64_t)(0 - m);
    } else {
        if (m > INT64_MAX)
            return false;
        *v = (int64_t)m;
    }
    return true;
}

bool limba_big_parse(limba_big *r, const char *s, size_t n, unsigned base)
{
    limba_big t;
    limba_big_init(&t);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                            : 99;
        if (d >= base)
            continue;
        uint64_t carry = d;
        for (uint32_t k = 0; k < t.n; k++) {
            carry += (uint64_t)t.w[k] * base;
            t.w[k] = (uint32_t)carry;
            carry >>= 32;
        }
        if (carry) {
            if (t.n >= MAXWORDS) {
                limba_big_free(&t);
                return false;
            }
            reserve(&t, t.n + 1);
            t.w[t.n++] = (uint32_t)carry;
        }
    }
    bool ok = fits(&t);
    move(r, &t);
    return ok;
}

const char *limba_big_str(const limba_big *a, char *buf, size_t size)
{
    /* digits in groups of nine, least significant first */
    limba_big t, ten9;
    limba_big_init(&t);
    limba_big_init(&ten9);
    limba_big_abs(&t, a);
    limba_big_set_u64(&ten9, 1000000000);
    uint32_t cap = t.n * 10 / 9 + 2, ng = 0;
    uint32_t *group = limba_xmalloc(cap * sizeof(*group));
    do {
        limba_big rem;
        limba_big_init(&rem);
        limba_big_divmod(&t, &rem, &t, &ten9);
        uint64_t g = 0;
        limba_big_to_u64(&rem, &g);
        group[ng++] = (uint32_t)g;
        limba_big_free(&rem);
    } while (t.n);
    size_t len = 0;
    char tmp[16];
#define PUT(str)                                                               \
    do {                                                                       \
        for (const char *p_ = (str); *p_; p_++)                                \
            if (len + 1 < size)                                                \
                buf[len++] = *p_;                                              \
    } while (0)
    if (a->neg)
        PUT("-");
    snprintf(tmp, sizeof(tmp), "%u", group[ng - 1]);
    PUT(tmp);
    for (uint32_t i = ng - 1; i-- > 0;) {
        snprintf(tmp, sizeof(tmp), "%09u", group[i]);
        PUT(tmp);
    }
#undef PUT
    if (size) {
        buf[len] = 0;
        if (len + 1 == size && size > 4)
            memcpy(buf + size - 4, "...", 4);
    }
    free(group);
    limba_big_free(&t);
    limba_big_free(&ten9);
    return buf;
}

/* ---- rationals ---- */

void limba_rat_init(limba_rat *a)
{
    limba_big_init(&a->num);
    limba_big_init(&a->den);
    limba_big_set_u64(&a->den, 1);
}

void limba_rat_free(limba_rat *a)
{
    limba_big_free(&a->num);
    limba_big_free(&a->den);
}

void limba_rat_copy(limba_rat *r, const limba_rat *a)
{
    limba_big_copy(&r->num, &a->num);
    limba_big_copy(&r->den, &a->den);
}

void limba_rat_set_big(limba_rat *r, const limba_big *a)
{
    limba_big_copy(&r->num, a);
    limba_big_set_u64(&r->den, 1);
}

static void normalise(limba_rat *r)
{
    if (r->den.neg) {
        r->den.neg = false;
        r->num.neg = r->num.n ? !r->num.neg : false;
    }
    if (r->num.n == 0) {
        limba_big_set_u64(&r->den, 1);
        return;
    }
    limba_big g;
    limba_big_init(&g);
    limba_big_gcd(&g, &r->num, &r->den);
    if (!(g.n == 1 && g.w[0] == 1)) {
        limba_big_divmod(&r->num, NULL, &r->num, &g);
        limba_big_divmod(&r->den, NULL, &r->den, &g);
    }
    limba_big_free(&g);
}

int limba_rat_cmp(const limba_rat *a, const limba_rat *b)
{
    limba_big x, y;
    limba_big_init(&x);
    limba_big_init(&y);
    /* the products may pass the limit: compare them anyway */
    limba_big_mul(&x, &a->num, &b->den);
    limba_big_mul(&y, &b->num, &a->den);
    int c = limba_big_cmp(&x, &y);
    limba_big_free(&x);
    limba_big_free(&y);
    return c;
}

static bool rat_addsub(limba_rat *r, const limba_rat *a, const limba_rat *b,
                       bool sub)
{
    limba_rat t;
    limba_rat_init(&t);
    limba_big x, y;
    limba_big_init(&x);
    limba_big_init(&y);
    bool ok =
        limba_big_mul(&x, &a->num, &b->den) &&
        limba_big_mul(&y, &b->num, &a->den) &&
        (sub ? limba_big_sub(&t.num, &x, &y) : limba_big_add(&t.num, &x, &y)) &&
        limba_big_mul(&t.den, &a->den, &b->den);
    limba_big_free(&x);
    limba_big_free(&y);
    if (ok) {
        normalise(&t);
        limba_rat_copy(r, &t);
    }
    limba_rat_free(&t);
    return ok;
}

bool limba_rat_add(limba_rat *r, const limba_rat *a, const limba_rat *b)
{
    return rat_addsub(r, a, b, false);
}

bool limba_rat_sub(limba_rat *r, const limba_rat *a, const limba_rat *b)
{
    return rat_addsub(r, a, b, true);
}

static bool rat_muldiv(limba_rat *r, const limba_big *n1, const limba_big *n2,
                       const limba_big *d1, const limba_big *d2)
{
    limba_rat t;
    limba_rat_init(&t);
    bool ok = limba_big_mul(&t.num, n1, n2) && limba_big_mul(&t.den, d1, d2);
    if (ok) {
        normalise(&t);
        limba_rat_copy(r, &t);
    }
    limba_rat_free(&t);
    return ok;
}

bool limba_rat_mul(limba_rat *r, const limba_rat *a, const limba_rat *b)
{
    return rat_muldiv(r, &a->num, &b->num, &a->den, &b->den);
}

bool limba_rat_div(limba_rat *r, const limba_rat *a, const limba_rat *b)
{
    return rat_muldiv(r, &a->num, &b->den, &a->den, &b->num);
}

void limba_rat_neg(limba_rat *r, const limba_rat *a)
{
    limba_rat_copy(r, a);
    limba_big_neg(&r->num, &r->num);
}

bool limba_rat_pow(limba_rat *r, const limba_rat *a, int64_t e)
{
    uint64_t m = e < 0 ? -(uint64_t)e : (uint64_t)e;
    limba_rat t;
    limba_rat_init(&t);
    bool ok = limba_big_pow(&t.num, e < 0 ? &a->den : &a->num, m) &&
              limba_big_pow(&t.den, e < 0 ? &a->num : &a->den, m);
    if (ok) {
        normalise(&t); /* also moves the sign of a negative base up */
        limba_rat_copy(r, &t);
    }
    limba_rat_free(&t);
    return ok;
}

bool limba_rat_parse(limba_rat *r, const char *s, size_t n)
{
    /* mantissa digits, the count of those after the point, the exponent */
    char *digits = limba_xmalloc(n + 1);
    size_t nd = 0, i = 0;
    int64_t frac = 0, exp = 0;
    bool point = false;
    for (; i < n && s[i] != 'e' && s[i] != 'E'; i++) {
        if (s[i] == '.')
            point = true;
        else if (s[i] >= '0' && s[i] <= '9') {
            digits[nd++] = s[i];
            frac += point;
        }
    }
    if (i < n) {
        bool neg = false;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            neg = s[i++] == '-';
        for (; i < n; i++) {
            if (s[i] < '0' || s[i] > '9')
                continue;
            if (exp < 1000000)
                exp = exp * 10 + (s[i] - '0');
        }
        if (neg)
            exp = -exp;
    }
    exp -= frac;
    limba_rat t;
    limba_rat_init(&t);
    /* the common literal: up to 19 digits and 10^-38..10^19, in 128
       bits; the only factors 10^k can share with the digits are 2 and 5 */
    size_t lead = 0;
    while (lead < nd && digits[lead] == '0')
        lead++;
    if (nd - lead <= 19 && exp >= -38 && exp <= 19) {
        unsigned __int128 v = 0, p10 = 1, den = 1;
        for (size_t k = lead; k < nd; k++)
            v = v * 10 + (unsigned)(digits[k] - '0');
        if (exp >= 0) {
            for (int64_t k = 0; k < exp; k++)
                p10 *= 10;
            set_u128(&r->num, v * p10); /* below 10^19 * 10^19 */
            limba_big_set_u64(&r->den, 1);
        } else {
            unsigned two = (unsigned)-exp, five = (unsigned)-exp;
            while (v && two && !(v & 1)) {
                v >>= 1;
                two--;
            }
            while (v && five && v % 5 == 0) {
                v /= 5;
                five--;
            }
            if (v == 0)
                two = five = 0;
            for (unsigned k = 0; k < five; k++)
                den *= 5;
            den <<= two; /* at most 10^38 */
            set_u128(&r->num, v);
            set_u128(&r->den, den);
        }
        free(digits);
        limba_rat_free(&t);
        return true;
    }
    limba_big p, ten;
    limba_big_init(&p);
    limba_big_init(&ten);
    limba_big_set_u64(&ten, 10);
    uint64_t m = exp < 0 ? -(uint64_t)exp : (uint64_t)exp;
    bool ok = limba_big_parse(&t.num, digits, nd, 10) &&
              limba_big_pow(&p, &ten, m) &&
              (exp < 0 ? (limba_big_copy(&t.den, &p), true)
                       : limba_big_mul(&t.num, &t.num, &p));
    if (ok) {
        normalise(&t);
        limba_rat_copy(r, &t);
    }
    free(digits);
    limba_big_free(&p);
    limba_big_free(&ten);
    limba_rat_free(&t);
    return ok;
}

/* round |a| to p bits of precision with exponents from emin (the least
   normal) to emax, into a double through ldexp; sign applied after */
/* the magnitude of a when below 2^bits, in *v */
static bool small_mag(const limba_big *a, int bits, uint64_t *v)
{
    if (a->n > 2)
        return false;
    uint64_t m = a->n ? a->w[0] : 0;
    if (a->n == 2)
        m |= (uint64_t)a->w[1] << 32;
    *v = m;
    return m < ((uint64_t)1 << bits);
}

static bool rat_round(const limba_rat *a, int mant, int emin, int emax,
                      double *out)
{
    if (a->num.n == 0) {
        *out = 0;
        return true;
    }
    /* both exact in the format: one IEEE 754 division rounds the
       quotient once, to nearest even (the quotient is neither tiny nor
       huge) */
    uint64_t n, d;
    if (small_mag(&a->num, mant, &n) && small_mag(&a->den, mant, &d)) {
        if (mant == 24) {
            float q = (float)n / (float)d;
            *out = q;
        } else {
            *out = (double)n / (double)d;
        }
        if (a->num.neg)
            *out = -*out;
        return true;
    }
    limba_big num, den, q, rem, t;
    limba_big_init(&num);
    limba_big_init(&den);
    limba_big_init(&q);
    limba_big_init(&rem);
    limba_big_init(&t);
    limba_big_abs(&num, &a->num);
    limba_big_copy(&den, &a->den);
    /* e = floor(log2(num / den)) */
    int64_t e = (int64_t)limba_big_bits(&num) - limba_big_bits(&den);
    if (e >= 0) {
        shl_raw(&t, &den, (uint32_t)e);
        limba_big_copy(&q, &num);
    } else {
        shl_raw(&q, &num, (uint32_t)-e);
        limba_big_copy(&t, &den);
    }
    if (mag_cmp(&q, &t) < 0)
        e--;
    int64_t p = mant;
    if (e < emin)
        p = mant - (emin - e);
    bool ok = true;
    if (e > emax) {
        ok = false; /* 2^e is past the largest finite value */
        *out = HUGE_VAL;
    } else if (p < 0) {
        *out = 0; /* below half the least subnormal */
    } else {
        /* m = round(num / den / 2^s), s = e - p + 1 */
        int64_t s = e - p + 1;
        if (s >= 0)
            shl_raw(&den, &den, (uint32_t)s);
        else
            shl_raw(&num, &num, (uint32_t)-s);
        limba_big_divmod(&q, &rem, &num, &den);
        limba_big_shl(&rem, &rem, 1);
        int c = mag_cmp(&rem, &den);
        uint64_t m = 0;
        limba_big_to_u64(&q, &m);
        if (c > 0 || (c == 0 && (m & 1)))
            m++;
        double v = ldexp((double)m, (int)s);
        if (v >= ldexp(1.0, emax + 1))
            ok = false;
        *out = v;
    }
    limba_big_free(&num);
    limba_big_free(&den);
    limba_big_free(&q);
    limba_big_free(&rem);
    limba_big_free(&t);
    if (a->num.neg)
        *out = -*out;
    return ok;
}

bool limba_rat_to_f64(const limba_rat *a, double *v)
{
    return rat_round(a, 53, -1022, 1023, v);
}

bool limba_rat_to_f32(const limba_rat *a, float *v)
{
    double d;
    /* the rounding to 24 bits is exact in a double */
    bool ok = rat_round(a, 24, -126, 127, &d);
    *v = (float)d;
    return ok;
}
