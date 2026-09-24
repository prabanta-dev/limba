/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lex.h - tokens of the text form (.lit).
 */
#ifndef LIMBA_IR_LEX_H
#define LIMBA_IR_LEX_H

#include <stddef.h>
#include <stdint.h>

enum {
    TK_EOF,
    TK_IDENT,  /* add, i64, func, icmp.slt, nan.0x7ff8000000000000 */
    TK_SYM,    /* @name, @"any bytes" */
    TK_TYPE,   /* %name, %"any bytes" */
    TK_OFFSET, /* @8, a field offset */
    TK_VALUE,  /* v12 */
    TK_BLOCK,  /* b3 */
    TK_SLOT,   /* $0 */
    TK_INT,    /* -12, 0x1f */
    TK_FLOAT,  /* 0x1.8p+1, 1.5, -inf */
    TK_STRING, /* "..." */
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE,
    TK_LBRACK,
    TK_RBRACK,
    TK_COMMA,
    TK_COLON,
    TK_EQ,
    TK_ARROW,
    TK_ELLIPSIS,
    TK_POS, /* !12: a position of the source */
    TK_BAD,
};

typedef struct {
    int kind;
    const char *s; /* the spelling; for SYM, TYPE and STRING the bytes
                      between the quotes, escapes still in */
    size_t n;
    unsigned line;
    uint64_t num; /* VALUE, BLOCK, SLOT, OFFSET: the number */
} limba_tok;

/* all the tokens of text, ending with TK_EOF; NULL and *bad_line on a
   character that starts no token */
limba_tok *limba_lex(const char *text, size_t len, size_t *count,
                     unsigned *bad_line);

/* the bytes of a quoted string with its escapes decoded, into out (at
   least tok->n bytes); returns the length, or SIZE_MAX if malformed */
size_t limba_unescape(const limba_tok *tok, char *out);

#endif
