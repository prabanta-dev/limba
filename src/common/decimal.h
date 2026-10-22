/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * decimal.h - the exact steps between binary reals and decimal digits
 * that need no number of any size: a real times a power of ten, rounded
 * exactly, and a few decimal digits read back exactly (Clinger's fast
 * path). Each answers "unknown" where its numbers would not fit; the
 * caller then takes the slow way. Portable C, no 128-bit type of the
 * compiler: the same steps, so the same digits, on every machine.
 */
#ifndef LIMBA_DECIMAL_H
#define LIMBA_DECIMAL_H

#include <stdbool.h>
#include <stdint.h>

/* a > 0 finite: *fl = floor(a * 10^q) and *half the sign of the fraction
   left minus 1/2 (-1, 0, 1); false (unknown) where this needs numbers
   past what these steps hold, or *fl past 64 bits */
bool limba_dec_scaled(double a, int q, uint64_t *fl, int *half);

/* the same, rounded to nearest, ties to even */
bool limba_dec_round(double a, int q, uint64_t *out);

/* d * 10^k for an integer d, rounded once to a double (f32 false) or to
   a float (f32 true, *v then holds the float); false (unknown) unless d
   and 10^|k| are exact in that type, when one product or quotient
   rounds it exactly */
bool limba_dec_read(uint64_t d, int k, bool f32, double *v);

/* 10^k for 0 <= k <= 19 */
uint64_t limba_dec_pow10(int k);

#endif
