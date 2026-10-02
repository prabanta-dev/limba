/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gen.h - random Luxia programs that know what they print. The generator
 * grows a well-typed program, prints its source and runs it itself with
 * the rules of the specification, written again here and shared with no
 * part of the front end: that run is the oracle for the whole chain from
 * the source to the IR. Integers of every family, ranges, enumerations,
 * Boolean, Float32 and Float64, Char and String, arrays (open and with
 * computed bounds too), records, pointers, exact constants, routines
 * with in, var and out parameters and results of every kind, the
 * statements of Luxia 0 and much of its library; the run-time errors
 * stop the program where they happen. Not public.
 */
#ifndef LIMBA_LUXIA_GEN_H
#define LIMBA_LUXIA_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* a unit of a program in more files: the name of its file without
   .luxia, its text, in the space of the library or of the program */
typedef struct {
    char name[16];
    char *text;
    bool library;
} limba_lxgen_file;

#define LIMBA_LXGEN_FILES 9

typedef struct {
    char *src; /* the program, NUL-terminated */
    char *out; /* what it prints, NUL-terminated */
    size_t outlen;
    char end[64]; /* "ok", "ok, N live, 0 bad frees" (N records made by
                     new never freed) or "trap CODE at LINE:COLUMN" */
    char *in;     /* what the program reads, NULL for nothing */
    size_t inlen;
    int argc; /* its command line, for arg */
    char **argv;
    /* the units the program uses: first Lib (lib.luxia; a place in it is
       "lib.luxia:LINE:COLUMN" in end), then sometimes a web of units of
       the program and of the library (§ 11) */
    limba_lxgen_file file[LIMBA_LXGEN_FILES];
    unsigned nfile;
    /* the codes of the warnings and notes expected, separated by spaces */
    char notes[32];
} limba_lxgen;

/* the program of a seed; false if no attempt ran within the limits */
bool limba_lxgen_make(uint64_t seed, limba_lxgen *p);
/* the same program in more files (§ 11): the routines that name nothing
   of the program go into the unit Lib, named directly or as Lib.Name,
   and Lib's initialisation prints "lib" first; often a web of units
   too, whose values the program prints first */
bool limba_lxgen_make_units(uint64_t seed, limba_lxgen *p);
void limba_lxgen_free(limba_lxgen *p);

#endif
