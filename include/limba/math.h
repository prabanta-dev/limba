/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * math.h - the functions of mathematics of the run time (math_sin ...
 * math_tanh_f32 of runtime.def), the same bits on every engine and machine
 * (doc/ir.md § 15.5): the reference interpreter, Meri and the folding
 * of constants call these, never the C library's. They come from
 * CORE-MATH (MIT), compiled into Limba from its portable C sources, with
 * no contraction of a * b + c: correctly rounded to nearest even, for f64
 * and, by functions of their own, for f32 (never the function of the f64
 * rounded again). The same bits everywhere follow from the correct
 * rounding; a NaN may differ in sign and payload.
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
double limba_math_atan2(double y, double x);
double limba_math_asin(double x);
double limba_math_acos(double x);
double limba_math_hypot(double x, double y);
double limba_math_log2(double x);
double limba_math_log10(double x);
double limba_math_sinh(double x);
double limba_math_cosh(double x);
double limba_math_tanh(double x);

/* the same of an f32, correctly rounded to f32 */
float limba_math_sin_f32(float x);
float limba_math_cos_f32(float x);
float limba_math_tan_f32(float x);
float limba_math_atan_f32(float x);
float limba_math_exp_f32(float x);
float limba_math_ln_f32(float x);
float limba_math_pow_f32(float x, float y);
float limba_math_atan2_f32(float y, float x);
float limba_math_asin_f32(float x);
float limba_math_acos_f32(float x);
float limba_math_hypot_f32(float x, float y);
float limba_math_log2_f32(float x);
float limba_math_log10_f32(float x);
float limba_math_sinh_f32(float x);
float limba_math_cosh_f32(float x);
float limba_math_tanh_f32(float x);

/* the runtime function rt of runtime.def (LIMBA_RT_MATH_*), any of the
   mathematics of a real: *out from args (two for pow, atan2 and hypot),
   true; false for another rt. For the functions _f32 the arguments are
   f32 values and *out is one, each held exactly in a double.
   sqrt, trunc, floor and ceil are exact in IEEE 754 */
bool limba_math_call(uint32_t rt, const double *args, double *out);

#endif
