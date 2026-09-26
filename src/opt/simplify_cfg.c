/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * simplify_cfg.c - the "cfg" pass: a branch whose condition is a constant
 * becomes a jump, a cbr with two identical targets becomes a jump; a jump
 * to a block that only jumps on goes where that block goes; a block
 * whose only predecessor jumps to it joins that predecessor; and the
 * blocks the entry no longer reaches go.
 */
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

/* x, as the edit of the pipeline makes it, a constant */
static bool is_iconst(limba_pass_ctx *c, const limba_func *f, uint32_t x,
                      int64_t *v)
{
    x = limba_edit_resolve(&c->e, x);
    if (x >= f->ninsts || f->insts[x].op != LIMBA_OP_ICONST)
        return false;
    *v = limba_int_norm(f->insts[x].imm, f->insts[x].type);
    return true;
}

/* br to block b with no arguments */
static void jump(limba_func *f, limba_inst *in, limba_id b)
{
    uint32_t first = f->noperands;
    LIMBA_GROW(f->operands, f->noperands, f->capoperands);
    f->operands[f->noperands++] = b;
    LIMBA_GROW(f->operands, f->noperands, f->capoperands);
    f->operands[f->noperands++] = 0;
    in->op = LIMBA_OP_BR;
    in->first = first;
    in->nops = 2;
}

static bool same_target(limba_pass_ctx *c, const limba_func *f, uint32_t a,
                        uint32_t b)
{
    uint32_t na = f->operands[a + 1];
    if (f->operands[a] != f->operands[b] || na != f->operands[b + 1])
        return false;
    for (uint32_t i = 0; i < na; i++)
        if (limba_edit_resolve(&c->e, f->operands[a + 2 + i]) !=
            limba_edit_resolve(&c->e, f->operands[b + 2 + i]))
            return false;
    return true;
}

/* the targets of terminator in that are block e, reached with no
   argument, retargeted to t with arguments args[0..n); false if none */
static bool thread(limba_func *f, limba_inst *in, limba_id e, limba_id t,
                   const uint32_t *args, uint32_t n)
{
    if (in->op == LIMBA_OP_SWITCH) {
        /* its targets take no arguments */
        uint32_t *o = f->operands + in->first;
        bool any = false;
        if (n)
            return false;
        if (o[1] == e) {
            o[1] = t;
            any = true;
        }
        for (uint32_t k = 0; k < o[2]; k++)
            if (o[5 + 3 * k] == e) {
                o[5 + 3 * k] = t;
                any = true;
            }
        return any;
    }
    if (in->op != LIMBA_OP_BR && in->op != LIMBA_OP_CBR)
        return false;
    /* the new operands, written apart: the pool may move */
    uint32_t *ops =
        limba_xmalloc(((size_t)in->nops + 2 * n + 2) * sizeof(uint32_t));
    uint32_t m = 0, k = in->first;
    bool any = false;
    if (in->op == LIMBA_OP_CBR)
        ops[m++] = f->operands[k++];
    for (int i = 0; i < (in->op == LIMBA_OP_CBR ? 2 : 1); i++) {
        limba_id b;
        uint32_t na;
        uint32_t a = limba_target(f, k, &b, &na);
        if (b == e && na == 0) {
            ops[m++] = t;
            ops[m++] = n;
            for (uint32_t j = 0; j < n; j++)
                ops[m++] = args[j];
            any = true;
        } else {
            ops[m++] = b;
            ops[m++] = na;
            for (uint32_t j = 0; j < na; j++)
                ops[m++] = f->operands[a + j];
        }
        k = a + na;
    }
    if (any) {
        uint32_t first = f->noperands;
        for (uint32_t j = 0; j < m; j++) {
            LIMBA_GROW(f->operands, f->noperands, f->capoperands);
            f->operands[f->noperands++] = ops[j];
        }
        in->first = first;
        in->nops = m;
    }
    free(ops);
    return any;
}

/* the jumps to a block that holds only a jump, with no parameter, go
   where it goes: the values it passes on are defined before all its
   predecessors */
static uint32_t skip_empty(limba_pass_ctx *x, limba_func *f,
                           const limba_cfg *cfg, uint8_t *touched)
{
    uint32_t changes = 0;
    for (uint32_t e = 1; e < f->nblocks; e++) {
        const limba_block *eb = &f->blocks[e];
        if (x->e.dead_block[e] || !limba_cfg_reachable(cfg, e))
            continue;
        /* one instruction still there, the jump (parameters replaced and
           instructions dropped by the edit do not count) */
        uint32_t live = 0;
        for (uint32_t k = 0; k < eb->ninsts && live < 2; k++)
            live += !x->e.dead[eb->insts[k]];
        const limba_inst *j = &f->insts[eb->insts[eb->ninsts - 1]];
        if (live != 1 || j->op != LIMBA_OP_BR)
            continue;
        limba_id t;
        uint32_t n;
        uint32_t a = limba_target(f, j->first, &t, &n);
        if (t == e)
            continue;
        uint32_t *args = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
        memcpy(args, f->operands + a, n * sizeof(uint32_t));
        for (uint32_t k = cfg->pfirst[e]; k < cfg->pfirst[e + 1]; k++) {
            uint32_t p = cfg->pred[k];
            if (p == e || x->e.dead_block[p])
                continue;
            const limba_block *pb = &f->blocks[p];
            limba_inst *in = &f->insts[pb->insts[pb->ninsts - 1]];
            if (thread(f, in, e, t, args, n)) {
                /* t gains predecessors the CFG does not know */
                touched[p] = touched[e] = touched[t] = 1;
                changes++;
            }
        }
        free(args);
    }
    return changes;
}

/* a block whose only predecessor jumps to it joins it: its parameters
   become the arguments, its instructions follow the predecessor's; a
   block is touched once a run, the CFG being old after */
static uint32_t join(limba_pass_ctx *x, limba_func *f, const limba_cfg *cfg,
                     uint8_t *touched)
{
    uint32_t changes = 0;
    for (uint32_t a = 0; a < f->nblocks; a++) {
        if (x->e.dead_block[a] || !limba_cfg_reachable(cfg, a) || touched[a])
            continue;
        limba_block *ab = &f->blocks[a];
        uint32_t jid = ab->insts[ab->ninsts - 1];
        const limba_inst *j = &f->insts[jid];
        if (j->op != LIMBA_OP_BR)
            continue;
        limba_id b;
        uint32_t n;
        uint32_t args = limba_target(f, j->first, &b, &n);
        limba_block *bb = &f->blocks[b];
        if (b == 0 || b == a || touched[b] || x->e.dead_block[b] ||
            cfg->pfirst[b + 1] - cfg->pfirst[b] != 1)
            continue;
        /* the arguments go to the parameters still there, in order */
        uint32_t live = 0;
        for (uint32_t i = 0; i < bb->nparams; i++)
            live += !x->e.dead[bb->insts[i]];
        if (live != n)
            continue;
        for (uint32_t i = 0, k = 0; i < bb->nparams; i++)
            if (!x->e.dead[bb->insts[i]])
                limba_edit_replace(&x->e, bb->insts[i],
                                   f->operands[args + k++]);
        x->e.dead[jid] = 1;
        uint32_t add = bb->ninsts - bb->nparams;
        if (ab->ninsts + add > ab->cap) {
            ab->cap = ab->ninsts + add;
            ab->insts = limba_xrealloc(ab->insts, ab->cap, sizeof(uint32_t));
        }
        for (uint32_t k = bb->nparams; k < bb->ninsts; k++) {
            uint32_t id = bb->insts[k];
            f->insts[id].block = a;
            ab->insts[ab->ninsts++] = id;
        }
        bb->ninsts = bb->nparams;
        x->e.dead_block[b] = 1;
        touched[a] = touched[b] = 1;
        changes++;
    }
    return changes;
}

uint32_t limba_pass_cfg(limba_pass_ctx *x, limba_func *f)
{
    uint32_t changes = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (x->e.dead_block[b])
            continue;
        const limba_block *bl = &f->blocks[b];
        limba_inst *in = &f->insts[bl->insts[bl->ninsts - 1]];
        int64_t c;
        if (in->op == LIMBA_OP_CBR) {
            uint32_t t = in->first + 1;
            uint32_t e = t + 2 + f->operands[t + 1];
            if (is_iconst(x, f, f->operands[in->first], &c)) {
                limba_inst_set_br(f, in, c ? t : e);
                changes++;
            } else if (same_target(x, f, t, e)) {
                limba_inst_set_br(f, in, t);
                changes++;
            }
        } else if (in->op == LIMBA_OP_SWITCH &&
                   is_iconst(x, f, f->operands[in->first], &c)) {
            const uint32_t *o = f->operands + in->first;
            limba_id to = o[1];
            for (uint32_t k = 0; k < o[2]; k++) {
                int64_t v =
                    (int64_t)((uint64_t)o[4 + 3 * k] << 32 | o[3 + 3 * k]);
                if (limba_int_norm(
                        v, f->insts[limba_edit_resolve(&x->e, o[0])].type) ==
                    c) {
                    to = o[5 + 3 * k];
                    break;
                }
            }
            jump(f, in, to);
            changes++;
        }
    }

    /* jumps through empty blocks, then blocks joined, on one CFG: the
       branches made simpler above only lost edges, so a block it gives a
       single predecessor has at most that one; the blocks that gain
       edges by a jump through are left for the next run */
    uint8_t *touched = limba_xcalloc((size_t)f->nblocks + 1, 1);
    /* the CFG kept, old on purpose when a branch changed above (read
       directly: limba_pass_cfg_of checks a kept one is exact) */
    const limba_cfg *cfg = x->have_cfg ? &x->cfg : limba_pass_cfg_of(x, f);
    changes += skip_empty(x, f, cfg, touched);
    changes += join(x, f, cfg, touched);
    free(touched);

    /* blocks the entry no longer reaches, by a walk along the jumps:
       marked, and with them what they hold */
    uint32_t dead = 0;
    if (changes) {
        uint8_t *seen = limba_xcalloc((size_t)f->nblocks + 1, 1);
        uint32_t *stack =
            limba_xmalloc(((size_t)f->nblocks + 1) * sizeof(uint32_t));
        uint32_t sp = 0, succ[2];
        seen[0] = 1;
        stack[sp++] = 0;
        while (sp) {
            uint32_t b = stack[--sp];
            uint32_t n = limba_succs(f, b, succ, 2);
            uint32_t *out = succ;
            if (n > 2) { /* a switch */
                out = limba_xmalloc(n * sizeof(uint32_t));
                limba_succs(f, b, out, n);
            }
            for (uint32_t k = 0; k < n; k++)
                if (out[k] < f->nblocks && !seen[out[k]]) {
                    seen[out[k]] = 1;
                    stack[sp++] = out[k];
                }
            if (out != succ)
                free(out);
        }
        for (uint32_t b = 1; b < f->nblocks; b++)
            if (!x->e.dead_block[b] && !seen[b]) {
                x->e.dead_block[b] = 1;
                for (uint32_t k = 0; k < f->blocks[b].ninsts; k++)
                    x->e.dead[f->blocks[b].insts[k]] = 1;
                /* emptied: its jumps are no edges for the next CFG */
                f->blocks[b].ninsts = 0;
                dead++;
            }
        free(seen);
        free(stack);
        limba_pass_cfg_drop(x); /* the branches and blocks changed */
    }
    return changes + dead;
}
