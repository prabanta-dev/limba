/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * math.h - the functions of mathematics of the run time (math_sin ...
 * math_pow of runtime.def), the same bits on every engine and machine
 * (progetto_ir.md § 11b): the reference interpreter, Meri and the folding
 * of constants call these, never the C library's. They come from musl
 * 1.2.5 (MIT), compiled into Limba from one portable C source, with no
 * contraction of a * b + c: error below 1 ulp, not always correctly
 * rounded. A function of a Float32 is the function of the Float64 rounded
 * once to a float.
 */
#ifndef LIMBA_MATH_H
#define LIMBA_MATH_H

#include <stdbool.h>
#include <stdint.h>

double limba_math_sin(double x);
double limba_math_cos(double x);
double limba_math_tan(double x);
double limba_math_atan(double x);
double limba_math_exp(double x);
double limba_math_ln(double x);
double limba_math_pow(double x, double y);

/* the runtime function rt of runtime.def (LIMBA_RT_MATH_*), any of the
   mathematics of a real: *out from args, true; false for another rt.
   sqrt, trunc, floor and ceil are exact in IEEE 754 */
bool limba_math_call(uint32_t rt, const double *args, double *out);

#endif
