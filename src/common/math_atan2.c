/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * math_atan2.c - limba_math_atan2: CORE-MATH's atan2, kept from a path
 * where it stops the process.
 *
 * CORE-MATH's atan2 (f6c0c37) calls exit(1) on some arguments with x > 0
 * and a tiny t = y / x, where its accurate path of 192 bits cannot round:
 * y, x = 0x1.8000000000003p-51, 0x1.0000000000003p+0, which upstream
 * lists among its worst cases. Those arguments never reach it here:
 *
 * - |t| < 2^-53: a midpoint m between doubles has 54 bits, so y - m x is
 *   a nonzero multiple of the unit of m x and |y / x - m| >= 2^-107 |t|,
 *   while |atan(q) - q| <= q^3 / 3 < 2^-107 |t|: atan2(y, x) rounds as
 *   y / x. (Upstream's own remedy, in its atan2.c but not enabled, takes
 *   the bound 0x1.250bfe1b082f4p-52, which its latest worst cases break:
 *   y, x = 0x1.e10ff969caaebp-54, 0x1.0c70fa9dbc847p-1);
 * - up to |t| <= 2^-40: atan(q) = q - q^3/3 + q^5/5 - ... with q = y / x
 *   exact, so the result is t, or the double below t in magnitude when
 *   t was rounded away from zero and atan(q) falls past the midpoint m
 *   between them. The sign of atan(q) - m is the sign of the integer
 *   15 x^4 (y - m x) - 5 x^2 y^3 + 3 y^5, up to the next term, below
 *   (15/7) y^7 / x^2, computed exactly in 640 bits.
 *
 * A tiny y is first scaled with x by 2^600 (the same angle, exactly). A
 * subnormal t, and everything else, goes to CORE-MATH.
 * Checked against MPFR, worst cases included (tools/mpfr/math_mpfr.c).
 */
#include "limba/math.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

double limba_cr_atan2(double, double);

/* an unsigned integer of 640 bits, 32 at a time (no 128-bit integer on
   every target) */
enum { LIMBS = 20 };
typedef struct {
    uint32_t w[LIMBS];
} wide;

static void w_set(wide *a, uint64_t v)
{
    memset(a, 0, sizeof(*a));
    a->w[0] = (uint32_t)v;
    a->w[1] = (uint32_t)(v >> 32);
}

/* a *= v, v below 2^32 */
static void w_mul(wide *a, uint32_t v)
{
    uint64_t carry = 0;
    for (int i = 0; i < LIMBS; i++) {
        uint64_t p = (uint64_t)a->w[i] * v + carry;
        a->w[i] = (uint32_t)p;
        carry = p >> 32;
    }
}

/* a *= v, v below 2^64 */
static void w_mul64(wide *a, uint64_t v)
{
    wide hi = *a;
    w_mul(a, (uint32_t)v);
    w_mul(&hi, (uint32_t)(v >> 32));
    /* a += hi << 32 */
    uint64_t carry = 0;
    for (int i = 1; i < LIMBS; i++) {
        uint64_t s = (uint64_t)a->w[i] + hi.w[i - 1] + carry;
        a->w[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

static void w_shl(wide *a, unsigned k)
{
    unsigned l = k / 32, b = k % 32;
    for (int i = LIMBS - 1; i >= 0; i--) {
        uint64_t v = i - (int)l >= 0 ? a->w[i - l] : 0,
                 u = b && i - (int)l - 1 >= 0 ? a->w[i - l - 1] : 0;
        a->w[i] = (uint32_t)((v << b) | (b ? u >> (32 - b) : 0));
    }
}

static void w_add(wide *a, const wide *b)
{
    uint64_t carry = 0;
    for (int i = 0; i < LIMBS; i++) {
        uint64_t s = (uint64_t)a->w[i] + b->w[i] + carry;
        a->w[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

/* a -= b, a >= b */
static void w_sub(wide *a, const wide *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < LIMBS; i++) {
        int64_t d = (int64_t)a->w[i] - b->w[i] - borrow;
        borrow = d < 0;
        a->w[i] = (uint32_t)d;
    }
}

static int w_cmp(const wide *a, const wide *b)
{
    for (int i = LIMBS - 1; i >= 0; i--)
        if (a->w[i] != b->w[i])
            return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* the position of the highest bit set, -1 for 0 */
static int w_top(const wide *a)
{
    for (int i = LIMBS - 1; i >= 0; i--)
        if (a->w[i])
            return 32 * i + 31 - __builtin_clz(a->w[i]);
    return -1;
}

/* v = s * 2^e, s an integer of 53 bits (v > 0, normal) */
static uint64_t split(double v, int *e)
{
    int k;
    double f = frexp(v, &k);
    *e = k - 53;
    return (uint64_t)ldexp(f, 53);
}

/* c * s1^n1 * s2^n2 * s3, shifted left by k, into r */
static void term(wide *r, uint32_t c, uint64_t s1, int n1, uint64_t s2, int n2,
                 uint64_t s3, unsigned k)
{
    w_set(r, c);
    for (int i = 0; i < n1; i++)
        w_mul64(r, s1);
    for (int i = 0; i < n2; i++)
        w_mul64(r, s2);
    w_mul64(r, s3);
    w_shl(r, k);
}

/* x > 0, y > 0 normal, t = RN(y / x) normal and inexact, rounded up, with
   2^-53 <= t <= 2^-40: t, or the double below it; 0 when
   the integers cannot tell */
static double tiny_up(double y, double x, double t)
{
    double below = nextafter(t, 0);
    /* m = (t + below) / 2 = M * 2^em, M odd; below has half the ulp of t
       when t is a power of two */
    int ex, ey, et;
    uint64_t X = split(x, &ex), Y = split(y, &ey), T = split(t, &et);
    uint64_t M;
    int em;
    if (T != (uint64_t)1 << 52) {
        M = 2 * T - 1;
        em = et - 1;
    } else {
        M = 4 * T - 1;
        em = et - 2;
    }
    /* P = 15 x^4 y + 3 y^5, N = 15 x^5 m + 5 x^2 y^3, each a term times
       2^(its exponent); all are aligned on the smallest exponent */
    int e1 = 4 * ex + ey, e2 = 5 * ey, e3 = 5 * ex + em, e4 = 2 * ex + 3 * ey;
    int lo = e1;
    if (e2 < lo)
        lo = e2;
    if (e3 < lo)
        lo = e3;
    if (e4 < lo)
        lo = e4;
    int hi = e1 > e3 ? e1 : e3;
    if (hi - lo > 640 - 330)
        return 0;
    wide p, n, a;
    term(&p, 15, X, 4, 1, 0, Y, (unsigned)(e1 - lo));
    term(&a, 3, Y, 4, 1, 0, Y, (unsigned)(e2 - lo));
    w_add(&p, &a);
    term(&n, 15, X, 5, 1, 0, M, (unsigned)(e3 - lo));
    term(&a, 5, X, 2, Y, 2, Y, (unsigned)(e4 - lo));
    w_add(&n, &a);
    int c = w_cmp(&p, &n);
    wide d = c >= 0 ? p : n;
    w_sub(&d, c >= 0 ? &n : &p);
    /* |P - N| against the bound of the next term, 15 x^5 q^7 / 7 below
       (15/7) y^7 / x^2, with a margin of 2^4 */
    int top = w_top(&d);
    double lb = 7 * log2(y) - 2 * log2(x) + 1.1 + 4;
    if (top < 0 || (double)(top + lo) <= lb)
        return 0;
    return c > 0 ? t : below;
}

double limba_math_atan2(double y, double x)
{
    if (x > 0 && x < 0x1p400 && y != 0 && fabs(y) < 0x1p-900) {
        y *= 0x1p600;
        x *= 0x1p600;
    }
    if (x > 0 && isfinite(x) && fabs(y) >= 0x1p-900) {
        double t = y / x, a = fabs(t);
        if (a > 0x1p-1022 && a <= 0x1p-40) {
            double r = fma(t, x, -y); /* exact: t x - y */
            if (r == 0 || a < 0x1p-53)
                return t;
            /* t rounded toward zero: atan(q) < q keeps it */
            if ((t > 0) == (r < 0))
                return t;
            double v = tiny_up(fabs(y), x, a);
            if (v != 0)
                return copysign(v, t);
        }
    }
    return limba_cr_atan2(y, x);
}
