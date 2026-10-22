/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fmt.c - the text of a real (see limba/fmt.h). The digits are the
 * fewest that, rounded from the exact value of v to nearest, ties to even,
 * read back to the same bits (as a float for a Float32); the layout is
 * Python's: positional when the exponent of the first digit is in -4..15,
 * else d.ddde+XX with at least two digits.
 *
 * The rounding and the reading back are exact steps of common/decimal.c
 * where their numbers fit; elsewhere, for now, snprintf and strtod of the
 * C library do the same (the slow way, still to be written: proposta
 * § 2, question 3).
 */
#include "limba/fmt.h"

#include "common/decimal.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the exponent of the first digit of a > 0: 10^E <= a < 10^(E+1);
   false if the exact steps cannot tell */
static bool first_exponent(double a, int *e)
{
    int g = (int)floor(log10(a)); /* a guess, checked exactly */
    for (int tries = 0; tries < 3; tries++) {
        uint64_t f;
        int h;
        if (!limba_dec_scaled(a, -g, &f, &h))
            return false;
        if (f >= 10)
            g++;
        else if (f == 0)
            g--;
        else {
            *e = g;
            return true;
        }
    }
    return false;
}

/* a > 0 rounded to p digits, as "%.*e" writes it with p - 1, into e (with
   a '-' if neg): 1 if it reads back as a (in a float if f32), 0 if not,
   -1 if the exact steps cannot tell (e is then not to be used) */
static int fast_digits(char *e, double a, bool neg, int ex, int p, bool f32)
{
    uint64_t d;
    if (!limba_dec_round(a, p - 1 - ex, &d))
        return -1;
    if (d == limba_dec_pow10(p)) { /* 9.96 to 2 digits: 1.0e+01 */
        d = limba_dec_pow10(p - 1);
        ex++;
    }
    char dig[24];
    for (int i = p - 1; i >= 0; i--) {
        dig[i] = (char)('0' + d % 10);
        d /= 10;
    }
    size_t n = 0;
    if (neg)
        e[n++] = '-';
    e[n++] = dig[0];
    if (p > 1) {
        e[n++] = '.';
        memcpy(e + n, dig + 1, (size_t)p - 1);
        n += (size_t)p - 1;
    }
    int x = ex < 0 ? -ex : ex;
    e[n++] = 'e';
    e[n++] = ex < 0 ? '-' : '+';
    if (x >= 100)
        e[n++] = (char)('0' + x / 100);
    e[n++] = (char)('0' + x / 10 % 10);
    e[n++] = (char)('0' + x % 10);
    e[n] = 0;
    uint64_t back = 0;
    for (int i = 0; i < p; i++)
        back = back * 10 + (uint64_t)(dig[i] - '0');
    double r;
    if (!limba_dec_read(back, ex - (p - 1), f32, &r))
        return -1;
    if (f32) {
        float rf = (float)r, af = (float)a;
        return memcmp(&rf, &af, sizeof(af)) == 0;
    }
    return memcmp(&r, &a, sizeof(a)) == 0;
}

/* the same by the C library: snprintf, then strtod or strtof */
static int slow_digits(char *e, double v, int p, bool f32)
{
    snprintf(e, LIMBA_FMT_F64_MAX, "%.*e", p - 1, v);
    if (f32) {
        float back = strtof(e, NULL), fv = (float)v;
        return memcmp(&back, &fv, sizeof(fv)) == 0;
    }
    double back = strtod(e, NULL);
    return memcmp(&back, &v, sizeof(v)) == 0;
}

/* v, a float when f32 */
static size_t fmt(char *buf, double v, bool f32)
{
    if (isnan(v)) /* every NaN alike, whatever its sign and bits */
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, "nan");
    if (isinf(v))
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, v < 0 ? "-inf" : "inf");
    char e[LIMBA_FMT_F64_MAX];
    bool neg = signbit(v);
    double a = fabs(v);
    int ex = 0;
    bool fast = a != 0 && first_exponent(a, &ex);
    if (a == 0)
        strcpy(e, neg ? "-0e+00" : "0e+00");
    else
        for (int p = 1; p <= 17; p++) {
            int r = fast ? fast_digits(e, a, neg, ex, p, f32) : -1;
            if (r < 0)
                r = slow_digits(e, v, p, f32);
            if (r)
                break;
        }
    /* e is [-]d[.ddd]e(+|-)XX: its digits and the exponent of the first */
    const char *s = e;
    if (neg)
        s++;
    char digits[20];
    size_t nd = 0;
    for (; *s != 'e'; s++)
        if (*s != '.')
            digits[nd++] = *s;
    int exp = atoi(s + 1);
    while (nd > 1 && digits[nd - 1] == '0')
        nd--;
    size_t n = 0;
    if (neg)
        buf[n++] = '-';
    if (exp >= -4 && exp < 16) {
        if (exp < 0) {
            buf[n++] = '0';
            buf[n++] = '.';
            for (int k = -1; k > exp; k--)
                buf[n++] = '0';
            memcpy(buf + n, digits, nd);
            n += nd;
        } else {
            for (int k = 0; k <= exp; k++)
                buf[n++] = (size_t)k < nd ? digits[k] : '0';
            buf[n++] = '.';
            if ((size_t)exp + 1 < nd) {
                memcpy(buf + n, digits + exp + 1, nd - (size_t)exp - 1);
                n += nd - (size_t)exp - 1;
            } else {
                buf[n++] = '0';
            }
        }
    } else {
        buf[n++] = digits[0];
        if (nd > 1) {
            buf[n++] = '.';
            memcpy(buf + n, digits + 1, nd - 1);
            n += nd - 1;
        }
        n += (size_t)snprintf(buf + n, LIMBA_FMT_F64_MAX - n, "e%c%02d",
                              exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
    }
    buf[n] = 0;
    return n;
}

size_t limba_fmt_f64(char *buf, double v)
{
    return fmt(buf, v, false);
}

size_t limba_fmt_f32(char *buf, float v)
{
    return fmt(buf, v, true);
}

/* the minus of the fixed form: every value with the sign bit set, also
   one that rounds to zero (-0.004 to two decimals is -0.00), as C and
   Python write it (proposta_luxia_1.md § 6.4, question 15). The one
   line to change if the rule changes */
static bool fixed_minus(double v)
{
    return signbit(v);
}

size_t limba_fmt_f64_fixed(char *buf, double v, int decimals)
{
    if (isnan(v)) /* nan, never -nan, as the short form */
        return (size_t)snprintf(buf, LIMBA_FMT_FIXED_MAX, "nan");
    if (isinf(v))
        return (size_t)snprintf(buf, LIMBA_FMT_FIXED_MAX,
                                v < 0 ? "-inf" : "inf");
    int d = decimals < 0                        ? 0
            : decimals > LIMBA_FMT_DECIMALS_MAX ? LIMBA_FMT_DECIMALS_MAX
                                                : decimals;
    uint64_t n = 0;
    double a = fabs(v);
    if (a != 0 && !limba_dec_round(a, d, &n)) {
        /* the slow way, for now the C library, with the same minus */
        size_t k = 0;
        if (fixed_minus(v))
            buf[k++] = '-';
        return k +
               (size_t)snprintf(buf + k, LIMBA_FMT_FIXED_MAX - k, "%.*f", d, a);
    }
    char dig[LIMBA_FMT_DECIMALS_MAX + 24];
    int nd = 0;
    do {
        dig[nd++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (nd < d + 1) /* at least one digit before the point */
        dig[nd++] = '0';
    size_t k = 0;
    if (fixed_minus(v))
        buf[k++] = '-';
    for (int i = nd - 1; i >= 0; i--) {
        buf[k++] = dig[i];
        if (i == d && d > 0)
            buf[k++] = '.';
    }
    buf[k] = 0;
    return k;
}
