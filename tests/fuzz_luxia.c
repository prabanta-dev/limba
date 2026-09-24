/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fuzz_luxia.c - fuzzing target of the Luxia front end: lexer, parser,
 * semantic phase and generation of the IR, which must always verify.
 * Whatever the bytes, nothing may crash, the tokens must end with one EOF
 * token, and the tokens, the tree and the report must print.
 *
 * With clang: clang -fsanitize=fuzzer,address -DLIMBA_FUZZER ... builds a
 * libFuzzer program. Without, it is a driver that runs the files named on
 * its command line through the same function, to replay a corpus or a
 * crash.
 */
#include "front/diag.h"
#include "front/source.h"
#include "luxia/lex.h"
#include "luxia/parse.h"
#include "luxia/lower.h"
#include "luxia/sema.h"

#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static FILE *null;
    if (!null)
        null = fopen("/dev/null", "w");
    limba_source src;
    limba_source_init(&src);
    uint32_t f = limba_source_add(&src, "fuzz.luxia", (const char *)data, size);
    if (f != UINT32_MAX) {
        limba_report rep;
        limba_report_init(&rep, &src, 'L', 0);
        limba_lx lx;
        limba_lx_init(&lx);
        limba_lx_run(&lx, &src, f, &rep);
        if (lx.ntok == 0 || lx.tok[lx.ntok - 1].kind != LX_EOF)
            abort();
        limba_lx_ast t;
        limba_lx_ast_init(&t);
        limba_lx_parse(&t, &lx, &src, &rep);
        if (rep.errors == 0) {
            limba_lxs sema;
            limba_lxs_init(&sema, &t, &lx, &src, &rep);
            limba_lxs_check(&sema);
            limba_module *m = limba_lxl_program(&sema);
            if (m && limba_verify(m, NULL) != 0)
                abort(); /* the generator made an invalid IR */
            limba_module_free(m);
            limba_lxs_free(&sema);
        }
        if (null) {
            limba_lx_dump(null, &lx, &src);
            limba_lx_ast_show(null, &t, &lx, t.root, 0);
            limba_report_print(&rep, null);
        }
        limba_lx_ast_free(&t);
        limba_lx_free(&lx);
        limba_report_free(&rep);
    }
    limba_source_free(&src);
    return 0;
}
#ifndef LIMBA_FUZZER
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)n + 1);
        if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
            perror(argv[i]);
            return 1;
        }
        fclose(f);
        LLVMFuzzerTestOneInput(buf, (size_t)n);
        free(buf);
    }
    return 0;
}
#endif
