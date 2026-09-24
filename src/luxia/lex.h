/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lex.h - the lexer of Luxia 0: a whole file becomes an array of tokens.
 *
 * Names are case-insensitive: the lexer interns them folded to lowercase,
 * so equal names have equal ids, and the spelling stays in the source text
 * (loc, len) for the rule that every use be spelled as its declaration.
 * Literals are converted here: the parser never reads digits or quotes.
 */
#ifndef LIMBA_LUXIA_LEX_H
#define LIMBA_LUXIA_LEX_H

#include "common/strtab.h"
#include "front/diag.h"
#include "front/source.h"

#include <stdint.h>
#include <stdio.h>

enum {
#define LX_TOKEN(name, text) LX_##name,
#define LX_KEYWORD(name, text) LX_KW_##name,
#include "tokens.def"
#undef LX_TOKEN
#undef LX_KEYWORD
    LX_NKINDS
};
#define LX_KW_FIRST LX_KW_ABS
#define LX_NKEYWORDS (LX_NKINDS - LX_KW_FIRST)

enum {
#define LX_ERROR(code, name) LXE_##name = code,
#include "errors.def"
#undef LX_ERROR
};

/* token flags */
#define LX_F_LINE 1u /* first token of its line */
#define LX_F_BIG 2u  /* INT past 64 bits: the value is read from the text */

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t spare;
    limba_loc loc;
    uint32_t len; /* bytes of source */
    /* IDENT and keywords: the name id; INT: index in ints; REAL: index in
       reals; CHAR: the code point; STRING: the id in the string table */
    uint32_t val;
} limba_lx_token;

typedef struct {
    limba_lx_token *tok; /* the last one is LX_EOF */
    uint32_t ntok, captok;
    uint64_t *ints;
    uint32_t nints, capints;
    double *reals;
    uint32_t nreals, capreals;
    limba_strtab *names;   /* folded names, the keywords first */
    limba_strtab *strings; /* values of the string literals */
} limba_lx;

/* an empty result, with its tables; the keywords are interned */
void limba_lx_init(limba_lx *lx);
void limba_lx_free(limba_lx *lx);

/* the tokens of a file; the errors go to rep */
void limba_lx_run(limba_lx *lx, const limba_source *src, uint32_t file,
                  limba_report *rep);

/* the text of a kind: "begin", ":=", "name" */
const char *limba_lx_kind_text(unsigned kind);

/* one token in a short form: begin, :=, id:conto, int:42, real:1.5,
   char:U+0061, str:"a""b", eof */
void limba_lx_show(FILE *out, const limba_lx *lx, const limba_lx_token *t);
/* all the tokens, one per line, with line and column */
void limba_lx_dump(FILE *out, const limba_lx *lx, const limba_source *src);

#endif
