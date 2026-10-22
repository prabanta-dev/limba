/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * val.c - the number of a text, as val reads it (see limba/val.h).
 */
#include "limba/val.h"

#include "common/decimal.h"
#include "common/xalloc.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int digit_of(char c, unsigned base)
{
    int v = c >= '0' && c <= '9'   ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                   : -1;
    return v >= 0 && (unsigned)v < base ? v : -1;
}

/* the digits of base at p[*i], a _ only between two of them, copied to
   buf at *k without the _; false if there is none */
static bool num_digits(const char *p, size_t n, size_t *i, unsigned base,
                       char *buf, size_t *k)
{
    size_t start = *i;
    while (*i < n) {
        if (digit_of(p[*i], base) >= 0)
            buf[(*k)++] = p[(*i)++];
        else if (p[*i] == '_' && *i > start && *i + 1 < n &&
                 digit_of(p[*i + 1], base) >= 0)
            (*i)++;
        else
            break;
    }
    return *i > start;
}

/* the literal p[0..n) in buf without its _: 1 for an integer of *base
   (the digits only), 2 for a real, 0 for no literal */
static int num_literal(const char *p, size_t n, char *buf, unsigned *base)
{
    size_t i = 0, k = 0;
    int kind = 1;
    *base = 10;
    if (n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'o' || p[1] == 'b')) {
        *base = p[1] == 'x' ? 16 : p[1] == 'o' ? 8 : 2;
        i = 2;
        if (!num_digits(p, n, &i, *base, buf, &k))
            return 0;
    } else {
        if (!num_digits(p, n, &i, 10, buf, &k))
            return 0;
        if (i < n && p[i] == '.') {
            buf[k++] = p[i++];
            if (!num_digits(p, n, &i, 10, buf, &k))
                return 0;
            kind = 2;
        }
        if (i < n && p[i] == 'e') {
            buf[k++] = p[i++];
            if (i < n && (p[i] == '+' || p[i] == '-'))
                buf[k++] = p[i++];
            if (!num_digits(p, n, &i, 10, buf, &k))
                return 0;
            kind = 2;
        }
    }
    buf[k] = 0;
    return i == n ? kind : 0;
}

/* the body of the number in s[0..len), without the spaces and tabs
   around and the sign (+, - or 0 for none); false if nothing is left */
static bool num_body(const char *s, size_t len, const char **p, size_t *n,
                     char *sign)
{
    const char *b = s, *e = s + len;
    while (b < e && (*b == ' ' || *b == '\t'))
        b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *sign = b < e && (*b == '+' || *b == '-') ? *b++ : 0;
    *p = b;
    *n = (size_t)(e - b);
    return b < e;
}

bool limba_val_int(const char *s, size_t len, uint64_t *mag, bool *neg)
{
    const char *p;
    size_t n;
    unsigned base;
    char sign;
    if (!num_body(s, len, &p, &n, &sign))
        return false;
    *neg = sign == '-';
    char *buf = limba_xmalloc(n + 1);
    bool ok = num_literal(p, n, buf, &base) == 1;
    *mag = 0;
    for (char *c = buf; ok && *c; c++)
        ok = !__builtin_mul_overflow(*mag, base, mag) &&
             !__builtin_add_overflow(*mag, (uint64_t)digit_of(*c, base), mag);
    free(buf);
    return ok;
}

bool limba_val_big(const char *s, size_t len, char *digits, size_t *n,
                   unsigned *base, bool *neg)
{
    const char *p;
    size_t k;
    char sign;
    if (!num_body(s, len, &p, &k, &sign))
        return false;
    *neg = sign == '-';
    char *buf = limba_xmalloc(k + 1);
    bool ok = num_literal(p, k, buf, base) == 1;
    *n = ok ? strlen(buf) : 0;
    if (ok)
        memcpy(digits, buf, *n);
    free(buf);
    return ok;
}

/* the decimal literal t (digits, '.', digits, 'e', sign, digits, as
   num_literal leaves it) read to the nearest real: its digits without
   the point, and the exponent of the last one; false past the type */
static bool dec_real(const char *t, bool f32, double *v)
{
    size_t len = strlen(t);
    char *dig = limba_xmalloc(len + 1);
    size_t nd = 0;
    long frac = 0, ev = 0;
    bool point = false;
    for (; *t && *t != 'e'; t++) {
        if (*t == '.')
            point = true;
        else {
            dig[nd++] = *t;
            frac += point;
        }
    }
    if (*t == 'e') {
        t++;
        bool minus = *t == '-';
        if (*t == '-' || *t == '+')
            t++;
        for (; *t; t++)
            if (ev < 100000000) /* past it, a zero or past the range */
                ev = ev * 10 + (*t - '0');
        if (minus)
            ev = -ev;
    }
    bool ok = limba_dec_read_digits(dig, nd, ev - frac, f32, v);
    free(dig);
    return ok;
}

/* the digits of base 2, 8 or 16 in t, rounded once to 53 bits (24 if
   f32), ties to even, exactly: a scaling by a power of two cannot round */
static double pow2_digits(const char *t, unsigned base, bool f32)
{
    unsigned b = base == 16 ? 4 : base == 8 ? 3 : 1;
    uint64_t acc = 0;
    long extra = 0;
    bool sticky = false;
    for (; *t; t++) {
        uint64_t dv = (uint64_t)digit_of(*t, base);
        if (acc >> (64 - b) == 0)
            acc = acc << b | dv;
        else {
            sticky |= dv != 0;
            extra += b;
        }
    }
    int prec = f32 ? 24 : 53, nb = 0;
    for (uint64_t c = acc; c; c >>= 1)
        nb++;
    if (nb > prec) {
        int sh = nb - prec;
        uint64_t rem = acc & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        acc >>= sh;
        extra += sh;
        if (rem > half || (rem == half && (sticky || (acc & 1))))
            acc++;
    }
    double r = ldexp((double)acc, extra > 100000 ? 100000 : (int)extra);
    return f32 ? (double)(float)r : r; /* past the float range: inf */
}

bool limba_val_real(const char *s, size_t len, bool f32, uint64_t *bits)
{
    const char *p;
    size_t n;
    char sign;
    unsigned base;
    if (!num_body(s, len, &p, &n, &sign))
        return false;
    double d;
    if (n == 3 && !memcmp(p, "inf", 3)) {
        d = INFINITY;
    } else if (n == 3 && !memcmp(p, "nan", 3)) {
        if (sign)
            return false; /* nan has no sign */
        d = NAN;
    } else {
        char *buf = limba_xmalloc(n + 1);
        int kind = num_literal(p, n, buf, &base);
        d = 0;
        if (kind == 1 && base != 10)
            d = pow2_digits(buf, base, f32);
        else if (kind && !dec_real(buf, f32, &d))
            d = INFINITY; /* past the largest finite value */
        free(buf);
        if (!kind || isinf(d))
            return false;
    }
    if (sign == '-')
        d = -d;
    if (f32) { /* rounded to a float already: exact */
        float f = (float)d;
        uint32_t u;
        memcpy(&u, &f, sizeof(u));
        *bits = u;
    } else {
        memcpy(bits, &d, sizeof(d));
    }
    return true;
}
