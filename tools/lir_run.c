/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lir_run.c - run a module with the reference interpreter, by hand:
 *
 *   lir_run [-O1] [--entry=name] file.lit|file.lir
 *
 * Prints what the program printed, then a line with how it ended and what
 * it returned, on standard error. Exit status: 0 returned, 1 anything else.
 */
#include "eval/eval.h"
#include "limba/ir.h"
#include "limba/opt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const status_text[] = {
    "returned",    "trap",      "unreachable", "limit",
    "unsupported", "bad entry", "halt",
};

int main(int argc, char **argv)
{
    const char *path = NULL, *entry = "main";
    bool opt = false;
    int first_arg = argc;
    for (int i = 1; i < argc && !path; i++) {
        if (!strcmp(argv[i], "-O1"))
            opt = true;
        else if (!strncmp(argv[i], "--entry=", 8))
            entry = argv[i] + 8;
        else {
            path = argv[i];
            first_arg = i + 1;
        }
    }
    if (!path) {
        fputs("usage: lir_run [-O1] [--entry=name] file.lit|file.lir "
              "[arguments of the program]\n",
              stderr);
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        return 1;
    }
    fclose(f);
    limba_diag d = {{0}, 0};
    size_t len = strlen(path);
    limba_module *m = len > 4 && !strcmp(path + len - 4, ".lir")
                          ? limba_read((const uint8_t *)buf, (size_t)n, &d)
                          : limba_parse(buf, (size_t)n, &d);
    free(buf);
    if (!m || limba_verify(m, &d) != 0 ||
        (opt && limba_optimize(m, NULL, &d) != 0)) {
        fprintf(stderr, "lir_run: %s: %s\n", path, d.msg);
        limba_module_free(m);
        return 1;
    }
    limba_eval_result r;
    /* the program reads standard input and its own arguments */
    limba_eval_limits lim = {0, 0, argc - first_arg, argv + first_arg, stdin};
    limba_eval(m, entry, &lim, &r);
    fwrite(r.out, 1, r.outlen, stdout);
    fprintf(stderr, "lir_run: %s", status_text[r.status]);
    if (r.status == LIMBA_EVAL_TRAP || r.status == LIMBA_EVAL_HALT)
        fprintf(stderr, " %" PRId64, r.code);
    if (r.pos && r.pos <= m->npos) {
        const limba_pos *p = &m->pos[r.pos - 1];
        size_t n;
        const char *file = limba_str(m, p->file, &n);
        fprintf(stderr, " at %.*s:%" PRIu32 ":%" PRIu32, (int)n, file, p->line,
                p->col);
    }
    if (r.status == LIMBA_EVAL_OK)
        fprintf(stderr, " %" PRId64, (int64_t)r.ret);
    fprintf(stderr, ", %" PRIu64 " steps\n", r.steps);
    int status = r.status == LIMBA_EVAL_OK     ? 0
                 : r.status == LIMBA_EVAL_HALT ? (int)r.code
                                               : 1;
    limba_eval_result_free(&r);
    limba_module_free(m);
    return status;
}
