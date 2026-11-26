/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * core_math.h - what the mathematics of CORE-MATH
 * (src/third_party/core-math, unchanged) needs around it when it is
 * compiled into Limba, given to each of its files with -include by
 * build.sh. Not part of CORE-MATH, and never included by Limba's own code.
 *
 * A limba_ name for every symbol its files define (each file defines only
 * its cr_ function), so that the library keeps Limba's prefix. Limba
 * reaches them through limba/math.h. CORE_MATH_SUPPORT_ERRNO is never
 * defined: no errno (doc/ir.md § 15.5).
 */
#ifndef LIMBA_CORE_MATH_H
#define LIMBA_CORE_MATH_H

#define cr_sin limba_cr_sin
#define cr_sinf limba_cr_sinf
#define cr_cos limba_cr_cos
#define cr_cosf limba_cr_cosf
#define cr_tan limba_cr_tan
#define cr_tanf limba_cr_tanf
#define cr_atan limba_cr_atan
#define cr_atanf limba_cr_atanf
#define cr_exp limba_cr_exp
#define cr_expf limba_cr_expf
#define cr_log limba_cr_log
#define cr_logf limba_cr_logf
#define cr_pow limba_cr_pow
#define cr_powf limba_cr_powf
#define cr_atan2 limba_cr_atan2
#define cr_atan2f limba_cr_atan2f
#define cr_asin limba_cr_asin
#define cr_asinf limba_cr_asinf
#define cr_acos limba_cr_acos
#define cr_acosf limba_cr_acosf
#define cr_hypot limba_cr_hypot
#define cr_hypotf limba_cr_hypotf
#define cr_log2 limba_cr_log2
#define cr_log2f limba_cr_log2f
#define cr_log10 limba_cr_log10
#define cr_log10f limba_cr_log10f
#define cr_sinh limba_cr_sinh
#define cr_sinhf limba_cr_sinhf
#define cr_cosh limba_cr_cosh
#define cr_coshf limba_cr_coshf
#define cr_tanh limba_cr_tanh
#define cr_tanhf limba_cr_tanhf

#endif
