/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * compile.c - the front end of Luxia as a library (limba/limba_luxia.h):
 * source, tokens, tree, semantic checks, then the IR a function at a
 * time, verified, optimised and given to the consumer.
 */
#include "limba/limba_luxia.h"

#include "common/xalloc.h"
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
#include <stdlib.h>
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

/* false to stop: the IR is not valid, or the consumer asked; the front
   end then makes no other body */
static bool stream_func(void *ctx, limba_module *m, limba_id fid, limba_edit *e)
{
    stream *st = ctx;
    if (st->z && !st->verify) {
        /* the last edit of the SSA and those of the passes, applied once */
        if (limba_optimizer_func_edit(st->z, m, fid, e, &st->d) != 0) {
            st->opt_bad = true;
            return false;
        }
    } else {
        limba_edit_end(e);
        if (st->verify && !st->v)
            st->v = limba_verifier_new(m);
        if (st->verify && limba_verifier_func(st->v, fid, &st->d) != 0) {
            st->bad = true;
            return false;
        }
        if (st->z && limba_optimizer_func(st->z, m, fid, &st->d) != 0) {
            st->opt_bad = true;
            return false;
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
    return !st->stopped;
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
/* ---- the units (specification § 11) ---- */

typedef struct {
    uint32_t root, first, last;
    bool library;
} unit_file;

typedef struct {
    const limba_luxia_options *o;
    limba_source *src;
    limba_report *rep;
    limba_lx *lx;
    limba_lx_ast *ast;
    char *dir; /* the directory of the program, "" for the current one */
    unit_file *units;
    uint32_t nunits, cap;
} loader;

/* lex and parse file f, a new file of the program */
static uint32_t add_file(loader *ld, uint32_t f, bool library)
{
    uint32_t tok = ld->lx->ntok;
    limba_lx_run(ld->lx, ld->src, f, ld->rep);
    uint32_t first = ld->ast->nnode;
    uint32_t root = limba_lx_parse_at(ld->ast, ld->lx, ld->src, ld->rep, tok);
    LIMBA_GROW(ld->units, ld->nunits, ld->cap);
    ld->units[ld->nunits] = (unit_file){root, first, ld->ast->nnode, library};
    return ld->nunits++;
}

static void set_irname(limba_source *src, uint32_t f, const char *name)
{
    size_t n = strlen(name);
    src->file[f].irname = limba_xmalloc(n + 1);
    memcpy(src->file[f].irname, name, n + 1);
}

/* the file of unit name in one space: the reader, then the directories;
   the index of the file in the source, UINT32_MAX if absent (unreadable:
   reported at ref, *bad set) */
static uint32_t find_file(loader *ld, bool library, const char *name,
                          uint32_t ref, bool *bad)
{
    const limba_luxia_options *o = ld->o;
    if (o->read_unit) {
        const char *text = NULL, *fpath = NULL;
        size_t n = 0;
        int r = o->read_unit(o->read_ctx,
                             library ? LIMBA_LUXIA_STDLIB_UNIT
                                     : LIMBA_LUXIA_PROGRAM_UNIT,
                             name, &text, &n, &fpath);
        if (r == LIMBA_LUXIA_UNIT_FOUND) {
            uint32_t f = limba_source_add(ld->src, fpath ? fpath : name,
                                          text ? text : "", text ? n : 0);
            if (f != UINT32_MAX)
                return f;
        }
        if (r != LIMBA_LUXIA_UNIT_ABSENT) {
            limba_report_add(ld->rep, LIMBA_ERROR, LXE_UNIT_NOT_FOUND,
                             ld->ast->node[ref].loc, (uint32_t)strlen(name),
                             "the unit '%s' cannot be read", name);
            *bad = true;
            return UINT32_MAX;
        }
    }
    /* the directories: the program's then -I, or the library's */
    uint32_t ndirs = 0;
    if (library) {
        ndirs = o->stdlib_path ? 1 : 0;
    } else {
        ndirs = 1;
        while (o->unit_path && o->unit_path[ndirs - 1])
            ndirs++;
    }
    for (uint32_t k = 0; k < ndirs; k++) {
        const char *dir = library  ? o->stdlib_path
                          : k == 0 ? ld->dir
                                   : o->unit_path[k - 1];
        char fpath[4096];
        int n = snprintf(fpath, sizeof(fpath), "%s%s%s.luxia", dir,
                         *dir && dir[strlen(dir) - 1] != '/' ? "/" : "", name);
        if (n < 0 || (size_t)n >= sizeof(fpath))
            continue;
        uint32_t f = limba_source_load(ld->src, fpath);
        if (f == UINT32_MAX) {
            if (errno == ENOENT || errno == ENOTDIR)
                continue;
            int err = errno;
            char why[128];
            if (strerror_r(err, why, sizeof(why)) != 0)
                snprintf(why, sizeof(why), "error %d", err);
            limba_report_add(ld->rep, LIMBA_ERROR, LXE_UNIT_NOT_FOUND,
                             ld->ast->node[ref].loc, (uint32_t)strlen(name),
                             "%s: %s", fpath, why);
            *bad = true;
            return UINT32_MAX;
        }
        /* its name in the IR: stable, wherever the program is (§ 7 of
           the proposal) */
        char irn[512];
        if (library)
            snprintf(irn, sizeof(irn), "<std>/%s.luxia", name);
        else if (k == 0)
            snprintf(irn, sizeof(irn), "%s.luxia", name);
        else
            snprintf(irn, sizeof(irn), "<I%u>/%s.luxia", k, name);
        set_irname(ld->src, f, irn);
        return f;
    }
    return UINT32_MAX;
}

/* the name at a place, as written */
static const char *written(loader *ld, limba_loc loc, uint32_t *n)
{
    limba_where w;
    *n = 0;
    if (!limba_source_where(ld->src, loc, &w))
        return "";
    const limba_srcfile *f = &ld->src->file[w.file];
    const char *s = f->text + (loc - f->base);
    while (s + *n < f->text + f->len &&
           ((s[*n] >= 'a' && s[*n] <= 'z') || (s[*n] >= 'A' && s[*n] <= 'Z') ||
            (s[*n] >= '0' && s[*n] <= '9') || s[*n] == '_'))
        (*n)++;
    return s;
}

/* does the library have a unit of this name (read nothing more) */
static bool library_has(loader *ld, const char *name)
{
    const limba_luxia_options *o = ld->o;
    if (o->read_unit) {
        const char *text = NULL, *fpath = NULL;
        size_t n = 0;
        if (o->read_unit(o->read_ctx, LIMBA_LUXIA_STDLIB_UNIT, name, &text, &n,
                         &fpath) == LIMBA_LUXIA_UNIT_FOUND)
            return true;
    }
    if (!o->stdlib_path)
        return false;
    char fpath[4096];
    size_t dl = strlen(o->stdlib_path);
    int k = snprintf(fpath, sizeof(fpath), "%s%s%s.luxia", o->stdlib_path,
                     dl && o->stdlib_path[dl - 1] != '/' ? "/" : "", name);
    if (k < 0 || (size_t)k >= sizeof(fpath))
        return false;
    FILE *f = fopen(fpath, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* the unit a REF of a uses names, from file u: loaded if it is not yet;
   its index + 1, 0 if not found (reported) */
static uint32_t resolve_use(loader *ld, uint32_t u, uint32_t ref)
{
    size_t n;
    const char *name =
        limba_strtab_get(ld->lx->names, ld->ast->node[ref].a, &n);
    bool library = ld->units[u].library;
    for (int space = library ? 1 : 0; space < 2; space++) {
        /* a unit of this space already there */
        for (uint32_t v = 0; v < ld->nunits; v++) {
            const limba_lx_node *r = &ld->ast->node[ld->units[v].root];
            if (ld->units[v].root && r->kind == LXN_UNIT && r->a &&
                ld->ast->node[r->a].a == ld->ast->node[ref].a &&
                ld->units[v].library == (space == 1))
                return v + 1;
        }
        bool bad = false;
        uint32_t f = find_file(ld, space == 1, name, ref, &bad);
        if (bad)
            return 0;
        if (f == UINT32_MAX)
            continue;
        uint32_t v = add_file(ld, f, space == 1);
        const limba_lx_node *r = &ld->ast->node[ld->units[v].root];
        if (!ld->units[v].root)
            return 0; /* not read: the lexer said why */
        if (r->kind != LXN_UNIT) {
            limba_report_add(ld->rep, LIMBA_ERROR, LXE_UNIT_FILE,
                             ld->ast->node[ref].loc, (uint32_t)n,
                             "%s is a program, not a unit",
                             ld->src->file[f].path);
            ld->units[v].root = 0;
            return 0;
        }
        if (!r->a || ld->ast->node[r->a].a != ld->ast->node[ref].a) {
            uint32_t nh = 0, nw;
            const char *held =
                r->a ? written(ld, ld->ast->node[r->a].loc, &nh) : "";
            const char *want = written(ld, ld->ast->node[ref].loc, &nw);
            limba_report_add(ld->rep, LIMBA_ERROR, LXE_UNIT_FILE, r->loc, 4,
                             "%s holds the unit '%.*s', not '%.*s': a unit "
                             "is in the file of its name, in lowercase",
                             ld->src->file[f].path, (int)nh, held, (int)nw,
                             want);
            ld->units[v].root = 0;
            return 0;
        }
        if (space == 0 && library_has(ld, name)) {
            /* the program's, a unit the library has too: two units, the
               library keeps its own (§ 11.4) */
            uint32_t nw;
            const char *want = written(ld, ld->ast->node[ref].loc, &nw);
            limba_report_add(ld->rep, LIMBA_NOTE, 0, ld->ast->node[ref].loc,
                             (uint32_t)n,
                             "'%.*s' is a unit of the program; the library "
                             "has one of the same name, which it keeps for "
                             "itself",
                             (int)nw, want);
        }
        return v + 1;
    }
    uint32_t nw;
    const char *want = written(ld, ld->ast->node[ref].loc, &nw);
    limba_report_add(ld->rep, LIMBA_ERROR, LXE_UNIT_NOT_FOUND,
                     ld->ast->node[ref].loc, (uint32_t)n,
                     "the unit '%.*s' is not found: no %s.luxia in the "
                     "directory of the program%s%s",
                     (int)nw, want, name,
                     ld->o->unit_path && ld->o->unit_path[0]
                         ? ", in the directories of -I"
                         : "",
                     ld->o->stdlib_path ? " nor in the library" : "");
    return 0;
}

/* the units the files use, loaded one after the other (§ 11.4) */
static void load_units(loader *ld)
{
    for (uint32_t u = 0; u < ld->nunits; u++) {
        if (!ld->units[u].root)
            continue;
        const limba_lx_node *r = &ld->ast->node[ld->units[u].root];
        uint32_t lists[2] = {r->b, r->kind == LXN_UNIT ? r->c : 0};
        for (int k = 0; k < 2; k++) {
            uint32_t l = lists[k];
            if (!l || !ld->ast->node[l].b)
                continue;
            uint32_t first = limba_lx_list_at(ld->ast, l, 0);
            if (ld->ast->node[first].kind != LXN_USES)
                continue;
            uint32_t names = ld->ast->node[first].a;
            for (uint32_t i = 0; i < ld->ast->node[names].b; i++) {
                uint32_t ref = limba_lx_list_at(ld->ast, names, i);
                uint32_t v = resolve_use(ld, u, ref);
                ld->ast->node[ref].b = v;
                /* the table of the tree may have moved */
                r = &ld->ast->node[ld->units[u].root];
            }
        }
    }
}

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
        /* strerror may share a buffer between threads */
        int err = errno;
        char why[128];
        if (strerror_r(err, why, sizeof(why)) != 0)
            snprintf(why, sizeof(why), "error %d", err);
        say(o, c, LIMBA_LUXIA_ERROR, "%s: %s", path, why);
        limba_source_free(&src);
        return LIMBA_LUXIA_ERRORS;
    }
    limba_report rep;
    limba_report_init(&rep, &src, 'L',
                      o->max_errors ? o->max_errors : MAX_ERRORS);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_ast ast;
    limba_lx_ast_init(&ast);
    /* the program, then the units it uses, into one tree */
    loader ld = {o, &src, &rep, &lx, &ast, NULL, NULL, 0, 0};
    {
        const char *slash = strrchr(path, '/');
        size_t dn = slash ? (size_t)(slash - path) + 1 : 0;
        ld.dir = limba_xmalloc(dn + 1);
        memcpy(ld.dir, path, dn);
        ld.dir[dn] = 0;
    }
    add_file(&ld, file, false);
    ast.root = ld.units[0].root;
    if (rep.errors == 0)
        load_units(&ld);
    ast.root = ld.units[0].root; /* the file compiled, not the last read */
    bool is_unit = ast.root && ast.node[ast.root].kind == LXN_UNIT;
    limba_lxs sema;
    bool checked = false;
    /* the tree of a program with syntax errors would give errors that
       are only their echo */
    if (rep.errors == 0) {
        limba_lxs_init(&sema, &ast, &lx, &src, &rep);
        for (uint32_t u = 0; u < ld.nunits; u++)
            limba_lxs_unit_add(&sema, ld.units[u].root, ld.units[u].first,
                               ld.units[u].last, ld.units[u].library);
        sema.main = 0;
        sema.suppress = off;
        if (platform >= 0)
            sema.target = (unsigned)platform;
        sema.no_external = o->no_external;
        limba_lxs_check(&sema);
        checked = true;
    }
    flush(&rep, o, c);

    int status = rep.errors ? LIMBA_LUXIA_ERRORS : LIMBA_LUXIA_OK;
    /* a unit alone: its errors that the IR finds, nothing given */
    limba_luxia_consumer quiet = *c;
    if (is_unit) {
        quiet.begin = NULL;
        quiet.func = NULL;
        quiet.end = NULL;
        c = &quiet;
        out = NULL;
    }
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
        if (m && st.stopped) {
            /* the module is not complete: nothing to verify */
            status = LIMBA_LUXIA_STOPPED;
        } else if (m) {
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
    free(ld.units);
    free(ld.dir);
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
