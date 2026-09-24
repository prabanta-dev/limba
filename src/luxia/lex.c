/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lex.c - the lexer of Luxia 0 (see lex.h). The lexical rules are those of
 * job/docs/luxia_0.md § 3.
 */
#include "lex.h"

#include "common/xalloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *const kind_text[] = {
#define LX_TOKEN(name, text) text,
#define LX_KEYWORD(name, text) text,
#include "tokens.def"
#undef LX_TOKEN
#undef LX_KEYWORD
};

const char *limba_lx_kind_text(unsigned kind)
{
    return kind < LX_NKINDS ? kind_text[kind] : "?";
}

void limba_lx_init(limba_lx *lx)
{
    memset(lx, 0, sizeof(*lx));
    lx->names = limba_strtab_new();
    lx->strings = limba_strtab_new();
    for (unsigned k = LX_KW_FIRST; k < LX_NKINDS; k++)
        limba_strtab_intern(lx->names, kind_text[k], strlen(kind_text[k]));
}

void limba_lx_free(limba_lx *lx)
{
    free(lx->tok);
    free(lx->ints);
    free(lx->reals);
    limba_strtab_free(lx->names);
    limba_strtab_free(lx->strings);
    memset(lx, 0, sizeof(*lx));
}

/* the state of one run */
typedef struct {
    limba_lx *lx;
    limba_report *rep;
    const char *text;
    uint32_t len;
    limba_loc base;
    uint32_t pos;    /* the next byte */
    bool line_start; /* no token yet on this line */
    char *buf;       /* folded names, literal values */
    size_t nbuf, capbuf;
} lexer;

static void buf_put(lexer *L, const char *p, size_t n)
{
    if (L->nbuf + n > L->capbuf) {
        while (L->nbuf + n > L->capbuf)
            L->capbuf = L->capbuf ? 2 * L->capbuf : 256;
        L->buf = limba_xrealloc(L->buf, L->capbuf, 1);
    }
    memcpy(L->buf + L->nbuf, p, n);
    L->nbuf += n;
}

static void error(lexer *L, unsigned code, uint32_t off, uint32_t len,
                  const char *fmt, const char *arg)
{
    limba_report_add(L->rep, LIMBA_ERROR, code, L->base + off, len, fmt, arg);
}

static limba_lx_token *emit(lexer *L, unsigned kind, uint32_t start,
                            uint32_t val)
{
    limba_lx *lx = L->lx;
    LIMBA_GROW(lx->tok, lx->ntok, lx->captok);
    limba_lx_token *t = &lx->tok[lx->ntok++];
    t->kind = (uint8_t)kind;
    t->flags = L->line_start ? LX_F_LINE : 0;
    t->spare = 0;
    t->loc = L->base + start;
    t->len = L->pos - start;
    t->val = val;
    L->line_start = false;
    return t;
}

static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static bool is_alnum(int c)
{
    return is_alpha(c) || is_digit(c) || c == '_';
}

static int at(const lexer *L, uint32_t i)
{
    return i < L->len ? (unsigned char)L->text[i] : 0;
}

static void name(lexer *L)
{
    uint32_t start = L->pos;
    while (is_alnum(at(L, L->pos)))
        L->pos++;
    L->nbuf = 0;
    bool folded = false;
    for (uint32_t i = start; i < L->pos; i++) {
        char c = L->text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
            folded = true;
        }
        buf_put(L, &c, 1);
    }
    uint32_t id = limba_strtab_intern(L->lx->names, L->buf, L->nbuf);
    if (id < LX_NKEYWORDS) {
        if (folded)
            error(L, LXE_KEYWORD_CASE, start, L->pos - start,
                  "the keyword '%s' is written in lowercase only",
                  kind_text[LX_KW_FIRST + id]);
        emit(L, LX_KW_FIRST + id, start, id);
        return;
    }
    emit(L, LX_IDENT, start, id);
}

static unsigned digit_value(int c)
{
    if (is_digit(c))
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (unsigned)(c - 'A' + 10);
    return 99;
}

/* digits of base with '_' only between two of them, from L->pos; false
   (and an error) on a misplaced '_' or no digit at all */
static bool digits(lexer *L, unsigned base, uint32_t start)
{
    uint32_t first = L->pos;
    bool ok = true;
    while (digit_value(at(L, L->pos)) < base || at(L, L->pos) == '_') {
        if (at(L, L->pos) == '_' &&
            (L->pos == first || digit_value(at(L, L->pos - 1)) >= base ||
             digit_value(at(L, L->pos + 1)) >= base))
            ok = false;
        L->pos++;
    }
    if (L->pos == first) {
        error(L, LXE_BAD_NUMBER, start, L->pos - start + 1, "%s",
              "a number needs digits here");
        return false;
    }
    if (!ok)
        error(L, LXE_BAD_NUMBER, start, L->pos - start, "%s",
              "'_' goes only between two digits");
    return ok;
}

/* a letter, digit or '_' right after a number: report and swallow it */
static bool tail(lexer *L, uint32_t start)
{
    if (!is_alnum(at(L, L->pos)))
        return true;
    char what[2] = {(char)at(L, L->pos), 0};
    while (is_alnum(at(L, L->pos)))
        L->pos++;
    error(L, LXE_BAD_NUMBER, start, L->pos - start,
          "'%s' cannot follow the digits of this number", what);
    return false;
}

static void integer_value(lexer *L, uint32_t start, uint32_t from,
                          unsigned base, bool ok)
{
    uint64_t v = 0;
    bool big = false;
    for (uint32_t i = from; i < L->pos; i++) {
        unsigned d = digit_value(at(L, i));
        if (d >= base)
            continue;
        if (__builtin_mul_overflow(v, base, &v) ||
            __builtin_add_overflow(v, d, &v))
            big = true;
    }
    limba_lx *lx = L->lx;
    LIMBA_GROW(lx->ints, lx->nints, lx->capints);
    lx->ints[lx->nints] = ok && !big ? v : 0;
    limba_lx_token *t = emit(L, LX_INT, start, lx->nints++);
    if (big && ok)
        t->flags |= LX_F_BIG;
}

static void number(lexer *L)
{
    uint32_t start = L->pos;
    int c1 = at(L, L->pos + 1);
    if (at(L, L->pos) == '0' && (c1 == 'x' || c1 == 'o' || c1 == 'b' ||
                                 c1 == 'X' || c1 == 'O' || c1 == 'B')) {
        unsigned base = c1 == 'x' || c1 == 'X'   ? 16
                        : c1 == 'o' || c1 == 'O' ? 8
                                                 : 2;
        bool ok = true;
        if (c1 == 'X' || c1 == 'O' || c1 == 'B') {
            error(L, LXE_BAD_NUMBER, start, 2, "%s",
                  "the base prefix is written in lowercase: 0x, 0o, 0b");
            ok = false;
        }
        L->pos += 2;
        ok = digits(L, base, start) && ok;
        ok = tail(L, start) && ok;
        integer_value(L, start, start + 2, base, ok);
        return;
    }
    bool ok = digits(L, 10, start);
    bool real = false;
    if (at(L, L->pos) == '.' && is_digit(at(L, L->pos + 1))) {
        real = true;
        L->pos++;
        ok = digits(L, 10, start) && ok;
    }
    if (at(L, L->pos) == 'e' || at(L, L->pos) == 'E') {
        if (at(L, L->pos) == 'E') {
            error(L, LXE_BAD_NUMBER, L->pos - 0, 1, "%s",
                  "the exponent is written with a lowercase e");
            ok = false;
        }
        real = true;
        L->pos++;
        if (at(L, L->pos) == '+' || at(L, L->pos) == '-')
            L->pos++;
        ok = digits(L, 10, start) && ok;
    }
    ok = tail(L, start) && ok;
    if (!real) {
        integer_value(L, start, start, 10, ok);
        return;
    }
    double v = 0;
    if (ok) {
        L->nbuf = 0;
        for (uint32_t i = start; i < L->pos; i++)
            if (L->text[i] != '_')
                buf_put(L, &L->text[i], 1);
        buf_put(L, "", 1);
        v = strtod(L->buf, NULL);
        if (isinf(v)) {
            error(L, LXE_REAL_RANGE, start, L->pos - start, "%s",
                  "this real number is too large for Float64");
            v = 0;
        }
    }
    limba_lx *lx = L->lx;
    LIMBA_GROW(lx->reals, lx->nreals, lx->capreals);
    lx->reals[lx->nreals] = v;
    emit(L, LX_REAL, start, lx->nreals++);
}

static void control_error(lexer *L, uint32_t off)
{
    error(L, LXE_CONTROL_IN_LITERAL, off, 1, "%s",
          "no control characters in a literal: use LF, CR, TAB or NUL");
}

static void character(lexer *L)
{
    uint32_t start = L->pos;
    L->pos++;
    /* ''' is the apostrophe */
    if (at(L, L->pos) == '\'' && at(L, L->pos + 1) == '\'') {
        L->pos += 2;
        emit(L, LX_CHAR, start, '\'');
        return;
    }
    int c = at(L, L->pos);
    if (c == '\'' || c == '\n' || L->pos >= L->len) {
        if (c == '\'')
            L->pos++;
        error(L, LXE_BAD_CHAR_LITERAL, start, L->pos - start, "%s",
              "a character literal holds one character: 'a', ''' for the "
              "apostrophe");
        emit(L, LX_CHAR, start, 0);
        return;
    }
    unsigned n;
    uint32_t cp = limba_utf8_decode(L->text + L->pos, &n);
    if (cp < 0x20 || cp == 0x7f)
        control_error(L, L->pos);
    L->pos += n;
    if (at(L, L->pos) == '\'')
        L->pos++;
    else
        error(L, LXE_BAD_CHAR_LITERAL, start, L->pos - start, "%s",
              "a character literal is closed by ': 'a'");
    emit(L, LX_CHAR, start, cp);
}

static void string(lexer *L)
{
    uint32_t start = L->pos;
    L->pos++;
    L->nbuf = 0;
    for (;;) {
        int c = at(L, L->pos);
        if (L->pos >= L->len || c == '\n') {
            error(L, LXE_OPEN_STRING, start, L->pos - start, "%s",
                  "string not closed on its line");
            break;
        }
        if (c == '"') {
            L->pos++;
            if (at(L, L->pos) != '"')
                break;
            buf_put(L, "\"", 1);
            L->pos++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            if (c != '\r' || at(L, L->pos + 1) != '\n')
                control_error(L, L->pos);
            L->pos++;
            continue;
        }
        buf_put(L, L->text + L->pos, 1);
        L->pos++;
    }
    uint32_t id = limba_strtab_intern(L->lx->strings, L->buf, L->nbuf);
    emit(L, LX_STRING, start, id);
}

/* (* ... *), nested; L->pos on the '(' */
static void comment(lexer *L)
{
    uint32_t start = L->pos;
    unsigned depth = 0;
    while (L->pos < L->len) {
        if (at(L, L->pos) == '(' && at(L, L->pos + 1) == '*') {
            depth++;
            L->pos += 2;
        } else if (at(L, L->pos) == '*' && at(L, L->pos + 1) == ')') {
            L->pos += 2;
            if (--depth == 0)
                return;
        } else {
            L->pos++;
        }
    }
    error(L, LXE_OPEN_COMMENT, start, 2, "%s",
          "comment never closed: (* needs its *)");
}

/* one token, or nothing if L->pos was on blanks or a comment */
static void token(lexer *L)
{
    uint32_t start = L->pos;
    int c = at(L, L->pos), d = at(L, L->pos + 1);
    unsigned kind;
    switch (c) {
    case ' ':
    case '\t':
    case '\r':
        L->pos++;
        return;
    case '\n':
        L->pos++;
        L->line_start = true;
        return;
    case '/':
        if (d == '/') {
            while (L->pos < L->len && at(L, L->pos) != '\n')
                L->pos++;
            return;
        }
        kind = LX_SLASH;
        break;
    case '(':
        if (d == '*') {
            comment(L);
            return;
        }
        kind = LX_LPAREN;
        break;
    case '"':
        string(L);
        return;
    case '\'':
        character(L);
        return;
    case '+':
        kind = LX_PLUS;
        break;
    case '-':
        kind = LX_MINUS;
        break;
    case '*':
        kind = d == '*' ? LX_POWER : LX_STAR;
        break;
    case '&':
        kind = LX_AMP;
        break;
    case '=':
        kind = LX_EQ;
        break;
    case '<':
        kind = d == '>' ? LX_NE : d == '=' ? LX_LE : LX_LT;
        break;
    case '>':
        kind = d == '=' ? LX_GE : LX_GT;
        break;
    case ':':
        kind = d == '=' ? LX_ASSIGN : LX_COLON;
        break;
    case ';':
        kind = LX_SEMI;
        break;
    case ',':
        kind = LX_COMMA;
        break;
    case '.':
        kind = d == '.' ? LX_DOTDOT : LX_DOT;
        break;
    case ')':
        kind = LX_RPAREN;
        break;
    case '[':
        kind = LX_LBRACK;
        break;
    case ']':
        kind = LX_RBRACK;
        break;
    case '^':
        kind = LX_CARET;
        break;
    default:
        if (is_alpha(c)) {
            name(L);
            return;
        }
        if (is_digit(c)) {
            number(L);
            return;
        }
        {
            unsigned n;
            uint32_t cp = limba_utf8_decode(L->text + L->pos, &n);
            char what[48];
            if (cp >= 0x21 && cp < 0x7f)
                snprintf(what, sizeof(what), "'%c'", (char)cp);
            else
                snprintf(what, sizeof(what), "U+%04X", (unsigned)cp);
            L->pos += n;
            error(L, LXE_BAD_CHAR, start, n,
                  cp == '_' ? "a name starts with a letter, not with %s"
                            : "%s starts no token of Luxia",
                  what);
        }
        return;
    }
    switch (kind) {
    case LX_POWER:
    case LX_NE:
    case LX_LE:
    case LX_GE:
    case LX_ASSIGN:
    case LX_DOTDOT:
        L->pos += 2;
        break;
    default:
        L->pos++;
    }
    emit(L, kind, start, 0);
}

void limba_lx_run(limba_lx *lx, const limba_source *src, uint32_t file,
                  limba_report *rep)
{
    const limba_srcfile *f = &src->file[file];
    lexer L = {lx, rep, f->text, f->len, f->base, 0, true, NULL, 0, 0};
    size_t bad = limba_utf8_check(f->text, f->len);
    if (bad < f->len) {
        error(&L, LXE_BAD_UTF8, (uint32_t)bad, 1, "%s",
              "the file is not UTF-8 from here on");
        L.pos = f->len;
    } else if (f->len >= 3 && !memcmp(f->text, "\xef\xbb\xbf", 3)) {
        L.pos = 3;
    }
    while (L.pos < L.len)
        token(&L);
    emit(&L, LX_EOF, L.pos, 0);
    free(L.buf);
}

static void show_quoted(FILE *out, const char *s, size_t n)
{
    fputc('"', out);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"')
            fputc('"', out);
        fputc(s[i], out);
    }
    fputc('"', out);
}

void limba_lx_show(FILE *out, const limba_lx *lx, const limba_lx_token *t)
{
    size_t n;
    const char *s;
    switch (t->kind) {
    case LX_EOF:
        fputs("eof", out);
        break;
    case LX_IDENT:
        s = limba_strtab_get(lx->names, t->val, &n);
        fprintf(out, "id:%.*s", (int)n, s);
        break;
    case LX_INT:
        if (t->flags & LX_F_BIG)
            fputs("int:big", out);
        else
            fprintf(out, "int:%llu", (unsigned long long)lx->ints[t->val]);
        break;
    case LX_REAL:
        fprintf(out, "real:%.17g", lx->reals[t->val]);
        break;
    case LX_CHAR:
        fprintf(out, "char:U+%04X", (unsigned)t->val);
        break;
    case LX_STRING:
        s = limba_strtab_get(lx->strings, t->val, &n);
        fputs("str:", out);
        show_quoted(out, s, n);
        break;
    default:
        fputs(limba_lx_kind_text(t->kind), out);
    }
}

void limba_lx_dump(FILE *out, const limba_lx *lx, const limba_source *src)
{
    for (uint32_t i = 0; i < lx->ntok; i++) {
        const limba_lx_token *t = &lx->tok[i];
        limba_where w;
        if (limba_source_where(src, t->loc, &w))
            fprintf(out, "%u:%u\t", w.line, w.col);
        limba_lx_show(out, lx, t);
        fputc('\n', out);
    }
}
