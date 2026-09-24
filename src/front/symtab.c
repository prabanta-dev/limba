/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * symtab.c - scopes and names (see symtab.h).
 */
#include "symtab.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

void limba_symtab_init(limba_symtab *st)
{
    memset(st, 0, sizeof(*st));
    st->index = limba_hash_new();
    LIMBA_GROW(st->sym, st->nsym, st->capsym);
    memset(&st->sym[st->nsym++], 0, sizeof(*st->sym));
    LIMBA_GROW(st->scope, st->nscope, st->capscope);
    memset(&st->scope[st->nscope++], 0, sizeof(*st->scope));
}

void limba_symtab_free(limba_symtab *st)
{
    free(st->sym);
    free(st->scope);
    limba_hash_free(st->index);
    memset(st, 0, sizeof(*st));
}

uint32_t limba_scope_new(limba_symtab *st, uint32_t parent, uint32_t kind)
{
    LIMBA_GROW(st->scope, st->nscope, st->capscope);
    st->scope[st->nscope] = (limba_scope){parent, kind};
    return st->nscope++;
}

static uint64_t key(uint32_t scope, uint32_t name)
{
    uint64_t k = (uint64_t)scope << 32 | name;
    return limba_fnv(&k, sizeof(k), LIMBA_FNV_SEED);
}

struct probe {
    const limba_symtab *st;
    uint32_t scope, name;
};

static bool same(const void *ctx, uint32_t id)
{
    const struct probe *p = ctx;
    return p->st->sym[id].scope == p->scope && p->st->sym[id].name == p->name;
}

limba_sym limba_sym_local(const limba_symtab *st, uint32_t scope, uint32_t name)
{
    struct probe p = {st, scope, name};
    uint32_t id = limba_hash_find(st->index, key(scope, name), same, &p);
    return id == UINT32_MAX ? 0 : id;
}

limba_sym limba_sym_lookup(const limba_symtab *st, uint32_t scope,
                           uint32_t name)
{
    for (; scope; scope = st->scope[scope].parent) {
        limba_sym s = limba_sym_local(st, scope, name);
        if (s)
            return s;
    }
    return 0;
}

limba_sym limba_sym_declare(limba_symtab *st, uint32_t scope, uint32_t name,
                            unsigned kind, limba_loc loc, uint32_t len,
                            limba_sym *dup)
{
    limba_sym old = limba_sym_local(st, scope, name);
    if (dup)
        *dup = old;
    if (old)
        return 0;
    LIMBA_GROW(st->sym, st->nsym, st->capsym);
    limba_sym s = st->nsym++;
    limba_symbol *y = &st->sym[s];
    memset(y, 0, sizeof(*y));
    y->kind = (uint8_t)kind;
    y->name = name;
    y->scope = scope;
    y->loc = loc;
    y->len = len;
    limba_hash_put(st->index, key(scope, name), s);
    return s;
}
