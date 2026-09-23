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

#endif
