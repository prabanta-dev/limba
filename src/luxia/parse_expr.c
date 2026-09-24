/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse_expr.c - the expressions of Luxia 0, with the shared Pratt engine
 * (front/pratt.h). The table below is the grammar of luxia_0.md § 13,
 * Expr to Factor, as powers:
 *
 *   1  and or xor             NOMIX
 *   2  = <> < <= > >= in      NONASSOC (in: SPECIAL, x in a..b)
 *   3  + - &, and + - prefix  left; the prefix reads a Term (power 4)
 *   4  * / div mod rem shl shr  left
 *   5  **                     NONASSOC, ATOM_LEFT; reads a Primary
 *      not abs prefix         read a Primary (power 6)
 *
 * A Primary is an operand: no infix operator has power 6.
 */
#include "front/pratt.h"
#include "parse.h"

#define L1 {1, 2, 0, 0, LIMBA_PRATT_NOMIX}
#define L2 {2, 3, 0, 0, LIMBA_PRATT_NONASSOC}
#define L3 {3, 4, 0, 0, 0}
#define L4 {4, 5, 0, 0, 0}

static const limba_pratt_op ops[LX_NKINDS] = {
    [LX_KW_AND] = L1,
    [LX_KW_OR] = L1,
    [LX_KW_XOR] = L1,
    [LX_EQ] = L2,
    [LX_NE] = L2,
    [LX_LT] = L2,
    [LX_LE] = L2,
    [LX_GT] = L2,
    [LX_GE] = L2,
    [LX_KW_IN] = {2, 3, 0, 0, LIMBA_PRATT_NONASSOC | LIMBA_PRATT_SPECIAL},
    [LX_PLUS] = {3, 4, 3, 4, 0},
    [LX_MINUS] = {3, 4, 3, 4, 0},
    [LX_AMP] = L3,
    [LX_STAR] = L4,
    [LX_SLASH] = L4,
    [LX_KW_DIV] = L4,
    [LX_KW_MOD] = L4,
    [LX_KW_REM] = L4,
    [LX_KW_SHL] = L4,
    [LX_KW_SHR] = L4,
    [LX_POWER] = {5, 6, 0, 0, LIMBA_PRATT_NONASSOC | LIMBA_PRATT_ATOM_LEFT},
    [LX_KW_NOT] = {0, 0, 5, 6, 0},
    [LX_KW_ABS] = {0, 0, 5, 6, 0},
};

static unsigned cb_kind(void *ctx)
{
    return lxp_kind(ctx);
}

static uint32_t cb_tok(void *ctx)
{
    return ((limba_lxp *)ctx)->pos;
}

static void cb_next(void *ctx)
{
    limba_lxp_next(ctx);
}

static uint32_t cb_unary(void *ctx, uint32_t op, uint32_t e)
{
    limba_lxp *P = ctx;
    const limba_lx_token *t = &P->lx->tok[op];
    uint32_t n = lxp_node(P, LXN_UNARY, t->loc, e, 0, 0, 0);
    P->t->node[n].op = t->kind;
    return n;
}

static uint32_t cb_binary(void *ctx, uint32_t op, uint32_t l, uint32_t r)
{
    limba_lxp *P = ctx;
    const limba_lx_token *t = &P->lx->tok[op];
    uint32_t n = lxp_node(P, LXN_BINARY, t->loc, l, r, 0, 0);
    P->t->node[n].op = t->kind;
    return n;
}

/* x in a..b, x in T */
static uint32_t cb_special(void *ctx, uint32_t op, uint32_t l)
{
    limba_lxp *P = ctx;
    uint32_t lo = limba_lxp_expr(P, LXP_SIMPLE), hi = 0;
    if (limba_lxp_accept(P, LX_DOTDOT))
        hi = limba_lxp_expr(P, LXP_SIMPLE);
    return lxp_node(P, LXN_IN, P->lx->tok[op].loc, l, lo, hi, 0);
}

static void cb_error(void *ctx, limba_pratt_err err, uint32_t tok,
                     uint32_t prev)
{
    limba_lxp *P = ctx;
    const char *op = limba_lx_kind_text(P->lx->tok[tok].kind);
    const char *pv =
        prev != UINT32_MAX ? limba_lx_kind_text(P->lx->tok[prev].kind) : "";
    switch (err) {
    case LIMBA_PRATT_ERR_CHAIN:
        limba_lxp_error(P, LXE_CHAINED, tok,
                        "'%s' after '%s': these operators do not chain, "
                        "add parentheses",
                        op, pv);
        break;
    case LIMBA_PRATT_ERR_MIX:
        limba_lxp_error(P, LXE_MIXED_LOGICAL, tok,
                        "'%s' after '%s' without parentheses: write "
                        "(a %s b) %s c or a %s (b %s c)",
                        op, pv, pv, op, pv, op);
        break;
    case LIMBA_PRATT_ERR_PREFIX:
        limba_lxp_error(P, LXE_PREFIX_PARENS, tok,
                        "'%s' cannot follow an operator here: put it in "
                        "parentheses, as in a * (-b)",
                        op);
        break;
    case LIMBA_PRATT_ERR_ATOM:
        limba_lxp_error(P, LXE_PREFIX_POWER, tok,
                        "'%s' after '%s': write (%s a) %s b or %s (a %s b)", op,
                        pv, pv, op, pv, op);
        break;
    }
}

static uint32_t primary(void *ctx);

static const limba_pratt luxia_pratt = {
    ops,      cb_kind,   cb_tok,     cb_next,  primary,
    cb_unary, cb_binary, cb_special, cb_error,
};

uint32_t limba_lxp_expr(limba_lxp *P, unsigned min)
{
    return limba_pratt_parse(&luxia_pratt, P, min);
}

bool limba_lxp_starts_expr(unsigned k)
{
    switch (k) {
    case LX_INT:
    case LX_REAL:
    case LX_CHAR:
    case LX_STRING:
    case LX_IDENT:
    case LX_LPAREN:
    case LX_PLUS:
    case LX_MINUS:
    case LX_KW_NOT:
    case LX_KW_ABS:
    case LX_KW_TRUE:
    case LX_KW_FALSE:
    case LX_KW_NIL:
    case LX_KW_NEW:
        return true;
    }
    return false;
}

static uint32_t argument(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t e = limba_lxp_expr(P, LXP_EXPR);
    if (!limba_lxp_accept(P, LX_COLON))
        return e;
    uint32_t width = limba_lxp_expr(P, LXP_EXPR), decimals = 0;
    if (limba_lxp_accept(P, LX_COLON))
        decimals = limba_lxp_expr(P, LXP_EXPR);
    return lxp_node(P, LXN_FMT, loc, e, width, decimals, 0);
}

uint32_t limba_lxp_designator(limba_lxp *P)
{
    const limba_lx_token *t = &P->lx->tok[P->pos];
    uint32_t d = lxp_node(P, LXN_REF, t->loc, t->val, 0, 0, 0);
    limba_lxp_next(P);
    for (;;) {
        limba_loc loc = lxp_loc(P);
        switch (lxp_kind(P)) {
        case LX_DOT:
            limba_lxp_next(P);
            if (lxp_kind(P) != LX_IDENT) {
                limba_lxp_expected(P, "the name of a field after '.'");
                return d;
            }
            d = lxp_node(P, LXN_SEL, loc, d, P->lx->tok[P->pos].val, 0, 0);
            limba_lxp_next(P);
            break;
        case LX_LBRACK:
            limba_lxp_next(P);
            d = lxp_node(P, LXN_INDEX, loc, d, limba_lxp_expr(P, LXP_EXPR), 0,
                         0);
            limba_lxp_expect(P, LX_RBRACK, "after the index");
            break;
        case LX_CARET:
            limba_lxp_next(P);
            d = lxp_node(P, LXN_DEREF, loc, d, 0, 0, 0);
            break;
        case LX_LPAREN: {
            limba_lxp_next(P);
            uint32_t mark = limba_lx_list_begin(P->t);
            if (lxp_kind(P) != LX_RPAREN) {
                do
                    limba_lx_list_push(P->t, argument(P));
                while (limba_lxp_accept(P, LX_COMMA));
            }
            limba_lxp_expect(P, LX_RPAREN, "after the arguments");
            uint32_t args = limba_lx_list_end(P->t, mark, loc);
            d = lxp_node(P, LXN_CALL, loc, d, args, 0, 0);
            break;
        }
        default:
            return d;
        }
    }
}

static uint32_t primary(void *ctx)
{
    limba_lxp *P = ctx;
    const limba_lx_token *t = &P->lx->tok[P->pos];
    uint32_t n;
    switch (t->kind) {
    case LX_INT:
        n = lxp_node(P, LXN_INT, t->loc, t->val, 0, 0, 0);
        if (t->flags & LX_F_BIG)
            P->t->node[n].flags |= LXN_F_BIG;
        break;
    case LX_REAL:
        n = lxp_node(P, LXN_REAL, t->loc, t->val, 0, 0, 0);
        break;
    case LX_CHAR:
        n = lxp_node(P, LXN_CHAR, t->loc, t->val, 0, 0, 0);
        break;
    case LX_STRING:
        n = lxp_node(P, LXN_STRING, t->loc, t->val, 0, 0, 0);
        break;
    case LX_KW_TRUE:
    case LX_KW_FALSE:
        n = lxp_node(P, LXN_BOOL, t->loc, t->kind == LX_KW_TRUE, 0, 0, 0);
        break;
    case LX_KW_NIL:
        n = lxp_node(P, LXN_NIL, t->loc, 0, 0, 0, 0);
        break;
    case LX_IDENT:
        return limba_lxp_designator(P);
    case LX_LPAREN:
        limba_lxp_next(P);
        n = limba_lxp_expr(P, LXP_EXPR);
        limba_lxp_expect(P, LX_RPAREN, "to close the '('");
        return n;
    case LX_KW_NEW: {
        limba_loc loc = t->loc;
        limba_lxp_next(P);
        limba_lxp_expect(P, LX_LPAREN, "after 'new': new(T)");
        n = lxp_node(P, LXN_NEW, loc, limba_lxp_type(P), 0, 0, 0);
        limba_lxp_expect(P, LX_RPAREN, "after the type");
        return n;
    }
    default:
        limba_lxp_expected(P, "a value");
        return lxp_node(P, LXN_ERROR, t->loc, 0, 0, 0, 0);
    }
    limba_lxp_next(P);
    return n;
}
