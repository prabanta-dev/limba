/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lir_gen.c - print the random program of a seed, to look at it or to run
 * it with lir_run:  lir_gen SEED > prog.lit
 */
#include "eval/gen.h"
#include "limba/ir.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("usage: lir_gen SEED\n", stderr);
        return 2;
    }
    limba_module *m = limba_gen(strtoull(argv[1], NULL, 0));
    limba_diag d = {{0}, 0};
    if (limba_verify(m, &d) != 0)
        fprintf(stderr, "lir_gen: the program does not verify: %s\n", d.msg);
    limba_print(m, stdout);
    limba_module_free(m);
    return 0;
}
