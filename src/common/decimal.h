/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * decimal.h - the exact steps between binary reals and decimal digits:
 * a real times a power of ten, rounded exactly, and decimal digits read
 * back to the nearest real. The common cases in numbers of 128 bits
 * (two words) and Clinger's fast path; the others with the integers of
 * common/bigint.c. Portable C, no 128-bit type of the compiler and no
 * conversion of the C library: the same digits on every machine.
 */
#ifndef LIMBA_DECIMAL_H
#define LIMBA_DECIMAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* a > 0 finite: *fl = floor(a * 10^q) and *half the sign of the fraction
   left minus 1/2 (-1, 0, 1); false only if *fl is past 64 bits or q is
   outside -800..800 */
bool limba_dec_scaled(double a, int q, uint64_t *fl, int *half);

/* the same, rounded to nearest, ties to even */
bool limba_dec_round(double a, int q, uint64_t *out);

/* d * 10^k for an integer d, rounded once to nearest even to a double
   (f32 false) or to a float (f32 true, *v then holds the float); false
   only past the largest finite value */
bool limba_dec_read(uint64_t d, int k, bool f32, double *v);

/* the same for the decimal digits t[0..n) ('0'..'9', any count) times
   10^k: what val reads; false past the largest finite value */
bool limba_dec_read_digits(const char *t, size_t n, long k, bool f32,
                           double *v);

/* a > 0 finite times 10^q, 0 <= q <= 800, rounded to nearest even, as
   decimal digits into buf (size bytes, NUL-terminated): the fixed form
   past 64 bits. Returns the count of digits */
size_t limba_dec_round_text(double a, int q, char *buf, size_t size);

/* 10^k for 0 <= k <= 19 */
uint64_t limba_dec_pow10(int k);

#endif
