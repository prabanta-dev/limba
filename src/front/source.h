/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * source.h - the source files of a compilation and the positions in them.
 *
 * A position (limba_loc) is one 32-bit number. Every file occupies a range
 * of a single space, as SourceLocation does in Clang, so a token or an IR
 * instruction carries file, line and column in 4 bytes; the line and the
 * column are found again only when a message needs them.
 */
#ifndef LIMBA_FRONT_SOURCE_H
#define LIMBA_FRONT_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t limba_loc;
#define LIMBA_NOLOC 0u /* no position: the first file starts at 1 */

typedef struct {
    char *path;     /* as given */
    char *text;     /* the bytes, followed by a NUL */
    uint32_t len;   /* without the NUL */
    limba_loc base; /* the position of text[0]; text[len] is base + len */
    uint32_t *line; /* offsets of the line starts, line[0] = 0 */
    uint32_t nlines;
    uint8_t *ascii; /* per line: 1 when it is all ASCII, and a column is a
                       byte */
} limba_srcfile;

typedef struct {
    uint32_t file; /* index in limba_source.file */
    uint32_t off;  /* byte offset in the file */
    uint32_t line; /* from 1 */
    uint32_t col;  /* from 1, in UTF-8 characters */
} limba_where;

typedef struct {
    limba_srcfile *file; /* in order of base */
    uint32_t count, cap;
    limba_loc next; /* the base of the next file */
    /* the last answer of limba_source_where, a memo that changes no
       answer: the next position is often further on the same line */
    limba_loc memo_loc;
    limba_where memo;
} limba_source;

void limba_source_init(limba_source *s);
void limba_source_free(limba_source *s);

/* read a file; its index, or UINT32_MAX with errno set (EFBIG when the
   space of positions is full) */
uint32_t limba_source_load(limba_source *s, const char *path);
/* the same from bytes in memory, which are copied */
uint32_t limba_source_add(limba_source *s, const char *path, const char *text,
                          size_t len);

/* where loc is; false for LIMBA_NOLOC or a position of no file */
bool limba_source_where(const limba_source *s, limba_loc loc, limba_where *w);

/* the bytes of line (from 1) of file, without its line end */
const char *limba_source_line(const limba_source *s, uint32_t file,
                              uint32_t line, uint32_t *len);

/* the offset of the first byte of p that is not valid UTF-8 (overlong
   forms, surrogates and values past U+10FFFF included), or len */
size_t limba_utf8_check(const char *p, size_t len);
/* the code point at p, valid UTF-8 already checked; *n its bytes */
uint32_t limba_utf8_decode(const char *p, unsigned *n);

#endif
