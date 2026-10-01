/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * compile.c - the front end of Luxia as a library (limba/limba_luxia.h):
 * source, tokens, tree, semantic checks, then the IR a function at a
 * time, verified, optimised and given to the consumer.
 */
#include "limba/limba_luxia.h"

#include "front/diag.h"
#include "front/source.h"
#include "luxia/lex.h"
#include "luxia/lower.h"
#include "luxia/parse.h"
#include "luxia/sema.h"
#include "opt/pass.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* errors reported before giving up, by default */
#define MAX_ERRORS 20

/* a diagnosis of no place: printed as limba prints it, and given */
static void say(const limba_luxia_options *o, const limba_luxia_consumer *c,
                int sev, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void say(const limba_luxia_options *o, const limba_luxia_consumer *c,
                int sev, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (o->diag_out)
        fprintf(o->diag_out, "limba: %s%s\n",
                sev == LIMBA_LUXIA_WARNING ? "warning: " : "", msg);
    if (c->diag) {
        limba_luxia_diag d = {sev, "", NULL, 0, 0, 0, msg};
        c->diag(c->ctx, &d);
    }
}

/* the diagnoses of rep: printed, then given one by one */
static void flush(const limba_report *rep, const limba_luxia_options *o,
                  const limba_luxia_consumer *c)
{
    if (o->diag_out)
        limba_report_print(rep, o->diag_out);
    if (!c->diag)
        return;
    for (uint32_t i = 0; i < rep->count; i++) {
        const limba_report_item *it = &rep->item[i];
        limba_luxia_diag d = {it->sev == LIMBA_ERROR     ? LIMBA_LUXIA_ERROR
                              : it->sev == LIMBA_WARNING ? LIMBA_LUXIA_WARNING
                                                         : LIMBA_LUXIA_NOTE,
                              "",
                              NULL,
                              0,
                              0,
                              it->len,
                              it->msg};
        if (it->code)
            snprintf(d.code, sizeof(d.code), "%c%04u", rep->letter,
                     (unsigned)it->code);
        limba_where w;
        if (rep->src && limba_source_where(rep->src, it->loc, &w)) {
            d.file = rep->src->file[w.file].path;
            d.line = w.line;
            d.col = w.col;
        }
        c->diag(c->ctx, &d);
    }
    if (rep->full) {
        char msg[64];
        snprintf(msg, sizeof(msg), "stopped after %u errors", rep->errors);
        limba_luxia_diag d = {LIMBA_LUXIA_NOTE, "", NULL, 0, 0, 0, msg};
        c->diag(c->ctx, &d);
    }
}

/* each function as it is complete: verified, optimised, given, cleared,
   while it is still in the cache and before the next one needs the
   memory */
typedef struct {
    const limba_luxia_consumer *c;
    limba_verifier *v;
    limba_optimizer *z; /* NULL at level 0 */
    bool verify;
    bool bad;     /* the IR made is not valid */
    bool opt_bad; /* a pass broke it */
    bool stopped; /* the consumer asked to stop */
    bool begun;   /* begin was called */
    limba_diag d;
} stream;

static void stream_func(void *ctx, limba_module *m, limba_id fid, limba_edit *e)
{
    stream *st = ctx;
    if (st->bad || st->opt_bad || st->stopped) {
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
    const limba_luxia_consumer *c = st->c;
    if (!st->begun) {
        /* every function, global and extern is declared already */
        st->begun = true;
        if (c->begin)
            c->begin(c->ctx, m);
    }
    if (c->func && c->func(c->ctx, m, fid) != 0)
        st->stopped = true;
    if (!c->keep_bodies)
        limba_func_clear(&m->funcs[fid]);
}

/* the bits of the checks named in list, separated by commas; false if
   a name is no check */
static bool suppress_bits(const char *list, unsigned *bits,
                          const limba_luxia_options *o,
                          const limba_luxia_consumer *c)
{
    *bits = 0;
    while (list && *list) {
        const char *end = strchr(list, ',');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        unsigned b = lxs_check_bits(list, n);
        if (!b) {
            say(o, c, LIMBA_LUXIA_ERROR,
                "--suppress: '%.*s' is no check: index_check, "
                "range_check, overflow_check, division_check, "
                "conversion_check, shift_check, nil_check, "
                "dangling_check or all_checks",
                (int)n, list);
            return false;
        }
        *bits |= b;
        list = end ? end + 1 : NULL;
    }
    return true;
}

/* text NULL: the file at path */
static int compile(const char *path, const char *text, size_t len,
                   const limba_luxia_options *o, const limba_luxia_consumer *c,
                   limba_module **out)
{
    static const limba_luxia_options no_options;
    static const limba_luxia_consumer no_consumer;
    if (!o)
        o = &no_options;
    if (!c)
        c = &no_consumer;
    if (out)
        *out = NULL;
    unsigned off = 0;
    if (!suppress_bits(o->suppress, &off, o, c))
        return LIMBA_LUXIA_OPTIONS;
    int platform =
        o->target ? lxs_target_find(o->target, strlen(o->target)) : -1;
    if (o->target && platform < 0) {
        say(o, c, LIMBA_LUXIA_ERROR,
            "--target: '%s' is no platform: x86_64-linux, aarch64-linux or "
            "x86_64-windows",
            o->target);
        return LIMBA_LUXIA_OPTIONS;
    }
    if (off)
        say(o, c, LIMBA_LUXIA_WARNING,
            "--suppress turns checks off in the whole of %s: where one "
            "would fail, the behaviour is undefined",
            path);
    limba_source src;
    limba_source_init(&src);
    uint32_t file = text ? limba_source_add(&src, path, text, len)
                         : limba_source_load(&src, path);
    if (file == UINT32_MAX) {
        say(o, c, LIMBA_LUXIA_ERROR, "%s: %s", path, strerror(errno));
        limba_source_free(&src);
        return LIMBA_LUXIA_ERRORS;
    }
    limba_report rep;
    limba_report_init(&rep, &src, 'L',
                      o->max_errors ? o->max_errors : MAX_ERRORS);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &src, file, &rep);
    limba_lx_ast ast;
    limba_lx_ast_init(&ast);
    limba_lx_parse(&ast, &lx, &src, &rep);
    limba_lxs sema;
    bool checked = false;
    /* the tree of a program with syntax errors would give errors that
       are only their echo */
    if (rep.errors == 0) {
        limba_lxs_init(&sema, &ast, &lx, &src, &rep);
        sema.suppress = off;
        if (platform >= 0)
            sema.target = (unsigned)platform;
        sema.no_external = o->no_external;
        limba_lxs_check(&sema);
        checked = true;
    }
    flush(&rep, o, c);

    int status = rep.errors ? LIMBA_LUXIA_ERRORS : LIMBA_LUXIA_OK;
    if (status == LIMBA_LUXIA_OK) {
        /* what was told is not told again */
        limba_report_free(&rep);
        limba_report_init(&rep, &src, 'L',
                          o->max_errors ? o->max_errors : MAX_ERRORS);
        /* the IR a front end makes is verified in the builds for testing,
           and on request: in a release it is left to who reads it (as
           Clang leaves the verifier out), and to the tests */
        bool verify = o->verify || (o->opt && o->opt->verify_each);
#ifndef NDEBUG
        verify = true;
#endif
        stream st = {
            c,      NULL,  o->level > 0 ? limba_optimizer_new(o->opt) : NULL,
            verify, false, false,
            false,  false, {{0}, 0}};
        limba_module *m = limba_lxl_program_each(&sema, stream_func, &st);
        flush(&rep, o, c);
        if (m) {
            limba_diag d = {{0}, 0};
            bool bad = verify && (st.bad || limba_verify_decls(m, &d) != 0);
            if (st.bad || st.opt_bad)
                d = st.d;
            if (bad) {
                say(o, c, LIMBA_LUXIA_ERROR,
                    "%s: internal error, the IR made is not valid: %s", path,
                    d.msg);
                status = LIMBA_LUXIA_INTERNAL;
            } else if (st.opt_bad) {
                say(o, c, LIMBA_LUXIA_ERROR, "%s: %s", path, d.msg);
                status = LIMBA_LUXIA_INTERNAL;
            } else if (st.stopped) {
                status = LIMBA_LUXIA_STOPPED;
            }
        } else {
            status = LIMBA_LUXIA_ERRORS;
        }
        if (st.begun && c->end)
            c->end(c->ctx, status == LIMBA_LUXIA_OK ? m : NULL, status);
        if (status == LIMBA_LUXIA_OK && out)
            *out = m;
        else if (!o->no_free)
            limba_module_free(m);
        limba_verifier_free(st.v);
        limba_optimizer_free(st.z); /* the statistics, if asked */
    }
    limba_report_free(&rep);
    if (o->no_free)
        return status;
    if (checked)
        limba_lxs_free(&sema);
    limba_lx_ast_free(&ast);
    limba_lx_free(&lx);
    limba_source_free(&src);
    return status;
}

int limba_luxia_compile_file(const char *path, const limba_luxia_options *o,
                             const limba_luxia_consumer *c, limba_module **out)
{
    return compile(path, NULL, 0, o, c, out);
}

int limba_luxia_compile_text(const char *name, const char *text, size_t len,
                             const limba_luxia_options *o,
                             const limba_luxia_consumer *c, limba_module **out)
{
    return compile(name, text ? text : "", text ? len : 0, o, c, out);
}
