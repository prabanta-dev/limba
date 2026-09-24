/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * luxia.c - the limba program on a Luxia source. For now the front end
 * stops at the syntax tree: --emit=tokens and --emit=ast print what it
 * made, and anything else only reports the errors.
 */
#include "luxia.h"

#include "front/diag.h"
#include "front/source.h"
#include "luxia/lex.h"
#include "luxia/parse.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* errors reported before giving up */
#define MAX_ERRORS 20

int limba_luxia_main(const char *in, const char *emit, const char *outpath,
                     bool check)
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
    bool tokens = emit && !strcmp(emit, "tokens");
    if (!tokens)
        limba_lx_parse(&ast, &lx, &src, &rep);
    limba_report_print(&rep, stderr);

    int status = rep.errors ? 1 : 0;
    if (!check && emit && (tokens || !strcmp(emit, "ast"))) {
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
    } else if (!check && status == 0) {
        fprintf(stderr,
                "limba: %s: the Luxia front end stops at the syntax tree "
                "for now (--emit=tokens, --emit=ast)\n",
                in);
        status = 2;
    }
    limba_lx_ast_free(&ast);
    limba_lx_free(&lx);
    limba_report_free(&rep);
    limba_source_free(&src);
    return status;
}
