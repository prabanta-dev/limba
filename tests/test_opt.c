/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_opt.c - the optimiser. Run from the root of the repository (build.sh
 * test does); it reads tests/opt and tests/ir/ok and writes nothing.
 *
 * tests/opt, every .lit: a module, a line ";; expect", the module the
 *     pipeline must make of it (compared after printing both);
 * tests/ir/ok and tests/opt, every .lit: optimised with a verification
 *     after every pass, then optimised again, which must change nothing.
 */
#define _GNU_SOURCE
#include "limba/ir.h"
#include "limba/opt.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

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

static char *print(const limba_module *m)
{
    char *s = NULL;
    size_t len;
    FILE *f = open_memstream(&s, &len);
    limba_print(m, f);
    fclose(f);
    return s;
}

static const limba_opt_options verify_each = {true, NULL, NULL};

/* optimise: it must verify after every pass, and a second run must find
   nothing left to do */
static limba_module *optimise(const char *path, const char *text, size_t len)
{
    limba_diag d = {{0}, 0};
    limba_module *m = limba_parse(text, len, &d);
    CHECK(m, "%s: %s", path, d.msg);
    if (!m)
        return NULL;
    CHECK(limba_verify(m, &d) == 0, "%s: before: %s", path, d.msg);
    if (limba_optimize(m, &verify_each, &d) != 0) {
        CHECK(false, "%s: %s", path, d.msg);
        limba_module_free(m);
        return NULL;
    }
    char *once = print(m);
    CHECK(limba_optimize(m, &verify_each, &d) == 0, "%s: again: %s", path,
          d.msg);
    char *twice = print(m);
    CHECK(!strcmp(once, twice), "%s: a second run still changes:\n%s", path,
          twice);
    free(once);
    free(twice);
    return m;
}

static void test_expect(const char *path)
{
    size_t len;
    char *text = slurp(path, &len);
    CHECK(text, "%s: cannot read", path);
    if (!text)
        return;
    char *sep = strstr(text, "\n;; expect");
    CHECK(sep, "%s: no ';; expect' line", path);
    if (!sep) {
        free(text);
        return;
    }
    size_t inlen = (size_t)(sep - text) + 1;
    const char *want = strchr(sep + 1, '\n');
    want = want ? want + 1 : sep + strlen(sep);

    limba_module *got = optimise(path, text, inlen);
    limba_diag d = {{0}, 0};
    limba_module *exp = limba_parse(want, strlen(want), &d);
    CHECK(exp, "%s: the expected module: %s", path, d.msg);
    if (got && exp) {
        char *g = print(got), *e = print(exp);
        CHECK(!strcmp(g, e), "%s: optimised to\n%s---\nexpected\n%s", path, g,
              e);
        free(g);
        free(e);
    }
    limba_module_free(got);
    limba_module_free(exp);
    free(text);
}

static void test_corpus(const char *path)
{
    size_t len;
    char *text = slurp(path, &len);
    CHECK(text, "%s: cannot read", path);
    if (!text)
        return;
    char *sep = strstr(text, "\n;; expect");
    if (sep)
        len = (size_t)(sep - text) + 1;
    limba_module_free(optimise(path, text, len));
    free(text);
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
    int cases = each("tests/opt", test_expect);
    int corpus = each("tests/ir/ok", test_corpus);
    CHECK(cases > 0 && corpus > 0, "no test files found");
    printf("test_opt: %d expected results, %d corpus modules, %d failures\n",
           cases, corpus, failures);
    return failures ? 1 : 0;
}
