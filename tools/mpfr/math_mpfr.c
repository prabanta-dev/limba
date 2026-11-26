/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * math_mpfr.c - the correct rounding of limba/math.h against MPFR, a tool
 * of the machine of development only (doc/ir.md § 15.5): MPFR is never a
 * dependency of Limba, and build.sh does not build this file (it lies in
 * a directory of its own). Build it after build.sh release:
 *
 *   gcc -std=gnu11 -O2 -pthread -Iinclude tools/mpfr/math_mpfr.c \
 *       lib/x86_64-linux-release/liblimba.a -lmpfr -lgmp -lm \
 *       -o bin/x86_64-linux/math_mpfr
 *
 * Every result is compared bit for bit with the one MPFR rounds to
 * nearest even in the format (subnormals and overflow included); a NaN
 * only as a NaN (doc/ir.md § 6 leaves its sign and payload open).
 *
 *   math_mpfr wc NAME FILE      the worst cases of CORE-MATH (a .wc file:
 *                               one argument, or two with a comma, a line)
 *   math_mpfr rand NAME N SEED  N arguments of random bits
 *   math_mpfr exh NAME [LO HI]  every f32 argument with bits in [LO, HI],
 *                               in threads (one-argument _f32 only)
 *   math_mpfr tiny NAME N SEED  atan2 or atan2_f32 with x > 0 and a
 *                               tiny y / x, and y, x of few bits (the
 *                               hard cases of math.c)
 *   math_mpfr ref NAME FILE K   K arguments of FILE, evenly spread, as
 *                               lines "name x [y] r" in hex bits, for
 *                               tests/math/mpfr.txt; FILE "rand:SEED"
 *                               takes K arguments of random bits
 *
 * NAME is sin cos tan atan exp ln pow atan2 asin acos hypot log2 log10
 * sinh cosh tanh, with _f32 for the functions of an f32. The exit status
 * is 1 when a result differs.
 */
#include "limba/ir.h"
#include "limba/math.h"

#include <inttypes.h>
#include <math.h>
#include <mpfr.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NFUNC = 16 };
static const char *const names[NFUNC] = {
    "sin",  "cos",  "tan",   "atan", "exp",   "ln",   "pow",  "atan2",
    "asin", "acos", "hypot", "log2", "log10", "sinh", "cosh", "tanh"};
static const uint32_t rts[NFUNC] = {
    LIMBA_RT_MATH_SIN,   LIMBA_RT_MATH_COS,   LIMBA_RT_MATH_TAN,
    LIMBA_RT_MATH_ATAN,  LIMBA_RT_MATH_EXP,   LIMBA_RT_MATH_LN,
    LIMBA_RT_MATH_POW,   LIMBA_RT_MATH_ATAN2, LIMBA_RT_MATH_ASIN,
    LIMBA_RT_MATH_ACOS,  LIMBA_RT_MATH_HYPOT, LIMBA_RT_MATH_LOG2,
    LIMBA_RT_MATH_LOG10, LIMBA_RT_MATH_SINH,  LIMBA_RT_MATH_COSH,
    LIMBA_RT_MATH_TANH};
static const uint32_t rts32[NFUNC] = {
    LIMBA_RT_MATH_SIN_F32,   LIMBA_RT_MATH_COS_F32,   LIMBA_RT_MATH_TAN_F32,
    LIMBA_RT_MATH_ATAN_F32,  LIMBA_RT_MATH_EXP_F32,   LIMBA_RT_MATH_LN_F32,
    LIMBA_RT_MATH_POW_F32,   LIMBA_RT_MATH_ATAN2_F32, LIMBA_RT_MATH_ASIN_F32,
    LIMBA_RT_MATH_ACOS_F32,  LIMBA_RT_MATH_HYPOT_F32, LIMBA_RT_MATH_LOG2_F32,
    LIMBA_RT_MATH_LOG10_F32, LIMBA_RT_MATH_SINH_F32,  LIMBA_RT_MATH_COSH_F32,
    LIMBA_RT_MATH_TANH_F32};

static int func;
static bool f32;

static bool binary(int f)
{
    return f == 6 || f == 7 || f == 10;
}

static int exact(int f, mpfr_t r, const mpfr_t x, const mpfr_t y)
{
    switch (f) {
    case 0:
        return mpfr_sin(r, x, MPFR_RNDN);
    case 1:
        return mpfr_cos(r, x, MPFR_RNDN);
    case 2:
        return mpfr_tan(r, x, MPFR_RNDN);
    case 3:
        return mpfr_atan(r, x, MPFR_RNDN);
    case 4:
        return mpfr_exp(r, x, MPFR_RNDN);
    case 5:
        return mpfr_log(r, x, MPFR_RNDN);
    case 6:
        return mpfr_pow(r, x, y, MPFR_RNDN);
    case 7:
        return mpfr_atan2(r, x, y, MPFR_RNDN); /* atan2(y, x), y first */
    case 8:
        return mpfr_asin(r, x, MPFR_RNDN);
    case 9:
        return mpfr_acos(r, x, MPFR_RNDN);
    case 10:
        return mpfr_hypot(r, x, y, MPFR_RNDN);
    case 11:
        return mpfr_log2(r, x, MPFR_RNDN);
    case 12:
        return mpfr_log10(r, x, MPFR_RNDN);
    case 13:
        return mpfr_sinh(r, x, MPFR_RNDN);
    case 14:
        return mpfr_cosh(r, x, MPFR_RNDN);
    default:
        return mpfr_tanh(r, x, MPFR_RNDN);
    }
}

/* the result rounded once to nearest even in the format, subnormals and
   overflow as IEEE 754 */
static double reference(double x, double y)
{
    mpfr_set_emin(f32 ? -148 : -1073);
    mpfr_set_emax(f32 ? 128 : 1024);
    mpfr_prec_t p = f32 ? 24 : 53;
    mpfr_t a, b, r;
    mpfr_inits2(64, a, b, (mpfr_ptr)0);
    mpfr_init2(r, p);
    mpfr_set_d(a, x, MPFR_RNDN);
    mpfr_set_d(b, y, MPFR_RNDN);
    int t = exact(func, r, a, b);
    t = mpfr_check_range(r, t, MPFR_RNDN);
    mpfr_subnormalize(r, t, MPFR_RNDN);
    double d =
        f32 ? (double)mpfr_get_flt(r, MPFR_RNDN) : mpfr_get_d(r, MPFR_RNDN);
    mpfr_clears(a, b, r, (mpfr_ptr)0);
    return d;
}

static double ours(double x, double y)
{
    double a[2] = {x, y}, r = 0;
    limba_math_call(f32 ? rts32[func] : rts[func], a, &r);
    return r;
}

static uint64_t bits(double x)
{
    uint64_t u;
    memcpy(&u, &x, sizeof(u));
    return u;
}

static bool same(double r, double w)
{
    return isnan(w) ? isnan(r) : bits(r) == bits(w);
}

static unsigned long long wrong;

static bool check(double x, double y)
{
    double r = ours(x, y), w = reference(x, y);
    if (same(r, w))
        return true;
    if (wrong++ < 20)
        fprintf(stderr, "%s%s(%a, %a) = %a, MPFR %a\n", names[func],
                f32 ? "_f32" : "", x, y, r, w);
    return false;
}

static uint64_t seed;

static uint64_t next(void)
{
    uint64_t z = (seed += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static double random_arg(void)
{
    uint64_t u = next();
    if (f32) {
        uint32_t v = (uint32_t)u;
        float g;
        memcpy(&g, &v, sizeof(g));
        return g;
    }
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

/* one line of a .wc file: false at its end; *skip for a line with no
   argument (a comment) or an argument strtod does not read (snan) */
static bool read_wc(FILE *in, double *x, double *y, bool *skip)
{
    char line[512];
    if (!fgets(line, sizeof(line), in))
        return false;
    char *h = strchr(line, '#');
    if (h)
        *h = 0;
    *skip = true;
    *y = 0;
    char *p = line, *e;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '\n' || strstr(p, "snan"))
        return true;
    *x = strtod(p, &e);
    if (e == p)
        return true;
    if (binary(func)) {
        p = strchr(e, ',');
        if (!p)
            return true;
        *y = strtod(p + 1, &e);
        if (e == p + 1)
            return true;
    }
    if (f32 && ((double)(float)*x != *x && !isnan(*x)))
        return true;
    *skip = false;
    return true;
}

static void emit(double x, double y)
{
    double r = reference(x, y);
    const char *n = names[func];
    if (f32) {
        float a = (float)x, b = (float)y, c = (float)r;
        uint32_t u, v, w;
        memcpy(&u, &a, 4);
        memcpy(&v, &b, 4);
        memcpy(&w, &c, 4);
        if (binary(func))
            printf("%s_f32 0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32 "\n",
                   n, u, v, w);
        else
            printf("%s_f32 0x%08" PRIx32 " 0x%08" PRIx32 "\n", n, u, w);
    } else if (binary(func))
        printf("%s 0x%016" PRIx64 " 0x%016" PRIx64 " 0x%016" PRIx64 "\n", n,
               bits(x), bits(y), bits(r));
    else
        printf("%s 0x%016" PRIx64 " 0x%016" PRIx64 "\n", n, bits(x), bits(r));
}

/* every f32 of bits [lo, hi], split among the threads */
static uint64_t exh_lo, exh_hi;
static unsigned nthreads;

static void *exh_part(void *arg)
{
    unsigned k = (unsigned)(uintptr_t)arg;
    unsigned long long bad = 0;
    for (uint64_t u = exh_lo + k; u <= exh_hi; u += nthreads) {
        uint32_t v = (uint32_t)u;
        float g;
        memcpy(&g, &v, sizeof(g));
        double r = ours(g, 0), w = reference(g, 0);
        if (!same(r, w) && bad++ < 5)
            fprintf(stderr, "%s_f32(%a) = %a, MPFR %a\n", names[func],
                    (double)g, r, w);
    }
    mpfr_free_cache();
    return (void *)(uintptr_t)bad;
}

int main(int argc, char **argv)
{
    limba_set_program("math_mpfr");
    if (argc < 3) {
        fprintf(stderr, "usage: math_mpfr wc|rand|exh|ref NAME ...\n");
        return 2;
    }
    const char *nm = argv[2];
    size_t n = strlen(nm);
    f32 = n > 4 && !strcmp(nm + n - 4, "_f32");
    for (func = 0; func < NFUNC; func++)
        if (strlen(names[func]) == (f32 ? n - 4 : n) &&
            !strncmp(names[func], nm, strlen(names[func])))
            break;
    if (func == NFUNC) {
        fprintf(stderr, "math_mpfr: no function %s\n", nm);
        return 2;
    }
    unsigned long long count = 0;
    if (!strcmp(argv[1], "wc") && argc == 4) {
        FILE *in = fopen(argv[3], "r");
        if (!in) {
            perror(argv[3]);
            return 2;
        }
        double x, y;
        bool skip;
        while (read_wc(in, &x, &y, &skip))
            if (!skip) {
                check(x, y);
                count++;
            }
        fclose(in);
    } else if (!strcmp(argv[1], "rand") && argc == 5) {
        unsigned long long k = strtoull(argv[3], NULL, 10);
        seed = strtoull(argv[4], NULL, 10);
        for (; count < k; count++) {
            double x = random_arg(), y = binary(func) ? random_arg() : 0;
            check(x, y);
        }
    } else if (!strcmp(argv[1], "tiny") && argc == 5 && func == 7) {
        unsigned long long k = strtoull(argv[3], NULL, 10);
        seed = strtoull(argv[4], NULL, 10);
        for (; count < k; count++) {
            int ex = (int)(next() % 2000) - 1000,
                er = f32 ? -150 + (int)(next() % 130)
                         : -1080 + (int)(next() % 1040);
            double x, y;
            if (next() & 1) {
                /* a number of few bits over another */
                x = ldexp(1 + (double)(next() % 64) * 0x1p-52, ex);
                y = ldexp(1 + (double)(next() % 64) * 0x1p-52, ex + er);
            } else {
                x = ldexp(1 + (double)(next() >> 12) * 0x1p-52, ex);
                y = x * ldexp(1 + (double)(next() >> 12) * 0x1p-52, er);
            }
            if (next() & 1)
                y = -y;
            if (f32) {
                x = (float)ldexp(x, -ex + (int)(next() % 200) - 100);
                y = (float)y;
            }
            check(y, x);
        }
    } else if (!strcmp(argv[1], "exh") && f32 && !binary(func)) {
        exh_lo = argc > 3 ? strtoull(argv[3], NULL, 0) : 0;
        exh_hi = argc > 4 ? strtoull(argv[4], NULL, 0) : 0xffffffffull;
        const char *t = getenv("MATH_MPFR_THREADS");
        nthreads = t ? (unsigned)atoi(t) : 16;
        pthread_t th[256];
        if (nthreads < 1 || nthreads > 256)
            nthreads = 16;
        for (unsigned k = 0; k < nthreads; k++)
            pthread_create(&th[k], NULL, exh_part, (void *)(uintptr_t)k);
        for (unsigned k = 0; k < nthreads; k++) {
            void *b;
            pthread_join(th[k], &b);
            wrong += (uintptr_t)b;
        }
        count = exh_hi - exh_lo + 1;
    } else if (!strcmp(argv[1], "ref") && argc == 5) {
        unsigned long long k = strtoull(argv[4], NULL, 10);
        if (!strncmp(argv[3], "rand:", 5)) {
            seed = strtoull(argv[3] + 5, NULL, 10);
            for (; count < k; count++) {
                double x = random_arg(), y = binary(func) ? random_arg() : 0;
                if (isnan(x) || isnan(y)) {
                    count--;
                    continue;
                }
                emit(x, y);
            }
            return 0;
        }
        /* the lines with an argument first, then k of them */
        FILE *in = fopen(argv[3], "r");
        if (!in) {
            perror(argv[3]);
            return 2;
        }
        double x, y;
        bool skip;
        unsigned long long all = 0;
        while (read_wc(in, &x, &y, &skip))
            all += !skip && !isnan(x) && !isnan(y);
        rewind(in);
        unsigned long long i = 0;
        while (read_wc(in, &x, &y, &skip)) {
            if (skip || isnan(x) || isnan(y))
                continue;
            /* the i-th of all, taken when it starts a new k-th */
            if (all <= k || (i * k) / all != ((i + 1) * k) / all) {
                emit(x, y);
                count++;
            }
            i++;
        }
        fclose(in);
        return 0;
    } else {
        fprintf(stderr, "math_mpfr: wrong arguments\n");
        return 2;
    }
    printf("%s%s: %llu arguments, %llu differ from MPFR\n", names[func],
           f32 ? "_f32" : "", count, wrong);
    mpfr_free_cache();
    return wrong != 0;
}
