/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * pratt.h - operator-precedence parsing of expressions (Pratt 1973, "Top
 * Down Operator Precedence"), shared by the front ends.
 *
 * A language gives a table, indexed by its token kinds, and a few
 * functions: read an operand, build a node, parse the right side of a
 * special operator, report an error. The loop and the rules of rigour are
 * here:
 *
 *   - an infix operator binds while its power is at least the minimum the
 *     caller asks for; its right operand must have power rbp (lbp + 1 for
 *     a left-associative operator);
 *   - NONASSOC: a second operator of the same power right after is an
 *     error (a < b < c);
 *   - NOMIX: operators of the same power may repeat but not mix
 *     (a and b or c);
 *   - ATOM_LEFT: the left operand may not be a prefix operation of the
 *     same frame (not a ** b);
 *   - a prefix operator whose power is below the minimum is an error
 *     (a * -b): it needs parentheses.
 *
 * On an error the parse goes on as if the rule held, so that one mistake
 * gives one message. The functions are static inline: a front end that
 * calls limba_pratt_parse with a static const table and static functions
 * gets a loop specialised for its language, without indirect calls.
 */
#ifndef LIMBA_FRONT_PRATT_H
#define LIMBA_FRONT_PRATT_H

#include <stdint.h>

#define LIMBA_PRATT_NONASSOC 1u
#define LIMBA_PRATT_NOMIX 2u
#define LIMBA_PRATT_ATOM_LEFT 4u
#define LIMBA_PRATT_SPECIAL 8u /* the language parses the right side */

typedef struct {
    uint8_t lbp;     /* power as an infix operator, 0 if it is not one */
    uint8_t rbp;     /* minimum power of its right operand */
    uint8_t pbp;     /* power as a prefix operator, 0 if it is not one */
    uint8_t operand; /* minimum power of the operand of the prefix */
    uint8_t flags;
} limba_pratt_op;

typedef enum {
    LIMBA_PRATT_ERR_CHAIN,  /* NONASSOC operator repeated: tok, prev */
    LIMBA_PRATT_ERR_MIX,    /* NOMIX operators mixed: tok, prev */
    LIMBA_PRATT_ERR_PREFIX, /* prefix operator below the minimum: tok */
    LIMBA_PRATT_ERR_ATOM,   /* ATOM_LEFT after a prefix operation: tok,
                               prev is the prefix operator */
} limba_pratt_err;

typedef struct {
    const limba_pratt_op *ops; /* indexed by token kind */
    /* the kind of the current token, and a handle of it (its index) */
    unsigned (*kind)(void *ctx);
    uint32_t (*tok)(void *ctx);
    void (*next)(void *ctx);
    /* an operand that is not a prefix operation */
    uint32_t (*operand)(void *ctx);
    uint32_t (*unary)(void *ctx, uint32_t op, uint32_t e);
    uint32_t (*binary)(void *ctx, uint32_t op, uint32_t l, uint32_t r);
    /* SPECIAL: the operator token is consumed, parse the rest */
    uint32_t (*special)(void *ctx, uint32_t op, uint32_t l);
    void (*error)(void *ctx, limba_pratt_err err, uint32_t tok, uint32_t prev);
} limba_pratt;

static inline uint32_t limba_pratt_parse(const limba_pratt *P, void *ctx,
                                         unsigned min)
{
    const limba_pratt_op *o = &P->ops[P->kind(ctx)];
    uint32_t left, prefix_tok = UINT32_MAX;
    if (o->pbp) {
        uint32_t t = P->tok(ctx);
        if (o->pbp < min)
            P->error(ctx, LIMBA_PRATT_ERR_PREFIX, t, UINT32_MAX);
        P->next(ctx);
        uint32_t e = limba_pratt_parse(P, ctx, o->operand);
        left = P->unary(ctx, t, e);
        prefix_tok = t;
    } else {
        left = P->operand(ctx);
    }
    /* the last operator applied in this frame, for the chain rules */
    uint32_t prev_tok = UINT32_MAX;
    unsigned prev_kind = 0, prev_lbp = 0;
    for (;;) {
        unsigned k = P->kind(ctx);
        o = &P->ops[k];
        if (o->lbp == 0 || o->lbp < min)
            return left;
        uint32_t t = P->tok(ctx);
        if (prev_tok != UINT32_MAX && o->lbp == prev_lbp) {
            const limba_pratt_op *po = &P->ops[prev_kind];
            if (po->flags & LIMBA_PRATT_NONASSOC)
                P->error(ctx, LIMBA_PRATT_ERR_CHAIN, t, prev_tok);
            else if ((po->flags & LIMBA_PRATT_NOMIX) && k != prev_kind)
                P->error(ctx, LIMBA_PRATT_ERR_MIX, t, prev_tok);
        }
        if ((o->flags & LIMBA_PRATT_ATOM_LEFT) && prefix_tok != UINT32_MAX &&
            prev_tok == UINT32_MAX)
            P->error(ctx, LIMBA_PRATT_ERR_ATOM, t, prefix_tok);
        P->next(ctx);
        if (o->flags & LIMBA_PRATT_SPECIAL) {
            left = P->special(ctx, t, left);
        } else {
            uint32_t right = limba_pratt_parse(P, ctx, o->rbp);
            left = P->binary(ctx, t, left, right);
        }
        prev_tok = t;
        prev_kind = k;
        prev_lbp = o->lbp;
    }
}

#endif
