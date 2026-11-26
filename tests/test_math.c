/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_math.c - the mathematics of the run time (limba/math.h), correctly
 * rounded (doc/ir.md § 15.5), for f64 and f32:
 * - the same bits on every engine and machine, with no reference: a hash
 *   of the results on fixed arguments, written here once (x86-64, 25 Nov
 *   2026); a machine or compiler that computes one bit differently fails;
 * - the correct rounding against mpmath: tests/math/reference.txt,
 *   written by tools/math_reference.py, gives for each argument the
 *   double nearest the exact result (and the one on the other side), and
 *   the result must be the nearest;
 * - the correct rounding against MPFR on worst cases: tests/math/mpfr.txt,
 *   written by tools/mpfr/math_mpfr.c (ref) from the worst cases of
 *   CORE-MATH, for f64 and f32;
 * - the special cases of IEEE 754 § 9.2.1, value and sign; a NaN as any
 *   NaN.
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
            if (failures < 40) {                                               \
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

enum {
    SIN,
    COS,
    TAN,
    ATAN,
    EXP,
    LN,
    POW,
    ATAN2,
    ASIN,
    ACOS,
    HYPOT,
    LOG2,
    LOG10,
    SINH,
    COSH,
    TANH,
    NFUNC
};
static const char *const names[NFUNC] = {
    "sin",  "cos",  "tan",   "atan", "exp",   "ln",   "pow",  "atan2",
    "asin", "acos", "hypot", "log2", "log10", "sinh", "cosh", "tanh"};

/* two arguments */
static bool binary(int f)
{
    return f == POW || f == ATAN2 || f == HYPOT;
}

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
    case ATAN2:
        *x = next() & 1 ? uniform(-20, 20) : any_finite();
        *y = next() & 1 ? uniform(-20, 20) : any_finite();
        break;
    case ASIN:
    case ACOS:
        *x = uniform(-1, 1);
        break;
    case HYPOT:
        *x = next() & 1 ? uniform(-1e3, 1e3) : any_finite();
        *y = next() & 1 ? uniform(-1e3, 1e3) : any_finite();
        break;
    case LOG2:
    case LOG10:
        *x = next() & 1 ? uniform(0, 4) : fabs(any_finite());
        break;
    case SINH:
    case COSH:
        *x = next() & 1 ? uniform(-1, 1) : uniform(-710, 710);
        break;
    case TANH:
        *x = uniform(-20, 20);
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
    case POW:
        return limba_math_pow(x, y);
    case ATAN2:
        return limba_math_atan2(x, y);
    case ASIN:
        return limba_math_asin(x);
    case ACOS:
        return limba_math_acos(x);
    case HYPOT:
        return limba_math_hypot(x, y);
    case LOG2:
        return limba_math_log2(x);
    case LOG10:
        return limba_math_log10(x);
    case SINH:
        return limba_math_sinh(x);
    case COSH:
        return limba_math_cosh(x);
    default:
        return limba_math_tanh(x);
    }
}

static float ours32(int f, float x, float y)
{
    switch (f) {
    case SIN:
        return limba_math_sin_f32(x);
    case COS:
        return limba_math_cos_f32(x);
    case TAN:
        return limba_math_tan_f32(x);
    case ATAN:
        return limba_math_atan_f32(x);
    case EXP:
        return limba_math_exp_f32(x);
    case LN:
        return limba_math_ln_f32(x);
    case POW:
        return limba_math_pow_f32(x, y);
    case ATAN2:
        return limba_math_atan2_f32(x, y);
    case ASIN:
        return limba_math_asin_f32(x);
    case ACOS:
        return limba_math_acos_f32(x);
    case HYPOT:
        return limba_math_hypot_f32(x, y);
    case LOG2:
        return limba_math_log2_f32(x);
    case LOG10:
        return limba_math_log10_f32(x);
    case SINH:
        return limba_math_sinh_f32(x);
    case COSH:
        return limba_math_cosh_f32(x);
    default:
        return limba_math_tanh_f32(x);
    }
}

static uint32_t bits32(float x)
{
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    return u;
}

static float from_bits32(uint32_t u)
{
    float x;
    memcpy(&x, &u, sizeof(x));
    return x;
}

/* the hash of the bits of every result, 20000 arguments a function: the
   bits of x86-64 (gcc 14, glibc 2.41, CORE-MATH f6c0c37, 25 Nov 2026),
   which correct rounding makes those of every machine; then the same of
   the functions of an f32, on the arguments rounded to f32 */
static const uint64_t golden[NFUNC] = {
    0x868b4e6eebb517e7ull, 0x53efd92f7e5d0346ull, 0xc588f94411155f04ull,
    0x5105ca9129e57883ull, 0xf011a8b57c6f4ee0ull, 0xa0f99b73c885fc52ull,
    0x3ae9ffc6d25827c6ull, 0xeae3dd0a5aca5c3eull, 0x28dd5a0df58e2e30ull,
    0x02003a37823d058bull, 0x45df4dea4528ebedull, 0x83f1a5ad597dfe8dull,
    0xe3bec72640b18f76ull, 0x1810f1a0a00ef8bbull, 0xeafcdde5c29d8d73ull,
    0x02a84b6533abd2bcull,
};
static const uint64_t golden32[NFUNC] = {
    0x06c54b0bffe104f5ull, 0xdf9e1cc4d3e4e935ull, 0x1ed8b16d61a42a10ull,
    0x2b57c070ae9016c8ull, 0x1a68f98b6c60ca32ull, 0x3d1c283110d005feull,
    0xb949fb6a2bd034abull, 0xb838699dbe30a7a3ull, 0xb6d07d716316f400ull,
    0x5b6e48281eb345f3ull, 0xd274e2aa392fa41bull, 0xb1158709a8049c7aull,
    0xac138383245dfc41ull, 0x8ab108945e012785ull, 0xcdadeef64edb3c28ull,
    0x6d5741321c9240f1ull,
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
    for (int f = 0; f < NFUNC; f++) {
        seed = 2000 + (uint64_t)f;
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned i = 0; i < 20000; i++) {
            double x = 0, y = 0;
            args(f, &x, &y);
            uint32_t u = bits32(ours32(f, (float)x, (float)y));
            for (int b = 0; b < 4; b++)
                h = (h ^ ((u >> (8 * b)) & 0xff)) * 0x100000001b3ull;
        }
        CHECK(h == golden32[f], "%s_f32: hash %#llx, expected %#llx", names[f],
              (unsigned long long)h, (unsigned long long)golden32[f]);
    }
}

/* the correct rounding: each result the double nearest the exact one
   (mpmath) */
static void test_reference(void)
{
    FILE *in = fopen("tests/math/reference.txt", "r");
    CHECK(in, "tests/math/reference.txt: cannot read");
    if (!in)
        return;
    char name[16];
    unsigned lines = 0;
    while (fscanf(in, "%15s", name) == 1) {
        int f = 0;
        while (f < NFUNC && strcmp(names[f], name))
            f++;
        unsigned long long xb, yb = 0, nb, ob;
        bool ok = f < NFUNC && fscanf(in, "%llx", &xb) == 1 &&
                  (!binary(f) || fscanf(in, "%llx", &yb) == 1) &&
                  fscanf(in, "%llx %llx", &nb, &ob) == 2;
        CHECK(ok, "tests/math/reference.txt: line %u malformed", lines + 1);
        if (!ok)
            break;
        lines++;
        double x = from_bits(xb), y = from_bits(yb);
        uint64_t r = bits(ours(f, x, y));
        CHECK(r == nb, "%s(%a, %a) = %a, not correctly rounded: %a", names[f],
              x, y, from_bits(r), from_bits(nb));
        (void)ob;
    }
    fclose(in);
    CHECK(lines >= 16000, "tests/math/reference.txt: %u lines", lines);
}

/* the worst cases, against MPFR: "name x [y] r", name with _f32 for an
   f32; a comment line starts with # */
static void test_mpfr(void)
{
    FILE *in = fopen("tests/math/mpfr.txt", "r");
    CHECK(in, "tests/math/mpfr.txt: cannot read");
    if (!in)
        return;
    char line[256], name[24];
    unsigned lines = 0, of32 = 0;
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '#')
            continue;
        unsigned long long a, b, c;
        int n = sscanf(line, "%23s %llx %llx %llx", name, &a, &b, &c);
        size_t k = strlen(name);
        bool f32 = k > 4 && !strcmp(name + k - 4, "_f32");
        if (f32)
            name[k - 4] = 0;
        int f = 0;
        while (f < NFUNC && strcmp(names[f], name))
            f++;
        bool ok = f < NFUNC && n == (binary(f) ? 4 : 3);
        CHECK(ok, "tests/math/mpfr.txt: line %u malformed", lines + 1);
        if (!ok)
            break;
        lines++;
        if (!binary(f))
            c = b, b = 0;
        if (f32) {
            of32++;
            float x = from_bits32((uint32_t)a), y = from_bits32((uint32_t)b);
            uint32_t r = bits32(ours32(f, x, y));
            CHECK(r == (uint32_t)c, "%s_f32(%a, %a) = %a, MPFR %a", names[f],
                  (double)x, (double)y, (double)from_bits32(r),
                  (double)from_bits32((uint32_t)c));
        } else {
            double x = from_bits(a), y = from_bits(b);
            uint64_t r = bits(ours(f, x, y));
            CHECK(r == c, "%s(%a, %a) = %a, MPFR %a", names[f], x, y,
                  from_bits(r), from_bits(c));
        }
    }
    fclose(in);
    CHECK(lines >= 15000 && of32 >= 7000, "tests/math/mpfr.txt: %u lines",
          lines);
}

/* value and sign; a NaN as any NaN */
static void special(int f, double x, double y, double want)
{
    double r = ours(f, x, y);
    bool ok = isnan(want) ? isnan(r) : bits(r) == bits(want);
    CHECK(ok, "%s(%a, %a) = %a, expected %a", names[f], x, y, r, want);
}

static void special32(int f, float x, float y, float want)
{
    float r = ours32(f, x, y);
    bool ok = isnan(want) ? isnan(r) : bits32(r) == bits32(want);
    CHECK(ok, "%s_f32(%a, %a) = %a, expected %a", names[f], (double)x,
          (double)y, (double)r, (double)want);
}

static void test_special(void)
{
    float inff = INFINITY, nanf_ = NAN;
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
    double pi = 0x1.921fb54442d18p+1, half_pi = 0x1.921fb54442d18p+0;
    special(ATAN2, 0.0, -0.0, pi);
    special(ATAN2, -0.0, -0.0, -pi);
    special(ATAN2, 0.0, 0.0, 0.0);
    special(ATAN2, -0.0, 0.0, -0.0);
    special(ATAN2, 1.0, 0.0, half_pi);
    special(ATAN2, -1.0, -0.0, -half_pi);
    special(ATAN2, 1.0, -inf, pi);
    special(ATAN2, -1.0, inf, -0.0);
    special(ATAN2, inf, inf, 0x1.921fb54442d18p-1);
    special(ATAN2, nan, 1.0, nan);
    special(ASIN, -0.0, 0, -0.0);
    special(ASIN, 1.0, 0, half_pi);
    special(ASIN, 1.5, 0, nan);
    special(ACOS, 1.0, 0, 0.0);
    special(ACOS, -1.0, 0, pi);
    special(ACOS, -2.0, 0, nan);
    special(HYPOT, inf, nan, inf);
    special(HYPOT, nan, -inf, inf);
    special(HYPOT, -3.0, 4.0, 5.0);
    special(HYPOT, nan, 1.0, nan);
    special(LOG2, 1.0, 0, 0.0);
    special(LOG2, 0.0, 0, -inf);
    special(LOG2, -0.0, 0, -inf);
    special(LOG2, -1.0, 0, nan);
    special(LOG2, 1024.0, 0, 10.0);
    special(LOG2, inf, 0, inf);
    special(LOG10, 1.0, 0, 0.0);
    special(LOG10, -0.0, 0, -inf);
    special(LOG10, 1e22, 0, 22.0);
    special(LOG10, -1.0, 0, nan);
    special(SINH, -0.0, 0, -0.0);
    special(SINH, -inf, 0, -inf);
    special(SINH, 711.0, 0, inf);
    special(COSH, -0.0, 0, 1.0);
    special(COSH, -inf, 0, inf);
    special(COSH, 711.0, 0, inf);
    special(TANH, -0.0, 0, -0.0);
    special(TANH, inf, 0, 1.0);
    special(TANH, -inf, 0, -1.0);
    special(TANH, nan, 0, nan);
    /* the f32 ones: value and sign */
    float pif = 0x1.921fb6p+1f;
    special32(SIN, -0.0f, 0, -0.0f);
    special32(COS, inff, 0, nanf_);
    special32(EXP, -inff, 0, 0.0f);
    special32(EXP, 89.0f, 0, inff);
    special32(LN, -0.0f, 0, -inff);
    special32(LN, -1.0f, 0, nanf_);
    special32(POW, nanf_, -0.0f, 1.0f);
    special32(POW, -0.0f, -1.0f, -inff);
    special32(POW, -2.0f, 3.0f, -8.0f);
    special32(ATAN2, 0.0f, -0.0f, pif);
    special32(ATAN2, -0.0f, -0.0f, -pif);
    special32(HYPOT, inff, nanf_, inff);
    special32(HYPOT, -3.0f, 4.0f, 5.0f);
    special32(LOG2, 1024.0f, 0, 10.0f);
    special32(LOG10, 1e10f, 0, 10.0f);
    special32(SINH, -inff, 0, -inff);
    special32(COSH, -0.0f, 0, 1.0f);
    special32(TANH, -inff, 0, -1.0f);
}

int main(void)
{
    limba_set_program("test_math");
    test_special();
    test_bits();
    test_reference();
    test_mpfr();
    printf("test_math: %u checks of f64 and f32: special cases, the same "
           "bits on fixed arguments, correctly rounded against mpmath and "
           "against MPFR on worst cases; %d failures\n",
           checked, failures);
    return failures ? 1 : 0;
}
