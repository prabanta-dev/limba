/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fmt.h - how a real is written: the text of print_f64, print_f32,
 * str_from_f64, str_from_f32 and str_from_f64_fixed (runtime.def), byte
 * for byte. A runtime that writes reals calls these, so that every
 * back end writes what the reference interpreter writes.
 *
 * They use snprintf and strtod: call them under the C locale (LC_NUMERIC
 * "C"), or the point may become a comma.
 */
#ifndef LIMBA_FMT_H
#define LIMBA_FMT_H

#include <stddef.h>

/* the size of a buffer for limba_fmt_f64 and limba_fmt_f32 */
#define LIMBA_FMT_F64_MAX 32
/* the most decimals of the fixed form, and the size of its buffer: a
   sign, 309 digits before the point, the point, 100 decimals, NUL */
#define LIMBA_FMT_DECIMALS_MAX 100
#define LIMBA_FMT_FIXED_MAX 416

/* v in the shortest form that reads back exactly, as Python's repr
   writes it (luxia-0.md § 9.1): 0.1, 100.0, 1e+16, 1.5e-05, -0.0, inf,
   -inf, nan (a NaN never has a sign). Positional when the exponent of
   the first digit is in -4..15, else d.ddde+XX with at least two digits
   of exponent. Writes into buf (LIMBA_FMT_F64_MAX bytes), NUL-terminated;
   returns the length */
size_t limba_fmt_f64(char *buf, double v);
/* the same for a Float32: the fewest digits that read back as v in a
   float, so 0.1f is 0.1 */
size_t limba_fmt_f32(char *buf, float v);
/* v in fixed notation with decimals digits after the point (none, and
   no point, for 0), rounded from the exact value of v; decimals is
   clamped to 0..LIMBA_FMT_DECIMALS_MAX (the program checked it before).
   inf, -inf and nan as above. Writes into buf (LIMBA_FMT_FIXED_MAX
   bytes), NUL-terminated; returns the length */
size_t limba_fmt_f64_fixed(char *buf, double v, int decimals);

#endif
