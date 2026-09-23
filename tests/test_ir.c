/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_ir.c - the IR: text and binary forms and the verifier. Run from the
 * root of the repository (build.sh test does); it reads tests/ir and
 * writes nothing.
 *
 * tests/ir/ok, every .lit: must parse and verify; the printed text is a fixed
 *                     point (print, parse, print gives the same text); the
 *                     binary form survives read and write byte for byte and
 *                     prints the same text; every truncation of the binary
 *                     and every single flipped byte is refused or verified,
 *                     never a crash
 * tests/ir/bad, every .lit: the first line says "; expect: <text>"; parsing or
 *                     verifying must fail with a message that contains it
 */
#define _GNU_SOURCE
#include "common/leb128.h"
#include "limba/ir.h"

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

static char *print(const limba_module *m, size_t *len)
{
    char *s = NULL;
    FILE *f = open_memstream(&s, len);
    limba_print(m, f);
    fclose(f);
    return s;
}

static void test_leb128(void)
{
    static const int64_t cases[] = {
        0, 1, -1, 63, 64, -64, -65, 127, 128, INT64_MAX, INT64_MIN, 1 << 20};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        limba_wbuf w = {0};
        limba_w_sleb(&w, cases[i]);
        limba_w_uleb(&w, (uint64_t)cases[i]);
        limba_rbuf r = {w.buf, w.buf + w.len, false};
        int64_t s = limba_r_sleb(&r);
        uint64_t u = limba_r_uleb(&r);
        CHECK(!r.bad && s == cases[i] && u == (uint64_t)cases[i] &&
                  r.p == r.end,
              "leb128 round trip of %lld", (long long)cases[i]);
        free(w.buf);
    }
    /* an unsigned value of more than 64 bits is refused */
    uint8_t big[11];
    memset(big, 0xff, 10);
    big[10] = 0x01;
    limba_rbuf r = {big, big + 11, false};
    limba_r_uleb(&r);
    CHECK(r.bad, "leb128 wider than 64 bits accepted");
}

/* reading damaged binaries must never crash; what reads must verify or be
   refused by the verifier, and what verifies must print */
static void damage(const uint8_t *buf, size_t len, const char *name)
{
    uint8_t *copy = malloc(len + 1);
    FILE *null = fopen("/dev/null", "w");
    for (size_t n = 0; n < len; n++) {
        limba_module *m = limba_read(buf, n, NULL);
        CHECK(!m, "%s: a binary cut at %zu bytes was read", name, n);
        limba_module_free(m);
    }
    for (size_t i = 0; i < len; i++) {
        memcpy(copy, buf, len);
        copy[i] ^= 0x5a;
        limba_module *m = limba_read(copy, len, NULL);
        if (m && limba_verify(m, NULL) == 0)
            limba_print(m, null);
        limba_module_free(m);
    }
    fclose(null);
    free(copy);
}

static void test_ok(const char *path)
{
    size_t len, l1, l2, l3;
    char *text = slurp(path, &len);
    limba_diag d = {{0}, 0};
    CHECK(text, "%s: cannot read", path);
    if (!text)
        return;
    limba_module *m1 = limba_parse(text, len, &d);
    CHECK(m1, "%s: %s", path, d.msg);
    free(text);
    if (!m1)
        return;
    CHECK(limba_verify(m1, &d) == 0, "%s: %s", path, d.msg);
    char *t1 = print(m1, &l1);

    limba_module *m2 = limba_parse(t1, l1, &d);
    CHECK(m2, "%s: reparse: %s", path, d.msg);
    if (m2) {
        char *t2 = print(m2, &l2);
        CHECK(l1 == l2 && !memcmp(t1, t2, l1),
              "%s: the printed text is not a fixed point:\n%s---\n%s", path, t1,
              t2);
        free(t2);
    }

    uint8_t *b1, *b3;
    size_t n1, n3;
    limba_write(m1, &b1, &n1);
    limba_module *m3 = limba_read(b1, n1, &d);
    CHECK(m3, "%s: read back: %s", path, d.msg);
    if (m3) {
        CHECK(limba_verify(m3, &d) == 0, "%s: read back: %s", path, d.msg);
        char *t3 = print(m3, &l3);
        CHECK(l1 == l3 && !memcmp(t1, t3, l1),
              "%s: the binary does not print the same text", path);
        limba_write(m3, &b3, &n3);
        CHECK(n1 == n3 && !memcmp(b1, b3, n1), "%s: write(read(write)) differs",
              path);
        free(b3);
        free(t3);
    }
    damage(b1, n1, path);
    free(b1);
    free(t1);
    limba_module_free(m1);
    limba_module_free(m2);
    limba_module_free(m3);
}

static void test_bad(const char *path)
{
    size_t len;
    char *text = slurp(path, &len);
    CHECK(text, "%s: cannot read", path);
    if (!text)
        return;
    const char *tag = "; expect: ";
    CHECK(!strncmp(text, tag, strlen(tag)), "%s: no '; expect:' line", path);
    char expect[200] = "";
    const char *e = text + strlen(tag);
    size_t n = strcspn(e, "\n");
    if (n < sizeof(expect)) {
        memcpy(expect, e, n);
        expect[n] = 0;
    }
    limba_diag d = {{0}, 0};
    limba_module *m = limba_parse(text, len, &d);
    bool failed = !m || limba_verify(m, &d) != 0;
    CHECK(failed, "%s: accepted, but it should not be", path);
    CHECK(!failed || strstr(d.msg, expect), "%s: says \"%s\", expected \"%s\"",
          path, d.msg, expect);
    limba_module_free(m);
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
    test_leb128();
    int ok = each("tests/ir/ok", test_ok);
    int bad = each("tests/ir/bad", test_bad);
    CHECK(ok > 0 && bad > 0, "no test files found");
    printf("test_ir: %d valid and %d invalid modules, %d failures\n", ok, bad,
           failures);
    return failures ? 1 : 0;
}
