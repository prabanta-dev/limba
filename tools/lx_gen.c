/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lx_gen.c - print the random Luxia program of a seed, or (-e) what it
 * must print and how it must end:
 *   lx_gen SEED > prog.luxia;  lx_gen -e SEED
 */
#include "luxia/gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int expect = argc == 3 && !strcmp(argv[1], "-e");
    if (argc != 2 + expect) {
        fputs("usage: lx_gen [-e] SEED\n", stderr);
        return 2;
    }
    limba_lxgen p;
    if (!limba_lxgen_make(strtoull(argv[1 + expect], NULL, 0), &p)) {
        fputs("lx_gen: no program within the limits\n", stderr);
        return 1;
    }
    if (expect) {
        fwrite(p.out, 1, p.outlen, stdout);
        printf("-- %s\n", p.end);
    } else {
        fputs(p.src, stdout);
    }
    limba_lxgen_free(&p);
    return 0;
}
