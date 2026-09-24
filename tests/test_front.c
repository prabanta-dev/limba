/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_front.c - the shared pieces of the front ends against expected
 * results: integers and rationals of any size. The rounding of rationals
 * to double and float is also checked against strtod and strtof, which
 * glibc rounds correctly, on random literals.
 */
#include "front/bigint.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "test_front: line %d: ", __LINE__);                \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* r must be initialised */
static void big(limba_big *r, const char *dec)
{
    bool neg = dec[0] == '-';
    limba_big_parse(r, dec + neg, strlen(dec + neg), 10);
    if (neg)
        limba_big_neg(r, r);
}

static void expect_str(const limba_big *a, const char *want, int line)
{
    char buf[256];
    limba_big_str(a, buf, sizeof(buf));
    if (strcmp(buf, want)) {
        fprintf(stderr, "test_front: line %d: %s, expected %s\n", line, buf,
                want);
        failures++;
    }
}
#define EXPECT(a, want) expect_str(a, want, __LINE__)

static void test_integers(void)
{
    limba_big a, b, r, q, m;
    limba_big_init(&a);
    limba_big_init(&b);
    limba_big_init(&r);
    limba_big_init(&q);
    limba_big_init(&m);
    big(&a, "0");
    EXPECT(&a, "0");
    big(&a, "123456789012345678901234567890");
    EXPECT(&a, "123456789012345678901234567890");
    limba_big_parse(&a, "ffff_ffff_ffff_ffff_ffff", 24, 16);
    EXPECT(&a, "1208925819614629174706175");
    limba_big_parse(&a, "1010", 4, 2);
    EXPECT(&a, "10");

    big(&a, "5");
    big(&b, "-7");
    limba_big_add(&r, &a, &b);
    EXPECT(&r, "-2");
    big(&a, "-5");
    big(&b, "-5");
    limba_big_sub(&r, &a, &b);
    EXPECT(&r, "0");
    CHECK(!r.neg, "a negative zero");
    limba_big_neg(&r, &r);
    CHECK(!r.neg, "a negative zero after neg");

    big(&a, "18446744073709551615");
    limba_big_mul(&r, &a, &a);
    EXPECT(&r, "340282366920938463426481119284349108225");
    /* the result may be an operand */
    limba_big_add(&a, &a, &a);
    EXPECT(&a, "36893488147419103230");

    /* truncated division */
    big(&a, "7");
    big(&b, "-2");
    limba_big_divmod(&q, &m, &a, &b);
    EXPECT(&q, "-3");
    EXPECT(&m, "1");
    big(&a, "-7");
    big(&b, "2");
    limba_big_divmod(&q, &m, &a, &b);
    EXPECT(&q, "-3");
    EXPECT(&m, "-1");
    big(&a, "1000000000000000000000000000007");
    big(&b, "1000000000000000");
    limba_big_divmod(&q, &m, &a, &b);
    EXPECT(&q, "1000000000000000");
    EXPECT(&m, "7");
    big(&a, "340282366920938463426481119284349108225");
    big(&b, "18446744073709551615");
    limba_big_divmod(&q, &m, &a, &b);
    EXPECT(&q, "18446744073709551615");
    EXPECT(&m, "0");

    big(&a, "2");
    CHECK(limba_big_pow(&r, &a, 100), "2 ** 100 fits");
    EXPECT(&r, "1267650600228229401496703205376");
    CHECK(!limba_big_pow(&r, &a, 20000), "2 ** 20000 is past the limit");
    CHECK(limba_big_pow(&r, &a, 16000), "2 ** 16000 fits");
    CHECK(limba_big_bits(&r) == 16001, "2 ** 16000 has 16001 bits");
    CHECK(!limba_big_mul(&r, &r, &r), "(2 ** 16000) ** 2 is past the limit");

    big(&a, "462");
    big(&b, "1071");
    limba_big_gcd(&r, &a, &b);
    EXPECT(&r, "21");

    int64_t i;
    uint64_t u;
    big(&a, "-9223372036854775808");
    CHECK(limba_big_to_i64(&a, &i) && i == INT64_MIN, "INT64_MIN");
    big(&a, "9223372036854775808");
    CHECK(!limba_big_to_i64(&a, &i), "2 ** 63 is past int64");
    CHECK(limba_big_to_u64(&a, &u) && u == (uint64_t)1 << 63, "2 ** 63");
    big(&a, "18446744073709551616");
    CHECK(!limba_big_to_u64(&a, &u), "2 ** 64 is past uint64");
    big(&a, "-1");
    CHECK(!limba_big_to_u64(&a, &u), "-1 is not a uint64");

    limba_big_free(&a);
    limba_big_free(&b);
    limba_big_free(&r);
    limba_big_free(&q);
    limba_big_free(&m);
}

/* r must be initialised */
static void rat(limba_rat *r, const char *lit)
{
    if (!limba_rat_parse(r, lit, strlen(lit)))
        fprintf(stderr, "test_front: cannot parse %s\n", lit), failures++;
}

static double f64(const char *lit, bool *ok)
{
    limba_rat r;
    limba_rat_init(&r);
    rat(&r, lit);
    double v;
    *ok = limba_rat_to_f64(&r, &v);
    limba_rat_free(&r);
    return v;
}

static void test_rationals(void)
{
    limba_rat a, b, c, r;
    limba_rat_init(&a);
    limba_rat_init(&b);
    limba_rat_init(&c);
    limba_rat_init(&r);
    rat(&a, "0.1");
    rat(&b, "0.2");
    rat(&c, "0.3");
    limba_rat_add(&r, &a, &b);
    CHECK(limba_rat_cmp(&r, &c) == 0, "0.1 + 0.2 = 0.3 exactly");
    rat(&a, "2");
    rat(&b, "3");
    limba_rat_div(&c, &a, &b);
    limba_rat_pow(&r, &c, -2);
    EXPECT(&r.num, "9");
    EXPECT(&r.den, "4");
    rat(&a, "2");
    limba_rat_neg(&a, &a);
    limba_rat_pow(&r, &a, 3);
    EXPECT(&r.num, "-8");
    CHECK(limba_rat_is_int(&r), "-8 is an integer");
    rat(&a, "1_000.250_0e-3");
    EXPECT(&a.num, "4001");
    EXPECT(&a.den, "4000");

    bool ok;
    CHECK(f64("0.1", &ok) == 0.1 && ok, "0.1");
    CHECK(f64("1.5e-9", &ok) == 1.5e-9 && ok, "1.5e-9");
    CHECK(f64("5e-324", &ok) == 4.9406564584124654e-324 && ok,
          "the least subnormal");
    /* exactly half the least subnormal, 2^-1075: ties to even, zero */
    limba_rat half;
    limba_rat_init(&half);
    limba_big_set_u64(&half.num, 1);
    limba_big_set_u64(&half.den, 1);
    limba_big_shl(&half.den, &half.den, 1075);
    double hd;
    CHECK(limba_rat_to_f64(&half, &hd) && hd == 0, "2^-1075 rounds to zero");
    limba_big_set_u64(&half.num, 3); /* 1.5 * 2^-1075 */
    limba_big_shl(&half.den, &half.den, 1);
    limba_big_set_u64(&half.num, 3);
    CHECK(limba_rat_to_f64(&half, &hd) && hd == 4.9406564584124654e-324,
          "0.75 of the least subnormal rounds up to it");
    limba_rat_free(&half);
    CHECK(f64("2.4703282292062328e-324", &ok) == 4.9406564584124654e-324,
          "just above half the least subnormal");
    CHECK(f64("1.7976931348623157e308", &ok) == DBL_MAX && ok, "DBL_MAX");
    f64("1.8e308", &ok);
    CHECK(!ok, "1.8e308 is past double");
    CHECK(f64("9007199254740993", &ok) == 9007199254740992.0,
          "2^53 + 1 ties to even, down");
    CHECK(f64("9007199254740995", &ok) == 9007199254740996.0,
          "2^53 + 3 ties to even, up");
    rat(&a, "0.1"); /* the sign of a literal is an operator */
    limba_rat_neg(&a, &a);
    float f;
    CHECK(limba_rat_to_f32(&a, &f) && f == -0.1f, "-0.1f");
    rat(&a, "3.4028235e38");
    CHECK(limba_rat_to_f32(&a, &f) && f == FLT_MAX, "FLT_MAX");
    rat(&a, "3.5e38");
    CHECK(!limba_rat_to_f32(&a, &f), "3.5e38 is past float");
    rat(&a, "1e-50");
    CHECK(limba_rat_to_f32(&a, &f) && f == 0, "1e-50 is zero in float");
    rat(&a, "1e4000");
    double d;
    CHECK(!limba_rat_to_f64(&a, &d), "1e4000 is past double");
    CHECK(!limba_rat_parse(&a, "1e5000", 6), "1e5000 is past the limit");

    limba_rat_free(&a);
    limba_rat_free(&b);
    limba_rat_free(&c);
    limba_rat_free(&r);
}

/* random decimal literals rounded as strtod and strtof round them */
static void test_rounding(unsigned count)
{
    uint64_t s = 0x9e3779b97f4a7c15ull;
    unsigned wrong = 0;
    for (unsigned k = 0; k < count; k++) {
        char lit[64];
        int n = 0;
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        unsigned digits = 1 + s % 20;
        for (unsigned i = 0; i < digits; i++) {
            s ^= s << 13, s ^= s >> 7, s ^= s << 17;
            lit[n++] = (char)('0' + s % 10);
            if (i == 0)
                lit[n++] = '.';
        }
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        int exp = (int)(s % 700) - 350;
        if (k % 4 == 0)
            exp = (int)(s % 90) - 45; /* the range of float */
        n += snprintf(lit + n, sizeof(lit) - (size_t)n, "e%d", exp);
        limba_rat r;
        limba_rat_init(&r);
        rat(&r, lit);
        double d, want = strtod(lit, NULL);
        bool ok = limba_rat_to_f64(&r, &d);
        if (ok != !isinf(want) || (ok && d != want)) {
            if (wrong++ < 5)
                fprintf(stderr, "test_front: %s: %.17g, strtod %.17g\n", lit, d,
                        want);
        }
        float f, wantf = strtof(lit, NULL);
        ok = limba_rat_to_f32(&r, &f);
        if (ok != !isinf(wantf) || (ok && f != wantf)) {
            if (wrong++ < 5)
                fprintf(stderr, "test_front: %s: %.9g, strtof %.9g\n", lit,
                        (double)f, (double)wantf);
        }
        limba_rat_free(&r);
    }
    failures += (int)wrong;
}

int main(void)
{
    test_integers();
    test_rationals();
    test_rounding(20000);
    printf("test_front: integers, rationals, 20000 random roundings to "
           "double and float, %d failures\n",
           failures);
    return failures ? 1 : 0;
}
