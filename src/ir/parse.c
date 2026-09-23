/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse.c - reading the text form (.lit) into a module. Two passes over the
 * tokens: the first gives every function, global and extern its id in
 * order of appearance, so a call may name a function defined further
 * down; the second reads everything. Inside a function, values and blocks
 * may be used before their definition: operands are resolved when the
 * function ends. The result is not verified: call limba_verify.
 */
#include "parse.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

/* ---- symbols (first pass) ---- */

static bool declare(P *p)
{
    int depth = 0;
    for (size_t i = 0; i + 1 < p->nt; i++) {
        const limba_tok *k = &p->t[i];
        if (k->kind == TK_LBRACE)
            depth++;
        else if (k->kind == TK_RBRACE)
            depth--;
        if (depth || k->kind != TK_IDENT || p->t[i + 1].kind != TK_SYM)
            continue;
        limba_id id = LIMBA_NONE, n;
        bool known = true;
        p->i = i + 1;
        n = lp_name(p, &p->t[i + 1]);
        if (p->err)
            return false;
        if (lp_is_word(k, "func"))
            id = limba_func_add(p->m, n, LIMBA_NONE, 0);
        else if (lp_is_word(k, "global"))
            id = limba_global_add(p->m, n, LIMBA_NONE, 0);
        else if (lp_is_word(k, "extern"))
            id = limba_extern_add(p->m, n, LIMBA_NONE, LIMBA_NONE, LIMBA_NONE);
        else
            known = false;
        if (known && id == LIMBA_NONE)
            return lp_fail(p, "%s", "a name defined twice");
    }
    p->i = 0;
    return true;
}

static bool global(P *p)
{
    limba_id n, t;
    if (!lp_sym(p, &n) || !lp_expect(p, TK_COLON, "':'") || !lp_type(p, &t))
        return false;
    limba_global *g = &p->m->globals[limba_global_find(p->m, n)];
    g->type = t;
    if (lp_accept(p, TK_EQ)) {
        const limba_tok *k = lp_peek(p);
        if (k->kind == TK_STRING) {
            limba_id s;
            if (!lp_string(p, &s))
                return false;
            g->init = LIMBA_INIT_STR;
            g->value = s;
        } else if (k->kind == TK_INT) {
            if (!lp_integer(p, &g->value))
                return false;
            g->init = LIMBA_INIT_INT;
        } else {
            if (!lp_real(p, &g->value))
                return false;
            g->init = LIMBA_INIT_FLOAT;
        }
    }
    for (;;) {
        if (lp_accept_word(p, "export"))
            g->flags |= LIMBA_SYM_EXPORT;
        else if (lp_accept_word(p, "const"))
            g->flags |= LIMBA_SYM_CONST;
        else
            return true;
    }
}

static bool external(P *p)
{
    limba_id n, t, s;
    if (!lp_sym(p, &n) || !lp_expect(p, TK_COLON, "':'") || !lp_type(p, &t) ||
        !lp_expect(p, TK_EQ, "'='") || !lp_string(p, &s))
        return false;
    limba_extern *e = &p->m->externs[limba_extern_find(p->m, n)];
    e->type = t;
    e->symbol = s;
    if (lp_accept_word(p, "in") && !lp_string(p, &e->library))
        return false;
    return true;
}

static bool structure(P *p)
{
    limba_member fields[256];
    uint32_t n = 0, size, align;
    if (lp_peek(p)->kind != TK_TYPE)
        return lp_fail(p, "%s expected", "a %name");
    limba_id nm = lp_name(p, lp_peek(p));
    p->i++;
    if (p->err || !lp_expect(p, TK_EQ, "'='") ||
        !lp_expect(p, TK_LBRACE, "'{'"))
        return false;
    while (!lp_accept(p, TK_RBRACE)) {
        if (n && !lp_expect(p, TK_COMMA, "',' or '}'"))
            return false;
        if (n == 256)
            return lp_fail(p, "%s", "more than 256 fields");
        if (!lp_type(p, &fields[n].type))
            return false;
        if (lp_peek(p)->kind != TK_OFFSET)
            return lp_fail(p, "%s expected", "a field offset @<n>");
        if (lp_peek(p)->num > UINT32_MAX)
            return lp_fail(p, "%s", "offset out of range");
        fields[n++].offset = (uint32_t)lp_peek(p)->num;
        p->i++;
    }
    if (!lp_expect_word(p, "size") || !lp_uinteger32(p, &size) ||
        !lp_expect_word(p, "align") || !lp_uinteger32(p, &align))
        return false;
    if (limba_type_struct(p->m, nm, fields, n, size, align) == LIMBA_NONE)
        return lp_fail(p, "%s", "a struct type defined twice");
    return true;
}

static bool module(P *p)
{
    if (!declare(p))
        return false;
    while (lp_peek(p)->kind != TK_EOF) {
        if (lp_accept_word(p, "module")) {
            if (!lp_string(p, &p->m->name))
                return false;
        } else if (lp_accept_word(p, "memory")) {
            if (lp_accept_word(p, "strict"))
                p->m->memory = LIMBA_MEM_STRICT;
            else if (lp_accept_word(p, "fb"))
                p->m->memory = LIMBA_MEM_FB;
            else
                return lp_fail(p, "%s", "memory strict or memory fb");
        } else if (lp_accept_word(p, "type")) {
            if (!structure(p))
                return false;
        } else if (lp_accept_word(p, "global")) {
            if (!global(p))
                return false;
        } else if (lp_accept_word(p, "extern")) {
            if (!external(p))
                return false;
        } else if (lp_accept_word(p, "func")) {
            if (!lp_func(p))
                return false;
        } else {
            return lp_fail(p, "%s",
                           "module, memory, type, global, extern or "
                           "func expected");
        }
    }
    /* every declared symbol must have been defined: its type says so */
    for (uint32_t i = 0; i < p->m->nfuncs; i++)
        if (p->m->funcs[i].type == LIMBA_NONE)
            return lp_fail(p, "%s", "a function without a type");
    return true;
}

limba_module *limba_parse(const char *text, size_t len, limba_diag *d)
{
    P p = {.d = d};
    unsigned bad = 0;
    p.t = limba_lex(text, len, &p.nt, &bad);
    if (!p.t) {
        limba_diag_set(d, bad, "line %u: a character that starts no token",
                       bad);
        return NULL;
    }
    p.m = limba_module_new();
    p.buf = limba_xmalloc(len + 1);
    bool ok = module(&p);
    free(p.t);
    free(p.buf);
    free(p.vnum);
    free(p.bnum);
    free(p.vfix);
    free(p.bfix);
    free(p.ops);
    free(p.pend);
    limba_hash_free(p.vidx);
    limba_hash_free(p.bidx);
    if (!ok) {
        limba_module_free(p.m);
        return NULL;
    }
    return p.m;
}
