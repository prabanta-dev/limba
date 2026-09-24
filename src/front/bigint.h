/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * bigint.h - integers and rationals of any size, for the constant
 * expressions of a front end: they are computed exactly and take a type
 * (and a check, and a rounding) only at the end, as in Ada (RM 4.9).
 *
 * A number is a sign and a magnitude in 32-bit words. Every operation
 * that could grow a number past LIMBA_BIG_MAXBITS fails (returns false)
 * instead: a constant like 2 ** 1000000 is an error of the program, not a
 * reason to exhaust the memory of the compiler. The result may be one of
 * the operands.
 */
#ifndef LIMBA_FRONT_BIGINT_H
#define LIMBA_FRONT_BIGINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIMBA_BIG_MAXBITS 16384

typedef struct {
    uint32_t *w; /* magnitude, least significant word first */
    uint32_t n;  /* words in use, no leading zero word; 0 for zero */
    uint32_t cap;
    bool neg; /* never set for zero */
} limba_big;

void limba_big_init(limba_big *a);
void limba_big_free(limba_big *a);
void limba_big_copy(limba_big *r, const limba_big *a);
void limba_big_set_u64(limba_big *r, uint64_t v);
void limba_big_set_i64(limba_big *r, int64_t v);

/* digits of base (2, 8, 10, 16), '_' skipped; false if too large */
bool limba_big_parse(limba_big *r, const char *s, size_t n, unsigned base);

static inline bool limba_big_is_zero(const limba_big *a)
{
    return a->n == 0;
}
/* -1, 0 or 1 */
int limba_big_sign(const limba_big *a);
int limba_big_cmp(const limba_big *a, const limba_big *b);
/* bits of the magnitude: 0 for zero, 1 for 1, 3 for 5 */
uint32_t limba_big_bits(const limba_big *a);

void limba_big_neg(limba_big *r, const limba_big *a);
void limba_big_abs(limba_big *r, const limba_big *a);
bool limba_big_add(limba_big *r, const limba_big *a, const limba_big *b);
bool limba_big_sub(limba_big *r, const limba_big *a, const limba_big *b);
bool limba_big_mul(limba_big *r, const limba_big *a, const limba_big *b);
/* truncated division: q = a / b rounded toward zero, m = a - q * b (the
   sign of a); b must not be zero; q or m may be NULL */
void limba_big_divmod(limba_big *q, limba_big *m, const limba_big *a,
                      const limba_big *b);
bool limba_big_shl(limba_big *r, const limba_big *a, uint32_t bits);
bool limba_big_pow(limba_big *r, const limba_big *a, uint64_t e);
void limba_big_gcd(limba_big *r, const limba_big *a, const limba_big *b);

/* the value if it fits */
bool limba_big_to_i64(const limba_big *a, int64_t *v);
bool limba_big_to_u64(const limba_big *a, uint64_t *v);

/* decimal digits, with a '-', into buf; truncated with "..." if longer */
const char *limba_big_str(const limba_big *a, char *buf, size_t size);

/* a rational: den > 0, gcd(num, den) = 1 */
typedef struct {
    limba_big num, den;
} limba_rat;

void limba_rat_init(limba_rat *a); /* zero */
void limba_rat_free(limba_rat *a);
void limba_rat_copy(limba_rat *r, const limba_rat *a);
void limba_rat_set_big(limba_rat *r, const limba_big *a);
/* a real literal: digits, '.', digits, 'e', sign, digits, '_' skipped */
bool limba_rat_parse(limba_rat *r, const char *s, size_t n);
static inline bool limba_rat_is_int(const limba_rat *a)
{
    return a->den.n == 1 && a->den.w[0] == 1;
}
int limba_rat_cmp(const limba_rat *a, const limba_rat *b);
bool limba_rat_add(limba_rat *r, const limba_rat *a, const limba_rat *b);
bool limba_rat_sub(limba_rat *r, const limba_rat *a, const limba_rat *b);
bool limba_rat_mul(limba_rat *r, const limba_rat *a, const limba_rat *b);
/* b must not be zero */
bool limba_rat_div(limba_rat *r, const limba_rat *a, const limba_rat *b);
/* integer exponent, negative too (a must not be zero then) */
bool limba_rat_pow(limba_rat *r, const limba_rat *a, int64_t e);
void limba_rat_neg(limba_rat *r, const limba_rat *a);

/* the nearest double or float, ties to even, subnormals included; false
   if the magnitude rounds past the largest finite value */
bool limba_rat_to_f64(const limba_rat *a, double *v);
bool limba_rat_to_f32(const limba_rat *a, float *v);

#endif
