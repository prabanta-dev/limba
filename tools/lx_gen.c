/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lx_gen.c - print the random Luxia program of a seed, or (-e) what it
 * must print and how it must end, (-i) the input it reads, (-a) its
 * command line, an argument a line:
 *   lx_gen SEED > prog.luxia;  lx_gen -i SEED > prog.in;  lx_gen -e SEED
 * or (-d) write into directory DIR the same program in two files, its
 * routines that name nothing of it in the unit Lib (specification § 11):
 *   lx_gen -d DIR SEED
 * DIR/prog/prog.luxia and DIR/prog/lib.luxia, DIR/std/ (the units of the
 * library, none yet), DIR/expected (as -e), DIR/input (as -i) and
 * DIR/args (as -a); the directories must exist or be creatable.
 */
#include "luxia/gen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool write_file(const char *dir, const char *name, const char *b,
                       size_t n)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, n, f) != n || fclose(f) != 0) {
        perror(path);
        return false;
    }
    return true;
}

static bool make_dir(const char *path)
{
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        perror(path);
        return false;
    }
    return true;
}

static int write_dir(const char *dir, const limba_lxgen *p)
{
    char prog[4096], std[4096];
    snprintf(prog, sizeof(prog), "%s/prog", dir);
    snprintf(std, sizeof(std), "%s/std", dir);
    if (!make_dir(dir) || !make_dir(prog) || !make_dir(std))
        return 1;
    size_t elen = p->outlen + strlen(p->end) + 8;
    char *exp = malloc(elen);
    if (!exp)
        return 1;
    memcpy(exp, p->out, p->outlen);
    int n = snprintf(exp + p->outlen, elen - p->outlen, "-- %s\n", p->end);
    size_t alen = 0;
    for (int k = 0; k < p->argc; k++)
        alen += strlen(p->argv[k]) + 1;
    char *args = malloc(alen + 1), *at = args;
    if (!args) {
        free(exp);
        return 1;
    }
    for (int k = 0; k < p->argc; k++)
        at += sprintf(at, "%s\n", p->argv[k]);
    bool ok = write_file(prog, "prog.luxia", p->src, strlen(p->src));
    for (unsigned k = 0; ok && k < p->nfile; k++) {
        char name[32];
        snprintf(name, sizeof(name), "%s.luxia", p->file[k].name);
        ok = write_file(p->file[k].library ? std : prog, name, p->file[k].text,
                        strlen(p->file[k].text));
    }
    ok = ok && write_file(dir, "expected", exp, p->outlen + (size_t)n) &&
         write_file(dir, "input", p->in ? p->in : "", p->inlen) &&
         write_file(dir, "args", args, alen);
    free(exp);
    free(args);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "-d")) {
        limba_lxgen p;
        if (!limba_lxgen_make_units(strtoull(argv[3], NULL, 0), &p)) {
            fputs("lx_gen: no program within the limits\n", stderr);
            return 1;
        }
        int st = write_dir(argv[2], &p);
        limba_lxgen_free(&p);
        return st;
    }
    const char *o = argc == 3 ? argv[1] : "";
    char what = strlen(o) == 2 && o[0] == '-' && strchr("eia", o[1]) ? o[1] : 0;
    if (argc != 2 + (what != 0)) {
        fputs("usage: lx_gen [-e | -i | -a] SEED\n"
              "       lx_gen -d DIR SEED\n",
              stderr);
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
