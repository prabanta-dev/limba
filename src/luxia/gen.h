/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gen.h - random Luxia programs that know what they print. The generator
 * grows a well-typed program, prints its source and runs it itself with
 * the rules of the specification, written again here and shared with no
 * part of the front end: that run is the oracle for the whole chain from
 * the source to the IR. Integers of every family, Boolean, routines with
 * in and var parameters, the statements of Luxia 0; the run-time errors
 * of arithmetic stop the program where they happen. Not public.
 */
#ifndef LIMBA_LUXIA_GEN_H
#define LIMBA_LUXIA_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *src; /* the program, NUL-terminated */
    char *out; /* what it prints, NUL-terminated */
    size_t outlen;
    char end[64]; /* "ok" or "trap CODE at LINE:COLUMN" */
} limba_lxgen;

/* the program of a seed; false if no attempt ran within the limits */
bool limba_lxgen_make(uint64_t seed, limba_lxgen *p);
void limba_lxgen_free(limba_lxgen *p);

#endif
