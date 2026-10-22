/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * cfg.c - the control-flow graph, read from the terminators, and the
 * dominator tree (Cooper, Harvey, Kennedy, "A Simple, Fast Dominance
 * Algorithm", 2001), numbered so that dominance is two comparisons.
 * Also the operand kinds of every format.
 */
#include "internal.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

bool limba_operand_kinds(const limba_func *f, const limba_inst *in,
                         uint8_t *kinds)
{
    const uint32_t *o = f->operands + in->first;
    uint32_t n = in->nops, k = 0;
    for (uint32_t i = 0; i < n; i++)
        kinds[i] = LIMBA_OK_VALUE;
    switch (limba_ops[in->op].format) {
    case LIMBA_F_ICONST:
    case LIMBA_F_FCONST:
    case LIMBA_F_SCONST:
    case LIMBA_F_TYPED:
    case LIMBA_F_SLOT:
    case LIMBA_F_GADDR:
    case LIMBA_F_FADDR:
    case LIMBA_F_NONE:
    case LIMBA_F_TRAP:
    case LIMBA_F_PARAM:
        return n == 0;
    case LIMBA_F_UN:
    case LIMBA_F_CONV:
    case LIMBA_F_LOAD:
    case LIMBA_F_CHECK:
        return n == 1;
    case LIMBA_F_BIN:
    case LIMBA_F_CMP:
    case LIMBA_F_STORE:
    case LIMBA_F_ADDR:
    case LIMBA_F_RC:
        return n == 2;
    case LIMBA_F_TERN:
    case LIMBA_F_MEM3:
    case LIMBA_F_RAISE:
        return n == 3;
    case LIMBA_F_CALL:
    case LIMBA_F_CALL_EXT:
    case LIMBA_F_CALL_RT:
        return true;
    case LIMBA_F_CALL_IND:
        return n >= 1;
    case LIMBA_F_RET:
        return n <= 1;
    case LIMBA_F_BR:
    case LIMBA_F_CBR:
        if (limba_ops[in->op].format == LIMBA_F_CBR) {
            if (n < 1)
                return false;
            k = 1; /* the condition */
        }
        for (int t = 0; t < (limba_ops[in->op].format == LIMBA_F_CBR ? 2 : 1);
             t++) {
            if (k + 2 > n)
                return false;
            kinds[k] = LIMBA_OK_BLOCK;
            kinds[k + 1] = LIMBA_OK_RAW;
            uint32_t args = o[k + 1];
            if (args > n - k - 2)
                return false;
            k += 2 + args;
        }
        return k == n;
    case LIMBA_F_SWITCH: {
        if (n < 3)
            return false;
        kinds[1] = LIMBA_OK_BLOCK;
        kinds[2] = LIMBA_OK_RAW;
        uint32_t cases = o[2];
        if (cases > (n - 3) / 3 || 3 + 3 * cases != n)
            return false;
        for (uint32_t c = 0; c < cases; c++) {
            kinds[3 + 3 * c] = LIMBA_OK_RAW;
            kinds[4 + 3 * c] = LIMBA_OK_RAW;
            kinds[5 + 3 * c] = LIMBA_OK_BLOCK;
        }
        return true;
    }
    }
    return false;
}

uint32_t limba_succs(const limba_func *f, limba_id b, limba_id *out,
                     uint32_t max)
{
    const limba_block *bl = &f->blocks[b];
    if (!bl->ninsts)
        return 0;
    const limba_inst *in = &f->insts[bl->insts[bl->ninsts - 1]];
    if (!(limba_ops[in->op].flags & LIMBA_OPF_TERMINATOR))
        return 0;
    uint8_t stack[64];
    uint8_t *kinds = in->nops <= 64 ? stack : limba_xmalloc(in->nops);
    uint32_t n = 0;
    if (limba_operand_kinds(f, in, kinds))
        for (uint32_t i = 0; i < in->nops; i++)
            if (kinds[i] == LIMBA_OK_BLOCK) {
                if (n < max)
                    out[n] = f->operands[in->first + i];
                n++;
            }
    if (kinds != stack)
        free(kinds);
    return n;
}

static uint32_t *xzero(uint32_t n)
{
    return limba_xcalloc(n + 1, sizeof(uint32_t));
}

static int pair_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* add to c the edges p -> handler(x) for every predecessor p of a
   protected block x, to a fixed point (a handler may be protected): each
   round makes the pairs again from the normal edges and those of the
   round before, sorted and unique; the rounds are the depth of nesting */
static void error_edges(const limba_func *f, limba_cfg *c)
{
    uint32_t n = c->n;
    bool any = false;
    for (uint32_t b = 0; b < n && !any; b++)
        any = f->blocks[b].handler && f->blocks[b].handler - 1 < n;
    if (!any)
        return;
    uint32_t nn = c->sfirst[n]; /* the normal edges */
    uint64_t *err = NULL;       /* (from << 32 | to), sorted */
    size_t nerr = 0;
    for (;;) {
        size_t cap = nn + nerr + 1, k = 0;
        uint64_t *cand = limba_xmalloc(cap * sizeof(uint64_t));
        /* every edge into a protected x, normal or of an error, gives
           its source an edge to the handler of x */
        for (uint32_t b = 0; b < n; b++)
            for (uint32_t i = c->sfirst[b]; i < c->sfirst[b + 1]; i++) {
                uint32_t x = c->succ[i], h = f->blocks[x].handler;
                if (h && h - 1 < n)
                    cand[k++] = (uint64_t)b << 32 | (h - 1);
            }
        for (size_t i = 0; i < nerr; i++) {
            uint32_t x = (uint32_t)err[i], h = f->blocks[x].handler;
            if (h && h - 1 < n)
                cand[k++] = (err[i] >> 32) << 32 | (h - 1);
        }
        for (size_t i = 0; i < nerr; i++)
            cand[k++] = err[i];
        qsort(cand, k, sizeof(uint64_t), pair_cmp);
        size_t u = 0;
        for (size_t i = 0; i < k; i++)
            if (!u || cand[i] != cand[u - 1])
                cand[u++] = cand[i];
        bool same = u == nerr;
        free(err);
        err = cand;
        nerr = u;
        if (same)
            break;
    }
    /* the rows again: each block's own edges, then its edges of errors */
    uint32_t *sfirst = xzero(n);
    for (uint32_t b = 0; b < n; b++)
        sfirst[b + 1] = c->sfirst[b + 1] - c->sfirst[b];
    for (size_t i = 0; i < nerr; i++)
        sfirst[(err[i] >> 32) + 1]++;
    for (uint32_t b = 0; b < n; b++)
        sfirst[b + 1] += sfirst[b];
    uint32_t *succ = limba_xmalloc((sfirst[n] + 1) * sizeof(uint32_t));
    uint32_t *fill = limba_xmalloc((n + 1) * sizeof(uint32_t));
    memcpy(fill, sfirst, (n + 1) * sizeof(uint32_t));
    for (uint32_t b = 0; b < n; b++)
        for (uint32_t i = c->sfirst[b]; i < c->sfirst[b + 1]; i++)
            succ[fill[b]++] = c->succ[i];
    for (size_t i = 0; i < nerr; i++)
        succ[fill[err[i] >> 32]++] = (uint32_t)err[i];
    free(fill);
    free(err);
    free(c->sfirst);
    free(c->succ);
    c->sfirst = sfirst;
    c->succ = succ;
}

bool limba_cfg_build(const limba_func *f, limba_cfg *c)
{
    uint32_t n = f->nblocks;
    memset(c, 0, sizeof(*c));
    c->n = n;
    c->sfirst = xzero(n);
    c->pfirst = xzero(n);
    c->idom = limba_xmalloc((n + 1) * sizeof(uint32_t));
    c->pre = xzero(n);
    c->post = xzero(n);
    bool ok = true;

    /* successors: count, then fill */
    for (uint32_t b = 0; b < n; b++)
        c->sfirst[b + 1] = c->sfirst[b] + limba_succs(f, b, NULL, 0);
    c->succ = limba_xmalloc((c->sfirst[n] + 1) * sizeof(uint32_t));
    for (uint32_t b = 0; b < n; b++) {
        uint32_t k = c->sfirst[b];
        limba_succs(f, b, c->succ + k, c->sfirst[b + 1] - k);
        for (uint32_t i = k; i < c->sfirst[b + 1]; i++)
            if (c->succ[i] >= n) {
                ok = false;
                c->succ[i] = 0; /* keep the graph usable */
            }
    }
    /* the edges of errors (progetto_ir.md § 11i): to the handler of a
       protected block from each of its predecessors, so that the handler
       sees nothing the block defines; again for a protected handler */
    error_edges(f, c);
    /* predecessors: the same edges turned round */
    for (uint32_t i = 0; i < c->sfirst[n]; i++)
        c->pfirst[c->succ[i] + 1]++;
    for (uint32_t b = 0; b < n; b++)
        c->pfirst[b + 1] += c->pfirst[b];
    c->pred = limba_xmalloc((c->pfirst[n] + 1) * sizeof(uint32_t));
    uint32_t *fill = limba_xmalloc((n + 1) * sizeof(uint32_t));
    memcpy(fill, c->pfirst, (n + 1) * sizeof(uint32_t));
    for (uint32_t b = 0; b < n; b++)
        for (uint32_t i = c->sfirst[b]; i < c->sfirst[b + 1]; i++)
            c->pred[fill[c->succ[i]]++] = b;

    /* reverse post-order from the entry, iterative */
    uint32_t *po = limba_xmalloc((n + 1) * sizeof(uint32_t)); /* postnum */
    uint32_t *order = limba_xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t *stack = limba_xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t *next = limba_xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t norder = 0, sp = 0;
    for (uint32_t b = 0; b < n; b++) {
        po[b] = UINT32_MAX;
        c->idom[b] = LIMBA_NONE;
    }
    if (n) {
        memset(next, 0, (n + 1) * sizeof(uint32_t));
        po[0] = UINT32_MAX - 1; /* on the stack */
        stack[sp++] = 0;
        while (sp) {
            uint32_t b = stack[sp - 1];
            uint32_t i = c->sfirst[b] + next[b];
            if (i < c->sfirst[b + 1]) {
                next[b]++;
                uint32_t s = c->succ[i];
                if (po[s] == UINT32_MAX) {
                    po[s] = UINT32_MAX - 1;
                    stack[sp++] = s;
                }
            } else {
                sp--;
                po[b] = norder;
                order[norder++] = b; /* post-order */
            }
        }
        /* iterate over reverse post-order to a fixed point */
        c->idom[0] = 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (uint32_t k = norder; k-- > 0;) {
                uint32_t b = order[k];
                if (b == 0)
                    continue;
                uint32_t nd = LIMBA_NONE;
                for (uint32_t i = c->pfirst[b]; i < c->pfirst[b + 1]; i++) {
                    uint32_t p = c->pred[i];
                    if (c->idom[p] == LIMBA_NONE)
                        continue;
                    if (nd == LIMBA_NONE) {
                        nd = p;
                        continue;
                    }
                    uint32_t x = p, y = nd;
                    while (x != y) {
                        while (po[x] < po[y])
                            x = c->idom[x];
                        while (po[y] < po[x])
                            y = c->idom[y];
                    }
                    nd = x;
                }
                if (nd != c->idom[b]) {
                    c->idom[b] = nd;
                    changed = true;
                }
            }
        }
        /* pre/post numbers of the dominator tree: children lists, then
           an iterative walk */
        uint32_t *cfirst = xzero(n), *child;
        for (uint32_t b = 1; b < n; b++)
            if (c->idom[b] != LIMBA_NONE)
                cfirst[c->idom[b] + 1]++;
        for (uint32_t b = 0; b < n; b++)
            cfirst[b + 1] += cfirst[b];
        child = limba_xmalloc((cfirst[n] + 1) * sizeof(uint32_t));
        memcpy(fill, cfirst, (n + 1) * sizeof(uint32_t));
        for (uint32_t b = 1; b < n; b++)
            if (c->idom[b] != LIMBA_NONE)
                child[fill[c->idom[b]]++] = b;
        uint32_t clock = 0;
        memset(next, 0, (n + 1) * sizeof(uint32_t));
        sp = 0;
        stack[sp++] = 0;
        c->pre[0] = clock++;
        while (sp) {
            uint32_t b = stack[sp - 1];
            uint32_t i = cfirst[b] + next[b];
            if (i < cfirst[b + 1]) {
                next[b]++;
                c->pre[child[i]] = clock++;
                stack[sp++] = child[i];
            } else {
                c->post[b] = clock++;
                sp--;
            }
        }
        free(cfirst);
        free(child);
    }
    free(po);
    free(order);
    free(stack);
    free(next);
    free(fill);
    return ok;
}

void limba_cfg_free(limba_cfg *c)
{
    free(c->sfirst);
    free(c->succ);
    free(c->pfirst);
    free(c->pred);
    free(c->idom);
    free(c->pre);
    free(c->post);
    memset(c, 0, sizeof(*c));
}

bool limba_cfg_reachable(const limba_cfg *c, limba_id b)
{
    return b < c->n && c->idom[b] != LIMBA_NONE;
}

bool limba_cfg_dominates(const limba_cfg *c, limba_id a, limba_id b)
{
    return c->pre[a] <= c->pre[b] && c->post[b] <= c->post[a];
}
