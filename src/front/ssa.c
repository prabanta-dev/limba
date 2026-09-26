/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ssa.c - SSA construction after Braun et al. (see ssa.h).
 */
#include "ssa.h"

#include "common/hash.h"
#include "common/xalloc.h"
#include "ir/internal.h"

#include <stdlib.h>
#include <string.h>

enum { T_NONE, T_BR, T_CBR, T_SWITCH, T_RET, T_UNREACHABLE };

typedef struct {
    limba_id from, to;
    uint32_t *args;
    uint32_t nargs, cap;
} edge;

typedef struct {
    uint8_t sealed, term;
    limba_id value;  /* cbr: condition; switch: value; ret: value */
    uint32_t e0, e1; /* br: e0; cbr: e0 then, e1 else; switch: e0 */
    uint32_t case_first, ncases;
    uint32_t *preds; /* edges */
    uint32_t npreds, cappreds;
    uint32_t *inc; /* incomplete parameters: pairs (var, param index) */
    uint32_t ninc, capinc;
    uint32_t nparams; /* parameters made by the builder */
} bstate;

typedef struct {
    int64_t value;
    uint32_t edge;
} scase;

typedef struct {
    uint32_t var;
    limba_id block, value;
} defent;

typedef struct {
    limba_id value, block;
    uint32_t var;
    uint32_t pos; /* among the parameters of its block */
} pinfo;

typedef struct {
    limba_id value, block;
    uint32_t tag;
} rd;

struct limba_ssa {
    limba_module *m;
    limba_id fid;
    uint32_t base_params; /* parameters of the entry: the function's */
    limba_id *vtype;
    uint32_t nvars, capvars;
    bstate *b;
    uint32_t nb, capb;
    edge *e;
    uint32_t ne, cape;
    scase *cases;
    uint32_t ncases, capcases;
    defent *defs;
    uint32_t ndefs, capdefs;
    limba_hash *defidx;
    pinfo *params;
    uint32_t nparams, capparams;
    /* per value: 0, a parameter index + 1, or UNDEF_MARK */
    uint32_t *vinfo;
    uint32_t nvinfo;
    rd *reads;
    uint32_t nreads, capreads;
};

#define UNDEF_MARK UINT32_MAX

limba_func *limba_ssa_func(limba_ssa *s)
{
    return &s->m->funcs[s->fid];
}

static void grow_blocks(limba_ssa *s, uint32_t n)
{
    while (s->nb < n) {
        LIMBA_GROW(s->b, s->nb, s->capb);
        memset(&s->b[s->nb++], 0, sizeof(bstate));
    }
}

limba_ssa *limba_ssa_new(limba_module *m, limba_id func)
{
    limba_ssa *s = limba_xcalloc(1, sizeof(*s));
    s->m = m;
    s->fid = func;
    s->defidx = limba_hash_new();
    limba_func *f = limba_ssa_func(s);
    grow_blocks(s, f->nblocks);
    s->b[0].sealed = 1;
    s->base_params = f->blocks[0].nparams;
    return s;
}

void limba_ssa_free(limba_ssa *s)
{
    if (!s)
        return;
    for (uint32_t i = 0; i < s->nb; i++) {
        free(s->b[i].preds);
        free(s->b[i].inc);
    }
    for (uint32_t i = 0; i < s->ne; i++)
        free(s->e[i].args);
    free(s->b);
    free(s->e);
    free(s->cases);
    free(s->defs);
    limba_hash_free(s->defidx);
    free(s->params);
    free(s->vinfo);
    free(s->reads);
    free(s->vtype);
    free(s);
}

static void set_vinfo(limba_ssa *s, limba_id v, uint32_t info)
{
    if (v >= s->nvinfo) {
        uint32_t n = v + 64;
        s->vinfo = limba_xrealloc(s->vinfo, n, sizeof(*s->vinfo));
        memset(s->vinfo + s->nvinfo, 0, (n - s->nvinfo) * sizeof(*s->vinfo));
        s->nvinfo = n;
    }
    s->vinfo[v] = info;
}

static uint32_t vinfo(const limba_ssa *s, limba_id v)
{
    return v < s->nvinfo ? s->vinfo[v] : 0;
}

uint32_t limba_ssa_var(limba_ssa *s, limba_id type)
{
    LIMBA_GROW(s->vtype, s->nvars, s->capvars);
    s->vtype[s->nvars] = type;
    return s->nvars++;
}

limba_id limba_ssa_block(limba_ssa *s)
{
    limba_id b = limba_block_add(limba_ssa_func(s));
    grow_blocks(s, b + 1);
    return b;
}

/* ---- definitions ---- */

struct probe {
    const limba_ssa *s;
    uint32_t var;
    limba_id block;
};

static bool same_def(const void *ctx, uint32_t id)
{
    const struct probe *p = ctx;
    return p->s->defs[id].var == p->var && p->s->defs[id].block == p->block;
}

static uint64_t def_key(uint32_t var, limba_id block)
{
    uint64_t k = (uint64_t)var << 32 | block;
    return limba_fnv(&k, sizeof(k), LIMBA_FNV_SEED);
}

void limba_ssa_def(limba_ssa *s, uint32_t var, limba_id block, limba_id value)
{
    struct probe p = {s, var, block};
    uint32_t id = limba_hash_find(s->defidx, def_key(var, block), same_def, &p);
    if (id != UINT32_MAX) {
        s->defs[id].value = value;
        return;
    }
    LIMBA_GROW(s->defs, s->ndefs, s->capdefs);
    s->defs[s->ndefs] = (defent){var, block, value};
    limba_hash_put(s->defidx, def_key(var, block), s->ndefs++);
}

static limba_id find_def(const limba_ssa *s, uint32_t var, limba_id block)
{
    struct probe p = {s, var, block};
    uint32_t id = limba_hash_find(s->defidx, def_key(var, block), same_def, &p);
    return id == UINT32_MAX ? LIMBA_NONE : s->defs[id].value;
}

static limba_id make_undef(limba_ssa *s, uint32_t var)
{
    limba_id v = limba_inst_add(limba_ssa_func(s), 0, LIMBA_OP_UNDEF,
                                s->vtype[var], 0, 0, 0, NULL, 0);
    set_vinfo(s, v, UNDEF_MARK);
    return v;
}

static uint32_t new_param(limba_ssa *s, limba_id block, uint32_t var)
{
    limba_id v = limba_param_add(limba_ssa_func(s), block, s->vtype[var]);
    LIMBA_GROW(s->params, s->nparams, s->capparams);
    s->params[s->nparams] = (pinfo){v, block, var, s->b[block].nparams++};
    set_vinfo(s, v, s->nparams + 1);
    return s->nparams++;
}

static limba_id read_var(limba_ssa *s, uint32_t var, limba_id block);

static void add_args(limba_ssa *s, uint32_t pi)
{
    limba_id block = s->params[pi].block;
    uint32_t var = s->params[pi].var;
    for (uint32_t i = 0; i < s->b[block].npreds; i++) {
        uint32_t ei = s->b[block].preds[i];
        limba_id v = read_var(s, var, s->e[ei].from);
        edge *e = &s->e[ei];
        LIMBA_GROW(e->args, e->nargs, e->cap);
        e->args[e->nargs++] = v;
    }
}

static limba_id read_var(limba_ssa *s, uint32_t var, limba_id block)
{
    limba_id v = find_def(s, var, block);
    if (v != LIMBA_NONE)
        return v;
    bstate *bs = &s->b[block];
    if (!bs->sealed) {
        uint32_t pi = new_param(s, block, var);
        bs = &s->b[block];
        LIMBA_GROW(bs->inc, bs->ninc, bs->capinc);
        bs->inc[bs->ninc++] = var;
        LIMBA_GROW(bs->inc, bs->ninc, bs->capinc);
        bs->inc[bs->ninc++] = pi;
        v = s->params[pi].value;
    } else if (block == 0 || bs->npreds == 0) {
        v = make_undef(s, var);
    } else if (bs->npreds == 1) {
        v = read_var(s, var, s->e[bs->preds[0]].from);
    } else {
        /* the definition first: a loop through this block ends here */
        uint32_t pi = new_param(s, block, var);
        limba_ssa_def(s, var, block, s->params[pi].value);
        add_args(s, pi);
        return s->params[pi].value;
    }
    limba_ssa_def(s, var, block, v);
    return v;
}

limba_id limba_ssa_use(limba_ssa *s, uint32_t var, limba_id block, uint32_t tag)
{
    limba_id v = read_var(s, var, block);
    if (tag != UINT32_MAX) {
        LIMBA_GROW(s->reads, s->nreads, s->capreads);
        s->reads[s->nreads++] = (rd){v, block, tag};
    }
    return v;
}

void limba_ssa_seal(limba_ssa *s, limba_id b)
{
    if (s->b[b].sealed)
        return;
    /* incomplete parameters get their arguments, in their order */
    for (uint32_t i = 0; i < s->b[b].ninc; i += 2)
        add_args(s, s->b[b].inc[i + 1]);
    s->b[b].ninc = 0;
    s->b[b].sealed = 1;
}

/* ---- terminators ---- */

static uint32_t new_edge(limba_ssa *s, limba_id from, limba_id to)
{
    LIMBA_GROW(s->e, s->ne, s->cape);
    s->e[s->ne] = (edge){from, to, NULL, 0, 0};
    bstate *t = &s->b[to];
    LIMBA_GROW(t->preds, t->npreds, t->cappreds);
    t->preds[t->npreds++] = s->ne;
    return s->ne++;
}

void limba_ssa_br(limba_ssa *s, limba_id from, limba_id to)
{
    uint32_t e0 = new_edge(s, from, to);
    s->b[from].term = T_BR;
    s->b[from].e0 = e0;
}

void limba_ssa_cbr(limba_ssa *s, limba_id from, limba_id cond, limba_id then_,
                   limba_id else_)
{
    uint32_t e0 = new_edge(s, from, then_);
    uint32_t e1 = new_edge(s, from, else_);
    bstate *b = &s->b[from];
    b->term = T_CBR;
    b->value = cond;
    b->e0 = e0;
    b->e1 = e1;
}

void limba_ssa_switch(limba_ssa *s, limba_id from, limba_id value,
                      limba_id dflt, const int64_t *values,
                      const limba_id *targets, uint32_t n)
{
    uint32_t e0 = new_edge(s, from, dflt);
    uint32_t first = s->ncases;
    for (uint32_t i = 0; i < n; i++) {
        /* one edge per target: its values come from the same block */
        uint32_t ei = UINT32_MAX;
        for (uint32_t k = first; k < s->ncases; k++)
            if (s->e[s->cases[k].edge].to == targets[i])
                ei = s->cases[k].edge;
        if (ei == UINT32_MAX)
            ei = new_edge(s, from, targets[i]);
        LIMBA_GROW(s->cases, s->ncases, s->capcases);
        s->cases[s->ncases++] = (scase){values[i], ei};
    }
    bstate *b = &s->b[from];
    b->term = T_SWITCH;
    b->value = value;
    b->e0 = e0;
    b->case_first = first;
    b->ncases = n;
}

void limba_ssa_ret(limba_ssa *s, limba_id from, limba_id value)
{
    s->b[from].term = T_RET;
    s->b[from].value = value;
}

void limba_ssa_unreachable(limba_ssa *s, limba_id from)
{
    s->b[from].term = T_UNREACHABLE;
}

bool limba_ssa_terminated(const limba_ssa *s, limba_id b)
{
    return s->b[b].term != T_NONE;
}

/* ---- the end ---- */

typedef struct {
    limba_ssa *s;
    uint8_t *reach;
    limba_id *rep;  /* per parameter: its replacement, or LIMBA_NONE */
    uint8_t *state; /* may_undef: 0 unknown, 1 visiting, 2 no, 3 yes */
    /* the parameters of each block, in order: those of block b are
       bparam[bfirst[b] .. bfirst[b + 1]) */
    uint32_t *bfirst, *bparam;
} fin;

static limba_id resolve(fin *F, limba_id v)
{
    for (;;) {
        uint32_t i = vinfo(F->s, v);
        if (i == 0 || i == UNDEF_MARK || F->rep[i - 1] == LIMBA_NONE)
            return v;
        v = F->rep[i - 1];
    }
}

static bool may_undef(fin *F, limba_id v)
{
    v = resolve(F, v);
    uint32_t i = vinfo(F->s, v);
    if (i == UNDEF_MARK)
        return true;
    if (i == 0)
        return false;
    uint32_t pi = i - 1;
    if (F->state[pi])
        return F->state[pi] == 3;
    F->state[pi] = 1;
    const pinfo *p = &F->s->params[pi];
    const bstate *b = &F->s->b[p->block];
    bool yes = false;
    for (uint32_t k = 0; k < b->npreds && !yes; k++) {
        const edge *e = &F->s->e[b->preds[k]];
        if (F->reach[e->from] && p->pos < e->nargs)
            yes = may_undef(F, e->args[p->pos]);
    }
    F->state[pi] = yes ? 3 : 2;
    return yes;
}

static void reachability(limba_ssa *s, uint8_t *reach)
{
    uint32_t *stack = limba_xmalloc((s->nb + 1) * sizeof(*stack));
    uint32_t n = 0;
    reach[0] = 1;
    stack[n++] = 0;
    while (n) {
        limba_id b = stack[--n];
        const bstate *bs = &s->b[b];
        uint32_t out[2] = {bs->e0, bs->e1}, nout = 0;
        if (bs->term == T_BR)
            nout = 1;
        else if (bs->term == T_CBR)
            nout = 2;
        else if (bs->term == T_SWITCH)
            nout = 1;
        for (uint32_t k = 0; k < nout; k++) {
            limba_id t = s->e[out[k]].to;
            if (!reach[t]) {
                reach[t] = 1;
                stack[n++] = t;
            }
        }
        if (bs->term == T_SWITCH)
            for (uint32_t k = 0; k < bs->ncases; k++) {
                limba_id t = s->e[s->cases[bs->case_first + k].edge].to;
                if (!reach[t]) {
                    reach[t] = 1;
                    stack[n++] = t;
                }
            }
    }
    free(stack);
}

/* the operands of an edge: block, number, the arguments of the live
   parameters */
static void push_target(fin *F, uint32_t **ops, uint32_t *n, uint32_t *cap,
                        uint32_t ei)
{
    const edge *e = &F->s->e[ei];
    limba_id to = e->to;
    LIMBA_GROW(*ops, *n, *cap);
    (*ops)[(*n)++] = to;
    uint32_t count_at = *n;
    LIMBA_GROW(*ops, *n, *cap);
    (*ops)[(*n)++] = 0;
    uint32_t count = 0;
    for (uint32_t k = F->bfirst[to]; k < F->bfirst[to + 1]; k++) {
        uint32_t i = F->bparam[k];
        const pinfo *p = &F->s->params[i];
        if (F->rep[i] != LIMBA_NONE)
            continue;
        LIMBA_GROW(*ops, *n, *cap);
        (*ops)[(*n)++] = p->pos < e->nargs ? resolve(F, e->args[p->pos]) : 0;
        count++;
    }
    (*ops)[count_at] = count;
}

void limba_ssa_finish(limba_ssa *s, void (*undefined)(void *ctx, uint32_t tag),
                      void *ctx)
{
    limba_edit ed;
    limba_ssa_finish_edit(s, undefined, ctx, &ed);
    limba_edit_end(&ed);
}

void limba_ssa_finish_edit(limba_ssa *s,
                           void (*undefined)(void *ctx, uint32_t tag),
                           void *ctx, limba_edit *e)
{
    fin F = {s,
             limba_xcalloc(s->nb + 1, 1),
             limba_xmalloc((s->nparams + 1) * sizeof(limba_id)),
             limba_xcalloc(s->nparams + 1, 1),
             NULL,
             NULL};
    reachability(s, F.reach);
    for (uint32_t i = 0; i < s->nparams; i++)
        F.rep[i] = LIMBA_NONE;
    F.bfirst = limba_xcalloc((size_t)s->nb + 2, sizeof(uint32_t));
    F.bparam = limba_xmalloc(((size_t)s->nparams + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < s->nparams; i++)
        F.bfirst[s->params[i].block + 1]++;
    for (uint32_t b = 0; b < s->nb; b++)
        F.bfirst[b + 1] += F.bfirst[b];
    {
        uint32_t *fill = limba_xmalloc(((size_t)s->nb + 1) * sizeof(uint32_t));
        memcpy(fill, F.bfirst, (size_t)s->nb * sizeof(uint32_t));
        for (uint32_t i = 0; i < s->nparams; i++)
            F.bparam[fill[s->params[i].block]++] = i;
        free(fill);
    }

    /* trivial parameters: every live argument is the same value, or the
       parameter itself */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < s->nparams; i++) {
            const pinfo *p = &s->params[i];
            if (F.rep[i] != LIMBA_NONE || !F.reach[p->block])
                continue;
            const bstate *b = &s->b[p->block];
            limba_id same = LIMBA_NONE;
            bool trivial = true;
            for (uint32_t k = 0; k < b->npreds && trivial; k++) {
                const edge *e = &s->e[b->preds[k]];
                if (!F.reach[e->from] || p->pos >= e->nargs)
                    continue;
                limba_id a = resolve(&F, e->args[p->pos]);
                if (a == p->value || a == same)
                    continue;
                if (same != LIMBA_NONE)
                    trivial = false;
                else
                    same = a;
            }
            if (!trivial)
                continue;
            F.rep[i] = same != LIMBA_NONE ? same : make_undef(s, p->var);
            changed = true;
        }
    }

    /* definite assignment */
    if (undefined) {
        uint32_t *told = NULL, ntold = 0, captold = 0;
        for (uint32_t r = 0; r < s->nreads; r++) {
            const rd *x = &s->reads[r];
            if (!F.reach[x->block] || !may_undef(&F, x->value))
                continue;
            bool seen = false;
            for (uint32_t k = 0; k < ntold && !seen; k++)
                seen = told[k] == x->tag;
            if (seen)
                continue;
            LIMBA_GROW(told, ntold, captold);
            told[ntold++] = x->tag;
            undefined(ctx, x->tag);
        }
        free(told);
    }

    /* the terminators */
    uint32_t *ops = NULL, nops = 0, capops = 0;
    for (limba_id b = 0; b < s->nb; b++) {
        if (!F.reach[b])
            continue;
        const bstate *bs = &s->b[b];
        limba_func *f = limba_ssa_func(s);
        nops = 0;
        switch (bs->term) {
        case T_BR:
            push_target(&F, &ops, &nops, &capops, bs->e0);
            limba_inst_add(f, b, LIMBA_OP_BR, LIMBA_T_VOID, 0, 0, 0, ops, nops);
            break;
        case T_CBR:
            LIMBA_GROW(ops, nops, capops);
            ops[nops++] = resolve(&F, bs->value);
            push_target(&F, &ops, &nops, &capops, bs->e0);
            push_target(&F, &ops, &nops, &capops, bs->e1);
            limba_inst_add(f, b, LIMBA_OP_CBR, LIMBA_T_VOID, 0, 0, 0, ops,
                           nops);
            break;
        case T_SWITCH:
            LIMBA_GROW(ops, nops, capops);
            ops[nops++] = resolve(&F, bs->value);
            LIMBA_GROW(ops, nops, capops);
            ops[nops++] = s->e[bs->e0].to;
            LIMBA_GROW(ops, nops, capops);
            ops[nops++] = bs->ncases;
            for (uint32_t k = 0; k < bs->ncases; k++) {
                const scase *c = &s->cases[bs->case_first + k];
                /* the value in two halves, low first */
                uint32_t v[3] = {(uint32_t)(uint64_t)c->value,
                                 (uint32_t)((uint64_t)c->value >> 32),
                                 s->e[c->edge].to};
                for (int j = 0; j < 3; j++) {
                    LIMBA_GROW(ops, nops, capops);
                    ops[nops++] = v[j];
                }
            }
            limba_inst_add(f, b, LIMBA_OP_SWITCH, LIMBA_T_VOID, 0, 0, 0, ops,
                           nops);
            break;
        case T_RET:
            if (bs->value != LIMBA_NONE) {
                LIMBA_GROW(ops, nops, capops);
                ops[nops++] = resolve(&F, bs->value);
            }
            limba_inst_add(f, b, LIMBA_OP_RET, LIMBA_T_VOID, 0, 0, 0, ops,
                           nops);
            break;
        default:
            limba_inst_add(f, b, LIMBA_OP_UNREACHABLE, LIMBA_T_VOID, 0, 0, 0,
                           NULL, 0);
        }
    }
    free(ops);

    /* parameters first in their blocks, in their order */
    limba_func *f = limba_ssa_func(s);
    uint32_t most = 0;
    for (limba_id b = 0; b < f->nblocks; b++)
        if (f->blocks[b].ninsts > most)
            most = f->blocks[b].ninsts;
    uint32_t *order = limba_xmalloc(((size_t)most + 1) * sizeof(*order));
    for (limba_id b = 0; b < f->nblocks; b++) {
        limba_block *bl = &f->blocks[b];
        uint32_t n = 0;
        for (uint32_t k = 0; k < bl->ninsts; k++)
            if (f->insts[bl->insts[k]].op == LIMBA_OP_PARAM)
                order[n++] = bl->insts[k];
        for (uint32_t k = 0; k < bl->ninsts; k++)
            if (f->insts[bl->insts[k]].op != LIMBA_OP_PARAM)
                order[n++] = bl->insts[k];
        if (n)
            memcpy(bl->insts, order, n * sizeof(*order));
    }
    free(order);

    limba_edit_begin(e, f);
    for (uint32_t i = 0; i < s->nparams; i++)
        if (F.rep[i] != LIMBA_NONE)
            limba_edit_replace(e, s->params[i].value,
                               resolve(&F, s->params[i].value));
    for (limba_id b = 0; b < f->nblocks; b++)
        if (!F.reach[b]) {
            e->dead_block[b] = 1;
            for (uint32_t k = 0; k < f->blocks[b].ninsts; k++)
                e->dead[f->blocks[b].insts[k]] = 1;
        }
    free(F.reach);
    free(F.rep);
    free(F.state);
    free(F.bfirst);
    free(F.bparam);
}
