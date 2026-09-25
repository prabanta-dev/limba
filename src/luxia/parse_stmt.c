/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse_stmt.c - the statements of Luxia 0 (see parse.h). Every statement
 * ends with ';'; every structured statement ends with 'end'.
 */
#include "parse.h"

/* the tokens that close a list of statements */
static bool closes(unsigned k)
{
    return k == LX_KW_END || k == LX_KW_ELSE || k == LX_KW_ELSIF ||
           k == LX_KW_UNTIL || k == LX_KW_WHEN || k == LX_EOF ||
           k == LX_KW_PROCEDURE || k == LX_KW_FUNCTION;
}

static bool starts_stmt(unsigned k)
{
    switch (k) {
    case LX_IDENT:
    case LX_KW_IF:
    case LX_KW_CASE:
    case LX_KW_WHILE:
    case LX_KW_REPEAT:
    case LX_KW_FOR:
    case LX_KW_LOOP:
    case LX_KW_EXIT:
    case LX_KW_CONTINUE:
    case LX_KW_RETURN:
    case LX_KW_VAR:
    case LX_KW_CONST:
    case LX_KW_PRAGMA:
        return true;
    }
    return false;
}

/* after an error: to the end of the statement, or to a line that starts
   a statement */
static void sync_stmt(limba_lxp *P, uint32_t start)
{
    if (P->pos == start && !closes(lxp_kind(P)) && lxp_kind(P) != LX_SEMI)
        limba_lxp_next(P);
    for (;;) {
        unsigned k = lxp_kind(P);
        if (k == LX_SEMI) {
            limba_lxp_next(P);
            return;
        }
        if (closes(k))
            return;
        if ((P->lx->tok[P->pos].flags & LX_F_LINE) && starts_stmt(k))
            return;
        limba_lxp_next(P);
    }
}

static uint32_t stmt(limba_lxp *P);

uint32_t limba_lxp_stmts(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t mark = limba_lx_list_begin(P->t);
    while (!closes(lxp_kind(P))) {
        if (lxp_kind(P) == LX_SEMI) {
            limba_lxp_error(P, LXE_EMPTY_STATEMENT, P->pos,
                            "an empty statement: remove this ';'");
            limba_lxp_next(P);
            continue;
        }
        if (lxp_kind(P) == LX_KW_BEGIN) {
            /* a Pascal block: drop the begin, its end closes the
               statement around it as it would in Luxia */
            limba_lxp_error(P, LXE_NOT_A_STATEMENT, P->pos,
                            "Luxia has no begin blocks: the statements go "
                            "straight after then, do, else");
            limba_lxp_next(P);
            continue;
        }
        uint32_t start = P->pos, errors = P->rep->errors;
        limba_lx_list_push(P->t, stmt(P));
        if (P->rep->errors > errors) {
            sync_stmt(P, start);
        } else if (!limba_lxp_expect(P, LX_SEMI,
                                     "at the end of the "
                                     "statement")) {
            sync_stmt(P, start);
        }
    }
    return limba_lx_list_end(P->t, mark, loc);
}

static uint32_t if_stmt(limba_lxp *P)
{
    uint32_t opener = P->pos;
    limba_loc loc = lxp_loc(P);
    uint32_t mark = limba_lx_list_begin(P->t);
    do {
        limba_loc aloc = lxp_loc(P);
        limba_lxp_next(P); /* if or elsif */
        uint32_t cond = limba_lxp_expr(P, LXP_EXPR);
        limba_lxp_expect(P, LX_KW_THEN, "after the condition");
        uint32_t body = limba_lxp_stmts(P);
        limba_lx_list_push(P->t, lxp_node(P, LXN_ARM, aloc, cond, body, 0, 0));
    } while (lxp_kind(P) == LX_KW_ELSIF);
    uint32_t arms = limba_lx_list_end(P->t, mark, loc), other = 0;
    if (limba_lxp_accept(P, LX_KW_ELSE))
        other = limba_lxp_stmts(P);
    limba_lxp_end(P, opener);
    return lxp_node(P, LXN_IF, loc, arms, other, 0, 0);
}

static uint32_t label(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t lo = limba_lxp_expr(P, LXP_SIMPLE), hi = 0;
    if (limba_lxp_accept(P, LX_DOTDOT))
        hi = limba_lxp_expr(P, LXP_SIMPLE);
    return lxp_node(P, LXN_LABEL, loc, lo, hi, 0, 0);
}

static uint32_t case_stmt(limba_lxp *P)
{
    uint32_t opener = P->pos;
    limba_loc loc = lxp_loc(P);
    limba_lxp_next(P);
    uint32_t sel = limba_lxp_expr(P, LXP_EXPR);
    limba_lxp_expect(P, LX_KW_OF, "after the value of the case");
    uint32_t mark = limba_lx_list_begin(P->t);
    while (lxp_kind(P) == LX_KW_WHEN) {
        limba_loc wloc = lxp_loc(P);
        limba_lxp_next(P);
        uint32_t lmark = limba_lx_list_begin(P->t);
        do
            limba_lx_list_push(P->t, label(P));
        while (limba_lxp_accept(P, LX_COMMA));
        uint32_t labels = limba_lx_list_end(P->t, lmark, wloc);
        limba_lxp_expect(P, LX_COLON, "after the values of the branch");
        uint32_t body = limba_lxp_stmts(P);
        limba_lx_list_push(P->t,
                           lxp_node(P, LXN_WHEN, wloc, labels, body, 0, 0));
    }
    uint32_t whens = limba_lx_list_end(P->t, mark, loc), other = 0;
    if (limba_lxp_accept(P, LX_KW_ELSE))
        other = limba_lxp_stmts(P);
    limba_lxp_end(P, opener);
    return lxp_node(P, LXN_CASE, loc, sel, whens, other, 0);
}

static uint32_t for_stmt(limba_lxp *P)
{
    uint32_t opener = P->pos;
    limba_loc loc = lxp_loc(P);
    limba_lxp_next(P);
    limba_lxp_expect(P, LX_KW_VAR,
                     "after 'for': the loop declares its variable, "
                     "for var i := 1 to n");
    uint32_t name = limba_lxp_name(P), type = 0;
    if (limba_lxp_accept(P, LX_COLON))
        type = limba_lxp_type(P);
    limba_lxp_expect(P, LX_ASSIGN, "and the first value");
    limba_loc rloc = lxp_loc(P);
    uint32_t from = limba_lxp_expr(P, LXP_EXPR);
    unsigned dir = lxp_kind(P);
    if (dir == LX_KW_TO || dir == LX_KW_DOWNTO)
        limba_lxp_next(P);
    else
        limba_lxp_expected(P, "'to' or 'downto'");
    uint32_t to = limba_lxp_expr(P, LXP_EXPR);
    limba_lxp_expect(P, LX_KW_DO, "after the range of the loop");
    uint32_t body = limba_lxp_stmts(P);
    limba_lxp_end(P, opener);
    uint32_t range = lxp_node(P, LXN_RANGE, rloc, from, to, 0, 0);
    uint32_t f = lxp_node(P, LXN_FOR, loc, name, type, range, body);
    P->t->node[f].op = (uint8_t)(dir == LX_KW_DOWNTO ? dir : LX_KW_TO);
    return f;
}

/* an assignment or a call */
static uint32_t simple_stmt(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t d = limba_lxp_designator(P);
    if (limba_lxp_accept(P, LX_ASSIGN))
        return lxp_node(P, LXN_ASSIGN, loc, d, limba_lxp_expr(P, LXP_EXPR), 0,
                        0);
    if (P->t->node[d].kind == LXN_CALL)
        return lxp_node(P, LXN_CALLST, loc, d, 0, 0, 0);
    if (lxp_kind(P) == LX_EQ)
        limba_lxp_error(P, LXE_NOT_A_STATEMENT, P->pos,
                        "'=' compares: assign with ':='");
    else
        limba_lxp_error(P, LXE_NOT_A_STATEMENT, P->pos,
                        "a statement assigns (x := ...) or calls a "
                        "routine (P(...), P() without arguments)");
    return lxp_node(P, LXN_ERROR, loc, 0, 0, 0, 0);
}

static uint32_t stmt(limba_lxp *P)
{
    uint32_t opener = P->pos;
    limba_loc loc = lxp_loc(P);
    uint32_t a, b;
    switch (lxp_kind(P)) {
    case LX_IDENT:
        return simple_stmt(P);
    case LX_KW_IF:
        return if_stmt(P);
    case LX_KW_CASE:
        return case_stmt(P);
    case LX_KW_WHILE:
        limba_lxp_next(P);
        a = limba_lxp_expr(P, LXP_EXPR);
        limba_lxp_expect(P, LX_KW_DO, "after the condition");
        b = limba_lxp_stmts(P);
        limba_lxp_end(P, opener);
        return lxp_node(P, LXN_WHILE, loc, a, b, 0, 0);
    case LX_KW_REPEAT:
        limba_lxp_next(P);
        a = limba_lxp_stmts(P);
        limba_lxp_expect(P, LX_KW_UNTIL, "to close the 'repeat'");
        b = limba_lxp_expr(P, LXP_EXPR);
        return lxp_node(P, LXN_REPEAT, loc, a, b, 0, 0);
    case LX_KW_FOR:
        return for_stmt(P);
    case LX_KW_LOOP:
        limba_lxp_next(P);
        a = limba_lxp_stmts(P);
        limba_lxp_end(P, opener);
        return lxp_node(P, LXN_LOOP, loc, a, 0, 0, 0);
    case LX_KW_EXIT:
    case LX_KW_CONTINUE: {
        unsigned kind = lxp_kind(P) == LX_KW_EXIT ? LXN_EXIT : LXN_CONTINUE;
        limba_lxp_next(P);
        a = limba_lxp_accept(P, LX_KW_WHEN) ? limba_lxp_expr(P, LXP_EXPR) : 0;
        return lxp_node(P, kind, loc, a, 0, 0, 0);
    }
    case LX_KW_RETURN:
        limba_lxp_next(P);
        a = limba_lxp_starts_expr(lxp_kind(P)) ? limba_lxp_expr(P, LXP_EXPR)
                                               : 0;
        return lxp_node(P, LXN_RETURN, loc, a, 0, 0, 0);
    case LX_KW_VAR:
        limba_lxp_next(P);
        return limba_lxp_vardecl(P);
    case LX_KW_CONST:
        limba_lxp_next(P);
        return limba_lxp_constdecl(P);
    case LX_KW_PRAGMA:
        return limba_lxp_pragma(P);
    default:
        limba_lxp_expected(P, "a statement");
        return lxp_node(P, LXN_ERROR, loc, 0, 0, 0, 0);
    }
}
