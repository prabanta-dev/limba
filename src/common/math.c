/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * math.c - the functions of limba/math.h: musl's, by the names
 * src/common/musl.h gives them.
 */
#include "limba/math.h"

#include "limba/ir.h"

#include <math.h>

double limba_musl_sin(double);
double limba_musl_cos(double);
double limba_musl_tan(double);
double limba_musl_atan(double);
double limba_musl_exp(double);
double limba_musl_log(double);
double limba_musl_pow(double, double);

double limba_math_sin(double x)
{
    return limba_musl_sin(x);
}

double limba_math_cos(double x)
{
    return limba_musl_cos(x);
}

double limba_math_tan(double x)
{
    return limba_musl_tan(x);
}

double limba_math_atan(double x)
{
    return limba_musl_atan(x);
}

double limba_math_exp(double x)
{
    return limba_musl_exp(x);
}

double limba_math_ln(double x)
{
    return limba_musl_log(x);
}

double limba_math_pow(double x, double y)
{
    return limba_musl_pow(x, y);
}

bool limba_math_call(uint32_t rt, const double *a, double *out)
{
    switch (rt) {
    case LIMBA_RT_MATH_SQRT:
        *out = sqrt(a[0]);
        return true;
    case LIMBA_RT_MATH_SIN:
        *out = limba_math_sin(a[0]);
        return true;
    case LIMBA_RT_MATH_COS:
        *out = limba_math_cos(a[0]);
        return true;
    case LIMBA_RT_MATH_TAN:
        *out = limba_math_tan(a[0]);
        return true;
    case LIMBA_RT_MATH_ATAN:
        *out = limba_math_atan(a[0]);
        return true;
    case LIMBA_RT_MATH_EXP:
        *out = limba_math_exp(a[0]);
        return true;
    case LIMBA_RT_MATH_LN:
        *out = limba_math_ln(a[0]);
        return true;
    case LIMBA_RT_MATH_POW:
        *out = limba_math_pow(a[0], a[1]);
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
    }
    return false;
}
