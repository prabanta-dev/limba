/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fmt_f64.h - a real in the shortest form that reads back exactly, as
 * Python's repr writes it (luxia_0.md § 9): 0.1, 100.0, 1e+16, 1.5e-05,
 * -0.0, inf, -inf, nan (a NaN never has a sign).
 */
#ifndef LIMBA_FMT_F64_H
#define LIMBA_FMT_F64_H

#include <stddef.h>

#define LIMBA_FMT_F64_MAX 32

/* write v into buf (LIMBA_FMT_F64_MAX bytes), NUL-terminated; the length */
size_t limba_fmt_f64(char *buf, double v);
/* the same for a Float32: the fewest digits that read back as v in a
   float, so 0.1f is 0.1 */
size_t limba_fmt_f32(char *buf, float v);

#endif
