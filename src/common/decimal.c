/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * decimal.c - exact steps between binary reals and decimal digits (see
 * decimal.h). The numbers of 128 bits are two words, so the steps are the
 * same C on every machine.
 */
#include "decimal.h"

#include "common/bigint.h"
#include "common/xalloc.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

/* every float operation here must round once, to its own type */
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "decimal.c needs FLT_EVAL_METHOD 0: each operation in its own type"
#endif

typedef struct {
    uint64_t hi, lo;
} u128;

static const uint64_t pow5[28] = {
    1ull,
    5ull,
    25ull,
    125ull,
    625ull,
    3125ull,
    15625ull,
    78125ull,
    390625ull,
    1953125ull,
    9765625ull,
    48828125ull,
    244140625ull,
    1220703125ull,
    6103515625ull,
    30517578125ull,
    152587890625ull,
    762939453125ull,
    3814697265625ull,
    19073486328125ull,
    95367431640625ull,
    476837158203125ull,
    2384185791015625ull,
    11920928955078125ull,
    59604644775390625ull,
    298023223876953125ull,
    1490116119384765625ull,
    7450580596923828125ull,
};

uint64_t limba_dec_pow10(int k)
{
    uint64_t p = 1;
    while (k-- > 0)
        p *= 10;
    return p;
}

/* a * b, whole */
static u128 mul64(uint64_t a, uint64_t b)
{
    uint64_t al = a & 0xffffffffu, ah = a >> 32;
    uint64_t bl = b & 0xffffffffu, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
    u128 r;
    r.lo = (mid << 32) | (ll & 0xffffffffu);
    r.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return r;
}

static unsigned bits64(uint64_t x)
{
    unsigned n = 0;
    while (x) {
        n++;
        x >>= 1;
    }
    return n;
}

static unsigned bits(u128 x)
{
    return x.hi ? 64 + bits64(x.hi) : bits64(x.lo);
}

/* x << n, n < 128 */
static u128 shl(u128 x, unsigned n)
{
    if (n == 0)
        return x;
    if (n >= 64)
        return (u128){x.lo << (n - 64), 0};
    return (u128){(x.hi << n) | (x.lo >> (64 - n)), x.lo << n};
}

/* x >> n, n < 128 */
static u128 shr(u128 x, unsigned n)
{
    if (n == 0)
        return x;
    if (n >= 64)
        return (u128){0, x.hi >> (n - 64)};
    return (u128){x.hi >> n, (x.lo >> n) | (x.hi << (64 - n))};
}

/* the low n bits of x, n < 128 */
static u128 low_bits(u128 x, unsigned n)
{
    if (n >= 64)
        return (u128){n == 64 ? 0 : x.hi & ((1ull << (n - 64)) - 1), x.lo};
    return (u128){0, n ? x.lo & ((1ull << n) - 1) : 0};
}

static int cmp(u128 a, u128 b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    return a.lo < b.lo ? -1 : a.lo > b.lo;
}

/* x / d and its remainder, 0 < d < 2^32 */
static u128 div_small(u128 x, uint64_t d, uint64_t *rem)
{
    uint32_t w[4] = {(uint32_t)(x.hi >> 32), (uint32_t)x.hi,
                     (uint32_t)(x.lo >> 32), (uint32_t)x.lo};
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t cur = (r << 32) | w[i];
        w[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
    *rem = r;
    return (u128){(uint64_t)w[0] << 32 | w[1], (uint64_t)w[2] << 32 | w[3]};
}

/* x * d + add, d < 2^32, the result below 2^128 */
static u128 mul_small(u128 x, uint64_t d, uint64_t add)
{
    u128 lo = mul64(x.lo, d);
    u128 r = {lo.hi + x.hi * d, lo.lo + add};
    if (r.lo < lo.lo)
        r.hi++;
    return r;
}

/* a > 0 finite as m * 2^e, m odd; false for anything else */
static bool split(double a, uint64_t *mm, int *ee)
{
    uint64_t u;
    memcpy(&u, &a, sizeof(u));
    unsigned ef = (unsigned)(u >> 52) & 0x7ff;
    uint64_t m = u & ((1ull << 52) - 1);
    int e;
    if (ef == 0x7ff || (ef == 0 && m == 0) || (u >> 63))
        return false; /* not a finite positive real */
    if (ef == 0)
        e = -1074;
    else {
        m |= 1ull << 52;
        e = (int)ef - 1075;
    }
    while (!(m & 1)) {
        m >>= 1;
        e++;
    }
    *mm = m;
    *ee = e;
    return true;
}

/* a * 10^q, a > 0 finite, as num / den, exactly */
static void big_value(double a, int q, limba_big *num, limba_big *den)
{
    uint64_t m;
    int e;
    split(a, &m, &e);
    limba_big ten, p;
    limba_big_init(&ten);
    limba_big_init(&p);
    limba_big_set_u64(num, m);
    limba_big_set_u64(den, 1);
    limba_big_set_u64(&ten, 10);
    limba_big_pow(&p, &ten, (uint64_t)(q < 0 ? -q : q));
    if (e >= 0)
        limba_big_shl(num, num, (uint32_t)e);
    else
        limba_big_shl(den, den, (uint32_t)-e);
    limba_big_mul(q >= 0 ? num : den, q >= 0 ? num : den, &p);
    limba_big_free(&ten);
    limba_big_free(&p);
}

/* the slow way of limba_dec_scaled: the integers of bigint.c */
static bool big_scaled(double a, int q, uint64_t *fl, int *half)
{
    limba_big num, den, quo, rem;
    limba_big_init(&num);
    limba_big_init(&den);
    limba_big_init(&quo);
    limba_big_init(&rem);
    big_value(a, q, &num, &den);
    limba_big_divmod(&quo, &rem, &num, &den);
    bool ok = limba_big_to_u64(&quo, fl);
    limba_big_shl(&rem, &rem, 1);
    *half = limba_big_cmp(&rem, &den);
    limba_big_free(&num);
    limba_big_free(&den);
    limba_big_free(&quo);
    limba_big_free(&rem);
    return ok;
}

/* the common cases in 128 bits; false when they cannot tell */
static bool fast_scaled(double a, int q, uint64_t *fl, int *half)
{
    uint64_t m;
    int e;
    if (!split(a, &m, &e))
        return false;
    /* a * 10^q = m * 5^q * 2^(e + q) */
    long t = (long)e + q;
    u128 num = {0, m};
    uint64_t den5 = 1;
    if (q >= 0) {
        if (q > 27)
            return false;
        num = mul64(m, pow5[q]);
    } else {
        if (-q > 13)
            return false; /* 5^13 is the last below 2^31 */
        den5 = pow5[-q];
    }
    unsigned den2 = 0;
    if (t > 0) {
        if (bits(num) + (unsigned long)t > 127)
            return false;
        num = shl(num, (unsigned)t);
    } else if (t < -1000)
        return false;
    else
        den2 = (unsigned)-t;
    /* the value is num / (den5 * 2^den2) */
    uint64_t r = 0;
    u128 quo = den5 > 1 ? div_small(num, den5, &r) : num;
    u128 f = den2 >= 128 ? (u128){0, 0} : shr(quo, den2);
    if (f.hi)
        return false;
    *fl = f.lo;
    /* the fraction (low + r / den5) / 2^den2 against 1/2 */
    if (den2 == 0) {
        *half = 2 * r < den5 ? -1 : 2 * r > den5;
        return true;
    }
    if (den2 >= 128) {
        /* quo < 2^116, so the value is below 2^-12 */
        *half = -1;
        return true;
    }
    u128 low = low_bits(quo, den2);
    if (den5 == 1) {
        *half = cmp(low, shl((u128){0, 1}, den2 - 1));
        return true;
    }
    if (den2 > 96)
        return false;
    *half = cmp(mul_small(low, den5, r), shl((u128){0, den5}, den2 - 1));
    return true;
}

bool limba_dec_scaled(double a, int q, uint64_t *fl, int *half)
{
    uint64_t m;
    int e;
    if (!split(a, &m, &e) || q < -800 || q > 800)
        return false;
    return fast_scaled(a, q, fl, half) || big_scaled(a, q, fl, half);
}

bool limba_dec_round(double a, int q, uint64_t *out)
{
    uint64_t f;
    int h;
    if (!limba_dec_scaled(a, q, &f, &h))
        return false;
    if (h > 0 || (h == 0 && (f & 1))) {
        if (f == UINT64_MAX)
            return false;
        f++;
    }
    *out = f;
    return true;
}

/* Clinger's fast path: false when it cannot tell */
static bool fast_read(uint64_t d, int k, bool f32, double *v)
{
    static const double p10[23] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    static const float p10f[11] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f,
                                   1e6f, 1e7f, 1e8f, 1e9f, 1e10f};
    if (f32) {
        if (d > (1ull << 24) || k < -10 || k > 10)
            return false;
        float x = (float)d;
        float r = k >= 0 ? x * p10f[k] : x / p10f[-k];
        *v = r;
        return true;
    }
    if (k > 22 && k <= 22 + 15) {
        /* the extra zeros into d, if it stays exact */
        uint64_t p = limba_dec_pow10(k - 22);
        if (d <= (1ull << 53) / p) {
            d *= p;
            k = 22;
        }
    }
    if (d > (1ull << 53) || k < -22 || k > 22)
        return false;
    double x = (double)d;
    *v = k >= 0 ? x * p10[k] : x / p10[-k];
    return true;
}

/* the digits t[0..n) times 10^k rounded once to the type, by the
   rationals of bigint.c (they round to nearest even, subnormals too) */
static bool big_read(const char *t, size_t n, long k, bool f32, double *v)
{
    limba_rat r;
    limba_big ten, p;
    limba_rat_init(&r);
    limba_big_init(&ten);
    limba_big_init(&p);
    limba_big_parse(&r.num, t, n, 10);
    limba_big_set_u64(&ten, 10);
    limba_big_pow(&p, &ten, (uint64_t)(k < 0 ? -k : k));
    if (k >= 0)
        limba_big_mul(&r.num, &r.num, &p);
    else
        limba_big_copy(&r.den, &p);
    bool ok;
    if (f32) {
        float f;
        ok = limba_rat_to_f32(&r, &f);
        *v = f;
    } else
        ok = limba_rat_to_f64(&r, v);
    limba_rat_free(&r);
    limba_big_free(&ten);
    limba_big_free(&p);
    return ok;
}

bool limba_dec_read(uint64_t d, int k, bool f32, double *v)
{
    if (fast_read(d, k, f32, v))
        return true;
    char t[24];
    int n = 0;
    do {
        t[n++] = (char)('0' + d % 10);
        d /= 10;
    } while (d);
    for (int i = 0; i < n / 2; i++) {
        char c = t[i];
        t[i] = t[n - 1 - i];
        t[n - 1 - i] = c;
    }
    return limba_dec_read_digits(t, (size_t)n, k, f32, v);
}

bool limba_dec_read_digits(const char *t, size_t n, long k, bool f32, double *v)
{
    while (n && *t == '0') {
        t++;
        n--;
    }
    while (n && t[n - 1] == '0') {
        n--;
        k++;
    }
    if (n == 0) {
        *v = 0;
        return true;
    }
    /* the magnitude: 10^(n + k - 1) <= value < 10^(n + k) */
    long top = (long)n + k;
    if (top - 1 > (f32 ? 38 : 308))
        return false; /* past the largest finite value */
    if (top < (f32 ? -46 : -325)) {
        *v = 0; /* below half the least subnormal */
        return true;
    }
    if (n <= 19) {
        uint64_t d = 0;
        for (size_t i = 0; i < n; i++)
            d = d * 10 + (uint64_t)(t[i] - '0');
        if (fast_read(d, (int)k, f32, v))
            return true;
    }
    /* past 800 digits, the rest only says whether it is zero: no halfway
       point of a double or a float has more than 767 significant digits,
       so a 1 after the first 800 rounds as all of them do */
    char *buf = NULL;
    if (n > 800) {
        bool rest = false;
        for (size_t i = 800; i < n && !rest; i++)
            rest = t[i] != '0';
        buf = limba_xmalloc(801);
        memcpy(buf, t, 800);
        buf[800] = rest ? '1' : '0';
        k += (long)n - 801;
        t = buf;
        n = 801;
    }
    bool ok = big_read(t, n, k, f32, v);
    free(buf);
    return ok;
}

size_t limba_dec_round_text(double a, int q, char *buf, size_t size)
{
    limba_big num, den, quo, rem;
    limba_big_init(&num);
    limba_big_init(&den);
    limba_big_init(&quo);
    limba_big_init(&rem);
    big_value(a, q, &num, &den);
    limba_big_divmod(&quo, &rem, &num, &den);
    limba_big_shl(&rem, &rem, 1);
    int c = limba_big_cmp(&rem, &den);
    if (c > 0 || (c == 0 && quo.n && (quo.w[0] & 1))) {
        limba_big one;
        limba_big_init(&one);
        limba_big_set_u64(&one, 1);
        limba_big_add(&quo, &quo, &one);
        limba_big_free(&one);
    }
    limba_big_str(&quo, buf, size);
    limba_big_free(&num);
    limba_big_free(&den);
    limba_big_free(&quo);
    limba_big_free(&rem);
    return strlen(buf);
}
