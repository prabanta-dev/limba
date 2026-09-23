/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse.h - the state of the reader of the text form and the helpers its
 * three files share: parse_base.c (tokens, literals, types), parse_func.c
 * (function bodies) and parse.c (the module). Not public.
 */
#ifndef LIMBA_IR_PARSE_H
#define LIMBA_IR_PARSE_H

#include "internal.h"
#include "lex.h"

#include "common/hash.h"

/* an operand to resolve at the end of the function */
typedef struct {
    uint32_t index; /* in f->operands */
    uint64_t num;   /* the number written, v<num> or b<num> */
    unsigned line;
} fixup;

typedef struct {
    limba_tok *t;
    size_t nt, i;
    limba_module *m;
    limba_diag *d;
    bool err;
    char *buf; /* for unescaped strings */
    /* the function being read */
    limba_func *f;
    limba_hash *vidx, *bidx;
    uint64_t *vnum; /* written number of each value id */
    uint32_t capvnum;
    uint64_t *bnum; /* written number of each block id */
    uint32_t capbnum;
    fixup *vfix, *bfix;
    uint32_t nvfix, capvfix, nbfix, capbfix;
    limba_id cur; /* current block, LIMBA_NONE before the first label */
    /* operands of the instruction being read */
    uint32_t *ops;
    uint32_t nops, capops;
    struct {
        uint32_t rel;
        uint64_t num;
        unsigned line;
        bool block;
    } *pend;
    uint32_t npend, cappend;
} P;

const limba_tok *lp_peek(P *p);
/* a diagnostic at the current token, once; always false */
bool lp_fail(P *p, const char *fmt, const char *what);
bool lp_is_word(const limba_tok *k, const char *w);
bool lp_accept(P *p, int kind);
bool lp_expect(P *p, int kind, const char *what);
bool lp_accept_word(P *p, const char *w);
bool lp_expect_word(P *p, const char *w);
/* a quoted or plain name, interned; LIMBA_NONE on a bad escape */
limba_id lp_name(P *p, const limba_tok *k);
bool lp_integer(P *p, int64_t *out);
bool lp_uinteger32(P *p, uint32_t *out);
bool lp_real(P *p, int64_t *bits);
bool lp_string(P *p, limba_id *id);
bool lp_type(P *p, limba_id *out);
bool lp_sym(P *p, limba_id *n);
/* func @name : type [export] { ... }, after the word func */
bool lp_func(P *p);

#endif
