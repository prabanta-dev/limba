/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_luxia_api.c - the front end of Luxia as a library, seen as a
 * consumer sees it, through limba/limba_luxia.h only: the order of the
 * calls, the module given back, the bodies freed or kept, the errors
 * found after some functions were given, a consumer that stops, and the
 * diagnoses.
 */
#include "limba/limba_luxia.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "test_luxia_api: line %d: ", __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* main is function 0 and calls Sum, but is given last */
static const char good[] = "program Calls;\n"
                           "\n"
                           "function Twice(x: Int32): Int32;\n"
                           "begin\n"
                           "  return 2 * x;\n"
                           "end Twice;\n"
                           "\n"
                           "function Sum(n: Int32): Int32;\n"
                           "begin\n"
                           "  var s: Int32 := 0;\n"
                           "  for var i := 1 to n do\n"
                           "    s := s + Twice(i);\n"
                           "  end;\n"
                           "  return s;\n"
                           "end Sum;\n"
                           "\n"
                           "begin\n"
                           "  writeln(Sum(10), \" \", \"done\");\n"
                           "end Calls.\n";

/* Second has a path without return: found when its body is made, after
   First was given */
static const char late[] = "program Late;\n"
                           "\n"
                           "function First(x: Int32): Int32;\n"
                           "begin\n"
                           "  return x + 1;\n"
                           "end First;\n"
                           "\n"
                           "function Second(x: Int32): Int32;\n"
                           "begin\n"
                           "  if x > 0 then\n"
                           "    return x;\n"
                           "  end;\n"
                           "end Second;\n"
                           "\n"
                           "begin\n"
                           "  writeln(First(1) + Second(2));\n"
                           "end Late.\n";

static const char syntax[] = "program Bad;\n"
                             "begin\n"
                             "  writeln(1 +);\n"
                             "end Bad.\n";

/* Sum calls Later, declared after it: given before Later's body is made */
static const char forward[] = "program Forward;\n"
                              "\n"
                              "function Sum(n: Int32): Int32;\n"
                              "begin\n"
                              "  var s: Int32 := 0;\n"
                              "  for var i := 1 to n do\n"
                              "    s := s + Later(i);\n"
                              "  end;\n"
                              "  return s;\n"
                              "end Sum;\n"
                              "\n"
                              "function Later(x: Int32): Int32;\n"
                              "begin\n"
                              "  return x * x;\n"
                              "end Later;\n"
                              "\n"
                              "begin\n"
                              "  writeln(Sum(4));\n"
                              "end Forward.\n";

/* four names never declared */
static const char many[] = "program Many;\n"
                           "begin\n"
                           "  writeln(a1);\n"
                           "  writeln(a2);\n"
                           "  writeln(a3);\n"
                           "  writeln(a4);\n"
                           "end Many.\n";

typedef struct {
    int begins, funcs, ends, diags;
    int func_before_begin; /* a function given before begin */
    int end_status;
    const limba_module *end_m;
    limba_id last_fid;
    int stop_after; /* func asks to stop after so many, 0 never */
    bool cleared;   /* every body was freed when end came */
    limba_writer *w;
    limba_luxia_diag first; /* the first diagnosis, its strings copied */
    char file[64], message[256];
    int last_severity; /* of the last diagnosis */
    char last[128];
    uint8_t given[16]; /* per function: given already */
    int forward_calls; /* calls of a function not given yet, its body
                          empty */
} seen;

static void on_begin(void *ctx, const limba_module *m)
{
    seen *s = ctx;
    s->begins++;
    (void)m;
}

static int on_func(void *ctx, limba_module *m, limba_id fid)
{
    seen *s = ctx;
    if (!s->begins)
        s->func_before_begin++;
    s->funcs++;
    s->last_fid = fid;
    const limba_func *f = &m->funcs[fid];
    for (uint32_t b = 0; b < f->nblocks; b++)
        for (uint32_t k = 0; k < f->blocks[b].ninsts; k++) {
            const limba_inst *x = &f->insts[f->blocks[b].insts[k]];
            limba_id g = (limba_id)x->imm;
            if (x->op == LIMBA_OP_CALL && g < 16 && !s->given[g] &&
                !m->funcs[g].nblocks)
                s->forward_calls++;
        }
    if (fid < 16)
        s->given[fid] = 1;
    if (s->w)
        limba_writer_func(s->w, m, fid);
    return s->stop_after && s->funcs >= s->stop_after;
}

static void on_end(void *ctx, const limba_module *m, int status)
{
    seen *s = ctx;
    s->ends++;
    s->end_m = m;
    s->end_status = status;
    s->cleared = true;
    for (uint32_t i = 0; m && i < m->nfuncs; i++)
        if (m->funcs[i].nblocks)
            s->cleared = false;
}

static void on_diag(void *ctx, const limba_luxia_diag *d)
{
    seen *s = ctx;
    s->last_severity = d->severity;
    snprintf(s->last, sizeof(s->last), "%s", d->message);
    if (!s->diags++) {
        s->first = *d;
        snprintf(s->file, sizeof(s->file), "%s", d->file ? d->file : "");
        snprintf(s->message, sizeof(s->message), "%s", d->message);
        s->first.file = d->file ? s->file : NULL;
        s->first.message = s->message;
    }
}

static limba_luxia_consumer consumer(seen *s, bool keep)
{
    memset(s, 0, sizeof(*s));
    s->last_fid = LIMBA_NONE;
    limba_luxia_consumer c = {s, on_begin, on_func, on_end, on_diag, keep};
    return c;
}

/* the module, kept whole, and the .lir written a function at a time:
   the same bytes; the calls in order */
static void test_good(int level)
{
    limba_luxia_options o = {0};
    o.level = level;
    seen s;
    limba_luxia_consumer c = consumer(&s, true);
    limba_module *m = NULL;
    int r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1, &o,
                                     &c, &m);
    CHECK(r == LIMBA_LUXIA_OK, "-O%d: result %d", level, r);
    CHECK(m != NULL, "-O%d: no module", level);
    if (!m)
        return;
    CHECK(s.begins == 1 && s.ends == 1, "-O%d: begin %d, end %d", level,
          s.begins, s.ends);
    CHECK(!s.func_before_begin, "-O%d: a function before begin", level);
    CHECK(s.funcs == (int)m->nfuncs, "-O%d: %d functions given of %u", level,
          s.funcs, m->nfuncs);
    CHECK(s.last_fid == 0, "-O%d: main given as %u, not last", level,
          s.last_fid);
    CHECK(s.end_m == m && s.end_status == LIMBA_LUXIA_OK,
          "-O%d: end with the module %p (%d)", level, (void *)s.end_m,
          s.end_status);
    CHECK(!s.cleared, "-O%d: bodies freed with keep_bodies", level);
    CHECK(!s.diags, "-O%d: %d diagnoses: %s", level, s.diags, s.message);
    uint8_t *whole;
    size_t nwhole;
    limba_write(m, &whole, &nwhole);
    limba_module_free(m);

    c = consumer(&s, false);
    s.w = limba_writer_new();
    r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1, &o, &c,
                                 &m);
    CHECK(r == LIMBA_LUXIA_OK && m, "-O%d: result %d", level, r);
    CHECK(s.cleared, "-O%d: bodies kept without keep_bodies", level);
    if (m) {
        uint8_t *parts;
        size_t nparts;
        limba_writer_end(s.w, m, &parts, &nparts);
        CHECK(nparts == nwhole && !memcmp(parts, whole, nwhole),
              "-O%d: the .lir a function at a time differs (%zu, %zu "
              "bytes)",
              level, nparts, nwhole);
        free(parts);
        limba_module_free(m);
    } else {
        limba_writer_free(s.w);
    }
    free(whole);

    /* no consumer, no options */
    r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1, NULL,
                                 NULL, &m);
    CHECK(r == LIMBA_LUXIA_OK && m, "no consumer: result %d", r);
    limba_module_free(m);
}

/* an error after a function was given: end throws away, out is NULL */
static void test_late(void)
{
    seen s;
    limba_luxia_consumer c = consumer(&s, false);
    limba_module *m = (limba_module *)&s;
    int r = limba_luxia_compile_text("late.luxia", late, sizeof(late) - 1, NULL,
                                     &c, &m);
    CHECK(r == LIMBA_LUXIA_ERRORS, "late: result %d", r);
    CHECK(m == NULL, "late: a module given back");
    CHECK(s.funcs == 1, "late: %d functions given, not First alone", s.funcs);
    CHECK(s.begins == 1 && s.ends == 1, "late: begin %d, end %d", s.begins,
          s.ends);
    CHECK(s.end_m == NULL && s.end_status == LIMBA_LUXIA_ERRORS,
          "late: end with %p (%d)", (void *)s.end_m, s.end_status);
    CHECK(s.diags == 1 && s.first.severity == LIMBA_LUXIA_ERROR &&
              !strcmp(s.first.code, "L0052") && !strcmp(s.file, "late.luxia") &&
              s.first.line == 8,
          "late: %d diagnoses, the first %s %s:%u:%u %s", s.diags, s.first.code,
          s.file, s.first.line, s.first.col, s.message);
}

/* a consumer that stops at the first function */
static void test_stop(void)
{
    seen s;
    limba_luxia_consumer c = consumer(&s, false);
    s.stop_after = 1;
    limba_module *m = NULL;
    int r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1,
                                     NULL, &c, &m);
    CHECK(r == LIMBA_LUXIA_STOPPED, "stop: result %d", r);
    CHECK(m == NULL && s.funcs == 1, "stop: module %p, %d functions", (void *)m,
          s.funcs);
    CHECK(s.ends == 1 && s.end_m == NULL && s.end_status == LIMBA_LUXIA_STOPPED,
          "stop: end %d with %p (%d)", s.ends, (void *)s.end_m, s.end_status);
}

/* a stop ends the making of bodies: the error of Second, after First,
   is never found */
static void test_stop_early(void)
{
    seen s;
    limba_luxia_consumer c = consumer(&s, false);
    s.stop_after = 1;
    limba_module *m = NULL;
    int r = limba_luxia_compile_text("late.luxia", late, sizeof(late) - 1, NULL,
                                     &c, &m);
    CHECK(r == LIMBA_LUXIA_STOPPED && !m, "stop early: result %d", r);
    CHECK(s.funcs == 1 && !s.diags,
          "stop early: %d functions, %d diagnoses (%s)", s.funcs, s.diags,
          s.message);
}

/* a call of a routine declared later: its body is not made yet when the
   caller is given (calls are resolved by index, afterwards) */
static void test_forward(int level)
{
    limba_luxia_options o = {0};
    o.level = level;
    seen s;
    limba_luxia_consumer c = consumer(&s, false);
    limba_module *m = NULL;
    int r = limba_luxia_compile_text("forward.luxia", forward,
                                     sizeof(forward) - 1, &o, &c, &m);
    CHECK(r == LIMBA_LUXIA_OK && m, "forward -O%d: result %d", level, r);
    CHECK(s.forward_calls >= 1,
          "forward -O%d: no call of a function not given yet", level);
    CHECK(m && s.funcs == (int)m->nfuncs && s.last_fid == 0,
          "forward -O%d: %d functions, the last %u", level, s.funcs,
          s.last_fid);
    limba_module_free(m);
}

/* the options a consumer may leave at their defaults */
static void test_options(void)
{
    /* the errors after max_errors are dropped, and a note says so */
    limba_luxia_options o = {0};
    o.max_errors = 2;
    seen s;
    limba_luxia_consumer c = consumer(&s, false);
    limba_module *m = NULL;
    int r = limba_luxia_compile_text("many.luxia", many, sizeof(many) - 1, &o,
                                     &c, &m);
    CHECK(r == LIMBA_LUXIA_ERRORS && !m, "max_errors: result %d", r);
    CHECK(s.diags == 3 && s.last_severity == LIMBA_LUXIA_NOTE &&
              strstr(s.last, "stopped after 2 errors"),
          "max_errors: %d diagnoses, the last '%s'", s.diags, s.last);

    /* printed as limba prints them */
    char *text = NULL;
    size_t ntext = 0;
    o.max_errors = 0;
    o.diag_out = open_memstream(&text, &ntext);
    r = limba_luxia_compile_text("bad.luxia", syntax, sizeof(syntax) - 1, &o,
                                 NULL, &m);
    fclose(o.diag_out);
    CHECK(r == LIMBA_LUXIA_ERRORS && text && strstr(text, "bad.luxia:3:") &&
              strstr(text, ": error[L") && strstr(text, "writeln(1 +);"),
          "diag_out: result %d, printed '%s'", r, text ? text : "");
    free(text);

    /* verified on request, and an empty text */
    o.diag_out = NULL;
    o.verify = true;
    r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1, &o,
                                 NULL, &m);
    CHECK(r == LIMBA_LUXIA_OK && m, "verify: result %d", r);
    limba_module_free(m);
    r = limba_luxia_compile_text("empty.luxia", NULL, 0, NULL, NULL, &m);
    CHECK(r == LIMBA_LUXIA_ERRORS && !m, "empty: result %d", r);

#ifndef __SANITIZE_ADDRESS__
    /* the memory of the compilation is left, the module is given back
       (a leak by design: not in the builds that look for leaks) */
    o.verify = false;
    o.no_free = true;
    r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1, &o,
                                 NULL, &m);
    CHECK(r == LIMBA_LUXIA_OK && m && m->nfuncs == 3, "no_free: result %d", r);
    limba_module_free(m);
#endif
}

/* errors before any function: neither begin nor end */
static void test_before(void)
{
    seen s;
    limba_luxia_consumer c = consumer(&s, false);
    limba_module *m = NULL;
    int r = limba_luxia_compile_text("bad.luxia", syntax, sizeof(syntax) - 1,
                                     NULL, &c, &m);
    CHECK(r == LIMBA_LUXIA_ERRORS && !m, "syntax: result %d", r);
    CHECK(!s.begins && !s.ends && !s.funcs, "syntax: begin %d, end %d",
          s.begins, s.ends);
    CHECK(s.diags >= 1 && s.first.code[0] == 'L' &&
              !strcmp(s.file, "bad.luxia") && s.first.line == 3 &&
              s.first.col > 1,
          "syntax: %d diagnoses, the first %s %s:%u:%u", s.diags, s.first.code,
          s.file, s.first.line, s.first.col);

    limba_luxia_options o = {0};
    o.suppress = "index_check,no_such_check";
    c = consumer(&s, false);
    r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1, &o, &c,
                                 &m);
    CHECK(r == LIMBA_LUXIA_OPTIONS && !m, "suppress: result %d", r);
    CHECK(s.diags == 1 && !s.first.code[0] && !s.first.file &&
              strstr(s.message, "no_such_check"),
          "suppress: %d diagnoses, the first '%s'", s.diags, s.message);

    o.suppress = NULL;
    o.target = "pdp11-unix";
    c = consumer(&s, false);
    r = limba_luxia_compile_text("calls.luxia", good, sizeof(good) - 1, &o, &c,
                                 &m);
    CHECK(r == LIMBA_LUXIA_OPTIONS && s.diags == 1 && !s.begins,
          "target: result %d, %d diagnoses", r, s.diags);

    c = consumer(&s, false);
    r = limba_luxia_compile_file("no/such/file.luxia", NULL, &c, &m);
    CHECK(r == LIMBA_LUXIA_ERRORS && !m && s.diags == 1 &&
              strstr(s.message, "no/such/file.luxia"),
          "file: result %d, %d diagnoses '%s'", r, s.diags, s.message);
}

int main(void)
{
    test_good(0);
    test_good(1);
    test_late();
    test_stop();
    test_stop_early();
    test_forward(0);
    test_forward(1);
    test_options();
    test_before();
    printf(
        "test_luxia_api: the calls, the module, the bodies, errors after "
        "functions given, stop, calls of later functions, options, diagnoses, "
        "%d failures\n",
        failures);
    return failures ? 1 : 0;
}
