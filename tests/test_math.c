/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_math.c - the mathematics of the run time (limba/math.h), two
 * checks and the special cases:
 * - the same bits on every engine and machine, with no reference: a hash
 *   of the results on fixed arguments, written here once (x86-64, 22 Oct
 *   2026); a machine or compiler that computes one bit differently fails;
 * - the precision against mpmath, within the error the library declares
 *   (below 1 ulp): tests/math/reference.txt, written by
 *   tools/math_reference.py, gives for each argument the two doubles
 *   around the exact result, and the result must be one of them;
 * - the special cases of IEEE 754 § 9.2.1, value and sign.
 * Run from the root of the repository (build.sh test does).
 */
#include "limba/ir.h"
#include "limba/math.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static unsigned checked;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checked++;                                                             \
        if (!(cond)) {                                                         \
            if (failures < 20) {                                               \
                fprintf(stderr, "test_math: ");                                \
                fprintf(stderr, __VA_ARGS__);                                  \
                fputc('\n', stderr);                                           \
            }                                                                  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static uint64_t seed;

static uint64_t next(void)
{
    uint64_t z = (seed += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t bits(double x)
{
    uint64_t u;
    memcpy(&u, &x, sizeof(u));
    return u;
}

static double from_bits(uint64_t u)
{
    double x;
    memcpy(&x, &u, sizeof(x));
    return x;
}

/* a real in [lo, hi), from the generator */
static double uniform(double lo, double hi)
{
    return lo + (hi - lo) * (double)(next() >> 11) * 0x1p-53;
}

/* a finite real of any size, from its bits */
static double any_finite(void)
{
    double x;
    do
        x = from_bits(next());
    while (!isfinite(x));
    return x;
}

enum { SIN, COS, TAN, ATAN, EXP, LN, POW, NFUNC };
static const char *const names[NFUNC] = {"sin", "cos", "tan", "atan",
                                         "exp", "ln",  "pow"};

/* the arguments of function f, the k-th time */
static void args(int f, double *x, double *y)
{
    *y = 0;
    switch (f) {
    case SIN:
    case COS:
    case TAN:
        switch (next() % 4) {
        case 0:
            *x = uniform(-4, 4);
            break;
        case 1:
            *x = uniform(-1e6, 1e6);
            break;
        case 2:
            *x = uniform(-1e-8, 1e-8);
            break;
        default:
            *x = any_finite(); /* large: the long reduction */
        }
        break;
    case ATAN:
        *x = next() & 1 ? uniform(-20, 20) : any_finite();
        break;
    case EXP:
        *x = uniform(-746, 710);
        break;
    case LN:
        *x = next() & 1 ? uniform(0, 4) : fabs(any_finite());
        break;
    case POW:
        if (next() % 4 == 0) { /* a negative base, an integer exponent */
            *x = -uniform(0.1, 20);
            *y = (double)((int)(next() % 61) - 30);
        } else {
            *x = uniform(0, 1000);
            *y = uniform(-60, 60);
        }
        break;
    }
}

static double ours(int f, double x, double y)
{
    switch (f) {
    case SIN:
        return limba_math_sin(x);
    case COS:
        return limba_math_cos(x);
    case TAN:
        return limba_math_tan(x);
    case ATAN:
        return limba_math_atan(x);
    case EXP:
        return limba_math_exp(x);
    case LN:
        return limba_math_ln(x);
    default:
        return limba_math_pow(x, y);
    }
}

/* the hash of the bits of every result, 20000 arguments a function: the
   bits of x86-64 (gcc 14, glibc 2.41, 22 Oct 2026). A machine that
   computes another bit fails here */
static const uint64_t golden[NFUNC] = {
    0xb75bc577edfe06d8ull, 0x752c64e2dd07432eull, 0x76a0931ee7e1c5c6ull,
    0x8dda83b3c5bb27adull, 0x16d6ea14f075aebbull, 0x3d4c66095b65f981ull,
    0x57220fba1d2e905bull,
};

static void test_bits(void)
{
    for (int f = 0; f < NFUNC; f++) {
        seed = 1000 + (uint64_t)f;
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned i = 0; i < 20000; i++) {
            double x = 0, y = 0;
            args(f, &x, &y);
            uint64_t u = bits(ours(f, x, y));
            for (int b = 0; b < 8; b++)
                h = (h ^ ((u >> (8 * b)) & 0xff)) * 0x100000001b3ull;
        }
        CHECK(h == golden[f], "%s: hash %#llx, expected %#llx", names[f],
              (unsigned long long)h, (unsigned long long)golden[f]);
    }
}

/* the precision: each result one of the two doubles around the exact one
   (mpmath), so below 1 ulp; how many are the nearest, on request */
static void test_reference(void)
{
    FILE *in = fopen("tests/math/reference.txt", "r");
    CHECK(in, "tests/math/reference.txt: cannot read");
    if (!in)
        return;
    char name[16];
    unsigned lines = 0, nearest[NFUNC] = {0}, count[NFUNC] = {0};
    while (fscanf(in, "%15s", name) == 1) {
        int f = 0;
        while (f < NFUNC && strcmp(names[f], name))
            f++;
        unsigned long long xb, yb = 0, nb, ob;
        bool ok = f < NFUNC && fscanf(in, "%llx", &xb) == 1 &&
                  (f != POW || fscanf(in, "%llx", &yb) == 1) &&
                  fscanf(in, "%llx %llx", &nb, &ob) == 2;
        CHECK(ok, "tests/math/reference.txt: line %u malformed", lines + 1);
        if (!ok)
            break;
        lines++;
        double x = from_bits(xb), y = from_bits(yb);
        uint64_t r = bits(ours(f, x, y));
        CHECK(r == nb || r == ob,
              "%s(%a, %a) = %a, outside 1 ulp: exact between %a and %a",
              names[f], x, y, from_bits(r), from_bits(nb), from_bits(ob));
        nearest[f] += r == nb;
        count[f]++;
    }
    fclose(in);
    CHECK(lines >= 7000, "tests/math/reference.txt: %u lines", lines);
    if (getenv("LIMBA_MATH_VERBOSE"))
        for (int f = 0; f < NFUNC; f++)
            fprintf(stderr, "%s: %u of %u rounded to nearest\n", names[f],
                    nearest[f], count[f]);
}

/* value and sign; a NaN as any NaN */
static void special(int f, double x, double y, double want)
{
    double r = ours(f, x, y);
    bool ok = isnan(want) ? isnan(r) : bits(r) == bits(want);
    CHECK(ok, "%s(%a, %a) = %a, expected %a", names[f], x, y, r, want);
}

static void test_special(void)
{
    double inf = INFINITY, nan = NAN;
    special(SIN, 0.0, 0, 0.0);
    special(SIN, -0.0, 0, -0.0);
    special(SIN, inf, 0, nan);
    special(SIN, nan, 0, nan);
    special(COS, 0.0, 0, 1.0);
    special(COS, -inf, 0, nan);
    special(TAN, -0.0, 0, -0.0);
    special(TAN, inf, 0, nan);
    special(ATAN, -0.0, 0, -0.0);
    special(ATAN, inf, 0, limba_math_atan(1e300));
    special(ATAN, -inf, 0, -limba_math_atan(1e300));
    special(EXP, -inf, 0, 0.0);
    special(EXP, inf, 0, inf);
    special(EXP, 0.0, 0, 1.0);
    special(EXP, 710.0, 0, inf);
    special(EXP, -746.0, 0, 0.0);
    special(EXP, nan, 0, nan);
    special(LN, 1.0, 0, 0.0);
    special(LN, 0.0, 0, -inf);
    special(LN, -0.0, 0, -inf);
    special(LN, -1.0, 0, nan);
    special(LN, inf, 0, inf);
    special(POW, nan, 0.0, 1.0);
    special(POW, nan, -0.0, 1.0);
    special(POW, 1.0, nan, 1.0);
    special(POW, -1.0, inf, 1.0);
    special(POW, -1.0, -inf, 1.0);
    special(POW, 0.0, -1.0, inf);
    special(POW, -0.0, -1.0, -inf);
    special(POW, -0.0, -2.0, inf);
    special(POW, -0.0, 3.0, -0.0);
    special(POW, -2.0, 0.5, nan);
    special(POW, -2.0, 3.0, -8.0);
    special(POW, 0.5, inf, 0.0);
    special(POW, 2.0, -inf, 0.0);
    special(POW, inf, -1.0, 0.0);
    special(POW, -inf, 3.0, -inf);
    special(POW, -inf, -3.0, -0.0);
    special(POW, 2.0, 1024.0, inf);
    special(POW, 2.0, -1075.0, 0.0);
    special(POW, 2.0, 10.0, 1024.0);
}

int main(void)
{
    limba_set_program("test_math");
    test_special();
    test_bits();
    test_reference();
    printf("test_math: %u checks: special cases, the same bits on fixed "
           "arguments, within 1 ulp of mpmath; %d failures\n",
           checked, failures);
    return failures ? 1 : 0;
}
