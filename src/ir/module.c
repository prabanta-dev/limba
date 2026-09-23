/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * module.c - building a module: strings, interned types, symbols,
 * functions, blocks and instructions.
 */
#include "internal.h"

#include "common/hash.h"
#include "common/strtab.h"
#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* size and alignment of the scalar types, indexed by kind */
static const struct {
    const char *name;
    uint32_t size;
} scalars[LIMBA_T_FIRST_USER] = {
    {"void", 0}, {"i1", 1},  {"i8", 1},  {"i16", 2}, {"i32", 4}, {"i64", 8},
    {"f32", 4},  {"f64", 8}, {"ptr", 8}, {"str", 8}, {"ref", 8},
};

const char *limba_scalar_name(limba_id t)
{
    return t < LIMBA_T_FIRST_USER ? scalars[t].name : NULL;
}

limba_id limba_scalar_find(const char *s, size_t len)
{
    for (limba_id t = 0; t < LIMBA_T_FIRST_USER; t++)
        if (strlen(scalars[t].name) == len &&
            memcmp(scalars[t].name, s, len) == 0)
            return t;
    return LIMBA_NONE;
}

bool limba_type_is_int(limba_id t)
{
    return t >= LIMBA_T_I1 && t <= LIMBA_T_I64;
}

bool limba_type_is_float(limba_id t)
{
    return t == LIMBA_T_F32 || t == LIMBA_T_F64;
}

unsigned limba_type_bits(limba_id t)
{
    switch (t) {
    case LIMBA_T_I1:
        return 1;
    case LIMBA_T_I8:
        return 8;
    case LIMBA_T_I16:
        return 16;
    case LIMBA_T_I32:
        return 32;
    case LIMBA_T_I64:
        return 64;
    }
    return 0;
}

limba_module *limba_module_new(void)
{
    limba_module *m = limba_xcalloc(1, sizeof(*m));
    m->name = LIMBA_NONE;
    m->memory = LIMBA_MEM_STRICT;
    m->strings = limba_strtab_new();
    m->typeidx = limba_hash_new();
    m->symidx = limba_hash_new();
    for (limba_id t = 0; t < LIMBA_T_FIRST_USER; t++) {
        LIMBA_GROW(m->types, m->ntypes, m->captypes);
        limba_type *ty = &m->types[m->ntypes++];
        memset(ty, 0, sizeof(*ty));
        ty->kind = (uint8_t)t;
        ty->size = scalars[t].size;
        ty->align = scalars[t].size ? scalars[t].size : 1;
        ty->name = LIMBA_NONE;
        ty->elem = LIMBA_NONE;
    }
    return m;
}

void limba_module_free(limba_module *m)
{
    if (!m)
        return;
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        limba_func *f = &m->funcs[i];
        for (uint32_t b = 0; b < f->nblocks; b++)
            free(f->blocks[b].insts);
        free(f->blocks);
        free(f->insts);
        free(f->operands);
        free(f->slots);
    }
    free(m->funcs);
    free(m->globals);
    free(m->externs);
    free(m->members);
    free(m->types);
    limba_hash_free(m->typeidx);
    limba_hash_free(m->symidx);
    limba_strtab_free(m->strings);
    free(m);
}

limba_id limba_str_intern(limba_module *m, const char *s, size_t len)
{
    return limba_strtab_intern(m->strings, s, len);
}

const char *limba_str(const limba_module *m, limba_id id, size_t *len)
{
    return limba_strtab_get(m->strings, id, len);
}

uint32_t limba_str_count(const limba_module *m)
{
    return limba_strtab_count(m->strings);
}

/* ---- interned types ---- */

/* a candidate type, described the same way it would be stored */
struct type_key {
    const limba_module *m;
    limba_type t;
    const limba_member *members; /* count of them in t.count */
};

static uint64_t type_hash(const struct type_key *k)
{
    uint64_t h = LIMBA_FNV_SEED;
    h = limba_fnv(&k->t.kind, 1, h);
    if (k->t.kind == LIMBA_TK_STRUCT)
        return limba_fnv(&k->t.name, sizeof(k->t.name), h);
    h = limba_fnv(&k->t.variadic, 1, h);
    h = limba_fnv(&k->t.elem, sizeof(k->t.elem), h);
    h = limba_fnv(&k->t.count, sizeof(k->t.count), h);
    if (k->t.kind == LIMBA_TK_FUNC && k->t.count)
        h = limba_fnv(k->members, k->t.count * sizeof(*k->members), h);
    return h;
}

static bool type_same(const void *ctx, uint32_t id)
{
    const struct type_key *k = ctx;
    const limba_type *t = &k->m->types[id];
    if (t->kind != k->t.kind)
        return false;
    if (t->kind == LIMBA_TK_STRUCT)
        return t->name == k->t.name;
    if (t->variadic != k->t.variadic || t->elem != k->t.elem ||
        t->count != k->t.count)
        return false;
    if (t->kind != LIMBA_TK_FUNC)
        return true;
    for (uint32_t i = 0; i < t->count; i++)
        if (k->m->members[t->first + i].type != k->members[i].type)
            return false;
    return true;
}

static limba_id type_add(limba_module *m, const struct type_key *k, uint64_t h)
{
    LIMBA_GROW(m->types, m->ntypes, m->captypes);
    limba_id id = m->ntypes++;
    limba_type *t = &m->types[id];
    *t = k->t;
    t->first = m->nmembers;
    if (k->t.kind == LIMBA_TK_FUNC || k->t.kind == LIMBA_TK_STRUCT)
        for (uint32_t i = 0; i < k->t.count; i++) {
            LIMBA_GROW(m->members, m->nmembers, m->capmembers);
            m->members[m->nmembers++] = k->members[i];
        }
    limba_hash_put(m->typeidx, h, id);
    return id;
}

limba_id limba_type_array(limba_module *m, limba_id elem, uint32_t count)
{
    struct type_key k = {.m = m};
    k.t.kind = LIMBA_TK_ARRAY;
    k.t.name = LIMBA_NONE;
    k.t.elem = elem;
    k.t.count = count;
    uint64_t h = type_hash(&k);
    limba_id id = limba_hash_find(m->typeidx, h, type_same, &k);
    if (id != LIMBA_NONE)
        return id;
    const limba_type *e = &m->types[elem];
    uint64_t size = (uint64_t)e->size * count;
    k.t.size = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
    k.t.align = e->align ? e->align : 1;
    return type_add(m, &k, h);
}

limba_id limba_type_func(limba_module *m, limba_id ret, const limba_id *params,
                         uint32_t nparams, bool variadic)
{
    limba_member stack[16];
    limba_member *mem =
        nparams <= 16 ? stack : limba_xmalloc(nparams * sizeof(*mem));
    for (uint32_t i = 0; i < nparams; i++)
        mem[i] = (limba_member){params[i], 0};
    struct type_key k = {.m = m, .members = mem};
    k.t.kind = LIMBA_TK_FUNC;
    k.t.variadic = variadic;
    k.t.name = LIMBA_NONE;
    k.t.elem = ret;
    k.t.count = nparams;
    k.t.align = 1;
    uint64_t h = type_hash(&k);
    limba_id id = limba_hash_find(m->typeidx, h, type_same, &k);
    if (id == LIMBA_NONE)
        id = type_add(m, &k, h);
    if (mem != stack)
        free(mem);
    return id;
}

limba_id limba_type_struct(limba_module *m, limba_id name,
                           const limba_member *fields, uint32_t nfields,
                           uint32_t size, uint32_t align)
{
    struct type_key k = {.m = m, .members = fields};
    k.t.kind = LIMBA_TK_STRUCT;
    k.t.name = name;
    k.t.elem = LIMBA_NONE;
    k.t.count = nfields;
    k.t.size = size;
    k.t.align = align;
    uint64_t h = type_hash(&k);
    if (limba_hash_find(m->typeidx, h, type_same, &k) != LIMBA_NONE)
        return LIMBA_NONE;
    return type_add(m, &k, h);
}

limba_id limba_type_find_struct(const limba_module *m, limba_id name)
{
    struct type_key k = {.m = m};
    k.t.kind = LIMBA_TK_STRUCT;
    k.t.name = name;
    return limba_hash_find(m->typeidx, type_hash(&k), type_same, &k);
}

/* ---- symbols: one name space for functions, globals and externs ---- */

enum { SYM_FUNC, SYM_GLOBAL, SYM_EXTERN };

struct sym_key {
    const limba_module *m;
    limba_id name;
    int kind;
};

static uint64_t sym_hash(limba_id name, int kind)
{
    uint64_t h = limba_fnv(&name, sizeof(name), LIMBA_FNV_SEED);
    return limba_fnv(&kind, sizeof(kind), h);
}

static bool sym_same(const void *ctx, uint32_t id)
{
    const struct sym_key *k = ctx;
    switch (k->kind) {
    case SYM_FUNC:
        return k->m->funcs[id].name == k->name;
    case SYM_GLOBAL:
        return k->m->globals[id].name == k->name;
    default:
        return k->m->externs[id].name == k->name;
    }
}

static limba_id sym_find(const limba_module *m, limba_id name, int kind)
{
    struct sym_key k = {m, name, kind};
    return limba_hash_find(m->symidx, sym_hash(name, kind), sym_same, &k);
}

limba_id limba_func_find(const limba_module *m, limba_id name)
{
    return sym_find(m, name, SYM_FUNC);
}

limba_id limba_global_find(const limba_module *m, limba_id name)
{
    return sym_find(m, name, SYM_GLOBAL);
}

limba_id limba_extern_find(const limba_module *m, limba_id name)
{
    return sym_find(m, name, SYM_EXTERN);
}

/* a name is taken in all three spaces: @x means one thing */
static bool name_taken(const limba_module *m, limba_id name)
{
    return limba_func_find(m, name) != LIMBA_NONE ||
           limba_global_find(m, name) != LIMBA_NONE ||
           limba_extern_find(m, name) != LIMBA_NONE;
}

limba_id limba_global_add(limba_module *m, limba_id name, limba_id type,
                          uint32_t flags)
{
    if (name_taken(m, name))
        return LIMBA_NONE;
    LIMBA_GROW(m->globals, m->nglobals, m->capglobals);
    limba_id id = m->nglobals++;
    m->globals[id] = (limba_global){name, type, flags, LIMBA_INIT_ZERO, 0};
    limba_hash_put(m->symidx, sym_hash(name, SYM_GLOBAL), id);
    return id;
}

limba_id limba_extern_add(limba_module *m, limba_id name, limba_id type,
                          limba_id symbol, limba_id library)
{
    if (name_taken(m, name))
        return LIMBA_NONE;
    LIMBA_GROW(m->externs, m->nexterns, m->capexterns);
    limba_id id = m->nexterns++;
    m->externs[id] = (limba_extern){name, type, symbol, library};
    limba_hash_put(m->symidx, sym_hash(name, SYM_EXTERN), id);
    return id;
}

limba_id limba_func_add(limba_module *m, limba_id name, limba_id type,
                        uint32_t flags)
{
    if (name_taken(m, name))
        return LIMBA_NONE;
    LIMBA_GROW(m->funcs, m->nfuncs, m->capfuncs);
    limba_id id = m->nfuncs++;
    limba_func *f = &m->funcs[id];
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->type = type;
    f->flags = flags;
    limba_hash_put(m->symidx, sym_hash(name, SYM_FUNC), id);
    return id;
}

/* ---- inside a function ---- */

limba_id limba_block_add(limba_func *f)
{
    LIMBA_GROW(f->blocks, f->nblocks, f->capblocks);
    limba_id id = f->nblocks++;
    memset(&f->blocks[id], 0, sizeof(f->blocks[id]));
    return id;
}

limba_id limba_slot_add(limba_func *f, uint32_t size, uint32_t align)
{
    LIMBA_GROW(f->slots, f->nslots, f->capslots);
    f->slots[f->nslots] = (limba_slot){size, align};
    return f->nslots++;
}

limba_id limba_inst_add(limba_func *f, limba_id b, unsigned op, limba_id type,
                        unsigned cc, int64_t imm, int64_t imm2,
                        const uint32_t *ops, uint32_t nops)
{
    LIMBA_GROW(f->insts, f->ninsts, f->capinsts);
    limba_id id = f->ninsts++;
    limba_inst *in = &f->insts[id];
    in->op = (uint16_t)op;
    in->cc = (uint8_t)cc;
    in->pad = 0;
    in->type = type;
    in->block = b;
    in->nops = nops;
    in->first = f->noperands;
    in->imm = imm;
    in->imm2 = imm2;
    for (uint32_t i = 0; i < nops; i++) {
        LIMBA_GROW(f->operands, f->noperands, f->capoperands);
        f->operands[f->noperands++] = ops[i];
    }
    limba_block *bl = &f->blocks[b];
    LIMBA_GROW(bl->insts, bl->ninsts, bl->cap);
    bl->insts[bl->ninsts++] = id;
    if (op == LIMBA_OP_PARAM)
        bl->nparams++;
    return id;
}

limba_id limba_param_add(limba_func *f, limba_id b, limba_id t)
{
    return limba_inst_add(f, b, LIMBA_OP_PARAM, t, 0, 0, 0, NULL, 0);
}

/* ---- odds and ends shared by the other files ---- */

void limba_diag_set(limba_diag *d, unsigned line, const char *fmt, ...)
{
    if (!d)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->msg, sizeof(d->msg), fmt, ap);
    va_end(ap);
    d->line = line;
}

uint32_t limba_target(const limba_func *f, uint32_t k, limba_id *block,
                      uint32_t *nargs)
{
    *block = f->operands[k];
    *nargs = f->operands[k + 1];
    return k + 2;
}

bool limba_type_sig(const limba_module *m, limba_id t, limba_sig *s)
{
    if (t >= m->ntypes || m->types[t].kind != LIMBA_TK_FUNC)
        return false;
    const limba_type *ty = &m->types[t];
    s->ret = ty->elem;
    s->n = ty->count;
    s->variadic = ty->variadic;
    s->mem = m->members + ty->first;
    return true;
}

bool limba_call_sig(const limba_module *m, const limba_inst *in, limba_sig *s)
{
    uint64_t id = (uint64_t)in->imm;
    switch (in->op) {
    case LIMBA_OP_CALL:
        return id < m->nfuncs && limba_type_sig(m, m->funcs[id].type, s);
    case LIMBA_OP_CALLEXT:
        return id < m->nexterns && limba_type_sig(m, m->externs[id].type, s);
    case LIMBA_OP_CALLIND:
        return id < m->ntypes && limba_type_sig(m, (limba_id)id, s);
    case LIMBA_OP_CALLRT:
        return limba_rt_sig((limba_id)(id < LIMBA_RT_COUNT ? id : LIMBA_NONE),
                            s);
    }
    return false;
}
