/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lex.c - tokens of the text form (see lex.h).
 */
#include "lex.h"

#include "common/xalloc.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool ident_char(int c)
{
    return isalnum(c) || c == '_' || c == '.' || c == '$';
}

/* digits only after one letter: v12, b3 */
static bool numbered(const char *s, size_t n, char first, uint64_t *num)
{
    if (n < 2 || s[0] != first || n > 11)
        return false;
    uint64_t v = 0;
    for (size_t i = 1; i < n; i++) {
        if (!isdigit((unsigned char)s[i]))
            return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *num = v;
    return true;
}

/* a closing quote, skipping escapes; the index after it, 0 if none */
static size_t quoted(const char *p, const char *end)
{
    size_t i = 1;
    while (p + i < end && p[i] != '"' && p[i] != '\n') {
        if (p[i] == '\\')
            i++;
        i++;
    }
    return p + i < end && p[i] == '"' ? i + 1 : 0;
}

/* a number: returns its length and whether it is a float */
static size_t number(const char *p, const char *end, bool *is_float)
{
    size_t i = 0;
    *is_float = false;
    if (p + i < end && (p[i] == '-' || p[i] == '+'))
        i++;
    if (end - p >= (ptrdiff_t)(i + 3) &&
        (strncmp(p + i, "inf", 3) == 0 || strncmp(p + i, "nan", 3) == 0)) {
        *is_float = true;
        return i + 3;
    }
    if (p + i + 1 < end && p[i] == '0' &&
        (p[i + 1] == 'x' || p[i + 1] == 'X')) {
        i += 2;
        while (p + i < end && (isxdigit((unsigned char)p[i]) || p[i] == '.')) {
            if (p[i] == '.')
                *is_float = true;
            i++;
        }
        if (p + i < end && (p[i] == 'p' || p[i] == 'P')) {
            *is_float = true;
            i++;
            if (p + i < end && (p[i] == '+' || p[i] == '-'))
                i++;
            while (p + i < end && isdigit((unsigned char)p[i]))
                i++;
        }
        return i;
    }
    while (p + i < end && isdigit((unsigned char)p[i]))
        i++;
    if (p + i < end && p[i] == '.') {
        *is_float = true;
        i++;
        while (p + i < end && isdigit((unsigned char)p[i]))
            i++;
    }
    if (p + i < end && (p[i] == 'e' || p[i] == 'E')) {
        *is_float = true;
        i++;
        if (p + i < end && (p[i] == '+' || p[i] == '-'))
            i++;
        while (p + i < end && isdigit((unsigned char)p[i]))
            i++;
    }
    return i;
}

limba_tok *limba_lex(const char *text, size_t len, size_t *count,
                     unsigned *bad_line)
{
    limba_tok *t = NULL;
    size_t n = 0, cap = 0;
    unsigned line = 1;
    const char *p = text, *end = text + len;

    for (;;) {
        while (p < end && (isspace((unsigned char)*p) || *p == ';')) {
            if (*p == ';')
                while (p < end && *p != '\n')
                    p++;
            else if (*p++ == '\n')
                line++;
        }
        LIMBA_GROW(t, n, cap);
        limba_tok *k = &t[n++];
        memset(k, 0, sizeof(*k));
        k->line = line;
        k->s = p;
        if (p >= end) {
            k->kind = TK_EOF;
            break;
        }
        int c = (unsigned char)*p;
        size_t w = 1;
        switch (c) {
        case '(':
            k->kind = TK_LPAREN;
            break;
        case ')':
            k->kind = TK_RPAREN;
            break;
        case '{':
            k->kind = TK_LBRACE;
            break;
        case '}':
            k->kind = TK_RBRACE;
            break;
        case '[':
            k->kind = TK_LBRACK;
            break;
        case ']':
            k->kind = TK_RBRACK;
            break;
        case ',':
            k->kind = TK_COMMA;
            break;
        case ':':
            k->kind = TK_COLON;
            break;
        case '=':
            k->kind = TK_EQ;
            break;
        case '"':
            w = quoted(p, end);
            k->kind = w ? TK_STRING : TK_BAD;
            k->s = p + 1;
            k->n = w ? w - 2 : 0;
            break;
        case '@':
        case '%':
            if (p + 1 < end && p[1] == '"') {
                w = quoted(p + 1, end);
                k->kind = w ? (c == '@' ? TK_SYM : TK_TYPE) : TK_BAD;
                k->s = p + 2;
                k->n = w ? w - 2 : 0;
                w = w ? w + 1 : 1;
            } else if (c == '@' && p + 1 < end &&
                       isdigit((unsigned char)p[1])) {
                w = 1;
                k->kind = TK_OFFSET;
                while (p + w < end && isdigit((unsigned char)p[w]) &&
                       k->num < UINT32_MAX)
                    k->num = k->num * 10 + (uint64_t)(p[w++] - '0');
                if (p + w < end && isdigit((unsigned char)p[w]))
                    k->kind = TK_BAD;
            } else {
                while (p + w < end && ident_char((unsigned char)p[w]))
                    w++;
                k->kind = w > 1 ? (c == '@' ? TK_SYM : TK_TYPE) : TK_BAD;
                k->s = p + 1;
                k->n = w - 1;
            }
            break;
        case '!':
            k->kind = TK_POS;
            while (p + w < end && isdigit((unsigned char)p[w]) &&
                   k->num < UINT32_MAX)
                k->num = k->num * 10 + (uint64_t)(p[w++] - '0');
            if (w == 1 || (p + w < end && isdigit((unsigned char)p[w])))
                k->kind = TK_BAD;
            break;
        case '$':
            k->kind = TK_SLOT;
            while (p + w < end && isdigit((unsigned char)p[w]) &&
                   k->num < UINT32_MAX)
                k->num = k->num * 10 + (uint64_t)(p[w++] - '0');
            if (w == 1 || (p + w < end && isdigit((unsigned char)p[w])))
                k->kind = TK_BAD;
            break;
        default:
            if (c == '-' && p + 1 < end && p[1] == '>') {
                k->kind = TK_ARROW;
                w = 2;
            } else if (c == '.' && end - p >= 3 && p[1] == '.' && p[2] == '.') {
                k->kind = TK_ELLIPSIS;
                w = 3;
            } else if (isdigit(c) || c == '-' || c == '+') {
                bool fl;
                w = number(p, end, &fl);
                k->kind = w > (size_t)(c == '-' || c == '+')
                              ? (fl ? TK_FLOAT : TK_INT)
                              : TK_BAD;
                if (!w)
                    w = 1;
            } else if (isalpha(c) || c == '_') {
                while (p + w < end && ident_char((unsigned char)p[w]))
                    w++;
                k->kind = TK_IDENT;
                if (numbered(p, w, 'v', &k->num))
                    k->kind = TK_VALUE;
                else if (numbered(p, w, 'b', &k->num))
                    k->kind = TK_BLOCK;
            } else {
                k->kind = TK_BAD;
            }
        }
        if (k->kind == TK_BAD) {
            *bad_line = line;
            free(t);
            return NULL;
        }
        /* strings and names set s and n themselves */
        if (k->kind != TK_STRING && k->kind != TK_SYM && k->kind != TK_TYPE)
            k->n = w;
        p += w;
    }
    *count = n;
    return t;
}

size_t limba_unescape(const limba_tok *tok, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < tok->n; i++) {
        char c = tok->s[i];
        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        if (++i >= tok->n)
            return SIZE_MAX;
        switch (tok->s[i]) {
        case 'n':
            out[o++] = '\n';
            break;
        case 't':
            out[o++] = '\t';
            break;
        case '"':
            out[o++] = '"';
            break;
        case '\\':
            out[o++] = '\\';
            break;
        case 'x': {
            if (i + 2 >= tok->n || !isxdigit((unsigned char)tok->s[i + 1]) ||
                !isxdigit((unsigned char)tok->s[i + 2]))
                return SIZE_MAX;
            char hex[3] = {tok->s[i + 1], tok->s[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
            break;
        }
        default:
            return SIZE_MAX;
        }
    }
    return o;
}
