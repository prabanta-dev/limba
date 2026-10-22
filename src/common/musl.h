/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * musl.h - what the mathematics of musl (src/third_party/musl, unchanged)
 * needs around it when it is compiled into Limba, given to each of its
 * files with -include by build.sh. Not part of musl, and never included
 * by Limba's own code.
 *
 * The C library's headers first, as musl's files would include them; then
 * musl's own word for an internal symbol, and a limba_musl_ name for every
 * symbol its files define, so that none meets a function of the C library
 * of the same name (sin, floor, ...) when a program links both. Limba
 * reaches them through limba/math.h.
 */
#ifndef LIMBA_MUSL_H
#define LIMBA_MUSL_H

#include <endian.h>
#include <float.h>
#include <math.h>
#include <stdint.h>

#define hidden __attribute__((__visibility__("hidden")))

#define sin limba_musl_sin
#define cos limba_musl_cos
#define tan limba_musl_tan
#define atan limba_musl_atan
#define exp limba_musl_exp
#define log limba_musl_log
#define pow limba_musl_pow
#define __sin limba_musl___sin
#define __cos limba_musl___cos
#define __tan limba_musl___tan
#define __rem_pio2 limba_musl___rem_pio2
#define __rem_pio2_large limba_musl___rem_pio2_large
#define __exp_data limba_musl___exp_data
#define __log_data limba_musl___log_data
#define __pow_log_data limba_musl___pow_log_data
#define __math_oflow limba_musl___math_oflow
#define __math_uflow limba_musl___math_uflow
#define __math_xflow limba_musl___math_xflow
#define __math_divzero limba_musl___math_divzero
#define __math_invalid limba_musl___math_invalid
#define scalbn limba_musl_scalbn
#define floor limba_musl_floor
#define fabs limba_musl_fabs
/* declared by libm.h, not compiled here: renamed all the same, since the
   C library may declare a different function of the same name */
#define __cosdf limba_musl___cosdf
#define __cosl limba_musl___cosl
#define __expo2 limba_musl___expo2
#define __expo2f limba_musl___expo2f
#define __lgammaf_r limba_musl___lgammaf_r
#define __lgamma_r limba_musl___lgamma_r
#define __math_divzerof limba_musl___math_divzerof
#define __math_invalidf limba_musl___math_invalidf
#define __math_invalidl limba_musl___math_invalidl
#define __math_oflowf limba_musl___math_oflowf
#define __math_uflowf limba_musl___math_uflowf
#define __math_xflowf limba_musl___math_xflowf
#define __p1evll limba_musl___p1evll
#define __polevll limba_musl___polevll
#define __rem_pio2f limba_musl___rem_pio2f
#define __rem_pio2l limba_musl___rem_pio2l
#define __sindf limba_musl___sindf
#define __sinl limba_musl___sinl
#define __tandf limba_musl___tandf
#define __tanl limba_musl___tanl
#undef __signgam /* a macro of newlib */
#define __signgam limba_musl___signgam

/* the prototypes of the C library came before the names changed */
double sin(double);
double cos(double);
double tan(double);
double atan(double);
double exp(double);
double log(double);
double pow(double, double);
double scalbn(double, int);
double floor(double);
double fabs(double);
#endif
