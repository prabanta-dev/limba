/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fmt.c - the text of a real (see limba/fmt.h). The digits are
 * the fewest that printf rounds to a string strtod (strtof for a Float32)
 * reads back to the same bits; the layout is Python's: positional when the
 * exponent of the first digit is in -4..15, else d.ddde+XX with at least two
 * digits.
 */
#include "limba/fmt.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* v, a float when f32 */
static size_t fmt(char *buf, double v, bool f32)
{
    if (isnan(v))
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, "nan");
    if (isinf(v))
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, v < 0 ? "-inf" : "inf");
    char e[LIMBA_FMT_F64_MAX];
    for (int p = 1; p <= 17; p++) {
        snprintf(e, sizeof(e), "%.*e", p - 1, v);
        if (f32) {
            float back = strtof(e, NULL), fv = (float)v;
            if (memcmp(&back, &fv, sizeof(fv)) == 0)
                break;
        } else {
            double back = strtod(e, NULL);
            if (memcmp(&back, &v, sizeof(v)) == 0)
                break;
        }
    }
    /* e is [-]d[.ddd]e(+|-)XX: its digits and the exponent of the first */
    const char *s = e;
    bool neg = *s == '-';
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

size_t limba_fmt_f64_fixed(char *buf, double v, int decimals)
{
    if (isnan(v)) /* nan, never -nan, as the short form */
        return (size_t)snprintf(buf, LIMBA_FMT_FIXED_MAX, "nan");
    int d = decimals < 0                        ? 0
            : decimals > LIMBA_FMT_DECIMALS_MAX ? LIMBA_FMT_DECIMALS_MAX
                                                : decimals;
    return (size_t)snprintf(buf, LIMBA_FMT_FIXED_MAX, "%.*f", d, v);
}
