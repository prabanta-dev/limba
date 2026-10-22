/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gen.h - random IR programs for the OPTDIFF net, in the manner of Csmith:
 * valid by construction, always terminating (small counted loops, no
 * recursion), the same program for the same seed. They mix what the
 * optimiser looks at: constants, identities, repeated expressions, branches
 * on constants, dead values, traps, memory. Not public.
 */
#ifndef LIMBA_GEN_H
#define LIMBA_GEN_H

#include "limba/ir.h"

#include <stdint.h>

/* a module with helper functions and a @main : fn() -> i64 */
limba_module *limba_gen(uint64_t seed);

/* the same, with flags: LIMBA_GEN_NO_EXCEPTIONS makes no region, raise,
   table of exceptions nor par_for, the programs of before them (an
   engine that does not run exceptions yet compares all the seeds) */
enum { LIMBA_GEN_NO_EXCEPTIONS = 1u << 0 };
limba_module *limba_gen_with(uint64_t seed, unsigned flags);

#endif
