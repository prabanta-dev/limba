/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * math.c - the functions of limba/math.h: CORE-MATH's, by the names
 * src/common/core_math.h gives them.
 */
#include "limba/math.h"

#include "limba/ir.h"

#include <math.h>

double limba_cr_sin(double);
float limba_cr_sinf(float);
double limba_cr_cos(double);
float limba_cr_cosf(float);
double limba_cr_tan(double);
float limba_cr_tanf(float);
double limba_cr_atan(double);
float limba_cr_atanf(float);
double limba_cr_exp(double);
float limba_cr_expf(float);
double limba_cr_log(double);
float limba_cr_logf(float);
double limba_cr_pow(double, double);
float limba_cr_powf(float, float);
double limba_cr_atan2(double, double);
float limba_cr_atan2f(float, float);
double limba_cr_asin(double);
float limba_cr_asinf(float);
double limba_cr_acos(double);
float limba_cr_acosf(float);
double limba_cr_hypot(double, double);
float limba_cr_hypotf(float, float);
double limba_cr_log2(double);
float limba_cr_log2f(float);
double limba_cr_log10(double);
float limba_cr_log10f(float);
double limba_cr_sinh(double);
float limba_cr_sinhf(float);
double limba_cr_cosh(double);
float limba_cr_coshf(float);
double limba_cr_tanh(double);
float limba_cr_tanhf(float);

double limba_math_sin(double x)
{
    return limba_cr_sin(x);
}

float limba_math_sin_f32(float x)
{
    return limba_cr_sinf(x);
}

double limba_math_cos(double x)
{
    return limba_cr_cos(x);
}

float limba_math_cos_f32(float x)
{
    return limba_cr_cosf(x);
}

double limba_math_tan(double x)
{
    return limba_cr_tan(x);
}

float limba_math_tan_f32(float x)
{
    return limba_cr_tanf(x);
}

double limba_math_atan(double x)
{
    return limba_cr_atan(x);
}

float limba_math_atan_f32(float x)
{
    return limba_cr_atanf(x);
}

double limba_math_exp(double x)
{
    return limba_cr_exp(x);
}

float limba_math_exp_f32(float x)
{
    return limba_cr_expf(x);
}

double limba_math_ln(double x)
{
    return limba_cr_log(x);
}

float limba_math_ln_f32(float x)
{
    return limba_cr_logf(x);
}

double limba_math_pow(double x, double y)
{
    return limba_cr_pow(x, y);
}

float limba_math_pow_f32(float x, float y)
{
    return limba_cr_powf(x, y);
}

/* limba_math_atan2 is in math_atan2.c */

float limba_math_atan2_f32(float y, float x)
{
    return limba_cr_atan2f(y, x);
}

double limba_math_asin(double x)
{
    return limba_cr_asin(x);
}

float limba_math_asin_f32(float x)
{
    return limba_cr_asinf(x);
}

double limba_math_acos(double x)
{
    return limba_cr_acos(x);
}

float limba_math_acos_f32(float x)
{
    return limba_cr_acosf(x);
}

double limba_math_hypot(double x, double y)
{
    return limba_cr_hypot(x, y);
}

float limba_math_hypot_f32(float x, float y)
{
    return limba_cr_hypotf(x, y);
}

double limba_math_log2(double x)
{
    return limba_cr_log2(x);
}

float limba_math_log2_f32(float x)
{
    return limba_cr_log2f(x);
}

double limba_math_log10(double x)
{
    return limba_cr_log10(x);
}

float limba_math_log10_f32(float x)
{
    return limba_cr_log10f(x);
}

double limba_math_sinh(double x)
{
    return limba_cr_sinh(x);
}

float limba_math_sinh_f32(float x)
{
    return limba_cr_sinhf(x);
}

double limba_math_cosh(double x)
{
    return limba_cr_cosh(x);
}

float limba_math_cosh_f32(float x)
{
    return limba_cr_coshf(x);
}

double limba_math_tanh(double x)
{
    return limba_cr_tanh(x);
}

float limba_math_tanh_f32(float x)
{
    return limba_cr_tanhf(x);
}

bool limba_math_call(uint32_t rt, const double *a, double *out)
{
    /* for the _f32 ones an f32 held in a double: exact both ways */
    switch (rt) {
    case LIMBA_RT_MATH_SQRT:
        *out = sqrt(a[0]);
        return true;
    case LIMBA_RT_MATH_TRUNC:
        *out = trunc(a[0]);
        return true;
    case LIMBA_RT_MATH_FLOOR:
        *out = floor(a[0]);
        return true;
    case LIMBA_RT_MATH_CEIL:
        *out = ceil(a[0]);
        return true;
    case LIMBA_RT_MATH_SIN:
        *out = limba_math_sin(a[0]);
        return true;
    case LIMBA_RT_MATH_SIN_F32:
        *out = limba_math_sin_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_COS:
        *out = limba_math_cos(a[0]);
        return true;
    case LIMBA_RT_MATH_COS_F32:
        *out = limba_math_cos_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_TAN:
        *out = limba_math_tan(a[0]);
        return true;
    case LIMBA_RT_MATH_TAN_F32:
        *out = limba_math_tan_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_ATAN:
        *out = limba_math_atan(a[0]);
        return true;
    case LIMBA_RT_MATH_ATAN_F32:
        *out = limba_math_atan_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_EXP:
        *out = limba_math_exp(a[0]);
        return true;
    case LIMBA_RT_MATH_EXP_F32:
        *out = limba_math_exp_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_LN:
        *out = limba_math_ln(a[0]);
        return true;
    case LIMBA_RT_MATH_LN_F32:
        *out = limba_math_ln_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_POW:
        *out = limba_math_pow(a[0], a[1]);
        return true;
    case LIMBA_RT_MATH_POW_F32:
        *out = limba_math_pow_f32((float)a[0], (float)a[1]);
        return true;
    case LIMBA_RT_MATH_ATAN2:
        *out = limba_math_atan2(a[0], a[1]);
        return true;
    case LIMBA_RT_MATH_ATAN2_F32:
        *out = limba_math_atan2_f32((float)a[0], (float)a[1]);
        return true;
    case LIMBA_RT_MATH_ASIN:
        *out = limba_math_asin(a[0]);
        return true;
    case LIMBA_RT_MATH_ASIN_F32:
        *out = limba_math_asin_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_ACOS:
        *out = limba_math_acos(a[0]);
        return true;
    case LIMBA_RT_MATH_ACOS_F32:
        *out = limba_math_acos_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_HYPOT:
        *out = limba_math_hypot(a[0], a[1]);
        return true;
    case LIMBA_RT_MATH_HYPOT_F32:
        *out = limba_math_hypot_f32((float)a[0], (float)a[1]);
        return true;
    case LIMBA_RT_MATH_LOG2:
        *out = limba_math_log2(a[0]);
        return true;
    case LIMBA_RT_MATH_LOG2_F32:
        *out = limba_math_log2_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_LOG10:
        *out = limba_math_log10(a[0]);
        return true;
    case LIMBA_RT_MATH_LOG10_F32:
        *out = limba_math_log10_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_SINH:
        *out = limba_math_sinh(a[0]);
        return true;
    case LIMBA_RT_MATH_SINH_F32:
        *out = limba_math_sinh_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_COSH:
        *out = limba_math_cosh(a[0]);
        return true;
    case LIMBA_RT_MATH_COSH_F32:
        *out = limba_math_cosh_f32((float)a[0]);
        return true;
    case LIMBA_RT_MATH_TANH:
        *out = limba_math_tanh(a[0]);
        return true;
    case LIMBA_RT_MATH_TANH_F32:
        *out = limba_math_tanh_f32((float)a[0]);
        return true;
    }
    return false;
}
