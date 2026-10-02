/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_unit.c - the units of Luxia (specification § 11): the scopes of
 * the files, the names they give each other, the bodies of the routines
 * of an interface, the cycles of the interfaces, and the order of the
 * initialisations, computed from what each one can read and write.
 *
 * A unit has two scopes, its interface (whose parent is the universe)
 * and its implementation (whose parent is the interface); the program
 * has one. A name not declared along the chain of scopes is looked for
 * in the interfaces of the units the file uses, by levels: the units of
 * the program before those of the library; two of one level make the
 * use ambiguous. Unit.Name is rewritten, before anything is checked,
 * into a qualified REF: a qualifier that names a unit used is always
 * the unit.
 */
#include "common/xalloc.h"
#include "luxia/sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- files and scopes ---- */

uint32_t limba_lxs_unit_add(limba_lxs *S, uint32_t root, uint32_t first,
                            uint32_t last, bool library)
{
    LIMBA_GROW(S->units, S->nunits, S->capunits);
    limba_lxs_unit *u = &S->units[S->nunits];
    memset(u, 0, sizeof(*u));
    u->root = root;
    u->first = first;
    u->last = last;
    u->library = library;
    const limba_lx_node *r = &S->t->node[root];
    if (r->kind == LXN_UNIT && r->a)
        u->name = S->t->node[r->a].a;
    return S->nunits++;
}

uint32_t lxs_scope_new(limba_lxs *S, uint32_t parent, unsigned kind)
{
    uint32_t s = limba_scope_new(&S->st, parent, kind);
    if (s >= S->nsunit) {
        uint32_t n = s + 64;
        S->sunit = limba_xrealloc(S->sunit, n, sizeof(*S->sunit));
        memset(S->sunit + S->nsunit, 0, (n - S->nsunit) * sizeof(*S->sunit));
        S->nsunit = n;
    }
    S->sunit[s] = parent && parent < S->nsunit ? S->sunit[parent] : 0;
    return s;
}

uint32_t lxs_unit_of(const limba_lxs *S, uint32_t scope, bool *impl)
{
    uint32_t v = scope < S->nsunit ? S->sunit[scope] : 0;
    if (!v) {
        *impl = true;
        return S->main;
    }
    *impl = (v >> 31) != 0;
    return (v & 0x7fffffffu) - 1;
}

uint32_t lxs_unit_at(const limba_lxs *S, uint32_t node)
{
    for (uint32_t i = 0; i < S->nunits; i++)
        if (node >= S->units[i].first && node < S->units[i].last)
            return i;
    return S->main;
}

static bool is_program(const limba_lxs *S, uint32_t u)
{
    return S->t->node[S->units[u].root].kind == LXN_PROGRAM;
}

/* the LIST of the units a part of u uses, 0 if none: the interface of a
   unit, or its implementation (the program has only this one) */
static uint32_t uses_list(const limba_lxs *S, uint32_t u, bool impl)
{
    const limba_lx_node *r = &S->t->node[S->units[u].root];
    uint32_t decls = r->kind == LXN_UNIT ? (impl ? r->c : r->b)
                     : impl              ? r->b
                                         : 0;
    if (!decls || !S->t->node[decls].b)
        return 0;
    uint32_t first = limba_lx_list_at(S->t, decls, 0);
    return S->t->node[first].kind == LXN_USES ? S->t->node[first].a : 0;
}

/* the REF nodes of the units visible from a part of u: the uses of the
   interface, and in the implementation those of the implementation too;
   the k-th, 0 past the last */
static uint32_t visible_ref(const limba_lxs *S, uint32_t u, bool impl,
                            uint32_t k)
{
    for (int part = 0; part < 2; part++) {
        if (part == 1 && !impl)
            break;
        uint32_t l = uses_list(S, u, part == 1);
        uint32_t n = l ? S->t->node[l].b : 0;
        if (k < n)
            return limba_lx_list_at(S->t, l, k);
        k -= n;
    }
    return 0;
}

/* the unit named so, used by a part of u (any part if any), or u itself;
   UINT32_MAX if none */
static uint32_t unit_named(const limba_lxs *S, uint32_t u, bool impl,
                           uint32_t name)
{
    if (S->units[u].name == name)
        return u;
    for (uint32_t k = 0;; k++) {
        uint32_t ref = visible_ref(S, u, impl, k);
        if (!ref)
            break;
        uint32_t v = S->t->node[ref].b;
        if (v && S->units[v - 1].name == name)
            return v - 1;
    }
    return UINT32_MAX;
}

static void name_of(const limba_lxs *S, uint32_t u, const char **s, uint32_t *n)
{
    const limba_lx_node *r = &S->t->node[S->units[u].root];
    if (r->a) {
        *s = lxs_text_at(S, S->t->node[r->a].loc);
        *n = lxs_ident_len(*s);
    } else {
        *s = "the program";
        *n = 11;
    }
}

static void name_of(const limba_lxs *S, uint32_t u, const char **s,
                    uint32_t *n);

/* a unit named at node as declared, the case rule (§ 2.2, § 11.3) */
static bool spelt(limba_lxs *S, uint32_t node, uint32_t v)
{
    const char *use = lxs_text_at(S, S->t->node[node].loc);
    uint32_t n = lxs_ident_len(use);
    const char *a;
    uint32_t na;
    name_of(S, v, &a, &na);
    if (n == na && memcmp(use, a, n) != 0) {
        lxs_error(S, LXE_SPELLING, node,
                  "'%.*s' is declared as '%.*s': write it the same way", (int)n,
                  use, (int)na, a);
        return false;
    }
    return true;
}

static void mark_named(limba_lxs *S, uint32_t u, uint32_t v)
{
    if (S->named && u != v)
        S->named[u * S->nunits + v] = 1;
}

bool lxs_restricted(const limba_lxs *S, uint32_t node)
{
    if (!S->nunits)
        return S->no_external;
    uint32_t u = lxs_unit_at(S, node);
    if (S->units[u].library)
        return S->units[u].restricted;
    return S->no_external || S->units[u].restricted;
}

uint32_t lxs_heading(const limba_lxs *S, limba_sym s)
{
    return s < S->nheading ? S->heading[s] : 0;
}

/* ---- names ---- */

limba_sym lxs_find(limba_lxs *S, uint32_t scope, uint32_t name, uint32_t node)
{
    limba_sym s = limba_sym_lookup(&S->st, scope, name);
    if ((s && S->st.sym[s].scope != S->universe) || !S->nunits)
        return s;
    bool impl;
    uint32_t u = lxs_unit_of(S, scope, &impl);
    limba_sym best = 0, other = 0, hidden = 0;
    uint32_t ub = 0, uo = 0, uh = 0;
    int level = 9;
    for (uint32_t k = 0;; k++) {
        uint32_t ref = visible_ref(S, u, impl, k);
        if (!ref)
            break;
        uint32_t v = S->t->node[ref].b;
        if (!v || !S->units[v - 1].intf)
            continue;
        v--;
        limba_sym c = limba_sym_local(&S->st, S->units[v].intf, name);
        if (!c)
            continue;
        int lev = S->units[v].library ? 2 : 1;
        if (lev < level) {
            if (best) {
                hidden = best;
                uh = ub;
            }
            best = c;
            ub = v;
            level = lev;
            other = 0;
        } else if (lev == level) {
            if (c != best) {
                other = c;
                uo = v;
            }
        } else {
            hidden = c;
            uh = v;
        }
    }
    if (!best)
        return s;
    if (!node)
        return other ? 0 : best;
    mark_named(S, u, ub);
    const char *use = lxs_text_at(S, S->t->node[node].loc);
    uint32_t n = lxs_ident_len(use);
    const char *a, *b;
    uint32_t na, nb;
    if (other) {
        mark_named(S, u, uo);
        name_of(S, ub, &a, &na);
        name_of(S, uo, &b, &nb);
        lxs_error(S, LXE_AMBIGUOUS, node,
                  "'%.*s' is given by '%.*s' and by '%.*s': write "
                  "%.*s.%.*s or %.*s.%.*s",
                  (int)n, use, (int)na, a, (int)nb, b, (int)na, a, (int)n, use,
                  (int)nb, b, (int)n, use);
        return best; /* reported: the first, so as not to echo it */
    }
    if (hidden) {
        name_of(S, ub, &a, &na);
        name_of(S, uh, &b, &nb);
        limba_report_add(S->rep, LIMBA_WARNING, LXE_HIDES, S->t->node[node].loc,
                         n,
                         "'%.*s' is %.*s.%.*s, which hides %.*s.%.*s of the "
                         "library: write %.*s.%.*s",
                         (int)n, use, (int)na, a, (int)n, use, (int)nb, b,
                         (int)n, use, (int)na, a, (int)n, use);
    }
    return best;
}

limba_sym lxs_qualified(limba_lxs *S, uint32_t scope, uint32_t unit,
                        uint32_t name, uint32_t node)
{
    bool impl;
    uint32_t u = lxs_unit_of(S, scope, &impl);
    const char *a;
    uint32_t na;
    name_of(S, unit, &a, &na);
    if (unit != u && unit_named(S, u, impl, S->units[unit].name) != unit) {
        lxs_error(S, LXE_UNKNOWN_NAME, node,
                  "'%.*s' is used in the implementation only: the interface "
                  "cannot name it",
                  (int)na, a);
        return 0;
    }
    mark_named(S, u, unit);
    limba_sym s = 0;
    if (unit == u && impl && S->units[u].impl)
        s = limba_sym_local(&S->st, S->units[u].impl, name);
    if (!s && S->units[unit].intf)
        s = limba_sym_local(&S->st, S->units[unit].intf, name);
    if (!s && unit == u && !S->units[u].intf)
        s = limba_sym_local(&S->st, S->units[u].impl, name);
    if (!s) {
        const char *use = lxs_text_at(S, S->t->node[node].loc);
        uint32_t n = lxs_ident_len(use);
        lxs_error(S, LXE_NOT_IN_INTERFACE, node,
                  unit == u ? "'%.*s' is not declared in '%.*s'"
                            : "'%.*s' is not in the interface of '%.*s'",
                  (int)n, use, (int)na, a);
    }
    return s;
}

/* is the name declared at the level of a file, hiding a name of a unit
   it uses, stated by a pragma hides(Unit.Name) of the file */
static bool hides_stated(limba_lxs *S, uint32_t u, uint32_t v, uint32_t name)
{
    for (uint32_t i = 0; i < S->nhides; i++) {
        const limba_lx_node *h = &S->t->node[S->hides[i]];
        if (lxs_unit_at(S, S->hides[i]) == u && h->b == v + 1 && h->a == name) {
            S->hid[i] = 1;
            return true;
        }
    }
    return false;
}

bool lxs_unit_declare(limba_lxs *S, uint32_t scope, uint32_t name_node)
{
    if (!S->nunits)
        return true;
    const limba_lx_node *x = &S->t->node[name_node];
    bool impl;
    uint32_t u = lxs_unit_of(S, scope, &impl);
    const char *text = lxs_text_at(S, x->loc);
    uint32_t n = lxs_ident_len(text);
    /* the names of the units a file uses, and its own (§ 11.3) */
    uint32_t v = unit_named(S, u, true, x->a);
    if (v != UINT32_MAX) {
        lxs_error(S, LXE_UNIT_NAME, name_node,
                  v == u ? "'%.*s' is the name of this unit: it cannot be "
                           "declared in it"
                         : "'%.*s' is the name of a unit used here: it cannot "
                           "be declared in this file",
                  (int)n, text);
        return false;
    }
    const limba_lxs_unit *f = &S->units[u];
    /* a name of the implementation is not one of the interface */
    if (f->intf && scope == f->impl) {
        limba_sym d = limba_sym_local(&S->st, f->intf, x->a);
        if (d) {
            lxs_error(S, LXE_DUPLICATE, name_node,
                      "'%.*s' is already declared in the interface", (int)n,
                      text);
            lxs_note(S, S->st.sym[d].loc, S->st.sym[d].len,
                     "the first declaration");
            return false;
        }
    }
    return true;
}

/* the declarations of the files that hide a name of a unit they use,
   once every file is declared (§ 11.3) */
static void hides_check(limba_lxs *S)
{
    for (uint32_t u = 0; u < S->nunits; u++) {
        const limba_lxs_unit *f = &S->units[u];
        for (uint32_t i = f->first; i < f->last && i < S->t->nnode; i++) {
            const limba_lx_node *x = &S->t->node[i];
            limba_sym s = x->kind == LXN_NAME ? S->sym[i] : 0;
            if (!s || S->st.sym[s].loc != x->loc ||
                (S->st.sym[s].scope != f->intf &&
                 S->st.sym[s].scope != f->impl))
                continue;
            bool impl = S->st.sym[s].scope == f->impl;
            const char *text = lxs_text_at(S, x->loc);
            uint32_t n = lxs_ident_len(text);
            for (uint32_t k = 0;; k++) {
                uint32_t ref = visible_ref(S, u, impl, k);
                if (!ref)
                    break;
                uint32_t w = S->t->node[ref].b;
                if (!w || !S->units[w - 1].intf)
                    continue;
                w--;
                if (!limba_sym_local(&S->st, S->units[w].intf, x->a) ||
                    hides_stated(S, u, w, x->a))
                    continue;
                const char *a;
                uint32_t na;
                name_of(S, w, &a, &na);
                limba_report_add(S->rep, LIMBA_WARNING, LXE_HIDES, x->loc, n,
                                 "'%.*s' hides '%.*s.%.*s': if it is meant, "
                                 "say it with pragma hides(%.*s.%.*s)",
                                 (int)n, text, (int)na, a, (int)n, text,
                                 (int)na, a, (int)n, text);
            }
        }
    }
}

/* ---- what the code uses ---- */

void lxs_use(limba_lxs *S, limba_sym what, uint32_t node, bool write)
{
    if (!S->who || !what)
        return;
    const limba_symbol *y = &S->st.sym[what];
    if (y->kind == LIMBA_LSYM_VAR) {
        /* only the variables of a file: a unit or the program */
        bool impl;
        uint32_t u = lxs_unit_of(S, y->scope, &impl);
        if (y->scope != S->units[u].intf && y->scope != S->units[u].impl)
            return;
    } else if (y->kind != LIMBA_LSYM_ROUTINE) {
        return;
    }
    LIMBA_GROW(S->uses, S->nuses, S->capuses);
    S->uses[S->nuses++] = (limba_lxs_use){S->who, what, node, write};
}

/* ---- before the names: Unit.Name, pragma hides ---- */

static bool pragma_named(limba_lxs *S, uint32_t node, const char *name)
{
    const limba_lx_node *x = &S->t->node[node];
    if (!x->a)
        return false;
    size_t n;
    const char *s = lxs_name(S, S->t->node[x->a].a, &n);
    return n == strlen(name) && !memcmp(s, name, n);
}

static void qualify(limba_lxs *S, uint32_t u)
{
    const limba_lxs_unit *f = &S->units[u];
    for (uint32_t i = f->first; i < f->last && i < S->t->nnode; i++) {
        limba_lx_node *x = &S->t->node[i];
        if (x->kind == LXN_SEL) {
            const limba_lx_node *b = &S->t->node[x->a];
            if (b->kind != LXN_REF || (b->flags & LXN_F_QUAL))
                continue;
            uint32_t v = unit_named(S, u, true, b->a);
            if (v == UINT32_MAX)
                continue;
            spelt(S, x->a, v);
            /* Unit.Name: a REF of the name, at its place (§ 11.3) */
            uint32_t name = x->b;
            limba_loc loc = (limba_loc)x->c;
            x->kind = LXN_REF;
            x->flags |= LXN_F_QUAL;
            x->a = name;
            x->b = v + 1;
            x->c = 0;
            x->loc = loc;
        } else if (x->kind == LXN_PRAGMA && pragma_named(S, i, "hides")) {
            for (uint32_t k = 0; x->b && k < S->t->node[x->b].b; k++) {
                uint32_t a = limba_lx_list_at(S->t, x->b, k);
                const limba_lx_node *y = &S->t->node[a];
                if (y->kind == LXN_REF && (y->flags & LXN_F_QUAL)) {
                    LIMBA_GROW(S->hides, S->nhides, S->caphides);
                    S->hid = limba_xrealloc(S->hid, S->caphides, 1);
                    S->hid[S->nhides] = 0;
                    S->hides[S->nhides++] = a;
                    mark_named(S, u, y->b - 1);
                } else {
                    lxs_error(S, LXE_C_PRAGMA, a,
                              "pragma hides names a name of a unit used: "
                              "'pragma hides(Unit.Name)'");
                }
            }
            x->flags = 0;
        }
    }
}

/* ---- the uses of each file: twice, itself, cycles of interfaces ---- */

static void check_uses(limba_lxs *S, uint32_t u)
{
    for (int part = 0; part < 2; part++) {
        uint32_t l = uses_list(S, u, part == 1);
        for (uint32_t i = 0; l && i < S->t->node[l].b; i++) {
            uint32_t ref = limba_lx_list_at(S->t, l, i);
            uint32_t v = S->t->node[ref].b;
            if (!v)
                continue;
            spelt(S, ref, v - 1);
            if (v - 1 == u) {
                lxs_error(S, LXE_UNIT_CYCLE, ref, "a unit does not use itself");
                S->t->node[ref].b = 0;
                continue;
            }
            /* named before, in this clause or in the interface */
            for (uint32_t k = 0;; k++) {
                uint32_t r2 = visible_ref(S, u, true, k);
                if (!r2 || r2 == ref)
                    break;
                if (S->t->node[r2].b == v) {
                    const char *a;
                    uint32_t na;
                    name_of(S, v - 1, &a, &na);
                    lxs_error(S, LXE_UNIT_TWICE, ref,
                              "'%.*s' is already used here", (int)na, a);
                    S->t->node[ref].b = 0;
                    break;
                }
            }
        }
    }
}

/* a cycle of interfaces: units whose interfaces use each other (§ 11.3);
   colour 0 unseen, 1 on the path, 2 done */
static bool cycle_from(limba_lxs *S, uint32_t u, uint8_t *colour,
                       uint32_t *path, uint32_t depth)
{
    colour[u] = 1;
    path[depth] = u;
    uint32_t l = uses_list(S, u, false);
    for (uint32_t i = 0; l && i < S->t->node[l].b; i++) {
        uint32_t ref = limba_lx_list_at(S->t, l, i);
        uint32_t v = S->t->node[ref].b;
        if (!v)
            continue;
        v--;
        if (colour[v] == 1) {
            char chain[512];
            size_t at = 0;
            uint32_t from = depth;
            while (from > 0 && path[from] != v)
                from--;
            for (uint32_t k = from; k <= depth && at < sizeof(chain) - 80;
                 k++) {
                const char *a;
                uint32_t na;
                name_of(S, path[k], &a, &na);
                at += (size_t)snprintf(chain + at, sizeof(chain) - at,
                                       "%.*s -> ", (int)na, a);
            }
            const char *a;
            uint32_t na;
            name_of(S, v, &a, &na);
            snprintf(chain + at, sizeof(chain) - at, "%.*s", (int)na, a);
            lxs_error(S, LXE_UNIT_CYCLE, ref,
                      "the interfaces use each other: %s; one of the uses "
                      "goes in an implementation",
                      chain);
            S->t->node[ref].b = 0;
            continue;
        }
        if (colour[v] == 0 && cycle_from(S, v, colour, path, depth + 1))
            return true;
    }
    colour[u] = 2;
    return false;
}

/* ---- the order of the initialisations (§ 11.5) ---- */

/* the file whose variable a symbol is, UINT32_MAX if not one */
static uint32_t var_unit(const limba_lxs *S, limba_sym s)
{
    const limba_symbol *y = &S->st.sym[s];
    if (y->kind != LIMBA_LSYM_VAR)
        return UINT32_MAX;
    bool impl;
    uint32_t u = lxs_unit_of(S, y->scope, &impl);
    return y->scope == S->units[u].intf || y->scope == S->units[u].impl
               ? u
               : UINT32_MAX;
}

/* a variable whose initial value is a constant of a scalar type or a
   String: it has it before any code runs */
static bool ready(const limba_lxs *S, limba_sym s)
{
    const limba_symbol *y = &S->st.sym[s];
    const limba_lx_node *d = &S->t->node[y->node];
    if (d->kind != LXN_VAR || !d->c || !S->val[d->c] || !y->type)
        return false;
    unsigned k = S->ts.t[y->type].kind;
    return k != LIMBA_LTK_RECORD && k != LIMBA_LTK_ARRAY &&
           k != LIMBA_LTK_BIGINT;
}

/* the uses grouped by who: those of key k (a routine symbol, or ns +
   the file of an initialisation) are idx[off[k] .. off[k + 1]) */
typedef struct {
    uint32_t *off, *idx, ns;
} use_index;

static uint32_t use_key(const use_index *x, uint32_t who)
{
    return who & LXS_INIT ? x->ns + (who & ~LXS_INIT) : who;
}

static void index_uses(limba_lxs *S, use_index *x)
{
    x->ns = S->st.nsym + 1;
    uint32_t nk = x->ns + S->nunits;
    x->off = limba_xcalloc(nk + 1, sizeof(*x->off));
    x->idx = limba_xcalloc(S->nuses + 1, sizeof(*x->idx));
    for (uint32_t i = 0; i < S->nuses; i++)
        x->off[use_key(x, S->uses[i].who) + 1]++;
    for (uint32_t k = 0; k < nk; k++)
        x->off[k + 1] += x->off[k];
    uint32_t *at = limba_xcalloc(nk, sizeof(*at));
    for (uint32_t i = 0; i < S->nuses; i++) {
        uint32_t k = use_key(x, S->uses[i].who);
        x->idx[x->off[k] + at[k]++] = i;
    }
    free(at);
}

/* the uses reached from who, through the calls: for each symbol (a
   variable or a routine) the first use that reaches it + 1, and wrote
   set if some use reached writes it */
static void reach(limba_lxs *S, const use_index *x, uint32_t who,
                  uint32_t *first, uint8_t *wrote, limba_sym *todo)
{
    uint32_t n = 0;
    todo[n++] = who;
    while (n) {
        uint32_t k = use_key(x, todo[--n]);
        for (uint32_t j = x->off[k]; j < x->off[k + 1]; j++) {
            uint32_t i = x->idx[j];
            const limba_lxs_use *u = &S->uses[i];
            limba_sym s = u->what;
            if (u->write && wrote)
                wrote[s] = 1;
            if (first[s])
                continue;
            first[s] = i + 1;
            if (S->st.sym[s].kind == LIMBA_LSYM_ROUTINE)
                todo[n++] = s;
        }
    }
}

static void describe(limba_lxs *S, char *buf, size_t cap, uint32_t u,
                     uint32_t v, limba_sym x, uint32_t use)
{
    const char *a, *b, *c;
    uint32_t na, nb, nc;
    name_of(S, u, &a, &na);
    name_of(S, v, &b, &nb);
    if (!x) {
        snprintf(buf, cap, "%.*s after %.*s (its interface uses it)", (int)na,
                 a, (int)nb, b);
        return;
    }
    size_t n;
    c = lxs_spell(S, x, &n);
    nc = (uint32_t)n;
    limba_where w = {0};
    const char *file = "";
    if (use &&
        limba_source_where(S->src, S->t->node[S->uses[use - 1].node].loc, &w))
        file = S->src->file[w.file].path;
    snprintf(buf, cap,
             "%.*s after %.*s (the initialisation of %.*s can reach "
             "%.*s.%.*s at %s:%u:%u)",
             (int)na, a, (int)nb, b, (int)na, a, (int)nb, b, (int)nc, c, file,
             w.line, w.col);
}

static void init_order(limba_lxs *S, const use_index *ux)
{
    uint32_t nu = S->nunits, ns = S->st.nsym + 1;
    /* per unit: what its initialisation reaches; what it writes */
    uint32_t **first = limba_xcalloc(nu, sizeof(*first));
    uint8_t **wrote = limba_xcalloc(nu, sizeof(*wrote));
    limba_sym *todo = limba_xcalloc(S->nuses + 2, sizeof(*todo));
    for (uint32_t u = 0; u < nu; u++) {
        first[u] = limba_xcalloc(ns, sizeof(**first));
        wrote[u] = limba_xcalloc(ns, 1);
        reach(S, ux, LXS_INIT | u, first[u], wrote[u], todo);
        /* its variables with an initial value computed at run time */
        for (limba_sym s = 1; s < ns; s++)
            if (var_unit(S, s) == u && !ready(S, s) &&
                S->t->node[S->st.sym[s].node].c)
                wrote[u][s] = 1;
    }
    /* U after V: V in the uses of the interface of U, or the
       initialisation of U reaches a variable that the one of V writes;
       why[u * nu + v]: the variable, or 0 for a use of the interface */
    uint8_t *after = limba_xcalloc((size_t)nu * nu, 1);
    limba_sym *why = limba_xcalloc((size_t)nu * nu, sizeof(*why));
    for (uint32_t u = 0; u < nu; u++) {
        if (is_program(S, u))
            continue;
        uint32_t l = uses_list(S, u, false);
        for (uint32_t i = 0; l && i < S->t->node[l].b; i++) {
            uint32_t v = S->t->node[limba_lx_list_at(S->t, l, i)].b;
            if (v && v - 1 != u)
                after[u * nu + v - 1] = 1;
        }
        for (uint32_t v = 0; v < nu; v++) {
            if (v == u || is_program(S, v))
                continue;
            for (limba_sym s = 1; s < ns; s++)
                if (first[u][s] && wrote[v][s] &&
                    S->st.sym[s].kind == LIMBA_LSYM_VAR) {
                    if (!after[u * nu + v])
                        why[u * nu + v] = s;
                    after[u * nu + v] = 1;
                    break;
                }
        }
    }
    /* the order of the uses, depth first from the file compiled: the
       units a unit uses, as written, before it */
    uint32_t *post = limba_xcalloc(nu, sizeof(*post));
    uint8_t *seen = limba_xcalloc(nu, 1);
    uint32_t *stk = limba_xcalloc(nu * 2 + 2, sizeof(*stk));
    uint32_t np = 0, sp = 0;
    stk[sp++] = S->main;
    stk[sp++] = 0;
    seen[S->main] = 1;
    while (sp) {
        uint32_t k = stk[--sp], u = stk[--sp];
        uint32_t ref = visible_ref(S, u, true, k);
        if (!ref) {
            post[u] = np++;
            continue;
        }
        stk[sp++] = u;
        stk[sp++] = k + 1;
        uint32_t v = S->t->node[ref].b;
        if (v && !seen[v - 1]) {
            seen[v - 1] = 1;
            stk[sp++] = v - 1;
            stk[sp++] = 0;
        }
    }
    for (uint32_t u = 0; u < nu; u++)
        if (!seen[u])
            post[u] = np++;
    /* placed one by one: of those whose constraints are met, the first
       in that order */
    uint8_t *placed = limba_xcalloc(nu, 1);
    uint32_t done = 0, units = 0;
    for (uint32_t u = 0; u < nu; u++)
        units += !is_program(S, u);
    while (done < units) {
        uint32_t pick = UINT32_MAX;
        for (uint32_t u = 0; u < nu; u++) {
            if (placed[u] || is_program(S, u))
                continue;
            bool ok = true;
            for (uint32_t v = 0; v < nu && ok; v++)
                if (after[u * nu + v] && !placed[v] && !is_program(S, v))
                    ok = false;
            if (ok && (pick == UINT32_MAX || post[u] < post[pick]))
                pick = u;
        }
        if (pick == UINT32_MAX) {
            /* a circle: follow the constraints from a unit left */
            uint32_t u = 0;
            while (placed[u] || is_program(S, u))
                u++;
            uint32_t *path = limba_xcalloc(nu + 1, sizeof(*path));
            uint8_t *on = limba_xcalloc(nu, 1);
            uint32_t len = 0;
            while (!on[u]) {
                on[u] = 1;
                path[len++] = u;
                uint32_t v = 0;
                while (!(after[u * nu + v] && !placed[v] && !is_program(S, v)))
                    v++;
                u = v;
            }
            uint32_t from = 0;
            while (path[from] != u)
                from++;
            char msg[1024];
            size_t at = 0;
            for (uint32_t k = from; k < len && at < sizeof(msg) - 200; k++) {
                uint32_t a = path[k], b = k + 1 < len ? path[k + 1] : u;
                char part[400];
                limba_sym x = why[a * nu + b];
                describe(S, part, sizeof(part), a, b, x, x ? first[a][x] : 0);
                at += (size_t)snprintf(msg + at, sizeof(msg) - at, "%s%s",
                                       k > from ? "; " : "", part);
            }
            uint32_t at_node = S->t->node[S->units[path[from]].root].a;
            lxs_error(S, LXE_INIT_ORDER, at_node ? at_node : S->units[u].root,
                      "the initialisations need each other: %s; the "
                      "constraint comes from the text, also from a branch "
                      "that never runs: move the code",
                      msg);
            free(path);
            free(on);
            for (uint32_t k = 0; k < nu; k++)
                if (!placed[k] && !is_program(S, k)) {
                    placed[k] = 1;
                    S->units[k].order = done++;
                }
            break;
        }
        placed[pick] = 1;
        S->units[pick].order = done++;
    }
    for (uint32_t u = 0; u < nu; u++)
        if (is_program(S, u))
            S->units[u].order = done++;
    for (uint32_t u = 0; u < nu; u++) {
        free(first[u]);
        free(wrote[u]);
    }
    free(first);
    free(wrote);
    free(todo);
    free(after);
    free(why);
    free(post);
    free(seen);
    free(stk);
    free(placed);
}

/* ---- the units, checked ---- */

void lxs_check_units(limba_lxs *S)
{
    uint32_t nu = S->nunits;
    S->named = limba_xcalloc((size_t)nu * nu, 1);
    /* the scopes */
    for (uint32_t u = 0; u < nu; u++) {
        limba_lxs_unit *f = &S->units[u];
        if (is_program(S, u)) {
            f->impl = lxs_scope_new(S, S->universe, LXS_PROGRAM);
            S->sunit[f->impl] = (u + 1) | 0x80000000u;
        } else {
            f->intf = lxs_scope_new(S, S->universe, LXS_PROGRAM);
            S->sunit[f->intf] = u + 1;
            f->impl = lxs_scope_new(S, f->intf, LXS_PROGRAM);
            S->sunit[f->impl] = (u + 1) | 0x80000000u;
        }
    }
    for (uint32_t u = 0; u < nu; u++) {
        check_uses(S, u);
        qualify(S, u);
        /* a unit named as a name of the language */
        const limba_lx_node *r = &S->t->node[S->units[u].root];
        if (r->kind == LXN_UNIT && r->a &&
            limba_sym_local(&S->st, S->universe, S->t->node[r->a].a)) {
            const char *a;
            uint32_t na;
            name_of(S, u, &a, &na);
            lxs_error(S, LXE_UNIT_NAME, r->a,
                      "'%.*s' is a name of the language: a unit cannot take "
                      "it",
                      (int)na, a);
        }
    }
    uint8_t *colour = limba_xcalloc(nu, 1);
    uint32_t *path = limba_xcalloc(nu + 1, sizeof(*path));
    for (uint32_t u = 0; u < nu; u++)
        if (!colour[u])
            cycle_from(S, u, colour, path, 0);
    free(colour);
    free(path);
}

/* under the program's restrictions(no_external), a routine of C of the
   library reached: an error at the first call of the chain, which the
   message tells (§ 10.4) */
static void library_c(limba_lxs *S, const uint32_t *first, uint32_t ns)
{
    for (limba_sym x = 1; x < ns; x++) {
        const limba_symbol *y = &S->st.sym[x];
        if (!first[x] || y->kind != LIMBA_LSYM_ROUTINE || !y->node)
            continue;
        const limba_lx_node *r = &S->t->node[y->node];
        if (!r->d || S->t->node[r->d].kind != LXN_EXTERNAL)
            continue;
        bool impl;
        if (!S->units[lxs_unit_of(S, y->scope, &impl)].library)
            continue;
        /* the chain, from the routine of C back to an initialisation */
        char chain[600];
        size_t at = 0;
        uint32_t use = first[x] - 1, node = S->uses[use].node;
        size_t n;
        const char *sp = lxs_spell(S, x, &n);
        at += (size_t)snprintf(chain, sizeof(chain), "the routine of C '%.*s'",
                               (int)n, sp);
        for (int depth = 0; depth < 16; depth++) {
            uint32_t who = S->uses[use].who;
            node = S->uses[use].node;
            if (who & LXS_INIT) {
                const char *a;
                uint32_t na;
                name_of(S, who & ~LXS_INIT, &a, &na);
                if (at < sizeof(chain))
                    at += (size_t)snprintf(
                        chain + at, sizeof(chain) - at,
                        is_program(S, who & ~LXS_INIT)
                            ? ", reached from the program"
                            : ", reached from the initialisation of %.*s",
                        (int)na, a);
                break;
            }
            sp = lxs_spell(S, who, &n);
            if (at < sizeof(chain))
                at += (size_t)snprintf(chain + at, sizeof(chain) - at,
                                       ", called by %.*s", (int)n, sp);
            if (!first[who])
                break;
            use = first[who] - 1;
        }
        lxs_error(S, LXE_LIBRARY_C, node,
                  "pragma restrictions(no_external) forbids the boundary "
                  "with C, which the library crosses here: %s",
                  chain);
    }
}

/* a pragma hides that hid nothing; a unit used and never named */
void lxs_unit_warnings(limba_lxs *S)
{
    hides_check(S);
    for (uint32_t i = 0; i < S->nhides; i++)
        if (!S->hid[i]) {
            const limba_lx_node *h = &S->t->node[S->hides[i]];
            const char *use = lxs_text_at(S, h->loc);
            uint32_t n = lxs_ident_len(use);
            limba_report_add(S->rep, LIMBA_WARNING, LXE_HIDES_NOTHING, h->loc,
                             n,
                             "pragma hides: no declaration of the file hides "
                             "'%.*s'",
                             (int)n, use);
        }
    for (uint32_t u = 0; u < S->nunits; u++)
        for (uint32_t k = 0;; k++) {
            uint32_t ref = visible_ref(S, u, true, k);
            if (!ref)
                break;
            uint32_t v = S->t->node[ref].b;
            if (v && !S->named[u * S->nunits + v - 1]) {
                const char *a;
                uint32_t na;
                name_of(S, v - 1, &a, &na);
                limba_report_add(S->rep, LIMBA_WARNING, LXE_UNIT_UNUSED,
                                 S->t->node[ref].loc, na,
                                 "'%.*s' is used but never named", (int)na, a);
            }
        }
    use_index ux;
    index_uses(S, &ux);
    /* what the initialisations reach: the routines and variables kept */
    uint32_t ns = ux.ns;
    uint32_t *first = limba_xcalloc(ns, sizeof(*first));
    limba_sym *todo = limba_xcalloc(S->nuses + 2, sizeof(*todo));
    for (uint32_t u = 0; u < S->nunits; u++)
        reach(S, &ux, LXS_INIT | u, first, NULL, todo);
    S->reached = limba_xcalloc(ns, 1);
    for (limba_sym s = 1; s < ns; s++)
        S->reached[s] = first[s] != 0;
    if (S->no_external)
        library_c(S, first, ns);
    free(first);
    free(todo);
    if (S->nunits > 1)
        init_order(S, &ux);
    else
        S->units[0].order = 0;
    free(ux.off);
    free(ux.idx);
}

void lxs_unit_free(limba_lxs *S)
{
    free(S->units);
    free(S->sunit);
    free(S->named);
    free(S->heading);
    free(S->uses);
    free(S->hides);
    free(S->hid);
    free(S->reached);
}
