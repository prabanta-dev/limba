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

const char *lxs_text_at(const limba_lxs *S, limba_loc loc)
{
    return text_at(S, loc);
}

uint32_t lxs_ident_len(const char *s)
{
    return ident_len(s);
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
    limba_sym s = universe_sym(S, spelling, LIMBA_LSYM_TYPE);
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
    {"move", LXB_MOVE},
    {"newcstring", LXB_NEWCSTRING},
    {"cvalue", LXB_CVALUE},
    {"freecstring", LXB_FREECSTRING},
    {"translate", LXB_TRANSLATE},
    {"reverse", LXB_REVERSE},
    {"occurrences", LXB_OCCURRENCES},
    {"readbytes", LXB_READBYTES},
    {"writebytes", LXB_WRITEBYTES},
};

/* the C types by name, in the order of csym (§ 3.13) */
static const char *const cnames[LXS_NCTYPES] = {
    "CChar",   "CSChar", "CUChar", "CShort",    "CUShort",    "CInt",
    "CUInt",   "CLong",  "CULong", "CLongLong", "CULongLong", "CSizeT",
    "CSSizeT", "CBool",  "CFloat", "CDouble"};

static const char *const target_names[] = {"x86_64-linux", "aarch64-linux",
                                           "x86_64-windows"};

const char *lxs_target_name(unsigned target)
{
    return target < 3 ? target_names[target] : "?";
}

int lxs_target_find(const char *name, size_t len)
{
    for (int i = 0; i < 3; i++)
        if (strlen(target_names[i]) == len &&
            !memcmp(target_names[i], name, len))
            return i;
    return -1;
}

/* the C types by name for the platform (limba --target): distinct
   types, the representation of its C */
static void c_types(limba_lxs *S)
{
    bool win = S->target == LXS_X86_64_WINDOWS;
    bool arm = S->target == LXS_AARCH64_LINUX;
    limba_ltype base[LXS_NCTYPES] = {
        arm ? S->ty_uint[0] : S->ty_int[0], /* char */
        S->ty_int[0],
        S->ty_uint[0],
        S->ty_int[1],
        S->ty_uint[1],
        S->ty_int[2],
        S->ty_uint[2],
        win ? S->ty_int[2] : S->ty_int[3],   /* long */
        win ? S->ty_uint[2] : S->ty_uint[3], /* unsigned long */
        S->ty_int[3],
        S->ty_uint[3],
        S->ty_uint[3],
        S->ty_int[3],
        S->ts.bool_,
        S->ty_f32,
        S->ty_f64};
    for (int i = 0; i < LXS_NCTYPES; i++) {
        limba_ltype t = limba_types_distinct(&S->ts, base[i]);
        S->st.sym[S->csym[i]].type = t;
        limba_types_set_name(&S->ts, t, S->csym[i]);
    }
    S->ty_cbool = S->st.sym[S->csym[13]].type;
}

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
    limba_sym byte = universe_sym(S, "Byte", LIMBA_LSYM_TYPE);
    S->st.sym[byte].type = S->ty_bits[0];
    S->ty_f32 = limba_types_float(&S->ts, 32);
    S->ty_f64 = limba_types_float(&S->ts, 64);
    universe_type(S, "Float32", S->ty_f32);
    universe_type(S, "Float64", S->ty_f64);
    universe_type(S, "Boolean", S->ts.bool_);
    universe_type(S, "Char", S->ts.char_);
    universe_type(S, "String", S->ts.string);
    universe_type(S, "BigInt", S->ts.bigint);
    /* the boundary with C: the opaque pointer, the C strings, the C types
       by name (their types made by c_types, for the platform) */
    S->ty_cpointer = limba_types_opaque(&S->ts);
    S->ty_cstring = limba_types_distinct(&S->ts, S->ty_cpointer);
    const char *const opaque[2] = {"CPointer", "CString"};
    for (int i = 0; i < 2; i++) {
        limba_sym s = universe_sym(S, opaque[i], LIMBA_LSYM_TYPE);
        limba_ltype t = i ? S->ty_cstring : S->ty_cpointer;
        S->st.sym[s].type = t;
        S->st.sym[s].flags |= LXS_CBORDER;
        limba_types_set_name(&S->ts, t, s);
    }
    for (int i = 0; i < LXS_NCTYPES; i++) {
        S->csym[i] = universe_sym(S, cnames[i], LIMBA_LSYM_TYPE);
        S->st.sym[S->csym[i]].flags |= LXS_CBORDER | LXS_CPLATFORM;
    }
    static const struct {
        const char *name;
        int code;
    } chars[] = {{"LF", 10}, {"CR", 13}, {"TAB", 9}, {"NUL", 0}};
    for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
        limba_sym s = universe_sym(S, chars[i].name, LIMBA_LSYM_CONST);
        S->st.sym[s].type = S->ts.char_;
        S->st.sym[s].value = lxs_value_int(S, chars[i].code);
    }
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        limba_sym s = universe_sym(S, builtins[i].name, LIMBA_LSYM_BUILTIN);
        S->st.sym[s].value = builtins[i].id;
        if (builtins[i].id >= LXB_NEWCSTRING)
            S->st.sym[s].flags |= LXS_CBORDER;
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
#if defined(__aarch64__) && !defined(_WIN32)
    S->target = LXS_AARCH64_LINUX;
#elif defined(_WIN64)
    S->target = LXS_X86_64_WINDOWS;
#else
    S->target = LXS_X86_64_LINUX;
#endif
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
    free(S->tconsts);
    lxs_unit_free(S);
    limba_types_free(&S->ts);
    limba_symtab_free(&S->st);
    memset(S, 0, sizeof(*S));
}

/* ---- names ---- */

/* the unit that qualifies a name: Unit.Name in an expression (a REF
   rewritten, b the unit + 1) or in a type (TNAME d, TBOX b: a REF of the
   unit); UINT32_MAX if none, UINT32_MAX - 1 if not a unit (reported) */
static uint32_t qualifier(limba_lxs *S, uint32_t scope, uint32_t node)
{
    limba_lx_node *x = lxs_node(S, node);
    if (x->kind == LXN_REF && (x->flags & LXN_F_QUAL))
        return x->b - 1;
    uint32_t q = x->kind == LXN_TNAME ? x->d : x->kind == LXN_TBOX ? x->b : 0;
    if (!q)
        return UINT32_MAX;
    uint32_t name = lxs_node(S, q)->a;
    bool impl;
    uint32_t u = lxs_unit_of(S, scope, &impl);
    if (S->units[u].name == name)
        return u;
    for (uint32_t v = 0; v < S->nunits; v++)
        if (S->units[v].name == name && v != u)
            return v; /* visibility is checked by lxs_qualified */
    const char *use = text_at(S, lxs_node(S, q)->loc);
    lxs_error(S, LXE_UNKNOWN_NAME, q, "'%.*s' is not a unit used here",
              (int)ident_len(use), use);
    return UINT32_MAX - 1;
}

limba_sym lxs_lookup(limba_lxs *S, uint32_t scope, uint32_t node)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t q = S->nunits ? qualifier(S, scope, node) : UINT32_MAX;
    if (q == UINT32_MAX - 1)
        return 0;
    limba_sym s = q != UINT32_MAX ? lxs_qualified(S, scope, q, x->a, node)
                                  : lxs_find(S, scope, x->a, node);
    x = lxs_node(S, node);
    const char *use = text_at(S, x->loc);
    uint32_t n = ident_len(use);
    if (!s && q != UINT32_MAX)
        return 0; /* reported */
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
    bool impl;
    /* before its declaration: in the same file (another file's names
       are all visible, § 11.3) */
    if ((y->kind == LIMBA_LSYM_CONST || y->kind == LIMBA_LSYM_VAR) &&
        y->loc != LIMBA_NOLOC && y->loc > x->loc && y->node &&
        lxs_node(S, y->node)->kind != LXN_TYPEDECL &&
        (S->nunits < 2 ||
         lxs_unit_of(S, y->scope, &impl) == lxs_unit_at(S, node))) {
        lxs_error(S, LXE_BEFORE_DECL, node,
                  "'%.*s' is used before its declaration", (int)n, use);
        lxs_note(S, y->loc, y->len, "declared here");
    }
    if (y->flags & LXS_CBORDER) {
        /* a name of the boundary with C (§ 10.4) */
        if (lxs_restricted(S, node))
            lxs_error(S, LXE_RESTRICTED, node,
                      "'%.*s' is of the boundary with C, which pragma "
                      "restrictions(no_external) forbids",
                      (int)n, use);
        if (y->flags & LXS_CPLATFORM)
            S->c_bound = true;
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
    if (!lxs_unit_declare(S, scope, name_node))
        return 0;
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

/* a routine of the implementation of a unit whose heading is in the
   interface: the body takes the symbol of the heading (§ 11.2) */
static bool bind_body(limba_lxs *S, uint32_t scope, uint32_t d)
{
    if (!S->nunits)
        return false;
    bool impl;
    uint32_t u = lxs_unit_of(S, scope, &impl);
    limba_lxs_unit *f = &S->units[u];
    limba_lx_node *x = lxs_node(S, d);
    if (!f->intf || scope != f->impl || !x->d ||
        lxs_node(S, x->a)->kind != LXN_NAME)
        return false;
    limba_sym s = limba_sym_local(&S->st, f->intf, lxs_node(S, x->a)->a);
    if (!s || S->st.sym[s].kind != LIMBA_LSYM_ROUTINE || lxs_heading(S, s) ||
        lxs_node(S, S->st.sym[s].node)->d)
        return false;
    if (s >= S->nheading) {
        uint32_t n = s + 64;
        S->heading = limba_xrealloc(S->heading, n, sizeof(*S->heading));
        memset(S->heading + S->nheading, 0,
               (n - S->nheading) * sizeof(*S->heading));
        S->nheading = n;
    }
    S->heading[s] = S->st.sym[s].node;
    S->st.sym[s].node = d;
    S->sym[x->a] = s;
    return true;
}

void lxs_declare_all(limba_lxs *S, uint32_t list, uint32_t scope, bool top)
{
    limba_lx_node *l = lxs_node(S, list);
    for (uint32_t i = 0; i < l->b; i++) {
        uint32_t d = limba_lx_list_at(S->t, list, i);
        limba_lx_node *x = lxs_node(S, d);
        switch (x->kind) {
        case LXN_CONST:
            mark(S, lxs_declare(S, scope, x->a, LIMBA_LSYM_CONST, d), top);
            break;
        case LXN_TYPEDECL: {
            mark(S, lxs_declare(S, scope, x->a, LIMBA_LSYM_TYPE, d), top);
            limba_lx_node *tn = lxs_node(S, x->b);
            if (tn->kind == LXN_TENUM) {
                limba_lx_node *vals = lxs_node(S, tn->a);
                for (uint32_t k = 0; k < vals->b; k++)
                    mark(S,
                         lxs_declare(S, scope, limba_lx_list_at(S->t, tn->a, k),
                                     LIMBA_LSYM_CONST, d),
                         top);
            }
            break;
        }
        case LXN_VAR: {
            limba_lx_node *names = lxs_node(S, x->a);
            for (uint32_t k = 0; k < names->b; k++)
                mark(S,
                     lxs_declare(S, scope, limba_lx_list_at(S->t, x->a, k),
                                 LIMBA_LSYM_VAR, d),
                     top);
            break;
        }
        case LXN_ROUTINE:
            if (!bind_body(S, scope, d))
                mark(S, lxs_declare(S, scope, x->a, LIMBA_LSYM_ROUTINE, d),
                     top);
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
    if (lxs_typed_const(S, d, s, t))
        return;
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
static void resolve_var_in(limba_lxs *S, uint32_t d, uint32_t scope, bool top);

static void resolve_var(limba_lxs *S, uint32_t d, uint32_t scope, bool top)
{
    /* the initial value of a variable of a file is part of its
       initialisation (§ 11.5) */
    uint32_t who = S->who;
    if (top && S->nunits) {
        bool impl;
        S->who = LXS_INIT | lxs_unit_of(S, scope, &impl);
    }
    resolve_var_in(S, d, scope, top);
    S->who = who;
}

static void resolve_var_in(limba_lxs *S, uint32_t d, uint32_t scope, bool top)
{
    limba_lx_node *x = lxs_node(S, d);
    limba_ltype t = x->b ? lxs_type(S, x->b, scope, top ? 0 : LXT_VAR) : 0;
    if (x->c) {
        limba_ltype vt = lxs_expr(S, x->c, scope, t);
        if (t) {
            lxs_assign_to(S, x->c, t, "the variable");
        } else if (x->b) {
            /* the type written is wrong, and reported */
        } else if (vt == S->ts.uint || vt == S->ts.ureal || vt == S->ts.nil) {
            lxs_error(S, LXE_NEED_TYPE, x->c,
                      "a constant without a type gives none to the "
                      "variable: write 'var x: T := ...'");
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
    limba_ltype t = type_node(S, x->b, y->scope, LXT_DECL, s);
    y = &S->st.sym[s];
    y->type = t;
    limba_types_set_name(&S->ts, t, s);
}

static void conform(limba_lxs *S, limba_sym s);

static void resolve_routine(limba_lxs *S, limba_sym s)
{
    limba_symbol *y = &S->st.sym[s];
    uint32_t outer = y->scope, d = y->node;
    if (lxs_heading(S, s)) {
        /* the body is in the implementation: its names are there */
        bool impl;
        outer = S->units[lxs_unit_of(S, outer, &impl)].impl;
    }
    uint32_t scope = lxs_scope_new(S, outer, LXS_ROUTINE);
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
            limba_sym ps_ = lxs_declare(S, scope, nn, LIMBA_LSYM_PARAM, p);
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
    if (lxs_heading(S, s))
        conform(S, s);
}

/* two types written in a heading and in its body: the same type, or
   ranges of the same base with the same bounds (§ 11.2) */
static bool same_type(limba_lxs *S, limba_ltype a, limba_ltype b)
{
    if (a == b || !a || !b)
        return true;
    const limba_typeinfo *x = lxs_ty(S, a), *y = lxs_ty(S, b);
    if ((x->flags & LIMBA_TF_RANGE) && (y->flags & LIMBA_TF_RANGE))
        return x->lo == y->lo && x->hi == y->hi &&
               same_type(S, x->base, y->base);
    if ((x->flags | y->flags) & LIMBA_TF_RANGE)
        return false;
    return limba_types_same(&S->ts, a, b);
}

/* the heading of the body conforms to that of the interface: textual for
   the names (kind, names, spelling, order, modes), semantic for the
   types */
static void conform(limba_lxs *S, limba_sym s)
{
    uint32_t h = lxs_heading(S, s), b = S->st.sym[s].node;
    limba_lx_node *hx = lxs_node(S, h), *bx = lxs_node(S, b);
    const char *why = NULL;
    char tb[128], tc[128], buf[400];
    if (hx->op != bx->op)
        why = "it is a procedure in one, a function in the other";
    /* the parameters, name by name */
    uint32_t hl = hx->b, bl = bx->b, hi = 0, hk = 0, bi = 0, bk = 0;
    uint32_t intf = S->st.sym[s].scope;
    bool impl;
    uint32_t implscope = S->units[lxs_unit_of(S, intf, &impl)].impl;
    while (!why) {
        while (hi < lxs_node(S, hl)->b &&
               hk >= lxs_node(S, lxs_node(S, limba_lx_list_at(S->t, hl, hi))->a)
                         ->b) {
            hi++;
            hk = 0;
        }
        while (bi < lxs_node(S, bl)->b &&
               bk >= lxs_node(S, lxs_node(S, limba_lx_list_at(S->t, bl, bi))->a)
                         ->b) {
            bi++;
            bk = 0;
        }
        bool hend = hi >= lxs_node(S, hl)->b, bend = bi >= lxs_node(S, bl)->b;
        if (hend || bend) {
            if (hend != bend)
                why = "the number of the parameters differs";
            break;
        }
        uint32_t hp = limba_lx_list_at(S->t, hl, hi);
        uint32_t bp = limba_lx_list_at(S->t, bl, bi);
        uint32_t hn = limba_lx_list_at(S->t, lxs_node(S, hp)->a, hk);
        uint32_t bn = limba_lx_list_at(S->t, lxs_node(S, bp)->a, bk);
        const char *ht = text_at(S, lxs_node(S, hn)->loc);
        const char *bt = text_at(S, lxs_node(S, bn)->loc);
        uint32_t hlen = ident_len(ht), blen = ident_len(bt);
        if (hlen != blen || memcmp(ht, bt, hlen)) {
            snprintf(buf, sizeof(buf),
                     "the parameter '%.*s' is '%.*s' in the interface",
                     (int)blen, bt, (int)hlen, ht);
            why = buf;
        } else if (lxs_node(S, hp)->op != lxs_node(S, bp)->op) {
            snprintf(buf, sizeof(buf),
                     "the parameter '%.*s' has another mode in the interface",
                     (int)blen, bt);
            why = buf;
        } else {
            limba_ltype t1 = lxs_type(S, lxs_node(S, hp)->b, intf, LXT_PARAM);
            limba_ltype t2 =
                lxs_type(S, lxs_node(S, bp)->b, implscope, LXT_PARAM);
            if (!same_type(S, t1, t2)) {
                snprintf(buf, sizeof(buf),
                         "the parameter '%.*s' is %s in the interface, %s "
                         "here",
                         (int)blen, bt, lxs_tname(S, t1, tb),
                         lxs_tname(S, t2, tc));
                why = buf;
            }
        }
        hk++;
        bk++;
    }
    if (!why && hx->c && bx->c) {
        limba_ltype t1 = lxs_type(S, hx->c, intf, 0);
        limba_ltype t2 = lxs_type(S, bx->c, implscope, 0);
        if (!same_type(S, t1, t2)) {
            snprintf(buf, sizeof(buf),
                     "the result is %s in the interface, %s here",
                     lxs_tname(S, t1, tb), lxs_tname(S, t2, tc));
            why = buf;
        }
    }
    if (why) {
        lxs_error(S, LXE_NOT_CONFORMING, bx->a,
                  "the heading is not the one of the interface: %s", why);
        lxs_note(S, hx->loc, 0, "the heading in the interface");
    }
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
        if (y->kind == LIMBA_LSYM_TYPE) {
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
    if (y->kind != LIMBA_LSYM_TYPE) {
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
    /* low > high is an empty range, as in Ada (§ 4.5) */
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
        if (t && lxs_ty(S, t)->kind == LIMBA_LTK_OPEN &&
            !(where & (LXT_PARAM | LXT_DECL | LXT_PTR))) {
            lxs_error(S, LXE_OPEN_ARRAY_PLACE, node,
                      "an array with the bounds of its argument is a type "
                      "for parameters and pointers only");
            return 0;
        }
        return t;
    }
    case LXN_TBOX:
        lxs_error(S, LXE_OPEN_ARRAY_PLACE, node,
                  "'range <>' is the index of an array parameter only");
        return 0;
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
                vs = lxs_declare(S, scope, nn, LIMBA_LSYM_CONST, 0);
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
        /* array[I range <>] of T: the bounds come with the argument */
        if (!(where & (LXT_PARAM | LXT_DECL | LXT_PTR))) {
            lxs_error(S, LXE_OPEN_ARRAY_PLACE, node,
                      "an array with the bounds of its argument is a type "
                      "for parameters and pointers only");
            return 0;
        }
        limba_ltype index = named(S, x->a, scope, false, &dyn);
        limba_ltype elem = type_node(S, x->b, scope, 0, 0);
        if (!index || !elem)
            return 0;
        if (!limba_types_is_discrete(&S->ts, index)) {
            lxs_error(S, LXE_TYPE_MISMATCH, x->a,
                      "an array is indexed by a discrete type, not %s",
                      lxs_tname(S, index, tb));
            return 0;
        }
        const limba_typeinfo *et = lxs_ty(S, elem);
        if (et->kind == LIMBA_LTK_OPEN ||
            (et->flags & (LIMBA_TF_DYNAMIC | LIMBA_TF_INCOMPLETE))) {
            lxs_error(S, LXE_TYPE_MISMATCH, x->b,
                      "the elements of an array have a size known in "
                      "advance");
            return 0;
        }
        return limba_types_open(&S->ts, index, elem);
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
        /* ^A with A an open array: the array new makes (§ 3.10) */
        limba_ltype target = type_node(S, x->a, scope, LXT_PTR, 0);
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

/* the declarations of the parts of a file: the interface (0 for the
   program) and the implementation or the program's */
static uint32_t part_decls(limba_lxs *S, uint32_t u, bool impl)
{
    limba_lx_node *r = lxs_node(S, S->units[u].root);
    if (r->kind == LXN_UNIT)
        return impl ? r->c : r->b;
    return impl ? r->b : 0;
}

static void pragmas_of(limba_lxs *S, uint32_t list, uint32_t scope,
                       const char *only)
{
    for (uint32_t i = 0; list && i < lxs_node(S, list)->b; i++) {
        uint32_t d = limba_lx_list_at(S->t, list, i);
        if (lxs_node(S, d)->kind != LXN_PRAGMA)
            continue;
        if (only ? lxs_pragma_is(S, d, only)
                 : !lxs_pragma_is(S, d, "convention") &&
                       !lxs_pragma_is(S, d, "restrictions") &&
                       !lxs_pragma_is(S, d, "hides")) {
            if (only)
                lxs_c_pragma(S, d, scope);
            else
                lxs_pragma(S, d);
        }
    }
}

void limba_lxs_check(limba_lxs *S)
{
    if (!S->nunits) {
        /* one file, as the tests give it */
        if (!S->t->root)
            return;
        limba_lxs_unit_add(S, S->t->root, 0, S->t->nnode, false);
    }
    uint32_t nu = S->nunits;
    c_types(S);
    lxs_check_units(S);
    /* pragma restrictions first: it governs every name of its file, and
       the program's every file (§ 11.6) */
    for (uint32_t u = 0; u < nu; u++)
        for (int part = 0; part < 2; part++)
            pragmas_of(S, part_decls(S, u, part == 1), 0, "restrictions");
    S->program = S->units[S->main].impl;
    for (uint32_t u = 0; u < nu; u++)
        if (lxs_node(S, S->units[u].root)->kind == LXN_PROGRAM)
            S->program = S->units[u].impl;
    for (uint32_t u = 0; u < nu; u++) {
        if (S->units[u].intf)
            lxs_declare_all(S, part_decls(S, u, false), S->units[u].intf, true);
        lxs_declare_all(S, part_decls(S, u, true), S->units[u].impl, true);
    }
    for (uint32_t u = 0; u < nu; u++)
        for (int part = 0; part < 2; part++)
            if (part_decls(S, u, part == 1))
                lxs_resolve_all(S, part_decls(S, u, part == 1));
    /* every heading of an interface has its body (§ 11.2) */
    for (uint32_t u = 0; u < nu; u++) {
        uint32_t l = part_decls(S, u, false);
        for (uint32_t i = 0; l && i < lxs_node(S, l)->b; i++) {
            uint32_t d = limba_lx_list_at(S->t, l, i);
            limba_lx_node *x = lxs_node(S, d);
            if (x->kind != LXN_ROUTINE || x->d || !S->sym[x->a] ||
                lxs_heading(S, S->sym[x->a]))
                continue;
            size_t n;
            const char *sp = lxs_spell(S, S->sym[x->a], &n);
            lxs_error(S, LXE_NO_BODY, x->a,
                      "'%.*s' has no body in the implementation", (int)n, sp);
        }
    }
    /* then the records with the C convention, before the routines of C
       that take them */
    for (uint32_t u = 0; u < nu; u++) {
        if (S->units[u].intf)
            pragmas_of(S, part_decls(S, u, false), S->units[u].intf,
                       "convention");
        pragmas_of(S, part_decls(S, u, true), S->units[u].impl, "convention");
    }
    for (uint32_t u = 0; u < nu; u++)
        for (int part = 0; part < 2; part++) {
            uint32_t l = part_decls(S, u, part == 1);
            for (uint32_t i = 0; l && i < lxs_node(S, l)->b; i++) {
                uint32_t d = limba_lx_list_at(S->t, l, i);
                limba_lx_node *x = lxs_node(S, d);
                if (x->kind == LXN_ROUTINE && x->d && S->sym[x->a]) {
                    S->who = S->sym[x->a];
                    lxs_routine_body(S, S->sym[x->a]);
                }
            }
            pragmas_of(S, l, 0, NULL);
        }
    /* the initialisations of the units, then the body of the program */
    for (uint32_t u = 0; u < nu; u++) {
        limba_lx_node *r = lxs_node(S, S->units[u].root);
        uint32_t body = r->kind == LXN_UNIT ? r->d : r->c;
        if (!body)
            continue;
        S->who = LXS_INIT | u;
        S->result = 0;
        S->in_routine = false;
        S->in_init = r->kind == LXN_UNIT;
        S->loops = 0;
        lxs_stmts(S, body, lxs_scope_new(S, S->units[u].impl, LXS_BLOCK));
    }
    S->in_init = false;
    S->who = 0;
    lxs_unit_warnings(S);
}

/* ---- the boundary with C (§ 3.13, § 8.5, § 10.4) ---- */

bool lxs_pragma_is(limba_lxs *S, uint32_t node, const char *name)
{
    limba_lx_node *x = lxs_node(S, node);
    if (!x->a)
        return false;
    size_t n;
    const char *s = lxs_name(S, lxs_node(S, x->a)->a, &n);
    return n == strlen(name) && !memcmp(s, name, n);
}

/* a scalar type that crosses to C as it is: an integer of fixed size, a
   real, CBool, an opaque pointer, a record with the C convention */
static bool c_scalar(limba_lxs *S, limba_ltype t)
{
    const limba_typeinfo *x = lxs_ty(S, t);
    switch (x->kind) {
    case LIMBA_LTK_INT:
        return !(x->flags & LIMBA_TF_RANGE);
    case LIMBA_LTK_FLOAT:
    case LIMBA_LTK_OPAQUE:
        return true;
    case LIMBA_LTK_BOOL:
        return t == S->ty_cbool;
    case LIMBA_LTK_RECORD:
        return (x->flags & LIMBA_TF_CONVC) != 0;
    }
    return false;
}

/* a field of a record with the C convention: such a scalar, or an
   array of fixed bounds of them */
static bool c_field(limba_lxs *S, limba_ltype t)
{
    const limba_typeinfo *x = lxs_ty(S, t);
    if (x->kind == LIMBA_LTK_ARRAY)
        return !(x->flags & LIMBA_TF_DYNAMIC) && c_field(S, x->elem);
    return c_scalar(S, t);
}

/* why t does not cross: a hint for the message */
static const char *c_why(limba_lxs *S, limba_ltype t)
{
    const limba_typeinfo *x = lxs_ty(S, t);
    switch (x->kind) {
    case LIMBA_LTK_STRING:
        return ": convert it with newcstring and pass a CString";
    case LIMBA_LTK_BIGINT:
        return ": it is counted, C knows no references";
    case LIMBA_LTK_BOOL:
        return ": C's _Bool is CBool";
    case LIMBA_LTK_RECORD:
        return ": give it the C layout, pragma convention(c, ...)";
    case LIMBA_LTK_POINTER:
        return ": give C an opaque pointer (new CPointer), or the object "
               "as a var parameter";
    case LIMBA_LTK_INT:
        return ": a range is not C's; convert the value afterwards";
    }
    return "";
}

void lxs_c_pragma(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t args = x->b, n = args ? lxs_node(S, args)->b : 0;
    size_t len;
    const char *a0 =
        n ? lxs_name(S, lxs_node(S, limba_lx_list_at(S->t, args, 0))->a, &len)
          : "";
    if (!n)
        len = 0;
    if (lxs_pragma_is(S, node, "restrictions")) {
        if (scope) {
            lxs_error(S, LXE_C_PRAGMA, node,
                      "pragma restrictions goes among the declarations of "
                      "the program or of a unit");
        } else if (n != 1 || len != 11 || memcmp(a0, "no_external", 11)) {
            lxs_error(S, LXE_C_PRAGMA, node,
                      "the restriction is no_external: "
                      "'pragma restrictions(no_external)'");
        } else if (!S->nunits ||
                   lxs_node(S, S->units[lxs_unit_at(S, node)].root)->kind ==
                       LXN_PROGRAM) {
            S->no_external = true; /* the whole program (§ 11.6) */
        } else {
            S->units[lxs_unit_at(S, node)].restricted = true;
        }
        return;
    }
    /* convention(c, R) */
    if (n != 2 || len != 1 || a0[0] != 'c') {
        lxs_error(S, LXE_C_PRAGMA, node,
                  "the convention is C: 'pragma convention(c, R)', R a "
                  "record type declared here");
        return;
    }
    if (lxs_restricted(S, node))
        lxs_error(S, LXE_RESTRICTED, node,
                  "pragma convention is of the boundary with C, which "
                  "pragma restrictions(no_external) forbids");
    uint32_t rn = limba_lx_list_at(S->t, args, 1);
    limba_sym r = limba_sym_local(&S->st, scope, lxs_node(S, rn)->a);
    limba_ltype t = 0;
    if (r && S->st.sym[r].kind == LIMBA_LSYM_TYPE) {
        lxs_force(S, r);
        t = S->st.sym[r].type;
    }
    if (!t || lxs_ty(S, t)->kind != LIMBA_LTK_RECORD) {
        lxs_error(S, LXE_C_PRAGMA, rn,
                  "pragma convention takes a record type declared in the "
                  "same declarations");
        return;
    }
    S->ts.t[t].flags |= LIMBA_TF_CONVC;
    S->c_bound = true;
    const limba_typeinfo *ty = lxs_ty(S, t);
    for (uint32_t i = 0; i < ty->count; i++) {
        limba_ltype ft = S->ts.field[ty->first + i].type;
        if (!c_field(S, ft)) {
            char tb[128], fb[128];
            lxs_error(S, LXE_C_BOUNDARY, rn,
                      "a field of %s is %s, which does not cross to C%s",
                      lxs_tname(S, t, tb), lxs_tname(S, ft, fb), c_why(S, ft));
        }
    }
}

void lxs_c_routine(limba_lxs *S, limba_sym s)
{
    limba_symbol *y = &S->st.sym[s];
    limba_lx_node *x = lxs_node(S, y->node);
    if (lxs_restricted(S, y->node)) {
        size_t n;
        const char *sp = lxs_spell(S, s, &n);
        lxs_error(S, LXE_RESTRICTED, x->a,
                  "'%.*s' is an external routine, which pragma "
                  "restrictions(no_external) forbids",
                  (int)n, sp);
    }
    limba_ltype sig = y->type;
    if (!sig)
        return;
    const limba_typeinfo *r = lxs_ty(S, sig);
    char tb[128];
    uint32_t plist = x->b, k = 0;
    for (uint32_t i = 0; i < lxs_node(S, plist)->b; i++) {
        uint32_t p = limba_lx_list_at(S->t, plist, i);
        uint32_t names = lxs_node(S, p)->a;
        for (uint32_t j = 0; j < lxs_node(S, names)->b; j++, k++) {
            limba_ltype t = S->ts.param[r->first + k].type;
            if (!t)
                continue;
            const limba_typeinfo *pt = lxs_ty(S, t);
            bool ok = c_scalar(S, t) || (((pt->kind == LIMBA_LTK_ARRAY &&
                                           !(pt->flags & LIMBA_TF_DYNAMIC)) ||
                                          pt->kind == LIMBA_LTK_OPEN) &&
                                         c_field(S, pt->elem));
            if (!ok)
                lxs_error(S, LXE_C_BOUNDARY, lxs_node(S, p)->b,
                          "%s does not cross to C%s", lxs_tname(S, t, tb),
                          c_why(S, t));
        }
    }
    if (x->c && r->elem && !c_scalar(S, r->elem))
        lxs_error(S, LXE_C_BOUNDARY, x->c, "%s does not cross to C%s",
                  lxs_tname(S, r->elem, tb), c_why(S, r->elem));
}
