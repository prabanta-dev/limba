/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_reals.c - the text of reals, written and read, against the C
 * library of this machine (glibc: its printf and strtod round exactly)
 * on many values: the shortest form (limba_fmt_f64, limba_fmt_f32), the
 * fixed one (limba_fmt_f64_fixed) and val (limba_val_real), all Limba's
 * own: short decimals for the 128-bit steps of common/decimal.c, random
 * bits, values past 64 bits, long texts and the halfway points between
 * two doubles for the integers of bigint.c.
 */
#include "common/decimal.h"
#include "limba/fmt.h"
#include "limba/ir.h"
#include "limba/val.h"

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
                fprintf(stderr, "test_reals: ");                               \
                fprintf(stderr, __VA_ARGS__);                                  \
                fputc('\n', stderr);                                           \
            }                                                                  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static uint64_t seed = 12345;

static uint64_t next(void)
{
    uint64_t z = (seed += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* ---- the reference: the C library, the way fmt.c wrote reals before ---- */

static size_t ref_fmt(char *buf, double v, bool f32)
{
    if (isnan(v))
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, "nan");
    if (isinf(v))
        return (size_t)snprintf(buf, LIMBA_FMT_F64_MAX, v < 0 ? "-inf" : "inf");
    char e[LIMBA_FMT_F64_MAX];
    for (int p = 1; p <= 17; p++) {
        snprintf(e, sizeof(e), "%.*e", p - 1, v);
        if (f32) {
            float back = strtof(e, NULL), fv = (float)v;
            if (memcmp(&back, &fv, sizeof(fv)) == 0)
                break;
        } else {
            double back = strtod(e, NULL);
            if (memcmp(&back, &v, sizeof(v)) == 0)
                break;
        }
    }
    const char *s = e;
    bool neg = *s == '-';
    if (neg)
        s++;
    char digits[20];
    size_t nd = 0;
    for (; *s != 'e'; s++)
        if (*s != '.')
            digits[nd++] = *s;
    int exp = atoi(s + 1);
    while (nd > 1 && digits[nd - 1] == '0')
        nd--;
    size_t n = 0;
    if (neg)
        buf[n++] = '-';
    if (exp >= -4 && exp < 16) {
        if (exp < 0) {
            buf[n++] = '0';
            buf[n++] = '.';
            for (int k = -1; k > exp; k--)
                buf[n++] = '0';
            memcpy(buf + n, digits, nd);
            n += nd;
        } else {
            for (int k = 0; k <= exp; k++)
                buf[n++] = (size_t)k < nd ? digits[k] : '0';
            buf[n++] = '.';
            if ((size_t)exp + 1 < nd) {
                memcpy(buf + n, digits + exp + 1, nd - (size_t)exp - 1);
                n += nd - (size_t)exp - 1;
            } else {
                buf[n++] = '0';
            }
        }
    } else {
        buf[n++] = digits[0];
        if (nd > 1) {
            buf[n++] = '.';
            memcpy(buf + n, digits + 1, nd - 1);
            n += nd - 1;
        }
        n += (size_t)snprintf(buf + n, LIMBA_FMT_F64_MAX - n, "e%c%02d",
                              exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
    }
    buf[n] = 0;
    return n;
}

/* ---- the checks ---- */

static void short_f64(double v)
{
    char a[LIMBA_FMT_F64_MAX], b[LIMBA_FMT_F64_MAX];
    limba_fmt_f64(a, v);
    ref_fmt(b, v, false);
    CHECK(!strcmp(a, b), "%a printed %s, expected %s", v, a, b);
}

static void short_f32(float v)
{
    char a[LIMBA_FMT_F64_MAX], b[LIMBA_FMT_F64_MAX];
    limba_fmt_f32(a, v);
    ref_fmt(b, v, true);
    CHECK(!strcmp(a, b), "float %a printed %s, expected %s", (double)v, a, b);
}

static void fixed(double v, int d)
{
    char a[LIMBA_FMT_FIXED_MAX], b[LIMBA_FMT_FIXED_MAX];
    limba_fmt_f64_fixed(a, v, d);
    if (isnan(v))
        strcpy(b, "nan");
    else
        snprintf(b, sizeof(b), "%.*f", d, v);
    CHECK(!strcmp(a, b), "%a to %d decimals printed %s, expected %s", v, d, a,
          b);
}

/* text read by val and by strtod/strtof */
static void read_text(const char *t, bool f32)
{
    uint64_t bits = 0, want;
    bool ok = limba_val_real(t, strlen(t), f32, &bits);
    if (f32) {
        float f = strtof(t, NULL);
        uint32_t u;
        memcpy(&u, &f, sizeof(u));
        want = u;
        if (isinf(f)) {
            CHECK(!ok, "%s read as a float, past its range", t);
            return;
        }
    } else {
        double x = strtod(t, NULL);
        memcpy(&want, &x, sizeof(want));
        if (isinf(x)) {
            CHECK(!ok, "%s read, past the range", t);
            return;
        }
    }
    CHECK(ok && bits == want, "%s read as %s: %llx, expected %llx", t,
          f32 ? "float" : "double", (unsigned long long)bits,
          (unsigned long long)want);
}

/* a decimal literal of Luxia: digits.digits, an exponent sometimes */
static void random_decimal(char *t, int ndig, int point, int exp)
{
    size_t n = 0;
    for (int i = 0; i < ndig; i++) {
        if (i == point)
            t[n++] = '.';
        t[n++] = (char)('0' + (i == 0 ? 1 + next() % 9 : next() % 10));
    }
    if (point >= ndig) {
        t[n++] = '.';
        t[n++] = '0';
    }
    if (exp)
        n += (size_t)sprintf(t + n, "e%d", exp);
    t[n] = 0;
}

static void test_values(unsigned count)
{
    static const double edges[] = {
        0.0,
        -0.0,
        1.0,
        0.1,
        0.5,
        2.5,
        1e15,
        1e16,
        1e22,
        1e23,
        9007199254740992.0,
        9007199254740993.0,
        5e-324,
        2.2250738585072014e-308,
        2.2250738585072009e-308,
        1.7976931348623157e308,
        123456789012345678.0,
        0.30000000000000004,
        1.0 / 3.0,
        1e-4,
        1.5e-5,
        -123.456,
        INFINITY,
        -INFINITY,
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        short_f64(edges[i]);
        short_f32((float)edges[i]);
        for (int d = 0; d <= 20; d++)
            fixed(edges[i], d);
    }
    double nan_neg = -NAN, nan_pos = NAN;
    short_f64(nan_neg);
    short_f64(nan_pos);
    fixed(nan_neg, 2);
    for (unsigned i = 0; i < count; i++) {
        /* random bits: the slow way, mostly */
        uint64_t z = next();
        double v;
        memcpy(&v, &z, sizeof(v));
        if (!isnan(v)) {
            short_f64(v);
            fixed(v, (int)(next() % 21));
        }
        uint32_t u = (uint32_t)next();
        float f;
        memcpy(&f, &u, sizeof(f));
        if (!isnan(f))
            short_f32(f);
        /* short decimals: k * 10^j, as programs write them */
        char t[64];
        uint64_t k = next() % limba_dec_pow10(1 + (int)(next() % 15));
        int j = (int)(next() % 51) - 25;
        snprintf(t, sizeof(t), "%llue%d", (unsigned long long)k, j);
        v = strtod(t, NULL);
        short_f64(next() & 1 ? -v : v);
        short_f32(strtof(t, NULL));
        fixed(v, (int)(next() % 26));
        /* ties of the fixed form: (2m + 1) / 2^n to n - 1 decimals */
        int nn = 1 + (int)(next() % 12);
        double tie = ldexp((double)(2 * (next() % 100000) + 1), -nn);
        fixed(tie, nn - 1);
        fixed(-tie, nn - 1);
    }
}

static void test_reading(unsigned count)
{
    static const char *const texts[] = {
        "0.0",
        "1.0",
        "0.1",
        "1e23",
        "9007199254740993.0",
        "2.2250738585072011e-308",
        "4.9406564584124654e-324",
        "1.7976931348623158e308",
        "1.8e308",
        "3.4028235e38",
        "3.4028236e38",
        "1e-46",
        "0.000000000000000000000000000001",
        "123456789012345678901234567890.0",
        "0e100000000",
        "1e-100000000",
        "0x1fffffffffffff",
        "0x20000000000001",
        "0x20000000000003",
        "0xffffffffffffffffffff",
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        read_text(texts[i], false);
        read_text(texts[i], true);
    }
    for (unsigned i = 0; i < count; i++) {
        char t[96];
        int ndig = 1 + (int)(next() % 25);
        int point = 1 + (int)(next() % (unsigned)ndig);
        int exp = next() % 3 ? 0 : (int)(next() % 661) - 330;
        random_decimal(t, ndig, point, exp);
        bool f32 = next() & 1;
        read_text(t, f32);
        /* digits of base 2 and 16, and the same as hexadecimal for strtod */
        unsigned nb = 1 + (unsigned)(next() % 100);
        char bin[128], hex[64];
        size_t h = 0;
        unsigned pad = (4 - nb % 4) % 4, acc = 0;
        for (unsigned b = 0; b < nb; b++)
            bin[b] = (char)('0' + (b == 0 ? 1 : (int)(next() & 1)));
        bin[nb] = 0;
        for (unsigned b = 0; b < nb + pad; b++) {
            acc = acc << 1 | (b < pad ? 0u : (unsigned)(bin[b - pad] - '0'));
            if (b % 4 == 3) {
                hex[h++] = "0123456789abcdef"[acc];
                acc = 0;
            }
        }
        hex[h] = 0;
        char lit[160], ref[80];
        snprintf(lit, sizeof(lit), "0b%s", bin);
        snprintf(ref, sizeof(ref), "0x%sp0", hex);
        uint64_t got = 0, want;
        f32 = next() & 1;
        bool ok = limba_val_real(lit, strlen(lit), f32, &got);
        if (f32) {
            float f = strtof(ref, NULL);
            uint32_t u;
            memcpy(&u, &f, sizeof(u));
            want = u;
        } else {
            double x = strtod(ref, NULL);
            memcpy(&want, &x, sizeof(want));
        }
        CHECK(ok && got == want, "%s read as %s: %llx, expected %llx", lit,
              f32 ? "float" : "double", (unsigned long long)got,
              (unsigned long long)want);
        snprintf(lit, sizeof(lit), "0x%s", hex);
        ok = limba_val_real(lit, strlen(lit), f32, &got);
        CHECK(ok && got == want, "%s read: %llx, expected %llx", lit,
              (unsigned long long)got, (unsigned long long)want);
    }
}

/* the exact rounding itself, where it divides by 5^r and by a power of
   two at once (large values with a binary fraction): the digits of
   printf at the same place. The shortest form seldom shows this branch,
   since its digits there are not the ones printed */
static void test_scaled(unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        double a = ldexp((double)(next() >> 11 | 1), (int)(next() % 14) - 0);
        a = ldexp(a, -(int)(next() % 13)); /* odd mantissa, binary fraction */
        if (a < 10)
            continue;
        int e10 = (int)floor(log10(a));
        int r = 1 + (int)(next() % 13);
        int p = e10 + 1 - r;
        if (p < 1 || p > 19)
            continue;
        char t[64];
        snprintf(t, sizeof(t), "%.*e", p - 1, a);
        uint64_t want = 0;
        for (const char *c = t; *c && *c != 'e'; c++)
            if (*c != '.')
                want = want * 10 + (uint64_t)(*c - '0');
        if (atoi(strchr(t, 'e') + 1) != e10)
            want *= 10; /* rounded up to the next power of ten */
        uint64_t got;
        bool ok = limba_dec_round(a, -r, &got);
        CHECK(!ok || got == want,
              "%a times 10^-%d rounded to %llu, expected "
              "%llu",
              a, r, (unsigned long long)got, (unsigned long long)want);
    }
}

/* the slow way: the fixed form past 64 bits and with many decimals,
   long decimal texts, and the exact halfway point between two doubles
   (a long double holds it; printf writes it exactly), read as it is,
   one digit up and one down */
static void test_slow(unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        double v;
        uint64_t z = next();
        memcpy(&v, &z, sizeof(v));
        if (isnan(v) || isinf(v))
            continue;
        fixed(v, (int)(next() % 101));
        /* a long text: the digits of v and more */
        char t[1200];
        snprintf(t, sizeof(t), "%.*e", (int)(20 + next() % 900), fabs(v));
        read_text(t, next() & 1);
        /* halfway between v and the next double, exactly */
        double a = fabs(v), b = nextafter(a, INFINITY);
        if (isinf(b) || a == 0)
            continue;
        long double mid = ((long double)a + (long double)b) / 2;
        snprintf(t, sizeof(t), "%.800Le", mid);
        char *e = strchr(t, 'e');
        char *last = e - 1;
        while (*last == '0')
            last--;
        size_t keep = (size_t)(last - t) + 1;
        char text[1200];
        snprintf(text, sizeof(text), "%.*s%s", (int)keep, t, e);
        read_text(text, false);
        /* past 800 digits: zeros, then a 1 that only the rest of the
           digits can tell (just past the half) */
        if (keep < 790) {
            snprintf(text, sizeof(text), "%.*s%0*d1%s", (int)keep, t,
                     (int)(850 - keep), 0, e);
            read_text(text, false);
        }
        /* one unit up in the last digit kept: past the half */
        if (*last >= '0' && *last <= '8') {
            (*last)++;
            snprintf(text, sizeof(text), "%.*s%s", (int)keep, t, e);
            read_text(text, false);
            (*last)--;
        }
        /* one down: below the half */
        if (*last >= '1' && *last <= '9') {
            (*last)--;
            snprintf(text, sizeof(text), "%.*s%s", (int)keep, t, e);
            read_text(text, false);
        }
    }
    /* ties of the fixed form past 64 bits: (2m + 1) / 2^n, n - 1
       decimals, m large */
    for (unsigned i = 0; i < count; i++) {
        int nn = 2 + (int)(next() % 30);
        double tie = ldexp((double)(2 * (next() >> 12) + 1), -nn);
        fixed(tie, nn - 1);
    }
    static const char *const far[] = {
        "1.0e400",
        "1.0e-400",
        "123.0e-330",
        "2.4703282292062328e-324",
        "2.4703282292062327e-324",
        "2.4703282292062328e-324",
        "7.0064923216240854e-46",
        "7.0064923216240853e-46",
        "1.7976931348623158e308",
        "1.7976931348623157e308",
        "3.4028235677973366e38",
        "3.4028235677973367e38",
    };
    for (size_t i = 0; i < sizeof(far) / sizeof(far[0]); i++) {
        read_text(far[i], false);
        read_text(far[i], true);
    }
    static const double big[] = {1e300, -1e300, 1.7976931348623157e308,
                                 1e20,  1e-300, 123456789012345678901.0};
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++)
        for (int d = 0; d <= 100; d += 7)
            fixed(big[i], d);
}

int main(void)
{
    limba_set_program("test_reals");
    test_values(20000);
    test_reading(20000);
    test_scaled(20000);
    test_slow(5000);
    printf("test_reals: %u checks against the C library: short and fixed "
           "forms, val, the slow way (past 64 bits, long texts, halfway "
           "points); %d failures\n",
           checked, failures);
    return failures ? 1 : 0;
}
