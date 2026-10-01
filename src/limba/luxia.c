/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * luxia.c - the limba program on a Luxia source: the IR through the
 * front end of limba/limba_luxia.h, written as each function is given;
 * --emit=tokens and --emit=ast print what the first steps made.
 */
#include "luxia.h"

#include "front/diag.h"
#include "front/source.h"
#include "limba/limba_luxia.h"
#include "luxia/lex.h"
#include "luxia/parse.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* errors reported before giving up */
#define MAX_ERRORS 20

/* write the module as text or binary; a .lir goes next to the input; wr,
   if not NULL, holds the functions written already, and is consumed */
static int write_module(const limba_module *m, const char *in, const char *emit,
                        const char *outpath, limba_writer *wr)
{
    int status = 0;
    if (emit && !strcmp(emit, "lit")) {
        FILE *out = outpath ? fopen(outpath, "w") : stdout;
        if (!out) {
            fprintf(stderr, "limba: %s: %s\n", outpath, strerror(errno));
            return 1;
        }
        limba_print(m, out);
        if (out != stdout && fclose(out) != 0)
            status = 1;
        return status;
    }
    char *derived = NULL;
    if (!outpath) {
        size_t n = strlen(in);
        derived = malloc(n + 1);
        if (!derived)
            return 1;
        memcpy(derived, in, n + 1);
        memcpy(derived + n - 6, ".lir", 5);
        outpath = derived;
    }
    uint8_t *buf;
    size_t blen;
    limba_writer_end(wr ? wr : limba_writer_new(), m, &buf, &blen);
    FILE *out = fopen(outpath, "wb");
    if (!out || fwrite(buf, 1, blen, out) != blen) {
        fprintf(stderr, "limba: %s: %s\n", outpath, strerror(errno));
        status = 1;
    }
    if (out && fclose(out) != 0)
        status = 1;
    free(buf);
    free(derived);
    return status;
}

/* each function as the front end gives it: into the .lir */
static int write_func(void *ctx, limba_module *m, limba_id fid)
{
    limba_writer_func(ctx, m, fid);
    return 0;
}

/* the tokens or the tree of a Luxia source, the front end used in part */
static int show(const char *in, bool tokens, bool check, const char *outpath)
{
    limba_source src;
    limba_source_init(&src);
    uint32_t file = limba_source_load(&src, in);
    if (file == UINT32_MAX) {
        fprintf(stderr, "limba: %s: %s\n", in, strerror(errno));
        limba_source_free(&src);
        return 1;
    }
    limba_report rep;
    limba_report_init(&rep, &src, 'L', MAX_ERRORS);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &src, file, &rep);
    limba_lx_ast ast;
    limba_lx_ast_init(&ast);
    if (!tokens)
        limba_lx_parse(&ast, &lx, &src, &rep);
    limba_report_print(&rep, stderr);
    int status = rep.errors ? 1 : 0;
    if (!check) {
        FILE *out = outpath ? fopen(outpath, "w") : stdout;
        if (!out) {
            fprintf(stderr, "limba: %s: %s\n", outpath, strerror(errno));
            status = 1;
        } else {
            if (tokens) {
                limba_lx_dump(out, &lx, &src);
            } else {
                limba_lx_ast_show(out, &ast, &lx, ast.root, 0);
                fputc('\n', out);
            }
            if (out != stdout && fclose(out) != 0)
                status = 1;
        }
    }
    limba_lx_ast_free(&ast);
    limba_lx_free(&lx);
    limba_source_free(&src);
    limba_report_free(&rep);
    return status;
}

int limba_luxia_main(const char *in, const char *emit, const char *outpath,
                     bool check, int level, const limba_opt_options *opt,
                     const char *suppress, const char *target,
                     const char *restrict_)
{
    if (restrict_ && strcmp(restrict_, "no_external")) {
        fprintf(stderr, "limba: --restrict: the restriction is no_external\n");
        return 2;
    }
    bool tokens = emit && !strcmp(emit, "tokens");
    if (tokens || (emit && !strcmp(emit, "ast")))
        return show(in, tokens, check, outpath);
    bool lit = emit && !strcmp(emit, "lit");
    limba_luxia_options o = {level, opt, check, suppress, target,
                             restrict_ != NULL, MAX_ERRORS, stderr,
#ifdef NDEBUG
                             /* the process ends now and gives the memory
                                back at once, as Clang's -disable-free:
                                freeing it piece by piece would cost 3 % */
                             true
#else
                             false
#endif
    };
    /* into a .lir, a function at a time; a .lit at the end, of the whole
       module */
    limba_writer *w = !check && !lit ? limba_writer_new() : NULL;
    limba_luxia_consumer c = {w, NULL, w ? write_func : NULL, NULL, NULL, lit};
    limba_module *m = NULL;
    int status = limba_luxia_compile_file(in, &o, &c, check ? NULL : &m);
    if (status == 0 && m) {
        status = write_module(m, in, emit, outpath, w);
        w = NULL;
        if (!o.no_free)
            limba_module_free(m);
    }
    limba_writer_free(w);
    return status;
}
