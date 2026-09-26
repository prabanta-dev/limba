/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * luxia.c - the limba program on a Luxia source. For now the front end
 * stops after the semantic checks: --emit=tokens and --emit=ast print
 * what it made, and anything else only reports the errors.
 */
#include "luxia.h"

#include "front/diag.h"
#include "front/source.h"
#include "luxia/lex.h"
#include "luxia/parse.h"
#include "luxia/lower.h"
#include "luxia/sema.h"
#include "opt/pass.h"

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

/* what the program allocated, freed in the builds for testing only (see
   the end of limba_luxia_main) */
#ifdef NDEBUG
#define release(x, call) ((void)(x))
#else
#define release(x, call) (call)
#endif

/* each function as it is complete: verified, optimised, written, cleared,
   while it is still in the cache and before the next one needs the
   memory */
typedef struct {
    limba_writer *w;
    limba_verifier *v;
    limba_optimizer *z; /* NULL at -O0 */
    bool verify;
    bool bad;     /* the IR made is not valid */
    bool opt_bad; /* a pass broke it */
    limba_diag d;
} stream;

static void stream_func(void *ctx, limba_module *m, limba_id fid, limba_edit *e)
{
    stream *st = ctx;
    if (st->bad || st->opt_bad) {
        limba_edit_cancel(e);
        return;
    }
    if (st->z && !st->verify) {
        /* the last edit of the SSA and those of the passes, applied once */
        if (limba_optimizer_func_edit(st->z, m, fid, e, &st->d) != 0) {
            st->opt_bad = true;
            return;
        }
    } else {
        limba_edit_end(e);
        if (st->verify && !st->v)
            st->v = limba_verifier_new(m);
        if (st->verify && limba_verifier_func(st->v, fid, &st->d) != 0) {
            st->bad = true;
            return;
        }
        if (st->z && limba_optimizer_func(st->z, m, fid, &st->d) != 0) {
            st->opt_bad = true;
            return;
        }
    }
    limba_writer_func(st->w, m, fid);
    limba_func_clear(&m->funcs[fid]);
}

/* the bits of the checks named in list, separated by commas; false if
   a name is no check */
static bool suppress_bits(const char *list, unsigned *bits)
{
    *bits = 0;
    while (list && *list) {
        const char *end = strchr(list, ',');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        unsigned b = lxs_check_bits(list, n);
        if (!b) {
            fprintf(stderr,
                    "limba: --suppress: '%.*s' is no check: index_check, "
                    "range_check, overflow_check, division_check, "
                    "conversion_check, shift_check, nil_check or "
                    "all_checks\n",
                    (int)n, list);
            return false;
        }
        *bits |= b;
        list = end ? end + 1 : NULL;
    }
    return true;
}

int limba_luxia_main(const char *in, const char *emit, const char *outpath,
                     bool check, int level, const limba_opt_options *opt,
                     const char *suppress)
{
    unsigned off = 0;
    if (!suppress_bits(suppress, &off))
        return 2;
    if (off)
        fprintf(stderr,
                "limba: warning: --suppress turns checks off in the whole "
                "of %s: where one would fail, the behaviour is undefined\n",
                in);
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
    limba_lxs sema;
    bool checked = false;
    if (!tokens) {
        limba_lx_parse(&ast, &lx, &src, &rep);
        /* the tree of a program with syntax errors would give errors
           that are only their echo */
        if (rep.errors == 0 && !(emit && !strcmp(emit, "ast"))) {
            limba_lxs_init(&sema, &ast, &lx, &src, &rep);
            sema.suppress = off;
            limba_lxs_check(&sema);
            checked = true;
        }
    }
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
    } else if (checked && status == 0) {
        /* what was printed is not printed again */
        limba_report_free(&rep);
        limba_report_init(&rep, &src, 'L', MAX_ERRORS);
        /* into a .lir, a function at a time */
        bool each = !check && !(emit && !strcmp(emit, "lit"));
        /* the IR a front end makes is verified in the builds for testing,
           and on request: in a release it is left to who reads it (as
           Clang leaves the verifier out), and to the tests */
        bool verify = check || (opt && opt->verify_each);
#ifndef NDEBUG
        verify = true;
#endif
        stream st = {each ? limba_writer_new() : NULL,
                     NULL,
                     each && level > 0 ? limba_optimizer_new(opt) : NULL,
                     verify,
                     false,
                     false,
                     {{0}, 0}};
        limba_module *m = each ? limba_lxl_program_each(&sema, stream_func, &st)
                               : limba_lxl_program(&sema);
        if (m) {
            limba_report_print(&rep, stderr);
            limba_diag d = {{0}, 0};
            bool bad = !verify ? false
                       : each  ? st.bad || limba_verify_decls(m, &d) != 0
                               : limba_verify(m, &d) != 0;
            if (st.bad || st.opt_bad)
                d = st.d;
            if (bad) {
                fprintf(stderr,
                        "limba: %s: internal error, the IR made is "
                        "not valid: %s\n",
                        in, d.msg);
                status = 3;
            } else if (st.opt_bad || (level > 0 && !each &&
                                      limba_optimize(m, opt, &d) != 0)) {
                fprintf(stderr, "limba: %s: %s\n", in, d.msg);
                status = 3;
            } else if (!check) {
                status = write_module(m, in, emit, outpath, st.w);
                st.w = NULL;
            }
            release(m, limba_module_free(m));
        } else {
            limba_report_print(&rep, stderr);
            status = 1;
        }
        limba_writer_free(st.w);
        limba_verifier_free(st.v);
        limba_optimizer_free(st.z); /* the statistics, if asked */
    }
#ifdef NDEBUG
    /* the process ends now and gives the memory back at once, as Clang's
       -disable-free: freeing it piece by piece would cost 3 % */
    (void)ast;
    (void)lx;
    (void)src;
#else
    if (checked)
        limba_lxs_free(&sema);
    limba_lx_ast_free(&ast);
    limba_lx_free(&lx);
    limba_source_free(&src);
#endif
    limba_report_free(&rep);
    return status;
}
