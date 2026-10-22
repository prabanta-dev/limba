/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * decimal.c - exact steps between binary reals and decimal digits (see
 * decimal.h). The numbers of 128 bits are two words, so the steps are the
 * same C on every machine.
 */
#include "decimal.h"

#include <float.h>
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

bool limba_dec_scaled(double a, int q, uint64_t *fl, int *half)
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

bool limba_dec_read(uint64_t d, int k, bool f32, double *v)
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
