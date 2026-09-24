/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fmt_f64.c - the shortest form of a real (see fmt_f64.h). The digits are
 * the fewest that printf rounds to a string strtod reads back to the same
 * bits; the layout is Python's: positional when the exponent of the first
 * digit is in -4..15, else d.ddde+XX with at least two digits.
 */
#include "fmt_f64.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t limba_fmt_f64(char *buf, double v)
{
    if (isnan(v))
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, "nan");
    if (isinf(v))
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, v < 0 ? "-inf" : "inf");
    char e[LIMBA_FMT_F64_MAX];
    for (int p = 1; p <= 17; p++) {
        snprintf(e, sizeof(e), "%.*e", p - 1, v);
        double back = strtod(e, NULL);
        if (memcmp(&back, &v, sizeof(v)) == 0)
            break;
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
