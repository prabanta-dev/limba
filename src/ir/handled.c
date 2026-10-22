/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * handled.c - the set "under a handler" (progetto_ir.md § 11i): the
 * functions an exception may cross on its way to a handler. Its roots are
 * the functions with a protected block and the bodies given to par_for;
 * then whatever a function of the set calls, directly, or through call.ind
 * of a signature, every function of that signature whose address the
 * module takes (faddr, a FUNC relocation of the data). The whole program
 * is in the module. One byte a function in m->handled.
 *
 * No one asks for it to be made: limba_module_handled makes it when the
 * IR changed since the last time, which every function of Limba that
 * changes what the set looks at says (limba_handled_touch: a stamp of
 * the process). A body that limba_func_clear frees leaves a summary in
 * its function: the calls, the addresses and whether it has a handler
 * are all the set needs.
 */
#include "internal.h"

#include "common/xalloc.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* bumped by every change of the IR the set sees, in any module: a set
   made at another stamp is made again */
static _Atomic uint64_t stamp = 1;

void limba_handled_touch(void)
{
    atomic_fetch_add_explicit(&stamp, 1, memory_order_relaxed);
}

struct limba_fsum {
    bool noted;
    bool has_handler;
    limba_id *callees; /* direct calls */
    uint32_t ncallees, capcallees;
    limba_id *taken; /* faddr */
    uint32_t ntaken, captaken;
    limba_id *sigs; /* types of call.ind */
    uint32_t nsigs, capsigs;
    limba_id *bodies; /* faddr given to par_for; LIMBA_NONE: unknown */
    uint32_t nbodies, capbodies;
};

static void push(limba_id **a, uint32_t *n, uint32_t *cap, limba_id v)
{
    for (uint32_t i = 0; i < *n; i++)
        if ((*a)[i] == v)
            return;
    LIMBA_GROW(*a, *n, *cap);
    (*a)[(*n)++] = v;
}

static void sum_free(struct limba_fsum *s)
{
    free(s->callees);
    free(s->taken);
    free(s->sigs);
    free(s->bodies);
    memset(s, 0, sizeof(*s));
}

/* what the set needs of the body of f */
static void summarise(const limba_func *f, struct limba_fsum *s)
{
    memset(s, 0, sizeof(*s));
    for (uint32_t b = 0; b < f->nblocks; b++)
        if (f->blocks[b].handler)
            s->has_handler = true;
    for (uint32_t b = 0; b < f->nblocks; b++)
        for (uint32_t k = 0; k < f->blocks[b].ninsts; k++) {
            const limba_inst *in = &f->insts[f->blocks[b].insts[k]];
            const uint32_t *o = f->operands + in->first;
            switch (in->op) {
            case LIMBA_OP_CALL:
                push(&s->callees, &s->ncallees, &s->capcallees,
                     (limba_id)in->imm);
                break;
            case LIMBA_OP_FADDR:
                push(&s->taken, &s->ntaken, &s->captaken, (limba_id)in->imm);
                break;
            case LIMBA_OP_CALLIND:
                push(&s->sigs, &s->nsigs, &s->capsigs, (limba_id)in->imm);
                break;
            case LIMBA_OP_CALLRT:
                if (in->imm == LIMBA_RT_PAR_FOR && in->nops >= 1) {
                    /* the body: a faddr of this function, or unknown */
                    const limba_inst *d =
                        o[0] < f->ninsts ? &f->insts[o[0]] : NULL;
                    push(&s->bodies, &s->nbodies, &s->capbodies,
                         d && d->op == LIMBA_OP_FADDR ? (limba_id)d->imm
                                                      : LIMBA_NONE);
                }
                break;
            }
        }
    s->noted = true;
}

void limba_handled_summary(limba_func *f)
{
    if (!f->nblocks) /* nothing new to keep */
        return;
    if (!f->sum)
        f->sum = limba_xcalloc(1, sizeof(*f->sum));
    sum_free(f->sum);
    summarise(f, f->sum);
}

void limba_handled_drop(limba_func *f)
{
    if (f->sum) {
        sum_free(f->sum);
        free(f->sum);
        f->sum = NULL;
    }
}

/* fn(ptr, i64) -> void, the type of a body of par_for */
static bool body_type(const limba_module *m, limba_id t)
{
    if (t >= m->ntypes || m->types[t].kind != LIMBA_TK_FUNC)
        return false;
    const limba_type *ty = &m->types[t];
    return ty->elem == LIMBA_T_VOID && ty->count == 2 && !ty->variadic &&
           m->members[ty->first].type == LIMBA_T_PTR &&
           m->members[ty->first + 1].type == LIMBA_T_I64;
}

static void compute(limba_module *m)
{
    uint32_t n = m->nfuncs;
    struct limba_fsum *sum = limba_xcalloc(n + 1, sizeof(*sum));
    for (uint32_t i = 0; i < n; i++) {
        const limba_func *f = &m->funcs[i];
        if (f->nblocks || !f->sum)
            summarise(f, &sum[i]);
    }
    const struct limba_fsum *at = sum;
#define SUM(i)                                                                 \
    (!m->funcs[i].nblocks && m->funcs[i].sum ? m->funcs[i].sum : &at[i])
    /* the functions whose address the module takes */
    uint8_t *taken = limba_xcalloc(n + 1, 1);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < SUM(i)->ntaken; k++)
            if (SUM(i)->taken[k] < n)
                taken[SUM(i)->taken[k]] = 1;
    for (uint32_t g = 0; g < m->nglobals; g++) {
        const limba_initdata *d = limba_global_data(m, g);
        for (uint32_t k = 0; d && k < d->nrelocs; k++)
            if (d->relocs[k].kind == LIMBA_RELOC_FUNC && d->relocs[k].id < n)
                taken[d->relocs[k].id] = 1;
    }
    free(m->handled);
    m->handled = limba_xcalloc(n + 1, 1);
    m->nhandled = n;
    limba_id *work = limba_xmalloc((n + 1) * sizeof(*work));
    uint32_t nwork = 0;
#define ADD(f)                                                                 \
    do {                                                                       \
        limba_id f_ = (f);                                                     \
        if (f_ < n && !m->handled[f_]) {                                       \
            m->handled[f_] = 1;                                                \
            work[nwork++] = f_;                                                \
        }                                                                      \
    } while (0)
    /* the roots */
    for (uint32_t i = 0; i < n; i++) {
        if (SUM(i)->has_handler)
            ADD(i);
        for (uint32_t k = 0; k < SUM(i)->nbodies; k++) {
            if (SUM(i)->bodies[k] != LIMBA_NONE) {
                ADD(SUM(i)->bodies[k]);
                continue;
            }
            /* a body not known: every function taken of its type */
            for (uint32_t g = 0; g < n; g++)
                if (taken[g] && body_type(m, m->funcs[g].type))
                    ADD(g);
        }
    }
    /* the closure */
    while (nwork) {
        limba_id f = work[--nwork];
        const struct limba_fsum *s = SUM(f);
        for (uint32_t k = 0; k < s->ncallees; k++)
            ADD(s->callees[k]);
        for (uint32_t k = 0; k < s->nsigs; k++)
            for (uint32_t g = 0; g < n; g++)
                if (taken[g] && m->funcs[g].type == s->sigs[k])
                    ADD(g);
    }
#undef ADD
#undef SUM
    free(work);
    free(taken);
    for (uint32_t i = 0; i < n; i++)
        sum_free(&sum[i]);
    free(sum);
}

bool limba_module_handled(const limba_module *m, limba_id fid)
{
    uint64_t now = atomic_load_explicit(&stamp, memory_order_relaxed);
    if (!m->handled || m->handled_stamp != now || m->nhandled != m->nfuncs) {
        /* a cache of the module, made again: the module itself does not
           change */
        limba_module *w = (limba_module *)m;
        compute(w);
        w->handled_stamp = now;
    }
    return fid < m->nfuncs && m->handled[fid];
}
