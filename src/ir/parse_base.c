/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse_base.c - reading the text form: tokens, literals and types.
 */
#include "parse.h"

#include "common/xalloc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

const limba_tok *lp_peek(P *p)
{
    return &p->t[p->i];
}

bool lp_fail(P *p, const char *fmt, const char *what)
{
    if (!p->err) {
        char msg[200];
        snprintf(msg, sizeof(msg), fmt, what);
        limba_diag_set(p->d, lp_peek(p)->line, "line %u: %s", lp_peek(p)->line,
                       msg);
    }
    p->err = true;
    return false;
}

bool lp_is_word(const limba_tok *k, const char *w)
{
    return k->kind == TK_IDENT && k->n == strlen(w) &&
           memcmp(k->s, w, k->n) == 0;
}

bool lp_accept(P *p, int kind)
{
    if (lp_peek(p)->kind != kind)
        return false;
    p->i++;
    return true;
}

bool lp_expect(P *p, int kind, const char *what)
{
    return lp_accept(p, kind) || lp_fail(p, "%s expected", what);
}

bool lp_accept_word(P *p, const char *w)
{
    if (!lp_is_word(lp_peek(p), w))
        return false;
    p->i++;
    return true;
}

bool lp_expect_word(P *p, const char *w)
{
    return lp_accept_word(p, w) || lp_fail(p, "'%s' expected", w);
}

/* a quoted or plain name, interned */
limba_id lp_name(P *p, const limba_tok *k)
{
    size_t n = limba_unescape(k, p->buf);
    if (n == SIZE_MAX) {
        lp_fail(p, "%s", "malformed escape in a name or string");
        return LIMBA_NONE;
    }
    return limba_str_intern(p->m, p->buf, n);
}

bool lp_integer(P *p, int64_t *out)
{
    const limba_tok *k = lp_peek(p);
    if (k->kind != TK_INT)
        return lp_fail(p, "%s expected", "an integer");
    char tmp[64];
    if (k->n >= sizeof(tmp))
        return lp_fail(p, "%s", "integer too long");
    memcpy(tmp, k->s, k->n);
    tmp[k->n] = 0;
    char *e;
    errno = 0;
    if (tmp[0] == '-') {
        long long v = strtoll(tmp, &e, 0);
        *out = v;
    } else {
        unsigned long long v = strtoull(tmp, &e, 0);
        *out = (int64_t)v; /* above INT64_MAX: two's complement */
    }
    if (errno || *e)
        return lp_fail(p, "%s", "integer out of range");
    p->i++;
    return true;
}

bool lp_uinteger32(P *p, uint32_t *out)
{
    int64_t v;
    if (!lp_integer(p, &v))
        return false;
    if (v < 0 || v > UINT32_MAX)
        return lp_fail(p, "%s", "a number from 0 to 2^32 - 1 expected");
    *out = (uint32_t)v;
    return true;
}

/* a float literal: hex or decimal, inf, nan, or nan.0x<bits> for a NaN
   whose bits must survive */
bool lp_real(P *p, int64_t *bits)
{
    const limba_tok *k = lp_peek(p);
    char tmp[64];
    if (k->n >= sizeof(tmp))
        return lp_fail(p, "%s", "number too long");
    memcpy(tmp, k->s, k->n);
    tmp[k->n] = 0;
    if (k->kind == TK_IDENT && strncmp(tmp, "nan.0x", 6) == 0) {
        char *e;
        errno = 0;
        uint64_t b = strtoull(tmp + 4, &e, 16);
        if (errno || *e)
            return lp_fail(p, "%s", "malformed NaN");
        *bits = (int64_t)b;
        p->i++;
        return true;
    }
    if (k->kind != TK_FLOAT && k->kind != TK_INT &&
        !(k->kind == TK_IDENT && (!strcmp(tmp, "inf") || !strcmp(tmp, "nan"))))
        return lp_fail(p, "%s expected", "a floating-point number");
    char *e;
    double x = strtod(tmp, &e);
    if (*e)
        return lp_fail(p, "%s", "malformed floating-point number");
    memcpy(bits, &x, sizeof(x));
    p->i++;
    return true;
}

bool lp_string(P *p, limba_id *id)
{
    if (lp_peek(p)->kind != TK_STRING)
        return lp_fail(p, "%s expected", "a string");
    *id = lp_name(p, lp_peek(p));
    p->i++;
    return !p->err;
}

/* ---- types ---- */

bool lp_type(P *p, limba_id *out)
{
    const limba_tok *k = lp_peek(p);
    if (k->kind == TK_TYPE) {
        limba_id n = lp_name(p, k);
        if (p->err)
            return false;
        *out = limba_type_find_struct(p->m, n);
        if (*out == LIMBA_NONE)
            return lp_fail(p, "%s", "unknown struct type");
        p->i++;
        return true;
    }
    if (lp_accept(p, TK_LBRACK)) {
        uint32_t count;
        limba_id elem;
        if (!lp_uinteger32(p, &count) || !lp_expect_word(p, "x") ||
            !lp_type(p, &elem) || !lp_expect(p, TK_RBRACK, "']'"))
            return false;
        if (elem == LIMBA_T_VOID || p->m->types[elem].kind == LIMBA_TK_FUNC)
            return lp_fail(p, "%s", "array of void or of functions");
        *out = limba_type_array(p->m, elem, count);
        return true;
    }
    if (lp_accept_word(p, "fn")) {
        limba_id params[256], ret;
        uint32_t n = 0;
        bool variadic = false;
        if (!lp_expect(p, TK_LPAREN, "'('"))
            return false;
        while (!lp_accept(p, TK_RPAREN)) {
            if (n && !lp_expect(p, TK_COMMA, "',' or ')'"))
                return false;
            if (lp_accept(p, TK_ELLIPSIS)) {
                variadic = true;
                if (!lp_expect(p, TK_RPAREN, "')' after '...'"))
                    return false;
                break;
            }
            if (n == 256)
                return lp_fail(p, "%s", "more than 256 parameters");
            if (!lp_type(p, &params[n++]))
                return false;
        }
        if (!lp_expect(p, TK_ARROW, "'->'") || !lp_type(p, &ret))
            return false;
        *out = limba_type_func(p->m, ret, params, n, variadic);
        return true;
    }
    if (k->kind == TK_IDENT) {
        *out = limba_scalar_find(k->s, k->n);
        if (*out != LIMBA_NONE) {
            p->i++;
            return true;
        }
    }
    return lp_fail(p, "%s expected", "a type");
}

bool lp_sym(P *p, limba_id *n)
{
    if (lp_peek(p)->kind != TK_SYM)
        return lp_fail(p, "%s expected", "a @name");
    *n = lp_name(p, lp_peek(p));
    p->i++;
    return !p->err;
}
