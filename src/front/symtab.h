/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * symtab.h - scopes and the names declared in them, shared by the front
 * ends. One hash from (scope, name) to the symbol; a lookup climbs the
 * chain of parent scopes. Looking up never declares, declaring never
 * shadows within the same scope: the duplicate is returned instead.
 * Symbols are never removed: when a block ends its scope is simply no
 * longer searched, and the tree still refers to them.
 */
#ifndef LIMBA_FRONT_SYMTAB_H
#define LIMBA_FRONT_SYMTAB_H

#include "common/hash.h"
#include "source.h"
#include "types.h"

#include <stdint.h>

typedef uint32_t limba_sym; /* 0: none */

enum {
    LIMBA_LSYM_NONE,
    LIMBA_LSYM_CONST,
    LIMBA_LSYM_TYPE,
    LIMBA_LSYM_VAR,
    LIMBA_LSYM_PARAM,
    LIMBA_LSYM_ROUTINE,
    LIMBA_LSYM_BUILTIN, /* a routine of the language, not of the program */
};

typedef struct {
    uint8_t kind;
    uint8_t mode;   /* PARAM: the language's mode */
    uint16_t flags; /* the language's */
    uint32_t name;
    uint32_t scope;
    limba_loc loc; /* where it was declared, its spelling */
    uint32_t len;
    limba_ltype type;
    uint32_t value; /* the language's: a constant, a builtin id, ... */
    uint32_t node;  /* the declaring node of the tree */
} limba_symbol;

typedef struct {
    uint32_t parent; /* 0 for the outermost */
    uint32_t kind;   /* the language's */
} limba_scope;

typedef struct {
    limba_symbol *sym; /* sym[0] unused */
    uint32_t nsym, capsym;
    limba_scope *scope; /* scope[0] unused */
    uint32_t nscope, capscope;
    limba_hash *index;
} limba_symtab;

void limba_symtab_init(limba_symtab *st);
void limba_symtab_free(limba_symtab *st);

uint32_t limba_scope_new(limba_symtab *st, uint32_t parent, uint32_t kind);

/* a new symbol, or 0 with *dup set to the one already there */
limba_sym limba_sym_declare(limba_symtab *st, uint32_t scope, uint32_t name,
                            unsigned kind, limba_loc loc, uint32_t len,
                            limba_sym *dup);
/* in scope only / in scope and its parents; 0 if not found */
limba_sym limba_sym_local(const limba_symtab *st, uint32_t scope,
                          uint32_t name);
limba_sym limba_sym_lookup(const limba_symtab *st, uint32_t scope,
                           uint32_t name);

static inline limba_symbol *limba_sym_get(limba_symtab *st, limba_sym s)
{
    return &st->sym[s];
}

#endif
