/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * luxia.h - the limba program on a Luxia source.
 */
#ifndef LIMBA_TOOL_LUXIA_H
#define LIMBA_TOOL_LUXIA_H

#include "limba/opt.h"

#include <stdbool.h>

/* compile a Luxia source: tokens, tree, or the IR (lir by default, lit),
   optimised at level, with the checks named in suppress (index_check,...,
   separated by commas) off in the whole file; emit and suppress may be
   NULL; the exit status of the program */
int limba_luxia_main(const char *in, const char *emit, const char *outpath,
                     bool check, int level, const limba_opt_options *opt,
                     const char *suppress);

#endif
