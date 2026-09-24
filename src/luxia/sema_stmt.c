/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_stmt.c - the statements of Luxia 0 (see sema.h and luxia_0.md
 * § 7-8). Every list of statements of a structured statement is a block
 * with its own scope, for the var and const it declares.
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdlib.h>

static void condition(limba_lxs *S, uint32_t node, uint32_t scope)
{
    lxs_expr(S, node, scope, S->ts.bool_);
    lxs_assign_to(S, node, S->ts.bool_, "a condition");
}

static void block(limba_lxs *S, uint32_t list, uint32_t scope)
{
    if (list)
        lxs_stmts(S, list, limba_scope_new(&S->st, scope, LXS_BLOCK));
}

static void loop_body(limba_lxs *S, uint32_t list, uint32_t scope)
{
    S->loops++;
    block(S, list, scope);
    S->loops--;
}

/* var and const as statements: declared after their value */
static void local_var(limba_lxs *S, uint32_t d, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, d);
    limba_type t = x->b ? lxs_type(S, x->b, scope, LXT_VAR) : 0;
    if (x->c) {
        limba_type vt = lxs_expr(S, x->c, scope, t);
        if (t) {
            lxs_assign_to(S, x->c, t, "the variable");
        } else if (vt == S->ts.uint || vt == S->ts.ureal || vt == S->ts.nil) {
            lxs_error(S, LXE_NEED_TYPE, x->c,
                      "a constant without a type gives none to the "
                      "variable: write var x: T := ...");
        } else {
            t = vt;
        }
    }
    uint32_t names = x->a;
    for (uint32_t k = 0; k < lxs_node(S, names)->b; k++) {
        limba_sym s = lxs_declare(S, scope, limba_lx_list_at(S->t, names, k),
                                  LIMBA_SYM_VAR, d);
        if (s)
            S->st.sym[s].type = t;
    }
}

static void local_const(limba_lxs *S, uint32_t d, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, d);
    limba_type t = x->b ? lxs_type(S, x->b, scope, 0) : 0;
    uint32_t errors = S->rep->errors;
    limba_type vt = lxs_expr(S, x->c, scope, t);
    uint32_t v = S->val[x->c];
    if (!v && vt && S->rep->errors == errors)
        lxs_error(S, LXE_NOT_CONSTANT, x->c,
                  "a constant needs a value known at compile time: for a "
                  "computed one declare a var");
    if (t) {
        lxs_assign_to(S, x->c, t, "the constant");
        vt = t;
        v = S->val[x->c];
    }
    limba_sym s = lxs_declare(S, scope, x->a, LIMBA_SYM_CONST, d);
    if (s) {
        S->st.sym[s].type = vt;
        S->st.sym[s].value = v;
    }
}

typedef struct {
    __int128 lo, hi;
    uint32_t node;
} interval;

static int by_lo(const void *a, const void *b)
{
    const interval *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo ? 1 : 0;
}

static void case_stmt(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t sel = x->a, whens = x->b, other = x->c;
    limba_type t = lxs_expr(S, sel, scope, 0);
    char tb[128];
    if (t && (t == S->ts.uint || !limba_types_is_discrete(&S->ts, t))) {
        lxs_error(S, LXE_TYPE_MISMATCH, sel,
                  "a case chooses on a discrete value with a type, not %s",
                  lxs_tname(S, t, tb));
        t = 0;
    }
    interval *iv = NULL;
    uint32_t niv = 0, cap = 0;
    for (uint32_t i = 0; i < lxs_node(S, whens)->b; i++) {
        uint32_t w = limba_lx_list_at(S->t, whens, i);
        uint32_t labels = lxs_node(S, w)->a;
        for (uint32_t k = 0; k < lxs_node(S, labels)->b; k++) {
            uint32_t lab = limba_lx_list_at(S->t, labels, k);
            uint32_t bounds[2] = {lxs_node(S, lab)->a, lxs_node(S, lab)->b};
            __int128 v[2] = {0, 0};
            bool ok = true;
            for (int j = 0; j < 2; j++) {
                if (!bounds[j]) {
                    v[1] = v[0];
                    continue;
                }
                lxs_expr(S, bounds[j], scope, t);
                if (!t)
                    continue;
                lxs_assign_to(S, bounds[j], t, "the case");
                if (!S->val[bounds[j]]) {
                    lxs_error(S, LXE_NOT_CONSTANT, bounds[j],
                              "the values of a case are known at compile "
                              "time");
                    ok = false;
                } else if (!lxs_value_to_int(S, S->val[bounds[j]], &v[j])) {
                    ok = false;
                }
            }
            if (t && ok) {
                LIMBA_GROW(iv, niv, cap);
                iv[niv++] = (interval){v[0], v[1], lab};
            }
        }
        block(S, lxs_node(S, w)->b, scope);
    }
    if (other)
        block(S, other, scope);
    if (!t) {
        free(iv);
        return;
    }
    qsort(iv, niv, sizeof(*iv), by_lo);
    const limba_typeinfo *ti = lxs_ty(S, t);
    __int128 next = ti->lo; /* the least value not covered yet */
    bool gap = false;
    for (uint32_t i = 0; i < niv; i++) {
        if (i > 0 && iv[i].lo <= iv[i - 1].hi)
            lxs_error(S, LXE_CASE_DUPLICATE, iv[i].node,
                      "this value is already in another branch");
        if (iv[i].lo > next)
            gap = true;
        if (iv[i].hi >= next)
            next = iv[i].hi + 1;
    }
    if (next <= ti->hi)
        gap = true;
    if (gap && !other)
        lxs_error(S, LXE_CASE_COVERAGE, node,
                  "some values of %s have no branch: add them or an else",
                  lxs_tname(S, t, tb));
    free(iv);
}

static void for_stmt(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t name = x->a, tnode = x->b, range = x->c, body = x->d;
    uint32_t from = lxs_node(S, range)->a, to = lxs_node(S, range)->b;
    limba_type t = tnode ? lxs_type(S, tnode, scope, 0) : 0;
    limba_type ft = lxs_expr(S, from, scope, t);
    limba_type tt = lxs_expr(S, to, scope, t);
    if (!t) {
        bool fu = ft == S->ts.uint || ft == S->ts.ureal;
        bool tu = tt == S->ts.uint || tt == S->ts.ureal;
        if (ft && !fu)
            t = lxs_base(S, ft);
        else if (tt && !tu)
            t = lxs_base(S, tt);
        else if (ft && tt)
            lxs_error(S, LXE_NEED_TYPE, range,
                      "the bounds are constants without a type: write "
                      "for var i: Int32 := ...");
    }
    char tb[128];
    if (t && !limba_types_is_discrete(&S->ts, t)) {
        lxs_error(S, LXE_TYPE_MISMATCH, range,
                  "a for counts over a discrete type, not %s",
                  lxs_tname(S, t, tb));
        t = 0;
    }
    if (t) {
        lxs_assign_to(S, from, t, "the loop");
        lxs_assign_to(S, to, t, "the loop");
    }
    uint32_t inner = limba_scope_new(&S->st, scope, LXS_BLOCK);
    limba_sym s = lxs_declare(S, inner, name, LIMBA_SYM_VAR, node);
    if (s) {
        S->st.sym[s].type = t;
        S->st.sym[s].flags |= LXS_LOOPVAR;
    }
    S->loops++;
    lxs_stmts(S, body, inner);
    S->loops--;
}

static void stmt(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    switch (x->kind) {
    case LXN_ASSIGN: {
        uint32_t target = x->a, value = x->b;
        limba_type t = lxs_expr(S, target, scope, 0);
        bool ok = lxs_writable(S, target, true);
        lxs_expr(S, value, scope, t);
        if (ok)
            lxs_assign_to(S, value, t, "the variable");
        break;
    }
    case LXN_CALLST:
        lxs_call_stmt(S, x->a, scope);
        break;
    case LXN_IF: {
        uint32_t arms = x->a, other = x->b;
        for (uint32_t i = 0; i < lxs_node(S, arms)->b; i++) {
            uint32_t arm = limba_lx_list_at(S->t, arms, i);
            condition(S, lxs_node(S, arm)->a, scope);
            block(S, lxs_node(S, arm)->b, scope);
        }
        block(S, other, scope);
        break;
    }
    case LXN_CASE:
        case_stmt(S, node, scope);
        break;
    case LXN_WHILE:
        condition(S, x->a, scope);
        loop_body(S, lxs_node(S, node)->b, scope);
        break;
    case LXN_REPEAT: {
        /* the condition sees the declarations of the body */
        uint32_t inner = limba_scope_new(&S->st, scope, LXS_BLOCK);
        S->loops++;
        lxs_stmts(S, x->a, inner);
        S->loops--;
        condition(S, lxs_node(S, node)->b, inner);
        break;
    }
    case LXN_FOR:
        for_stmt(S, node, scope);
        break;
    case LXN_LOOP:
        loop_body(S, x->a, scope);
        break;
    case LXN_EXIT:
    case LXN_CONTINUE:
        if (!S->loops)
            lxs_error(S, LXE_OUTSIDE_LOOP, node, "'%s' is used inside loops",
                      x->kind == LXN_EXIT ? "exit" : "continue");
        if (x->a)
            condition(S, x->a, scope);
        break;
    case LXN_RETURN: {
        bool function = S->in_routine && S->result != S->ts.void_;
        if (x->a && !function) {
            lxs_expr(S, x->a, scope, 0);
            lxs_error(S, LXE_RETURN_VALUE, node,
                      "only a function returns a value");
        } else if (!x->a && function) {
            lxs_error(S, LXE_RETURN_VALUE, node,
                      "a function returns a value: return x");
        } else if (x->a) {
            lxs_expr(S, x->a, scope, S->result);
            lxs_assign_to(S, x->a, S->result, "the result");
        }
        break;
    }
    case LXN_VAR:
        local_var(S, node, scope);
        break;
    case LXN_CONST:
        local_const(S, node, scope);
        break;
    }
}

void lxs_stmts(limba_lxs *S, uint32_t list, uint32_t scope)
{
    for (uint32_t i = 0; i < lxs_node(S, list)->b; i++)
        stmt(S, limba_lx_list_at(S->t, list, i), scope);
}

void lxs_routine_body(limba_lxs *S, limba_sym routine)
{
    limba_symbol *y = &S->st.sym[routine];
    uint32_t scope = y->value;
    limba_type sig = y->type;
    limba_lx_node *x = lxs_node(S, y->node);
    uint32_t body = x->d;
    if (!scope || !body)
        return;
    uint32_t locals = lxs_node(S, body)->a, stmts = lxs_node(S, body)->b;
    lxs_declare_all(S, locals, scope, false);
    lxs_resolve_all(S, locals);
    S->result = sig ? lxs_ty(S, sig)->elem : S->ts.void_;
    S->in_routine = true;
    S->loops = 0;
    lxs_stmts(S, stmts, scope);
}
