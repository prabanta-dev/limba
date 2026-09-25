/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gvn.c - the "gvn" pass: each instruction is folded first (fold.c), then
 * two pure instructions with the same operation, type and operands
 * compute the same value, so the one that dominates stays and the other
 * is replaced by it. Folding and merging in one walk, with one edit, cost
 * one rebuild of the function where two passes cost two, and give the
 * same code. The walk follows the dominator
 * tree with a table scoped to it: what a block computes is visible in the
 * blocks it dominates, and forgotten on the way back up. Instructions that
 * may trap, read memory or call are never merged (in SedaiBasic2 the
 * trapping ones were a source of wrong code under ON ERROR).
 *
 * A check is the exception that pays: check c dominated by a check of the
 * same c can never fail, since the first would have stopped the program;
 * it goes, whatever its code. The key of a check is its condition only.
 */
#include "pass.h"

#include "common/hash.h"
#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t id;   /* the instruction that holds the value */
    uint32_t next; /* earlier entry of the same bucket */
    uint32_t bucket;
} entry;

typedef struct {
    limba_func *f;
    bool fold;
    limba_edit e;
    uint32_t *heads; /* bucket -> last entry, UINT32_MAX when empty */
    uint32_t mask;
    entry *ents;
    uint32_t nents, capents;
} gctx;

static bool mergeable(const limba_inst *in)
{
    if (in->op == LIMBA_OP_CHECK)
        return true;
    const limba_op_info *op = &limba_ops[in->op];
    return (op->flags & LIMBA_OPF_PURE) && in->op != LIMBA_OP_PARAM &&
           in->op != LIMBA_OP_UNDEF && in->type != LIMBA_T_VOID;
}

static int64_t key_imm(const limba_inst *in)
{
    if (in->op == LIMBA_OP_CHECK)
        return 0; /* any code: the first check stops the program */
    return in->op == LIMBA_OP_ICONST ? limba_int_norm(in->imm, in->type)
                                     : in->imm;
}

/* the operands as they stand now; the two of a commutative operation in
   a fixed order */
static void operands(gctx *c, const limba_inst *in, uint32_t *o)
{
    for (uint32_t i = 0; i < in->nops; i++)
        o[i] = limba_edit_resolve(&c->e, c->f->operands[in->first + i]);
    if (in->nops == 2 && (limba_ops[in->op].flags & LIMBA_OPF_COMMUTATIVE) &&
        o[0] > o[1]) {
        uint32_t t = o[0];
        o[0] = o[1];
        o[1] = t;
    }
}

static uint64_t hash(gctx *c, const limba_inst *in)
{
    uint32_t o[3] = {0, 0, 0};
    operands(c, in, o);
    uint64_t h =
        limba_mix(LIMBA_FNV_SEED,
                  (uint64_t)in->op << 40 ^ (uint64_t)in->cc << 32 ^ in->type);
    h = limba_mix(h, (uint64_t)key_imm(in));
    h = limba_mix(h, (uint64_t)in->imm2);
    h = limba_mix(h, (uint64_t)o[0] << 32 | o[1]);
    return limba_mix(h, o[2]);
}

static bool same(gctx *c, const limba_inst *a, const limba_inst *b)
{
    uint32_t oa[3] = {0, 0, 0}, ob[3] = {0, 0, 0};
    if (a->op != b->op || a->cc != b->cc || a->type != b->type ||
        a->nops != b->nops || key_imm(a) != key_imm(b) || a->imm2 != b->imm2)
        return false;
    operands(c, a, oa);
    operands(c, b, ob);
    return !memcmp(oa, ob, sizeof(oa));
}

/* the value equal to instruction id already in scope, or id itself after
   putting it in scope */
static uint32_t lookup_or_add(gctx *c, uint32_t id)
{
    const limba_inst *in = &c->f->insts[id];
    uint32_t bucket = (uint32_t)hash(c, in) & c->mask;
    for (uint32_t k = c->heads[bucket]; k != UINT32_MAX; k = c->ents[k].next)
        if (same(c, &c->f->insts[c->ents[k].id], in))
            return c->ents[k].id;
    LIMBA_GROW(c->ents, c->nents, c->capents);
    c->ents[c->nents] = (entry){id, c->heads[bucket], bucket};
    c->heads[bucket] = c->nents++;
    return id;
}

/* id computes what leader does: a value is replaced, a check dropped */
static void merge(gctx *c, uint32_t id, uint32_t leader)
{
    if (c->f->insts[id].op == LIMBA_OP_CHECK)
        c->e.dead[id] = 1;
    else
        limba_edit_replace(&c->e, id, leader);
}

/* the instructions of block b: each folded, then merged with an equal
   value in scope, or put in scope */
static uint32_t visit(gctx *c, uint32_t b)
{
    const limba_block *bl = &c->f->blocks[b];
    uint32_t changes = 0;
    for (uint32_t k = 0; k < bl->ninsts; k++) {
        uint32_t id = bl->insts[k];
        if (c->fold && k >= bl->nparams && limba_fold_inst(c->f, &c->e, id)) {
            changes++;
            if (c->e.map[id] != id)
                continue; /* it is another value now */
        }
        if (!mergeable(&c->f->insts[id]))
            continue;
        uint32_t leader = lookup_or_add(c, id);
        if (leader != id) {
            merge(c, id, leader);
            changes++;
        }
    }
    return changes;
}

uint32_t limba_pass_gvn(limba_pass_ctx *x, limba_func *f)
{
    gctx c = {.f = f, .fold = x->fold};
    const limba_cfg *cfg = limba_pass_cfg_of(x, f); /* no branch changes */
    limba_edit_begin(&c.e, f);

    uint32_t size = 64;
    while (size < 2 * f->ninsts)
        size *= 2;
    c.mask = size - 1;
    c.heads = limba_xmalloc(size * sizeof(*c.heads));
    for (uint32_t i = 0; i < size; i++)
        c.heads[i] = UINT32_MAX;

    /* children of each block in the dominator tree */
    uint32_t n = f->nblocks;
    uint32_t *cfirst = limba_xcalloc((size_t)n + 2, sizeof(uint32_t));
    uint32_t *child = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *fill = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    for (uint32_t b = 1; b < n; b++)
        if (limba_cfg_reachable(cfg, b))
            cfirst[cfg->idom[b] + 1]++;
    for (uint32_t b = 0; b < n; b++)
        cfirst[b + 1] += cfirst[b];
    memcpy(fill, cfirst, n * sizeof(uint32_t));
    for (uint32_t b = 1; b < n; b++)
        if (limba_cfg_reachable(cfg, b))
            child[fill[cfg->idom[b]]++] = b;

    /* iterative walk: on entry number the block, on exit drop its entries */
    uint32_t *stack = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *next = limba_xcalloc((size_t)n + 1, sizeof(uint32_t));
    uint32_t *mark = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t sp = 0, changes = 0;
    stack[sp++] = 0;
    mark[0] = c.nents;
    changes += visit(&c, 0);
    while (sp) {
        uint32_t b = stack[sp - 1];
        if (cfirst[b] + next[b] < cfirst[b + 1]) {
            uint32_t s = child[cfirst[b] + next[b]++];
            mark[s] = c.nents;
            stack[sp++] = s;
            changes += visit(&c, s);
        } else {
            while (c.nents > mark[b]) { /* entries go in reverse order */
                entry *en = &c.ents[--c.nents];
                c.heads[en->bucket] = en->next;
            }
            sp--;
        }
    }

    if (changes)
        limba_edit_end(&c.e);
    else
        limba_edit_cancel(&c.e);
    free(stack);
    free(next);
    free(mark);
    free(cfirst);
    free(child);
    free(fill);
    free(c.heads);
    free(c.ents);
    return changes;
}
