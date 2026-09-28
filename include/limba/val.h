/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * val.h - the number written in a text, as val reads it (luxia-0.md
 * § 9.4): the rules of str_to_i64, str_to_u64, str_to_f64, str_to_f32 and
 * str_to_big (runtime.def). A runtime calls these, so that every back end reads
 * what the reference interpreter reads, byte for byte, as with limba/fmt.h.
 *
 * The text: spaces and tabs around, a sign (+ or -), then a literal of
 * Luxia: decimal, 0x, 0o or 0b digits, _ only between two digits; a real
 * with digits on both sides of the point and an optional exponent; inf,
 * or nan without a sign. They use strtod and strtof: call them under the
 * C locale (LC_NUMERIC "C").
 */
#ifndef LIMBA_VAL_H
#define LIMBA_VAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* an integer of s[0..len): its magnitude and sign; false for no integer,
   or one past 64 bits (the range of the type is the caller's) */
bool limba_val_int(const char *s, size_t len, uint64_t *mag, bool *neg);
/* an integer of s[0..len) of any size, for a BigInt: its digits of base
   (2, 8, 10 or 16) as characters, without the _, into digits (room for len
   bytes), their count in *n, and the sign; false for no integer. The
   caller turns them into its own number */
bool limba_val_big(const char *s, size_t len, char *digits, size_t *n,
                   unsigned *base, bool *neg);
/* a real (or an integer) of s[0..len), rounded once to f32 or to f64:
   *bits is the float of f32 (32 bits, zero-extended) or the double;
   false for no number, or a finite text beyond the type */
bool limba_val_real(const char *s, size_t len, bool f32, uint64_t *bits);

#endif
