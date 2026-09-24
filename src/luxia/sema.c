/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema.c - the semantic phase of Luxia 0 (see sema.h): the names of the
 * language, the lookup of names with the rules of spelling and order, the
 * declarations resolved on demand, the types, and the program.
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- messages ---- */

/* the source text at loc */
static const char *text_at(const limba_lxs *S, limba_loc loc)
{
    for (uint32_t i = 0; i < S->src->count; i++) {
        const limba_srcfile *f = &S->src->file[i];
        if (loc >= f->base && loc - f->base <= f->len)
            return f->text + (loc - f->base);
    }
    return "";
}

static uint32_t ident_len(const char *s)
{
    uint32_t n = 0;
    while ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z') ||
           (s[n] >= '0' && s[n] <= '9') || s[n] == '_')
        n++;
    return n;
}

/* the bytes a node spans for the mark under a message */
static uint32_t node_len(const limba_lxs *S, uint32_t node)
{
    const limba_lx_node *x = &S->t->node[node];
    uint32_t n = ident_len(text_at(S, x->loc));
    return n ? n : 1;
}

void lxs_error(limba_lxs *S, unsigned code, uint32_t node, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    limba_report_add(S->rep, LIMBA_ERROR, code, S->t->node[node].loc,
                     node_len(S, node), "%s", msg);
}

void lxs_note(limba_lxs *S, limba_loc loc, uint32_t len, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    limba_report_add(S->rep, LIMBA_NOTE, 0, loc, len, "%s", msg);
}

const char *lxs_name(const limba_lxs *S, uint32_t id, size_t *len)
{
    return limba_strtab_get(S->lx->names, id, len);
}

const char *lxs_spell(const limba_lxs *S, limba_sym s, size_t *len)
{
    if (s < S->nspelling && S->spelling[s]) {
        *len = strlen(S->spelling[s]);
        return S->spelling[s];
    }
    const limba_symbol *y = &S->st.sym[s];
    *len = y->len;
    return text_at(S, y->loc);
}

/* type names are symbols: printed as declared */
static const char *type_name(const void *ctx, uint32_t sym, size_t *len)
{
    return lxs_spell(ctx, sym, len);
}

const char *lxs_tname(const limba_lxs *S, limba_ltype t, char *buf)
{
    return limba_types_show(&S->ts, t, type_name, S, buf, 128);
}

/* ---- the names of the language ---- */

static limba_sym universe_sym(limba_lxs *S, const char *spelling, unsigned kind)
{
    char folded[32];
    size_t n = strlen(spelling);
    for (size_t i = 0; i < n; i++)
        folded[i] = (char)(spelling[i] >= 'A' && spelling[i] <= 'Z'
                               ? spelling[i] - 'A' + 'a'
                               : spelling[i]);
    uint32_t id = limba_strtab_intern(S->lx->names, folded, n);
    limba_sym s = limba_sym_declare(&S->st, S->universe, id, kind, LIMBA_NOLOC,
                                    (uint32_t)n, NULL);
    while (S->nspelling <= s) {
        S->spelling = limba_xrealloc(S->spelling, s + 16, sizeof(char *));
        while (S->nspelling < s + 16)
            S->spelling[S->nspelling++] = NULL;
    }
    S->spelling[s] = spelling;
    return s;
}

static void universe_type(limba_lxs *S, const char *spelling, limba_ltype t)
{
    limba_sym s = universe_sym(S, spelling, LIMBA_SYM_TYPE);
    S->st.sym[s].type = t;
    limba_types_set_name(&S->ts, t, s);
}

static const struct {
    const char *name;
    unsigned id;
} builtins[] = {
    {"write", LXB_WRITE},
    {"writeln", LXB_WRITELN},
    {"writebyte", LXB_WRITEBYTE},
    {"readline", LXB_READLINE},
    {"length", LXB_LENGTH},
    {"low", LXB_LOW},
    {"high", LXB_HIGH},
    {"copy", LXB_COPY},
    {"chr", LXB_CHR},
    {"ord", LXB_ORD},
    {"succ", LXB_SUCC},
    {"pred", LXB_PRED},
    {"str", LXB_STR},
    {"val", LXB_VAL},
    {"sqrt", LXB_SQRT},
    {"sin", LXB_SIN},
    {"cos", LXB_COS},
    {"tan", LXB_TAN},
    {"arctan", LXB_ARCTAN},
    {"exp", LXB_EXP},
    {"ln", LXB_LN},
    {"trunc", LXB_TRUNC},
    {"round", LXB_ROUND},
    {"floor", LXB_FLOOR},
    {"ceil", LXB_CEIL},
    {"dispose", LXB_DISPOSE},
    {"argcount", LXB_ARGCOUNT},
    {"arg", LXB_ARG},
    {"halt", LXB_HALT},
};

static void universe(limba_lxs *S)
{
    static const char *const ints[] = {"Int8", "Int16", "Int32", "Int64"};
    static const char *const uints[] = {"UInt8", "UInt16", "UInt32", "UInt64"};
    static const char *const bits[] = {"Bits8", "Bits16", "Bits32", "Bits64"};
    S->universe = limba_scope_new(&S->st, 0, LXS_UNIVERSE);
    for (int i = 0; i < 4; i++) {
        unsigned w = 8u << i;
        S->ty_int[i] = limba_types_int(&S->ts, w, LIMBA_TF_SIGNED);
        S->ty_uint[i] = limba_types_int(&S->ts, w, 0);
        S->ty_bits[i] = limba_types_int(&S->ts, w, LIMBA_TF_MODULAR);
        universe_type(S, ints[i], S->ty_int[i]);
        universe_type(S, uints[i], S->ty_uint[i]);
        universe_type(S, bits[i], S->ty_bits[i]);
    }
    /* Byte is Bits8 itself, not a copy */
    limba_sym byte = universe_sym(S, "Byte", LIMBA_SYM_TYPE);
    S->st.sym[byte].type = S->ty_bits[0];
    S->ty_f32 = limba_types_float(&S->ts, 32);
    S->ty_f64 = limba_types_float(&S->ts, 64);
    universe_type(S, "Float32", S->ty_f32);
    universe_type(S, "Float64", S->ty_f64);
    universe_type(S, "Boolean", S->ts.bool_);
    universe_type(S, "Char", S->ts.char_);
    universe_type(S, "String", S->ts.string);
    static const struct {
        const char *name;
        int code;
    } chars[] = {{"LF", 10}, {"CR", 13}, {"TAB", 9}, {"NUL", 0}};
    for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
        limba_sym s = universe_sym(S, chars[i].name, LIMBA_SYM_CONST);
        S->st.sym[s].type = S->ts.char_;
        S->st.sym[s].value = lxs_value_int(S, chars[i].code);
    }
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        limba_sym s = universe_sym(S, builtins[i].name, LIMBA_SYM_BUILTIN);
        S->st.sym[s].value = builtins[i].id;
    }
}

void limba_lxs_init(limba_lxs *S, limba_lx_ast *t, limba_lx *lx,
                    const limba_source *src, limba_report *rep)
{
    memset(S, 0, sizeof(*S));
    S->t = t;
    S->lx = lx;
    S->src = src;
    S->rep = rep;
    limba_types_init(&S->ts);
    limba_symtab_init(&S->st);
    uint32_t n = t->nnode ? t->nnode : 1;
    S->type = limba_xcalloc(n, sizeof(*S->type));
    S->sym = limba_xcalloc(n, sizeof(*S->sym));
    S->val = limba_xcalloc(n, sizeof(*S->val));
    lxs_value_new(S, 0); /* value 0: none */
    universe(S);
}

void limba_lxs_free(limba_lxs *S)
{
    for (uint32_t i = 0; i < S->nv; i++)
        limba_rat_free(&S->v[i].num);
    free(S->v);
    free(S->type);
    free(S->sym);
    free(S->val);
    free(S->spelling);
    limba_types_free(&S->ts);
    limba_symtab_free(&S->st);
    memset(S, 0, sizeof(*S));
}

/* ---- names ---- */

limba_sym lxs_lookup(limba_lxs *S, uint32_t scope, uint32_t node)
{
    limba_lx_node *x = lxs_node(S, node);
    limba_sym s = limba_sym_lookup(&S->st, scope, x->a);
    const char *use = text_at(S, x->loc);
    uint32_t n = ident_len(use);
    if (!s) {
        lxs_error(S, LXE_UNKNOWN_NAME, node, "'%.*s' is not declared", (int)n,
                  use);
        return 0;
    }
    S->sym[node] = s;
    size_t dn;
    const char *decl = lxs_spell(S, s, &dn);
    if (dn == n && memcmp(decl, use, n) != 0) {
        /* often a second name that collides with the first one: using
           the first would only echo the mistake */
        lxs_error(S, LXE_SPELLING, node,
                  "'%.*s' is declared as '%.*s': write it the same way", (int)n,
                  use, (int)dn, decl);
        S->sym[node] = 0;
        return 0;
    }
    limba_symbol *y = &S->st.sym[s];
    if ((y->kind == LIMBA_SYM_CONST || y->kind == LIMBA_SYM_VAR) &&
        y->loc != LIMBA_NOLOC && y->loc > x->loc && y->node &&
        lxs_node(S, y->node)->kind != LXN_TYPEDECL) {
        lxs_error(S, LXE_BEFORE_DECL, node,
                  "'%.*s' is used before its declaration", (int)n, use);
        lxs_note(S, y->loc, y->len, "declared here");
    }
    return s;
}

limba_sym lxs_declare(limba_lxs *S, uint32_t scope, uint32_t name_node,
                      unsigned kind, uint32_t decl)
{
    limba_lx_node *x = lxs_node(S, name_node);
    if (x->kind != LXN_NAME)
        return 0; /* a parse error already reported */
    const char *text = text_at(S, x->loc);
    uint32_t n = ident_len(text);
    limba_sym dup = limba_sym_local(&S->st, S->universe, x->a);
    if (dup) {
        lxs_error(S, LXE_DUPLICATE, name_node,
                  "'%.*s' is a name of the language", (int)n, text);
        return 0;
    }
    limba_sym s = limba_sym_declare(&S->st, scope, x->a, kind, x->loc, n, &dup);
    if (!s) {
        lxs_error(S, LXE_DUPLICATE, name_node,
                  "'%.*s' is already declared here", (int)n, text);
        lxs_note(S, S->st.sym[dup].loc, S->st.sym[dup].len,
                 "the first declaration");
        return 0;
    }
    S->st.sym[s].node = decl;
    S->sym[name_node] = s;
    return s;
}

/* ---- declarations, resolved on demand ---- */

static void mark(limba_lxs *S, limba_sym s, bool top)
{
    if (s)
        S->st.sym[s].flags |= LXS_UNRESOLVED | (top ? LXS_TOP : 0);
}

void lxs_declare_all(limba_lxs *S, uint32_t list, uint32_t scope, bool top)
{
    limba_lx_node *l = lxs_node(S, list);
    for (uint32_t i = 0; i < l->b; i++) {
        uint32_t d = limba_lx_list_at(S->t, list, i);
        limba_lx_node *x = lxs_node(S, d);
        switch (x->kind) {
        case LXN_CONST:
            mark(S, lxs_declare(S, scope, x->a, LIMBA_SYM_CONST, d), top);
            break;
        case LXN_TYPEDECL: {
            mark(S, lxs_declare(S, scope, x->a, LIMBA_SYM_TYPE, d), top);
            limba_lx_node *tn = lxs_node(S, x->b);
            if (tn->kind == LXN_TENUM) {
                limba_lx_node *vals = lxs_node(S, tn->a);
                for (uint32_t k = 0; k < vals->b; k++)
                    mark(S,
                         lxs_declare(S, scope, limba_lx_list_at(S->t, tn->a, k),
                                     LIMBA_SYM_CONST, d),
                         top);
            }
            break;
        }
        case LXN_VAR: {
            limba_lx_node *names = lxs_node(S, x->a);
            for (uint32_t k = 0; k < names->b; k++)
                mark(S,
                     lxs_declare(S, scope, limba_lx_list_at(S->t, x->a, k),
                                 LIMBA_SYM_VAR, d),
                     top);
            break;
        }
        case LXN_ROUTINE:
            mark(S, lxs_declare(S, scope, x->a, LIMBA_SYM_ROUTINE, d), top);
            break;
        }
    }
}

static void resolve_const(limba_lxs *S, limba_sym s)
{
    limba_symbol *y = &S->st.sym[s];
    uint32_t scope = y->scope, d = y->node;
    limba_lx_node *x = lxs_node(S, d);
    limba_ltype t = x->b ? lxs_type(S, x->b, scope, 0) : 0;
    uint32_t errors = S->rep->errors;
    limba_ltype vt = lxs_expr(S, x->c, scope, t);
    if (!S->val[x->c]) {
        if (vt && S->rep->errors == errors)
            lxs_error(S, LXE_NOT_CONSTANT, x->c,
                      "a constant needs a value known at compile time: "
                      "for a computed one declare a var");
        y = &S->st.sym[s];
        y->type = t ? t : 0;
        return;
    }
    if (t) {
        lxs_assign_to(S, x->c, t, "the constant");
        vt = t;
    }
    y = &S->st.sym[s];
    y->type = vt;
    y->value = S->val[x->c];
}

/* the names of a var declaration, together */
static void resolve_var(limba_lxs *S, uint32_t d, uint32_t scope, bool top)
{
    limba_lx_node *x = lxs_node(S, d);
    limba_ltype t = x->b ? lxs_type(S, x->b, scope, top ? 0 : LXT_VAR) : 0;
    if (x->c) {
        limba_ltype vt = lxs_expr(S, x->c, scope, t);
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
    limba_lx_node *names = lxs_node(S, x->a);
    for (uint32_t k = 0; k < names->b; k++) {
        limba_sym s = S->sym[limba_lx_list_at(S->t, x->a, k)];
        if (s) {
            S->st.sym[s].type = t;
            S->st.sym[s].flags &= (uint16_t)~(LXS_UNRESOLVED | LXS_RESOLVING);
        }
    }
}

static limba_ltype type_node(limba_lxs *S, uint32_t node, uint32_t scope,
                             unsigned where, limba_sym self);

static void resolve_typedecl(limba_lxs *S, limba_sym s)
{
    limba_symbol *y = &S->st.sym[s];
    limba_lx_node *x = lxs_node(S, y->node);
    limba_ltype t = type_node(S, x->b, y->scope, 0, s);
    y = &S->st.sym[s];
    y->type = t;
    limba_types_set_name(&S->ts, t, s);
}

static void resolve_routine(limba_lxs *S, limba_sym s)
{
    limba_symbol *y = &S->st.sym[s];
    uint32_t outer = y->scope, d = y->node;
    uint32_t scope = limba_scope_new(&S->st, outer, LXS_ROUTINE);
    S->st.sym[s].value = scope;
    limba_lx_node *x = lxs_node(S, d);
    uint32_t plist = x->b;
    limba_param *ps = NULL;
    uint32_t np = 0, cap = 0;
    for (uint32_t i = 0; i < lxs_node(S, plist)->b; i++) {
        uint32_t p = limba_lx_list_at(S->t, plist, i);
        limba_lx_node *px = lxs_node(S, p);
        unsigned mode = px->op == LX_KW_VAR   ? LXS_VAR
                        : px->op == LX_KW_OUT ? LXS_OUT
                                              : LXS_IN;
        limba_ltype pt = lxs_type(S, px->b, outer, LXT_PARAM);
        uint32_t names = px->a;
        for (uint32_t k = 0; k < lxs_node(S, names)->b; k++) {
            uint32_t nn = limba_lx_list_at(S->t, names, k);
            limba_sym ps_ = lxs_declare(S, scope, nn, LIMBA_SYM_PARAM, p);
            if (ps_) {
                S->st.sym[ps_].type = pt;
                S->st.sym[ps_].mode = (uint8_t)mode;
            }
            LIMBA_GROW(ps, np, cap);
            ps[np++] = (limba_param){pt, (uint8_t)mode};
        }
    }
    x = lxs_node(S, d);
    limba_ltype result = S->ts.void_;
    if (x->c) {
        result = lxs_type(S, x->c, outer, 0);
    }
    S->st.sym[s].type = limba_types_routine(&S->ts, ps, np, result);
    free(ps);
}

void lxs_force(limba_lxs *S, limba_sym s)
{
    limba_symbol *y = &S->st.sym[s];
    if (!(y->flags & LXS_UNRESOLVED))
        return;
    if (y->flags & LXS_RESOLVING) {
        if (y->type)
            return; /* a record or a pointer being built: usable */
        size_t n;
        const char *sp = lxs_spell(S, s, &n);
        limba_report_add(S->rep, LIMBA_ERROR, LXE_SELF_REFERENCE, y->loc,
                         y->len, "'%.*s' is defined in terms of itself", (int)n,
                         sp);
        y->flags &= (uint16_t)~(LXS_UNRESOLVED | LXS_RESOLVING);
        return;
    }
    y->flags |= LXS_RESOLVING;
    unsigned dk = y->node ? lxs_node(S, y->node)->kind : LXN_NONE;
    switch (dk) {
    case LXN_CONST:
        resolve_const(S, s);
        break;
    case LXN_TYPEDECL:
        if (y->kind == LIMBA_SYM_TYPE) {
            resolve_typedecl(S, s);
        } else {
            /* a value of an enumeration: resolving the type sets it */
            limba_lx_node *x = lxs_node(S, y->node);
            limba_sym ts = S->sym[x->a];
            if (ts)
                lxs_force(S, ts);
        }
        break;
    case LXN_VAR:
        resolve_var(S, y->node, y->scope, (y->flags & LXS_TOP) != 0);
        break;
    case LXN_ROUTINE:
        resolve_routine(S, s);
        break;
    }
    S->st.sym[s].flags &= (uint16_t)~(LXS_UNRESOLVED | LXS_RESOLVING);
}

void lxs_resolve_all(limba_lxs *S, uint32_t list)
{
    limba_lx_node *l = lxs_node(S, list);
    for (uint32_t i = 0; i < l->b; i++) {
        uint32_t d = limba_lx_list_at(S->t, list, i);
        limba_lx_node *x = lxs_node(S, d);
        uint32_t name =
            x->kind == LXN_VAR
                ? (lxs_node(S, x->a)->b ? limba_lx_list_at(S->t, x->a, 0) : 0)
                : x->a;
        if (name && S->sym[name])
            lxs_force(S, S->sym[name]);
    }
}

/* ---- types ---- */

limba_ltype lxs_base(const limba_lxs *S, limba_ltype t)
{
    while (S->ts.t[t].flags & LIMBA_TF_RANGE)
        t = S->ts.t[t].base;
    return t;
}

bool lxs_compatible(const limba_lxs *S, limba_ltype a, limba_ltype b)
{
    if (a == 0 || b == 0)
        return true; /* an error already reported */
    const limba_typeinfo *x = &S->ts.t[a], *y = &S->ts.t[b];
    if (x->root == y->root)
        return true;
    return limba_types_same(&S->ts, lxs_base(S, a), lxs_base(S, b));
}

/* a bound of a range: a constant of the base; 0 if not constant */
static uint32_t bound(limba_lxs *S, uint32_t node, uint32_t scope,
                      limba_ltype base, bool *ok)
{
    lxs_expr(S, node, scope, base);
    if (!S->val[node]) {
        *ok = false;
        return 0;
    }
    lxs_assign_to(S, node, base, "the bound");
    return S->val[node];
}

/* Name range lo..hi: a range type, or 0 for computed bounds (dyn set) */
static limba_ltype named(limba_lxs *S, uint32_t node, uint32_t scope,
                         bool allow_dynamic, bool *dynamic)
{
    limba_lx_node *x = lxs_node(S, node);
    limba_sym s = lxs_lookup(S, scope, node);
    if (!s)
        return 0;
    limba_symbol *y = &S->st.sym[s];
    if (y->kind != LIMBA_SYM_TYPE) {
        size_t n;
        const char *sp = lxs_spell(S, s, &n);
        lxs_error(S, LXE_NOT_A_TYPE, node, "'%.*s' is not a type", (int)n, sp);
        return 0;
    }
    lxs_force(S, s);
    limba_ltype t = S->st.sym[s].type;
    if (!x->b || !t)
        return t;
    if (!limba_types_is_discrete(&S->ts, t)) {
        char tb[128];
        lxs_error(S, LXE_BAD_RANGE, node,
                  "%s has no range: only discrete "
                  "types do",
                  lxs_tname(S, t, tb));
        return 0;
    }
    bool ok = true;
    limba_ltype base = lxs_base(S, t);
    uint32_t lo = bound(S, x->b, scope, base, &ok);
    uint32_t hi = bound(S, x->c, scope, base, &ok);
    if (!ok) {
        if (allow_dynamic) {
            *dynamic = true;
            return base;
        }
        lxs_error(S, LXE_DYNAMIC_PLACE, node,
                  "bounds computed at run time are allowed only in the "
                  "index of the array of a variable");
        return 0;
    }
    __int128 l, h;
    if (!lxs_value_to_int(S, lo, &l) || !lxs_value_to_int(S, hi, &h))
        return 0;
    if (l > h) {
        lxs_error(S, LXE_BAD_RANGE, node, "the range is empty: low > high");
        return 0;
    }
    const limba_typeinfo *ti = lxs_ty(S, t);
    if (l < ti->lo || h > ti->hi) {
        char tb[128];
        lxs_error(S, LXE_CONST_RANGE, node, "the range is past %s",
                  lxs_tname(S, t, tb));
        return 0;
    }
    return limba_types_range(&S->ts, t, l, h);
}

static limba_ltype type_node(limba_lxs *S, uint32_t node, uint32_t scope,
                             unsigned where, limba_sym self)
{
    limba_lx_node *x = lxs_node(S, node);
    bool dyn = false;
    char tb[128];
    switch (x->kind) {
    case LXN_TNAME: {
        limba_ltype t = named(S, node, scope, false, &dyn);
        return t;
    }
    case LXN_TNEW: {
        limba_ltype b = type_node(S, x->a, scope, 0, 0);
        if (!b)
            return 0;
        unsigned k = lxs_ty(S, b)->kind;
        if (k == LIMBA_LTK_OPEN || k == LIMBA_LTK_ROUTINE) {
            lxs_error(S, LXE_TYPE_MISMATCH, node,
                      "new makes a type from a named or scalar type");
            return 0;
        }
        return limba_types_distinct(&S->ts, b);
    }
    case LXN_TENUM: {
        uint32_t list = x->a, count = lxs_node(S, list)->b;
        limba_ltype e = limba_types_enum(&S->ts, count);
        for (uint32_t k = 0; k < count; k++) {
            uint32_t nn = limba_lx_list_at(S->t, list, k);
            limba_sym vs = S->sym[nn];
            if (!vs)
                vs = lxs_declare(S, scope, nn, LIMBA_SYM_CONST, 0);
            if (vs) {
                S->st.sym[vs].type = e;
                S->st.sym[vs].value = lxs_value_int(S, k);
                S->st.sym[vs].flags &=
                    (uint16_t)~(LXS_UNRESOLVED | LXS_RESOLVING);
            }
        }
        return e;
    }
    case LXN_TARRAY: {
        uint32_t in = x->a;
        limba_ltype index;
        if (lxs_node(S, in)->kind == LXN_TNAME)
            index = named(S, in, scope, (where & LXT_VAR) != 0, &dyn);
        else
            index = type_node(S, in, scope, 0, 0);
        limba_ltype elem = type_node(S, lxs_node(S, node)->b, scope, 0, 0);
        if (!index || !elem)
            return 0;
        if (!limba_types_is_discrete(&S->ts, index)) {
            lxs_error(S, LXE_TYPE_MISMATCH, in,
                      "an array is indexed by a discrete type, not %s",
                      lxs_tname(S, index, tb));
            return 0;
        }
        const limba_typeinfo *et = lxs_ty(S, elem);
        if (et->kind == LIMBA_LTK_OPEN ||
            (et->flags & (LIMBA_TF_DYNAMIC | LIMBA_TF_INCOMPLETE))) {
            lxs_error(S, LXE_TYPE_MISMATCH, lxs_node(S, node)->b,
                      "the elements of an array have a size known in "
                      "advance");
            return 0;
        }
        bool ok;
        limba_ltype a = limba_types_array(&S->ts, index, elem, dyn, &ok);
        if (!ok)
            lxs_error(S, LXE_CONST_RANGE, node,
                      "the array is larger than the memory can address");
        return a;
    }
    case LXN_TOPEN: {
        if (!(where & LXT_PARAM)) {
            lxs_error(S, LXE_OPEN_ARRAY_PLACE, node,
                      "an array of any length is a type for parameters "
                      "only");
            return 0;
        }
        limba_ltype elem = type_node(S, x->a, scope, 0, 0);
        return elem ? limba_types_open(&S->ts, elem) : 0;
    }
    case LXN_TRECORD: {
        limba_ltype r = limba_types_record_begin(&S->ts);
        if (self)
            S->st.sym[self].type = r;
        uint32_t list = x->a;
        uint32_t first = S->ts.nfield;
        for (uint32_t i = 0; i < lxs_node(S, list)->b; i++) {
            uint32_t f = limba_lx_list_at(S->t, list, i);
            limba_lx_node *fx = lxs_node(S, f);
            uint32_t names = fx->a;
            limba_ltype ft = type_node(S, fx->b, scope, 0, 0);
            if (ft && (lxs_ty(S, ft)->flags & LIMBA_TF_INCOMPLETE)) {
                lxs_error(S, LXE_SELF_REFERENCE, fx->b,
                          "a record cannot contain itself: use a pointer");
                ft = 0;
            }
            for (uint32_t k = 0; k < lxs_node(S, names)->b; k++) {
                uint32_t nn = limba_lx_list_at(S->t, names, k);
                limba_lx_node *nx = lxs_node(S, nn);
                if (nx->kind != LXN_NAME)
                    continue;
                bool dup = false;
                for (uint32_t j = first; j < S->ts.nfield; j++)
                    if (S->ts.field[j].name == nx->a)
                        dup = true;
                if (dup) {
                    lxs_error(S, LXE_DUPLICATE, nn,
                              "a second field with this name");
                    continue;
                }
                limba_types_record_field(&S->ts, r, nx->a, ft);
            }
        }
        if (!limba_types_record_end(&S->ts, r))
            lxs_error(S, LXE_CONST_RANGE, node,
                      "the record is larger than the memory can address");
        return r;
    }
    case LXN_TPTR: {
        limba_ltype p = limba_types_pointer(&S->ts, 0);
        if (self)
            S->st.sym[self].type = p;
        limba_ltype target = type_node(S, x->a, scope, 0, 0);
        limba_types_set_target(&S->ts, p, target);
        return p;
    }
    }
    return 0;
}

limba_ltype lxs_type(limba_lxs *S, uint32_t node, uint32_t scope,
                     unsigned where)
{
    return type_node(S, node, scope, where, 0);
}

/* ---- the program ---- */

void limba_lxs_check(limba_lxs *S)
{
    limba_lx_node *p = lxs_node(S, S->t->root);
    if (p->kind != LXN_PROGRAM)
        return;
    uint32_t decls = p->b, body = p->c;
    S->program = limba_scope_new(&S->st, S->universe, LXS_PROGRAM);
    lxs_declare_all(S, decls, S->program, true);
    lxs_resolve_all(S, decls);
    for (uint32_t i = 0; i < lxs_node(S, decls)->b; i++) {
        uint32_t d = limba_lx_list_at(S->t, decls, i);
        limba_lx_node *x = lxs_node(S, d);
        if (x->kind == LXN_ROUTINE && S->sym[x->a])
            lxs_routine_body(S, S->sym[x->a]);
    }
    S->result = 0;
    S->in_routine = false;
    S->loops = 0;
    lxs_stmts(S, body, limba_scope_new(&S->st, S->program, LXS_BLOCK));
}
