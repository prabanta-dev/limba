/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * pass.h - what a pass of the optimiser looks like. Not public.
 */
#ifndef LIMBA_OPT_PASS_H
#define LIMBA_OPT_PASS_H

#include "ir/internal.h"

/* A pass works on one function and returns how many changes it made: the
   manager counts them (a pass that never changes anything on the corpus
   is a pass that looks for a shape the IR no longer has) and stops
   iterating when a whole round changes nothing. The module is well formed
   on entry and must be on exit. */
typedef uint32_t (*limba_pass_fn)(limba_module *m, limba_func *f);

uint32_t limba_pass_cfg(limba_module *m, limba_func *f);
uint32_t limba_pass_fold(limba_module *m, limba_func *f);
uint32_t limba_pass_gvn(limba_module *m, limba_func *f);
uint32_t limba_pass_dce(limba_module *m, limba_func *f);

#endif
