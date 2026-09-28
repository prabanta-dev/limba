/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * licm.c - what does not change in a loop computed once, before it.
 *
 * A natural loop (the blocks that reach a back edge u -> h without going
 * through its header h) with one way in from outside gets a preheader, a
 * block that only jumps to h: that way in when it jumps nowhere else, else
 * a new block on the edge. Into it go, in their order:
 *
 *   - an instruction without effect (pure, or a call.rt that depends on
 *     its arguments only and cannot trap) whose operands come from outside
 *     the loop: running it once or never changes nothing;
 *   - ptr_live(p), p from outside, when nothing in the loop may free a
 *     block (a call, mem_free): its answer cannot change there, and asking
 *     it before the loop has no effect;
 *   - in the header, before any instruction with an effect: a check of a
 *     condition from outside, and a load.inv of an address from outside
 *     when nothing may free. The header runs at least once whenever the
 *     preheader does, and those come first in it: the first round would
 *     run them first too, and the next rounds see the same values. A load
 *     elsewhere could read a nil or freed block the checks before it
 *     guard; a check elsewhere could stop a program that never reaches it.
 *
 * A preheader is made only when something worth it goes into it (an empty
 * one the cfg pass would take away again; a constant alone saves no work,
 * it moves with what uses it). One CFG serves the whole pass: a preheader
 * made on the edge p -> h belongs to the loops that hold both p and h,
 * and comes just before h in the order of the dominator tree; building
 * the CFG again after each one cost more than the pass. progetto_ir.md
 * § 4.
 */
#define _GNU_SOURCE /* qsort_r */
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    limba_pass_ctx *x;
    limba_func *f;
    const limba_cfg *c;
    uint32_t *in_loop; /* per block: the stamp of the loop it is in */
    uint32_t stamp;
    uint8_t *hoist;   /* per instruction: goes to the preheader */
    uint32_t *blocks; /* the blocks of the loop, the header first */
    uint32_t nblocks;
    /* the preheaders made, blocks c->n on: the edge each is on */
    uint32_t *made_from, *made_to;
    uint32_t nmade;
} lctx;

static bool alive(const lctx *l, uint32_t id)
{
    return !l->x->e.dead[id];
}

/* every operand of in comes from outside the loop, or goes out of it */
static bool invariant(lctx *l, const limba_inst *in)
{
    if (!limba_only_values(in))
        return false;
    const uint32_t *o = l->f->operands + in->first;
    for (uint32_t i = 0; i < in->nops; i++) {
        uint32_t v = limba_edit_resolve(&l->x->e, o[i]);
        if (l->in_loop[l->f->insts[v].block] == l->stamp && !l->hoist[v])
            return false;
    }
    return true;
}

/* an instruction that may free a block of mem_alloc */
static bool may_free(const limba_inst *in)
{
    return in->op == LIMBA_OP_CALL || in->op == LIMBA_OP_CALLIND ||
           in->op == LIMBA_OP_CALLEXT ||
           (in->op == LIMBA_OP_CALLRT && in->imm == LIMBA_RT_MEM_FREE);
}

/* without effect wherever it runs: pure, or a call.rt of its arguments
   only that cannot trap */
static bool no_effect(const limba_inst *in)
{
    const limba_op_info *op = &limba_ops[in->op];
    if (in->op == LIMBA_OP_PARAM || in->op == LIMBA_OP_UNDEF)
        return false;
    if (op->flags & LIMBA_OPF_PURE)
        return true;
    if (in->op != LIMBA_OP_CALLRT)
        return false;
    uint32_t a = limba_rts[in->imm].attrs;
    return (a & LIMBA_RTA_PURE) &&
           !(a & (LIMBA_RTA_MAY_TRAP | LIMBA_RTA_IO | LIMBA_RTA_WRITES_MEM |
                  LIMBA_RTA_ALLOC | LIMBA_RTA_NORETURN));
}

/* the blocks of the natural loop of header h, stamped; false if it has
   no single way in from outside, whose block is then *out */
static bool loop_of(lctx *l, limba_id h, limba_id *out)
{
    const limba_cfg *c = l->c;
    uint32_t *list = l->blocks, n = 0, next = 1;
    l->stamp++;
    l->in_loop[h] = l->stamp;
    list[n++] = h;
    for (uint32_t k = c->pfirst[h]; k < c->pfirst[h + 1]; k++) {
        uint32_t u = c->pred[k];
        if (limba_cfg_reachable(c, u) && limba_cfg_dominates(c, h, u) &&
            l->in_loop[u] != l->stamp) {
            l->in_loop[u] = l->stamp;
            list[n++] = u;
        }
    }
    while (next < n) { /* the list is the work list too */
        uint32_t b = list[next++];
        for (uint32_t k = c->pfirst[b]; k < c->pfirst[b + 1]; k++) {
            uint32_t p = c->pred[k];
            if (limba_cfg_reachable(c, p) && l->in_loop[p] != l->stamp) {
                l->in_loop[p] = l->stamp;
                list[n++] = p;
            }
        }
    }
    /* a preheader made on an edge inside this loop is in it too */
    for (uint32_t j = 0; j < l->nmade; j++)
        if (l->in_loop[l->made_from[j]] == l->stamp &&
            l->in_loop[l->made_to[j]] == l->stamp) {
            l->in_loop[c->n + j] = l->stamp;
            list[n++] = c->n + j;
        }
    l->nblocks = n;
    uint32_t ways = 0;
    for (uint32_t k = c->pfirst[h]; k < c->pfirst[h + 1]; k++)
        if (l->in_loop[c->pred[k]] != l->stamp) {
            ways++;
            *out = c->pred[k];
        }
    return ways == 1;
}

/* the terminator of block b */
static limba_inst *terminator(limba_func *f, limba_id b)
{
    const limba_block *bl = &f->blocks[b];
    return &f->insts[bl->insts[bl->ninsts - 1]];
}

/* a new block on the edge from p to h, which jumps to h with the
   arguments p gave it; p then jumps to it without any */
static limba_id split(lctx *l, limba_id p, limba_id h)
{
    limba_func *f = l->f;
    limba_id nb = limba_block_add(f);
    limba_inst *t = terminator(f, p);
    uint32_t *ops = limba_xmalloc(((size_t)t->nops + 4) * sizeof(uint32_t)),
             m = 0;
    uint32_t *args = NULL, nargs = 0;
    uint32_t k = t->first;
    if (t->op == LIMBA_OP_CBR)
        ops[m++] = f->operands[k++];
    for (int i = 0; i < (t->op == LIMBA_OP_CBR ? 2 : 1); i++) {
        limba_id b;
        uint32_t na;
        uint32_t a = limba_target(f, k, &b, &na);
        if (b == h && !args) {
            args = limba_xmalloc(((size_t)na + 1) * sizeof(uint32_t));
            nargs = na;
            memcpy(args, f->operands + a, na * sizeof(uint32_t));
            ops[m++] = nb;
            ops[m++] = 0;
        } else {
            ops[m++] = b;
            ops[m++] = na;
            for (uint32_t j = 0; j < na; j++)
                ops[m++] = f->operands[a + j];
        }
        k = a + na;
    }
    uint32_t first = f->noperands;
    for (uint32_t j = 0; j < m; j++) {
        LIMBA_GROW(f->operands, f->noperands, f->capoperands);
        f->operands[f->noperands++] = ops[j];
    }
    t = terminator(f, p);
    t->first = first;
    t->nops = m;
    free(ops);
    uint32_t *br = limba_xmalloc(((size_t)nargs + 2) * sizeof(uint32_t));
    br[0] = h;
    br[1] = nargs;
    memcpy(br + 2, args, nargs * sizeof(uint32_t));
    limba_inst_add(f, nb, LIMBA_OP_BR, LIMBA_T_VOID, 0, 0, 0, br, nargs + 2);
    free(br);
    free(args);
    limba_edit_sync(&l->x->e);
    return nb;
}

/* instruction id moved to the end of block to, before its terminator */
static void move_to(limba_func *f, uint32_t id, limba_id to)
{
    limba_block *from = &f->blocks[f->insts[id].block];
    for (uint32_t k = 0; k < from->ninsts; k++)
        if (from->insts[k] == id) {
            memmove(from->insts + k, from->insts + k + 1,
                    (from->ninsts - k - 1) * sizeof(uint32_t));
            from->ninsts--;
            break;
        }
    limba_block *bl = &f->blocks[to];
    LIMBA_GROW(bl->insts, bl->ninsts, bl->cap);
    uint32_t term = bl->insts[bl->ninsts - 1];
    bl->insts[bl->ninsts - 1] = id;
    bl->insts[bl->ninsts++] = term;
    f->insts[id].block = to;
}

/* the place of block b in the preorder of the dominator tree: a
   preheader made on an edge to h just before h */
static uint64_t place(const lctx *l, uint32_t b)
{
    const limba_cfg *c = l->c;
    return b < c->n ? 2 * (uint64_t)c->pre[b] + 1
                    : 2 * (uint64_t)c->pre[l->made_to[b - c->n]];
}

static int by_preorder(const void *a, const void *b, void *ctx)
{
    uint64_t x = place(ctx, *(const uint32_t *)a),
             y = place(ctx, *(const uint32_t *)b);
    return x < y ? -1 : x > y;
}

/* the loop of header h: what goes out of it, then out it goes, into the
   block before it (its only way in, which jumps only to h); the number
   moved */
/* a constant: no work to save, it moves only with what uses it */
static bool constant(const limba_inst *in)
{
    return in->op == LIMBA_OP_ICONST || in->op == LIMBA_OP_FCONST ||
           in->op == LIMBA_OP_SCONST || in->op == LIMBA_OP_NULLV;
}

static uint32_t one_loop(lctx *l, limba_id h)
{
    limba_func *f = l->f;
    const limba_cfg *c = l->c;
    limba_id way;
    if (!loop_of(l, h, &way))
        return 0;
    bool frees = false;
    for (uint32_t i = 0; i < l->nblocks && !frees; i++) {
        limba_id b = l->blocks[i];
        if (l->x->e.dead_block[b])
            continue;
        for (uint32_t k = 0; k < f->blocks[b].ninsts && !frees; k++) {
            uint32_t id = f->blocks[b].insts[k];
            frees = alive(l, id) && may_free(&f->insts[id]);
        }
    }
    /* the blocks of the loop in the preorder of the dominator tree: a
       value is seen before its uses */
    uint32_t nb = 0, *order = limba_xmalloc(((size_t)l->nblocks + 1) *
                                            sizeof(uint32_t));
    for (uint32_t i = 0; i < l->nblocks; i++)
        if (!l->x->e.dead_block[l->blocks[i]])
            order[nb++] = l->blocks[i];
    qsort_r(order, nb, sizeof(*order), by_preorder, l);
    uint32_t *moved = NULL, nmoved = 0, capmoved = 0, worth = 0;
    for (uint32_t i = 0; i < nb; i++) {
        limba_id b = order[i];
        bool prefix = b == h; /* no effect before, in the header */
        const limba_block *bl = &f->blocks[b];
        for (uint32_t k = bl->nparams; k < bl->ninsts; k++) {
            uint32_t id = bl->insts[k];
            if (!alive(l, id))
                continue;
            const limba_inst *in = &f->insts[id];
            if (limba_ops[in->op].flags & LIMBA_OPF_TERMINATOR)
                break;
            bool go = false, inv = invariant(l, in);
            if (inv && no_effect(in))
                go = true;
            else if (inv && !frees && in->op == LIMBA_OP_CALLRT &&
                     in->imm == LIMBA_RT_PTR_LIVE)
                go = true;
            else if (inv && prefix && in->op == LIMBA_OP_CHECK)
                go = true;
            else if (inv && prefix && !frees && in->op == LIMBA_OP_LOADINV)
                go = true;
            if (go) {
                l->hoist[id] = 1;
                LIMBA_GROW(moved, nmoved, capmoved);
                moved[nmoved++] = id;
                worth += !constant(in);
            } else if (!no_effect(in)) {
                prefix = false; /* an effect: what follows stays */
            }
        }
    }
    free(order);
    if (worth && c->sfirst[way + 1] - c->sfirst[way] != 1) {
        /* the way in jumps elsewhere too: a preheader on its edge */
        l->made_from[l->nmade] = way;
        l->made_to[l->nmade++] = h;
        way = split(l, way, h);
    }
    for (uint32_t i = 0; i < nmoved; i++) {
        if (worth)
            move_to(f, moved[i], way);
        l->hoist[moved[i]] = 0;
    }
    free(moved);
    return worth ? nmoved : 0;
}

uint32_t limba_pass_licm(limba_pass_ctx *x, limba_func *f)
{
    uint32_t changes = 0;
    /* the headers: what an inner loop moves out, an outer one may move
       out again next round */
    const limba_cfg *c = limba_pass_cfg_of(x, f);
    uint32_t nh = 0, *heads = limba_xmalloc(((size_t)f->nblocks + 1) *
                                            sizeof(uint32_t));
    for (limba_id h = 0; h < f->nblocks; h++) {
        if (x->e.dead_block[h] || !limba_cfg_reachable(c, h))
            continue;
        for (uint32_t k = c->pfirst[h]; k < c->pfirst[h + 1]; k++) {
            uint32_t u = c->pred[k];
            if (limba_cfg_reachable(c, u) && limba_cfg_dominates(c, h, u)) {
                heads[nh++] = h;
                break;
            }
        }
    }
    /* one set of tables for all the loops: each preheader made adds one
       block and one instruction */
    lctx l = {x, f, c, NULL, 0, NULL, NULL, 0, NULL, NULL, 0};
    l.in_loop = limba_xcalloc((size_t)f->nblocks + nh + 1, sizeof(uint32_t));
    l.hoist = limba_xcalloc((size_t)f->ninsts + nh + 1, 1);
    l.blocks = limba_xmalloc(((size_t)f->nblocks + nh + 1) * sizeof(uint32_t));
    l.made_from = limba_xmalloc(((size_t)nh + 1) * sizeof(uint32_t));
    l.made_to = limba_xmalloc(((size_t)nh + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < nh; i++)
        changes += one_loop(&l, heads[i]);
    if (l.nmade)
        limba_pass_cfg_drop(x); /* blocks were added */
    free(l.in_loop);
    free(l.hoist);
    free(l.blocks);
    free(l.made_from);
    free(l.made_to);
    free(heads);
    return changes;
}
