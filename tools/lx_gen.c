/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lx_gen.c - print the random Luxia program of a seed, or (-e) what it
 * must print and how it must end, (-i) the input it reads, (-a) its
 * command line, an argument a line:
 *   lx_gen SEED > prog.luxia;  lx_gen -i SEED > prog.in;  lx_gen -e SEED
 */
#include "luxia/gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *o = argc == 3 ? argv[1] : "";
    char what = strlen(o) == 2 && o[0] == '-' && strchr("eia", o[1]) ? o[1] : 0;
    if (argc != 2 + (what != 0)) {
        fputs("usage: lx_gen [-e | -i | -a] SEED\n", stderr);
        return 2;
    }
    limba_lxgen p;
    if (!limba_lxgen_make(strtoull(argv[argc - 1], NULL, 0), &p)) {
        fputs("lx_gen: no program within the limits\n", stderr);
        return 1;
    }
    if (what == 'e') {
        fwrite(p.out, 1, p.outlen, stdout);
        printf("-- %s\n", p.end);
    } else if (what == 'i') {
        fwrite(p.in, 1, p.inlen, stdout);
    } else if (what == 'a') {
        for (int k = 0; k < p.argc; k++)
            puts(p.argv[k]);
    } else {
        fputs(p.src, stdout);
    }
    limba_lxgen_free(&p);
    return 0;
}
