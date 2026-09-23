/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_eval.c - the reference interpreter, and the optimiser judged by it.
 * Run from the root of the repository (build.sh test does); it reads tests/
 * and writes nothing.
 *
 * tests/eval, every .lit: the interpreter itself, against what its header
 *     says: "; output: <line>" per printed line, "; result: <n>" or
 *     "; trap: <code>". An oracle is checked before it judges.
 * tests/eval, tests/ir/ok, tests/opt (before ";; expect"), every .lit with
 *     a @main: the same program not optimised, fully optimised, and with
 *     each pass alone must print the same, end the same way and return the
 *     same value (the OPTDIFF net of SedaiBasic2).
 */
#define _GNU_SOURCE
#include "eval/eval.h"
#include "limba/ir.h"
#include "limba/opt.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, compared, skipped;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/* the module part of a file: everything before ";; expect" */
static limba_module *load(const char *path, char **text)
{
    size_t len = 0;
    *text = slurp(path, &len);
    CHECK(*text, "%s: cannot read", path);
    if (!*text)
        return NULL;
    char *sep = strstr(*text, "\n;; expect");
    if (sep)
        len = (size_t)(sep - *text) + 1;
    limba_diag d = {{0}, 0};
    limba_module *m = limba_parse(*text, len, &d);
    CHECK(m && limba_verify(m, &d) == 0, "%s: %s", path, d.msg);
    return m;
}

static bool has_main(const limba_module *m)
{
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        size_t n;
        const char *s = limba_str(m, m->funcs[i].name, &n);
        if (n == 4 && !memcmp(s, "main", 4))
            return true;
    }
    return false;
}

static void golden(const char *path)
{
    char *text;
    limba_module *m = load(path, &text);
    if (!m) {
        free(text);
        return;
    }
    /* the header */
    char want[4096] = "";
    size_t wlen = 0;
    int status = LIMBA_EVAL_OK;
    int64_t value = 0;
    bool has_value = false;
    for (const char *p = text; *p == ';'; p = strchr(p, '\n') + 1) {
        const char *eol = strchr(p, '\n');
        size_t n = (size_t)(eol - p);
        if (!strncmp(p, "; output: ", 10) && wlen + n < sizeof(want)) {
            memcpy(want + wlen, p + 10, n - 10);
            wlen += n - 10;
            want[wlen++] = '\n';
        } else if (!strncmp(p, "; result: ", 10)) {
            value = strtoll(p + 10, NULL, 10);
            has_value = true;
        } else if (!strncmp(p, "; trap: ", 8)) {
            status = LIMBA_EVAL_TRAP;
            value = strtoll(p + 8, NULL, 10);
        }
        if (!eol)
            break;
    }
    want[wlen] = 0;
    limba_eval_result r;
    limba_eval(m, "main", NULL, &r);
    CHECK(r.status == status, "%s: ended with status %d, expected %d", path,
          r.status, status);
    CHECK(status != LIMBA_EVAL_TRAP || r.code == value,
          "%s: trap %" PRId64 ", expected %" PRId64, path, r.code, value);
    CHECK(!has_value || (int64_t)r.ret == value,
          "%s: returned %" PRId64 ", expected %" PRId64, path, (int64_t)r.ret,
          value);
    CHECK(r.outlen == wlen && !memcmp(r.out, want, wlen),
          "%s: printed\n%s---\nexpected\n%s", path, r.out, want);
    limba_eval_result_free(&r);
    limba_module_free(m);
    free(text);
}

/* run path optimised with the passes not in skip */
static bool run(const char *path, const char *skip, limba_eval_result *r)
{
    char *text;
    limba_module *m = load(path, &text);
    free(text);
    if (!m)
        return false;
    limba_diag d = {{0}, 0};
    if (skip) {
        limba_opt_options o = {true, skip, NULL};
        if (limba_optimize(m, &o, &d) != 0) {
            CHECK(false, "%s: %s", path, d.msg);
            limba_module_free(m);
            return false;
        }
    }
    limba_eval(m, "main", NULL, r);
    limba_module_free(m);
    return true;
}

static void optdiff(const char *path)
{
    char *text;
    limba_module *m = load(path, &text);
    free(text);
    if (!m)
        return;
    bool runnable = has_main(m);
    limba_module_free(m);
    if (!runnable)
        return;

    limba_eval_result base;
    if (!run(path, NULL, &base))
        return;
    if (base.status == LIMBA_EVAL_UNSUPPORTED) {
        skipped++;
        limba_eval_result_free(&base);
        return;
    }
    /* "" = every pass; then each pass alone (skip all the others) */
    char skip[256];
    for (int k = -1; k < 0 || limba_pass_name((unsigned)k); k++) {
        skip[0] = 0;
        for (unsigned p = 0; k >= 0 && limba_pass_name(p); p++)
            if ((int)p != k) {
                strcat(skip, skip[0] ? "," : "");
                strcat(skip, limba_pass_name(p));
            }
        const char *label = k < 0 ? "all passes" : limba_pass_name((unsigned)k);
        limba_eval_result r;
        if (!run(path, skip, &r))
            continue;
        CHECK(r.status == base.status && r.code == base.code &&
                  r.ret == base.ret && r.outlen == base.outlen &&
                  !memcmp(r.out, base.out, base.outlen),
              "%s: with %s the program changes: status %d/%d, code "
              "%" PRId64 "/%" PRId64 ", result %" PRId64 "/%" PRId64
              ", output\n%s---\nwithout\n%s",
              path, label, r.status, base.status, r.code, base.code,
              (int64_t)r.ret, (int64_t)base.ret, r.out, base.out);
        compared++;
        limba_eval_result_free(&r);
    }
    limba_eval_result_free(&base);
}

static int by_name(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int each(const char *dir, void (*fn)(const char *))
{
    struct dirent **list;
    int n = scandir(dir, &list, NULL, by_name), done = 0;
    CHECK(n >= 0, "%s: cannot list (run from the root of the repository)", dir);
    for (int i = 0; i < n; i++) {
        const char *nm = list[i]->d_name;
        size_t k = strlen(nm);
        if (k > 4 && !strcmp(nm + k - 4, ".lit")) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, nm);
            fn(path);
            done++;
        }
        free(list[i]);
    }
    free(list);
    return done;
}

int main(void)
{
    int golden_n = each("tests/eval", golden);
    each("tests/eval", optdiff);
    each("tests/ir/ok", optdiff);
    each("tests/opt", optdiff);
    CHECK(golden_n > 0 && compared > 0, "no test files found");
    printf("test_eval: %d programs checked against their output, %d "
           "optimised runs compared, %d skipped (call.ext), %d failures\n",
           golden_n, compared, skipped, failures);
    return failures ? 1 : 0;
}
