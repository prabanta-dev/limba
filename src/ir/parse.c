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
        /* reloc OFFSET func @f, reloc OFFSET global @g: a use, not a
           declaration */
        if (i >= 2 && lp_is_word(&p->t[i - 2], "reloc"))
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

/* data "bytes" then reloc OFFSET (func @f | global @g [ADDEND] | str "s")
   ..., after "=" */
static bool global_data(P *p, limba_id g)
{
    const limba_tok *k = lp_peek(p);
    if (k->kind != TK_STRING)
        return lp_fail(p, "%s expected", "the bytes of the data");
    size_t n = limba_unescape(k, p->buf);
    if (n == SIZE_MAX)
        return lp_fail(p, "%s", "malformed escape in the data");
    if (n > UINT32_MAX)
        return lp_fail(p, "%s", "data too long");
    limba_global_set_data(p->m, g, p->buf, n);
    p->i++;
    while (lp_accept_word(p, "reloc")) {
        uint32_t off;
        limba_id id, n2;
        int64_t addend = 0;
        if (!lp_uinteger32(p, &off))
            return false;
        if (lp_accept_word(p, "func")) {
            if (!lp_sym(p, &n2))
                return false;
            id = limba_func_find(p->m, n2);
            if (id == LIMBA_NONE)
                return lp_fail(p, "%s",
                               "reloc of a function that does not "
                               "exist");
            limba_global_add_reloc(p->m, g, off, LIMBA_RELOC_FUNC, id, 0);
        } else if (lp_accept_word(p, "global")) {
            if (!lp_sym(p, &n2))
                return false;
            id = limba_global_find(p->m, n2);
            if (id == LIMBA_NONE)
                return lp_fail(p, "%s",
                               "reloc of a global that does not "
                               "exist");
            if (lp_peek(p)->kind == TK_INT && !lp_integer(p, &addend))
                return false;
            limba_global_add_reloc(p->m, g, off, LIMBA_RELOC_GLOBAL, id,
                                   addend);
        } else if (lp_accept_word(p, "str")) {
            if (!lp_string(p, &id))
                return false;
            limba_global_add_reloc(p->m, g, off, LIMBA_RELOC_STR, id, 0);
        } else
            return lp_fail(p, "%s expected", "func, global or str");
    }
    return true;
}

static bool global(P *p)
{
    limba_id n, t;
    if (!lp_sym(p, &n) || !lp_expect(p, TK_COLON, "':'") || !lp_type(p, &t))
        return false;
    limba_id gid = limba_global_find(p->m, n);
    limba_global *g = &p->m->globals[gid];
    g->type = t;
    if (lp_accept(p, TK_EQ)) {
        const limba_tok *k = lp_peek(p);
        if (lp_accept_word(p, "data")) {
            if (!global_data(p, gid))
                return false;
            g = &p->m->globals[gid];
        } else if (k->kind == TK_STRING) {
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
            if (!lp_real(p, g->type, &g->value))
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
        } else if (lp_accept_word(p, "language")) {
            /* language "luxia": the prefix of the messages of traps */
            if (!lp_string(p, &p->m->language))
                return false;
        } else if (lp_accept_word(p, "target")) {
            /* target "x86_64-linux": the C its externs follow */
            if (!lp_string(p, &p->m->target))
                return false;
        } else if (lp_accept_word(p, "message")) {
            /* message 105 "dangling pointer": the text of a trap */
            int64_t code;
            limba_id text;
            if (!lp_integer(p, &code) || !lp_string(p, &text))
                return false;
            limba_message_add(p->m, code, text);
        } else if (lp_accept_word(p, "exception")) {
            /* exception 1000 "ParseError" parent 900 (progetto_ir.md
               § 11i) */
            int64_t id, parent = -1;
            limba_id name;
            if (!lp_integer(p, &id) || !lp_string(p, &name))
                return false;
            if (lp_accept_word(p, "parent") && !lp_integer(p, &parent))
                return false;
            if (id < INT32_MIN || id > INT32_MAX || parent < INT32_MIN ||
                parent > INT32_MAX)
                return lp_fail(p, "%s", "an identity past 32 bits");
            limba_exception_add(p->m, (int32_t)id, name, (int32_t)parent);
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
        } else if (lp_accept_word(p, "pos")) {
            /* pos 3 "file" 12 5: in order, before the functions */
            int64_t k, line, col;
            limba_id file;
            if (!lp_integer(p, &k) || !lp_string(p, &file) ||
                !lp_integer(p, &line) || !lp_integer(p, &col))
                return false;
            if (k != (int64_t)p->m->npos + 1 || line < 0 || col < 0 ||
                line > UINT32_MAX || col > UINT32_MAX)
                return lp_fail(p, "%s", "pos: numbered in order from 1");
            LIMBA_GROW(p->m->pos, p->m->npos, p->m->cappos);
            p->m->pos[p->m->npos++] =
                (limba_pos){file, (uint32_t)line, (uint32_t)col};
        } else {
            return lp_fail(p, "%s",
                           "module, memory, type, global, extern, pos or "
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
    free(p.hnum);
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
    limba_handled_compute(p.m); /* the set "under a handler", § 11i */
    return p.m;
}
