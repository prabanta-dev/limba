/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse.c - the parser of Luxia 0 (see parse.h): the token cursor, the
 * errors, the declarations, the types and the program.
 */
#include "parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void limba_lxp_next(limba_lxp *P)
{
    if (lxp_kind(P) != LX_EOF)
        P->pos++;
}

bool limba_lxp_accept(limba_lxp *P, unsigned kind)
{
    if (lxp_kind(P) != kind)
        return false;
    limba_lxp_next(P);
    return true;
}

static void report(limba_lxp *P, limba_severity sev, unsigned code,
                   limba_loc loc, uint32_t len, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    limba_report_add(P->rep, sev, code, loc, len, "%s", msg);
}

void limba_lxp_error(limba_lxp *P, unsigned code, uint32_t tok, const char *fmt,
                     ...)
{
    if (P->last_error == P->pos)
        return;
    P->last_error = P->pos;
    const limba_lx_token *t = &P->lx->tok[tok];
    va_list ap;
    va_start(ap, fmt);
    report(P, LIMBA_ERROR, code, t->loc, t->len ? t->len : 1, fmt, ap);
    va_end(ap);
}

const char *limba_lxp_describe(const limba_lxp *P, uint32_t tok, char *buf,
                               size_t size)
{
    const limba_lx_token *t = &P->lx->tok[tok];
    limba_where w;
    const char *text = "";
    if (limba_source_where(P->src, t->loc, &w))
        text = P->src->file[w.file].text + w.off;
    int len = t->len > 40 ? 40 : (int)t->len;
    switch (t->kind) {
    case LX_EOF:
        snprintf(buf, size, "the end of the file");
        break;
    case LX_IDENT:
        snprintf(buf, size, "the name '%.*s'", len, text);
        break;
    case LX_INT:
    case LX_REAL:
        snprintf(buf, size, "the number %.*s", len, text);
        break;
    case LX_CHAR:
        snprintf(buf, size, "the character %.*s", len, text);
        break;
    case LX_STRING:
        snprintf(buf, size, "a string");
        break;
    default:
        snprintf(buf, size, "'%s'", limba_lx_kind_text(t->kind));
    }
    return buf;
}

void limba_lxp_expected(limba_lxp *P, const char *what)
{
    if (P->last_error == P->pos)
        return;
    P->last_error = P->pos;
    const limba_lx_token *t = &P->lx->tok[P->pos];
    limba_loc loc = t->loc;
    uint32_t len = t->len ? t->len : 1;
    /* a token missing at the end of a line is reported there */
    if (P->pos > 0 && (t->flags & LX_F_LINE)) {
        const limba_lx_token *p = &P->lx->tok[P->pos - 1];
        loc = p->loc + p->len;
        len = 1;
    }
    char buf[80];
    limba_report_add(P->rep, LIMBA_ERROR, LXE_EXPECTED, loc, len,
                     "expected %s, found %s", what,
                     limba_lxp_describe(P, P->pos, buf, sizeof(buf)));
}

bool limba_lxp_expect(limba_lxp *P, unsigned kind, const char *where)
{
    if (limba_lxp_accept(P, kind))
        return true;
    char what[80];
    snprintf(what, sizeof(what), "'%s'%s%s", limba_lx_kind_text(kind),
             where ? " " : "", where ? where : "");
    limba_lxp_expected(P, what);
    return false;
}

uint32_t limba_lxp_name(limba_lxp *P)
{
    const limba_lx_token *t = &P->lx->tok[P->pos];
    if (t->kind != LX_IDENT) {
        limba_lxp_expected(P, "a name");
        return lxp_node(P, LXN_ERROR, t->loc, 0, 0, 0, 0);
    }
    limba_lxp_next(P);
    return lxp_node(P, LXN_NAME, t->loc, t->val, 0, 0, 0);
}

uint32_t limba_lxp_names(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t mark = limba_lx_list_begin(P->t);
    do
        limba_lx_list_push(P->t, limba_lxp_name(P));
    while (limba_lxp_accept(P, LX_COMMA));
    return limba_lx_list_end(P->t, mark, loc);
}

/* the first token of the line of token i */
static uint32_t line_start(const limba_lxp *P, uint32_t i)
{
    while (i > 0 && !(P->lx->tok[i].flags & LX_F_LINE))
        i--;
    return i;
}

void limba_lxp_end(limba_lxp *P, uint32_t opener)
{
    const char *what = limba_lx_kind_text(P->lx->tok[opener].kind);
    if (lxp_kind(P) != LX_KW_END) {
        char msg[80];
        snprintf(msg, sizeof(msg), "'end' to close the '%s'", what);
        uint32_t errors = P->rep->errors;
        limba_lxp_expected(P, msg);
        if (P->rep->errors > errors)
            limba_report_add(P->rep, LIMBA_NOTE, 0, P->lx->tok[opener].loc,
                             P->lx->tok[opener].len, "the '%s' is here", what);
        return;
    }
    limba_where we, wo;
    uint32_t first = line_start(P, opener);
    if (limba_source_where(P->src, lxp_loc(P), &we) &&
        limba_source_where(P->src, P->lx->tok[first].loc, &wo) &&
        we.line != wo.line && we.col != wo.col)
        limba_report_add(P->rep, LIMBA_WARNING, LXE_END_ALIGN, lxp_loc(P), 3,
                         "this 'end' closes the '%s' of line %u: put it "
                         "in column %u, under the start of that line",
                         what, wo.line, wo.col);
    limba_lxp_next(P);
}

/* skip to a token where a declaration or the body can start */
static void sync_decl(limba_lxp *P)
{
    for (;;) {
        switch (lxp_kind(P)) {
        case LX_SEMI:
            limba_lxp_next(P);
            return;
        case LX_EOF:
        case LX_KW_CONST:
        case LX_KW_TYPE:
        case LX_KW_VAR:
        case LX_KW_PROCEDURE:
        case LX_KW_FUNCTION:
        case LX_KW_PRAGMA:
        case LX_KW_BEGIN:
            return;
        default:
            if (P->lx->tok[P->pos].flags & LX_F_LINE &&
                lxp_kind(P) == LX_IDENT && P->last_error != P->pos)
                return;
            limba_lxp_next(P);
        }
    }
}

uint32_t limba_lxp_type(limba_lxp *P)
{
    uint32_t start = P->pos;
    limba_loc loc = lxp_loc(P);
    switch (lxp_kind(P)) {
    case LX_IDENT: {
        uint32_t name = P->lx->tok[P->pos].val, lo = 0, hi = 0;
        limba_lxp_next(P);
        if (limba_lxp_accept(P, LX_KW_RANGE)) {
            /* I range <>: the bounds come with the argument (Ada's box) */
            if (limba_lxp_accept(P, LX_NE))
                return lxp_node(P, LXN_TBOX, loc, name, 0, 0, 0);
            lo = limba_lxp_expr(P, LXP_SIMPLE);
            limba_lxp_expect(P, LX_DOTDOT, "between the bounds of a range");
            hi = limba_lxp_expr(P, LXP_SIMPLE);
        }
        return lxp_node(P, LXN_TNAME, loc, name, lo, hi, 0);
    }
    case LX_KW_NEW:
        limba_lxp_next(P);
        return lxp_node(P, LXN_TNEW, loc, limba_lxp_type(P), 0, 0, 0);
    case LX_LPAREN: {
        limba_lxp_next(P);
        uint32_t names = limba_lxp_names(P);
        limba_lxp_expect(P, LX_RPAREN, "after the values");
        return lxp_node(P, LXN_TENUM, loc, names, 0, 0, 0);
    }
    case LX_KW_ARRAY:
        limba_lxp_next(P);
        {
            limba_lxp_expect(P, LX_LBRACK, "after array");
            uint32_t index = limba_lxp_type(P);
            limba_lxp_expect(P, LX_RBRACK, "after the index type");
            limba_lxp_expect(P, LX_KW_OF, NULL);
            uint32_t elem = limba_lxp_type(P);
            bool open = P->t->node[index].kind == LXN_TBOX;
            return lxp_node(P, open ? LXN_TOPEN : LXN_TARRAY, loc, index, elem,
                            0, 0);
        }
    case LX_KW_RECORD: {
        limba_lxp_next(P);
        uint32_t mark = limba_lx_list_begin(P->t);
        while (lxp_kind(P) == LX_IDENT) {
            limba_loc floc = lxp_loc(P);
            uint32_t names = limba_lxp_names(P);
            limba_lxp_expect(P, LX_COLON, "after the field names");
            uint32_t type = limba_lxp_type(P);
            limba_lx_list_push(P->t,
                               lxp_node(P, LXN_FIELD, floc, names, type, 0, 0));
            if (!limba_lxp_expect(P, LX_SEMI, "after the field"))
                sync_decl(P);
        }
        uint32_t fields = limba_lx_list_end(P->t, mark, loc);
        limba_lxp_end(P, start);
        return lxp_node(P, LXN_TRECORD, loc, fields, 0, 0, 0);
    }
    case LX_CARET:
        limba_lxp_next(P);
        return lxp_node(P, LXN_TPTR, loc, limba_lxp_type(P), 0, 0, 0);
    default:
        limba_lxp_expected(P, "a type");
        return lxp_node(P, LXN_ERROR, loc, 0, 0, 0, 0);
    }
}

uint32_t limba_lxp_constdecl(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t name = limba_lxp_name(P), type = 0;
    if (limba_lxp_accept(P, LX_COLON))
        type = limba_lxp_type(P);
    if (lxp_kind(P) == LX_ASSIGN) {
        limba_lxp_error(P, LXE_EXPECTED, P->pos,
                        "a constant is given with '=', not ':='");
        limba_lxp_next(P);
    } else {
        limba_lxp_expect(P, LX_EQ, "and the value of the constant");
    }
    uint32_t value = limba_lxp_expr(P, LXP_EXPR);
    return lxp_node(P, LXN_CONST, loc, name, type, value, 0);
}

uint32_t limba_lxp_vardecl(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t names = limba_lxp_names(P), type = 0, init = 0;
    if (limba_lxp_accept(P, LX_COLON)) {
        type = limba_lxp_type(P);
        if (limba_lxp_accept(P, LX_ASSIGN))
            init = limba_lxp_expr(P, LXP_EXPR);
    } else if (limba_lxp_accept(P, LX_ASSIGN)) {
        init = limba_lxp_expr(P, LXP_EXPR);
    } else {
        limba_lxp_expected(P, "':' and a type, or ':=' and a value");
    }
    return lxp_node(P, LXN_VAR, loc, names, type, init, 0);
}

static uint32_t typedecl(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t name = limba_lxp_name(P);
    limba_lxp_expect(P, LX_EQ, "and the type");
    return lxp_node(P, LXN_TYPEDECL, loc, name, limba_lxp_type(P), 0, 0);
}

static uint32_t params(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    uint32_t mark = limba_lx_list_begin(P->t);
    if (!limba_lxp_expect(P, LX_LPAREN, "with the parameters, also none: P()"))
        return limba_lx_list_end(P->t, mark, loc);
    if (lxp_kind(P) != LX_RPAREN) {
        do {
            limba_loc ploc = lxp_loc(P);
            unsigned mode = 0;
            if (lxp_kind(P) == LX_KW_VAR || lxp_kind(P) == LX_KW_OUT) {
                mode = lxp_kind(P);
                limba_lxp_next(P);
            }
            uint32_t names = limba_lxp_names(P);
            limba_lxp_expect(P, LX_COLON, "and the type of the parameter");
            uint32_t type = limba_lxp_type(P);
            uint32_t p = lxp_node(P, LXN_PARAM, ploc, names, type, 0, 0);
            P->t->node[p].op = (uint8_t)mode;
            limba_lx_list_push(P->t, p);
        } while (limba_lxp_accept(P, LX_SEMI));
    }
    limba_lxp_expect(P, LX_RPAREN, "after the parameters");
    return limba_lx_list_end(P->t, mark, loc);
}

/* the optional name after the end of a routine or of the program */
static void end_name(limba_lxp *P, uint32_t name)
{
    if (lxp_kind(P) != LX_IDENT)
        return;
    uint32_t id = P->t->node[name].a;
    if (P->t->node[name].kind == LXN_NAME && P->lx->tok[P->pos].val != id) {
        size_t n;
        const char *s = limba_strtab_get(P->lx->names, id, &n);
        limba_lxp_error(P, LXE_END_NAME, P->pos,
                        "this end closes '%.*s': write that name or none",
                        (int)n, s);
    }
    limba_lxp_next(P);
}

static uint32_t decls(limba_lxp *P, bool top);

static uint32_t routine(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    unsigned kind = lxp_kind(P);
    limba_lxp_next(P);
    uint32_t name = limba_lxp_name(P);
    uint32_t ps = params(P), result = 0;
    if (kind == LX_KW_FUNCTION) {
        limba_lxp_expect(P, LX_COLON, "and the type of the result");
        result = limba_lxp_type(P);
    } else if (lxp_kind(P) == LX_COLON) {
        limba_lxp_error(P, LXE_EXPECTED, P->pos,
                        "a procedure has no result: declare a function");
        limba_lxp_next(P);
        limba_lxp_type(P);
    }
    limba_lxp_expect(P, LX_SEMI, "after the heading");
    uint32_t locals = decls(P, false);
    uint32_t begin = P->pos;
    limba_loc bloc = lxp_loc(P);
    limba_lxp_expect(P, LX_KW_BEGIN, NULL);
    uint32_t body = limba_lxp_stmts(P);
    limba_lxp_end(P, begin);
    end_name(P, name);
    limba_lxp_expect(P, LX_SEMI, "after the end of the routine");
    uint32_t b = lxp_node(P, LXN_BODY, bloc, locals, body, 0, 0);
    uint32_t r = lxp_node(P, LXN_ROUTINE, loc, name, ps, result, b);
    P->t->node[r].op = (uint8_t)kind;
    return r;
}

uint32_t limba_lxp_pragma(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    limba_lxp_next(P);
    uint32_t name = 0, checks = 0;
    if (lxp_kind(P) == LX_IDENT)
        name = limba_lxp_name(P);
    else
        limba_lxp_expected(P, "the name of the pragma");
    if (limba_lxp_expect(P, LX_LPAREN, "after the name of the pragma")) {
        checks = limba_lxp_names(P);
        limba_lxp_expect(P, LX_RPAREN, "after the names of the checks");
    }
    return lxp_node(P, LXN_PRAGMA, loc, name, checks, 0, 0);
}

static uint32_t decls(limba_lxp *P, bool top)
{
    limba_loc loc = lxp_loc(P);
    uint32_t mark = limba_lx_list_begin(P->t);
    for (;;) {
        unsigned k = lxp_kind(P);
        switch (k) {
        case LX_KW_CONST:
        case LX_KW_TYPE:
        case LX_KW_VAR:
            limba_lxp_next(P);
            do {
                uint32_t d = k == LX_KW_CONST  ? limba_lxp_constdecl(P)
                             : k == LX_KW_TYPE ? typedecl(P)
                                               : limba_lxp_vardecl(P);
                limba_lx_list_push(P->t, d);
                if (!limba_lxp_expect(P, LX_SEMI, "after the declaration"))
                    sync_decl(P);
            } while (lxp_kind(P) == LX_IDENT);
            break;
        case LX_KW_PRAGMA:
            limba_lx_list_push(P->t, limba_lxp_pragma(P));
            if (!limba_lxp_expect(P, LX_SEMI, "after the pragma"))
                sync_decl(P);
            break;
        case LX_KW_PROCEDURE:
        case LX_KW_FUNCTION:
            if (!top)
                limba_lxp_error(P, LXE_NESTED_ROUTINE, P->pos,
                                "a routine cannot be declared inside "
                                "another one in Luxia 0");
            limba_lx_list_push(P->t, routine(P));
            break;
        case LX_KW_BEGIN:
        case LX_EOF:
            return limba_lx_list_end(P->t, mark, loc);
        default:
            limba_lxp_expected(P, "a declaration or 'begin'");
            limba_lxp_next(P);
            sync_decl(P);
        }
    }
}

static void program(limba_lxp *P)
{
    limba_loc loc = lxp_loc(P);
    if (!limba_lxp_expect(P, LX_KW_PROGRAM, "at the start: program Name;"))
        sync_decl(P);
    uint32_t name = 0, body;
    if (lxp_kind(P) == LX_IDENT) {
        name = limba_lxp_name(P);
        limba_lxp_expect(P, LX_SEMI, "after the name of the program");
    }
    uint32_t ds = decls(P, true);
    uint32_t begin = P->pos;
    if (limba_lxp_expect(P, LX_KW_BEGIN, "and the statements of the program")) {
        body = limba_lxp_stmts(P);
        limba_lxp_end(P, begin);
        end_name(P, name);
        limba_lxp_expect(P, LX_DOT, "after the end of the program");
    } else {
        body = limba_lx_list_end(P->t, limba_lx_list_begin(P->t), loc);
    }
    if (lxp_kind(P) != LX_EOF)
        limba_lxp_error(P, LXE_AFTER_END, P->pos,
                        "nothing may follow the end of the program");
    P->t->root = lxp_node(P, LXN_PROGRAM, loc, name, ds, body, 0);
}

void limba_lx_parse(limba_lx_ast *t, const limba_lx *lx,
                    const limba_source *src, limba_report *rep)
{
    limba_lxp P = {lx, t, rep, src, 0, UINT32_MAX};
    program(&P);
}

void limba_lx_parse_expr(limba_lx_ast *t, const limba_lx *lx,
                         const limba_source *src, limba_report *rep)
{
    limba_lxp P = {lx, t, rep, src, 0, UINT32_MAX};
    t->root = limba_lxp_expr(&P, LXP_EXPR);
    if (lxp_kind(&P) != LX_EOF)
        limba_lxp_expected(&P, "the end of the expression");
}
