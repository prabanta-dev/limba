/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lir_gen.c - print the random program of a seed, to look at it or to run
 * it with lir_run:  lir_gen [--no-exceptions] SEED > prog.lit
 *
 * --no-exceptions  no region, raise, table of exceptions nor par_for: the
 *                  program the seed gave before exceptions, for an engine
 *                  that does not run them yet
 */
#include "eval/gen.h"
#include "limba/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    limba_set_program("lir_gen");
    unsigned flags = 0;
    if (argc == 3 && !strcmp(argv[1], "--no-exceptions"))
        flags = LIMBA_GEN_NO_EXCEPTIONS;
    else if (argc != 2) {
        fputs("usage: lir_gen [--no-exceptions] SEED\n", stderr);
        return 2;
    }
    limba_module *m = limba_gen_with(strtoull(argv[argc - 1], NULL, 0), flags);
    limba_diag d = {{0}, 0};
    if (limba_verify(m, &d) != 0)
        fprintf(stderr, "lir_gen: the program does not verify: %s\n", d.msg);
    limba_print(m, stdout);
    limba_module_free(m);
    return 0;
}
