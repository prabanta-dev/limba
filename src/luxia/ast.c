/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ast.c - the Luxia syntax tree (see ast.h).
 */
#include "ast.h"

#include "common/xalloc.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *text;
    const char *fields;
} node_info[] = {
#define LX_NODE(name, text, fields) {text, fields},
#include "nodes.def"
#undef LX_NODE
};

const char *limba_lx_node_text(unsigned kind)
{
    return kind < LXN_NKINDS ? node_info[kind].text : "?";
}

void limba_lx_ast_init(limba_lx_ast *t)
{
    memset(t, 0, sizeof(*t));
    limba_lx_node_new(t, LXN_NONE, LIMBA_NOLOC, 0, 0, 0, 0);
}

void limba_lx_ast_free(limba_lx_ast *t)
{
    free(t->node);
    free(t->pool);
    free(t->stack);
    memset(t, 0, sizeof(*t));
}

uint32_t limba_lx_node_new(limba_lx_ast *t, unsigned kind, limba_loc loc,
                           uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    LIMBA_GROW(t->node, t->nnode, t->capnode);
    t->node[t->nnode] = (limba_lx_node){(uint8_t)kind, 0, 0, loc, a, b, c, d};
    return t->nnode++;
}

uint32_t limba_lx_list_begin(const limba_lx_ast *t)
{
    return t->nstack;
}

void limba_lx_list_push(limba_lx_ast *t, uint32_t node)
{
    LIMBA_GROW(t->stack, t->nstack, t->capstack);
    t->stack[t->nstack++] = node;
}

uint32_t limba_lx_list_end(limba_lx_ast *t, uint32_t mark, limba_loc loc)
{
    uint32_t n = t->nstack - mark;
    while (t->npool + n > t->cappool) {
        t->cappool = t->cappool ? 2 * t->cappool : 256;
        t->pool = limba_xrealloc(t->pool, t->cappool, sizeof(*t->pool));
    }
    uint32_t start = t->npool;
    if (n)
        memcpy(t->pool + start, t->stack + mark, n * sizeof(*t->pool));
    t->npool += n;
    t->nstack = mark;
    return limba_lx_node_new(t, LXN_LIST, loc, start, n, 0, 0);
}

/* a node printed as a single word */
static bool is_leaf(const limba_lx_ast *t, uint32_t n)
{
    unsigned k = t->node[n].kind;
    return n == 0 || k == LXN_NAME || k == LXN_REF || k == LXN_INT ||
           k == LXN_REAL || k == LXN_CHAR || k == LXN_STRING || k == LXN_BOOL ||
           k == LXN_NIL || k == LXN_ERROR ||
           (k == LXN_LIST && t->node[n].b == 0);
}

static void show_leaf(FILE *out, const limba_lx_ast *t, const limba_lx *lx,
                      uint32_t n)
{
    const limba_lx_node *x = &t->node[n];
    size_t len;
    const char *s;
    switch (n ? x->kind : LXN_NONE) {
    case LXN_NONE:
        fputc('-', out);
        break;
    case LXN_NAME:
    case LXN_REF:
        s = limba_strtab_get(lx->names, x->a, &len);
        fwrite(s, 1, len, out);
        break;
    case LXN_INT:
        if (x->flags & LXN_F_BIG)
            fputs("big", out);
        else
            fprintf(out, "%llu", (unsigned long long)lx->ints[x->a]);
        break;
    case LXN_REAL:
        fprintf(out, "%.17g", lx->reals[x->a]);
        break;
    case LXN_CHAR:
        if (x->a > 0x20 && x->a < 0x7f && x->a != '\'')
            fprintf(out, "'%c'", (char)x->a);
        else
            fprintf(out, "U+%04X", (unsigned)x->a);
        break;
    case LXN_STRING:
        s = limba_strtab_get(lx->strings, x->a, &len);
        fputc('"', out);
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '"')
                fputc('"', out);
            fputc(s[i], out);
        }
        fputc('"', out);
        break;
    case LXN_BOOL:
        fputs(x->a ? "true" : "false", out);
        break;
    case LXN_NIL:
        fputs("nil", out);
        break;
    case LXN_ERROR:
        fputs("error", out);
        break;
    case LXN_LIST:
        fputs("[]", out);
        break;
    }
}

static void newline(FILE *out, int indent)
{
    fputc('\n', out);
    for (int i = 0; i < indent; i++)
        fputc(' ', out);
}

void limba_lx_ast_show(FILE *out, const limba_lx_ast *t, const limba_lx *lx,
                       uint32_t n, int indent)
{
    if (is_leaf(t, n)) {
        show_leaf(out, t, lx, n);
        return;
    }
    /* a subtree that fits in the line goes on one line */
    if (indent >= 0) {
        char *flat = NULL;
        size_t len = 0;
        FILE *m = open_memstream(&flat, &len);
        if (m) {
            limba_lx_ast_show(m, t, lx, n, -1);
            fclose(m);
            bool fits = (size_t)indent + len <= 78;
            if (fits)
                fwrite(flat, 1, len, out);
            free(flat);
            if (fits)
                return;
        }
    }
    const limba_lx_node *x = &t->node[n];
    int inner = indent < 0 ? -1 : indent + 2;
    if (x->kind == LXN_LIST) {
        fputc('[', out);
        for (uint32_t i = 0; i < x->b; i++) {
            if (inner >= 0)
                newline(out, inner);
            else if (i)
                fputc(' ', out);
            limba_lx_ast_show(out, t, lx, limba_lx_list_at(t, n, i), inner);
        }
        fputc(']', out);
        return;
    }
    const char *f = node_info[x->kind].fields;
    uint32_t v[4] = {x->a, x->b, x->c, x->d};
    int used = 4;
    while (used > 0 && f[used - 1] == '-')
        used--;
    /* one line when every child is a single word */
    bool flat = indent < 0;
    if (!flat) {
        flat = true;
        for (int i = 0; i < used; i++)
            if ((f[i] == 'n' || f[i] == 'l') && !is_leaf(t, v[i]))
                flat = false;
    }
    fprintf(out, "(%s", node_info[x->kind].text);
    if (x->op)
        fprintf(out, " %s", limba_lx_kind_text(x->op));
    for (int i = 0; i < used; i++) {
        if (flat)
            fputc(' ', out);
        else
            newline(out, inner);
        switch (f[i]) {
        case 'n':
        case 'l':
            limba_lx_ast_show(out, t, lx, v[i], flat ? -1 : inner);
            break;
        case 'i': {
            size_t len;
            const char *s = limba_strtab_get(lx->names, v[i], &len);
            fwrite(s, 1, len, out);
            break;
        }
        default:
            fprintf(out, "%u", v[i]);
        }
    }
    fputc(')', out);
}
