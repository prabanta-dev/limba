/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * diag.h - the diagnoses of a compilation: errors, warnings and notes with
 * a position and a stable code, gathered so that one run reports many of
 * them, and printed like GCC and Rust do:
 *
 *   hello.luxia:3:9: error[L0003]: string not closed on its line
 *       3 | writeln("hello);
 *         |         ^~~~~~~
 */
#ifndef LIMBA_FRONT_DIAG_H
#define LIMBA_FRONT_DIAG_H

#include "source.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { LIMBA_ERROR, LIMBA_WARNING, LIMBA_NOTE } limba_severity;

typedef struct {
    uint8_t sev;   /* limba_severity */
    uint16_t code; /* 0: none (notes) */
    limba_loc loc;
    uint32_t len; /* bytes marked from loc, 0 or 1 for a point */
    char *msg;
} limba_report_item;

typedef struct {
    const limba_source *src;
    char letter; /* before the code: 'L' for Luxia */
    limba_report_item *item;
    uint32_t count, cap;
    uint32_t errors;
    uint32_t max_errors; /* 0: no limit */
    bool full;           /* max_errors reached: later errors are dropped */
} limba_report;

void limba_report_init(limba_report *r, const limba_source *src, char letter,
                       uint32_t max_errors);
void limba_report_free(limba_report *r);

/* record a diagnosis; false once the error limit is reached, so that the
   caller may stop */
bool limba_report_add(limba_report *r, limba_severity sev, unsigned code,
                      limba_loc loc, uint32_t len, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

/* every diagnosis in order of arrival, with the line of source it points
   at, and a last line if errors were dropped */
void limba_report_print(const limba_report *r, FILE *out);

#endif
