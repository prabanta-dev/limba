/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse.h - the parser of Luxia 0: recursive descent for declarations and
 * statements, Pratt (front/pratt.h) for expressions. The grammar is the
 * EBNF of job/docs/luxia_0.md § 13, LL(1): one token decides every choice
 * and the parser never goes back.
 *
 * After an error the parser skips to a point where it can go on (the end
 * of the statement, the next declaration) and reports again only once it
 * has moved on, so that one mistake gives one message.
 */
#ifndef LIMBA_LUXIA_PARSE_H
#define LIMBA_LUXIA_PARSE_H

#include "ast.h"
#include "front/diag.h"

#include <stdbool.h>

/* a whole file: the tokens of lx become the tree t, rooted at t->root */
void limba_lx_parse(limba_lx_ast *t, const limba_lx *lx,
                    const limba_source *src, limba_report *rep);
/* one expression and the end of the file, for the tests */
void limba_lx_parse_expr(limba_lx_ast *t, const limba_lx *lx,
                         const limba_source *src, limba_report *rep);

/* ---- inside the parser ---- */

typedef struct {
    const limba_lx *lx;
    limba_lx_ast *t;
    limba_report *rep;
    const limba_source *src;
    uint32_t pos;        /* the current token */
    uint32_t last_error; /* the token of the last error, or UINT32_MAX */
} limba_lxp;

static inline unsigned lxp_kind(const limba_lxp *P)
{
    return P->lx->tok[P->pos].kind;
}

static inline limba_loc lxp_loc(const limba_lxp *P)
{
    return P->lx->tok[P->pos].loc;
}

static inline uint32_t lxp_node(limba_lxp *P, unsigned kind, limba_loc loc,
                                uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return limba_lx_node_new(P->t, kind, loc, a, b, c, d);
}

void limba_lxp_next(limba_lxp *P);
bool limba_lxp_accept(limba_lxp *P, unsigned kind);
/* the current token is kind: take it; else report "expected ..." */
bool limba_lxp_expect(limba_lxp *P, unsigned kind, const char *where);
/* an error at a token (its whole span), unless one was just given there */
void limba_lxp_error(limba_lxp *P, unsigned code, uint32_t tok, const char *fmt,
                     ...) __attribute__((format(printf, 4, 5)));
/* "expected X, found Y", placed after the previous token when Y is on a
   later line (a missing ';' belongs to the line it is missing from) */
void limba_lxp_expected(limba_lxp *P, const char *what);
/* the current token as a message says it: 'end', name 'x', ... */
const char *limba_lxp_describe(const limba_lxp *P, uint32_t tok, char *buf,
                               size_t size);

uint32_t limba_lxp_name(limba_lxp *P);  /* ident -> NAME */
uint32_t limba_lxp_names(limba_lxp *P); /* IdentList -> LIST of NAME */
uint32_t limba_lxp_type(limba_lxp *P);
uint32_t limba_lxp_constdecl(limba_lxp *P);
uint32_t limba_lxp_vardecl(limba_lxp *P);
/* close the construct opened by token opener: end, with the check that
   it stands under the start of the opening line */
void limba_lxp_end(limba_lxp *P, uint32_t opener);

uint32_t limba_lxp_stmts(limba_lxp *P);
uint32_t limba_lxp_expr(limba_lxp *P, unsigned min);
uint32_t limba_lxp_designator(limba_lxp *P);
bool limba_lxp_starts_expr(unsigned kind);

/* the powers of the Pratt table, for the callers that need a level */
#define LXP_EXPR 1   /* Expr: everything */
#define LXP_SIMPLE 3 /* Simple: no comparison, no and/or/xor */

#endif
