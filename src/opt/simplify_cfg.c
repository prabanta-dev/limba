/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * simplify_cfg.c - the "cfg" pass: a branch whose condition is a constant
 * becomes a jump, a cbr with two identical targets becomes a jump, and the
 * blocks the entry no longer reaches go.
 */
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>

static bool is_iconst(const limba_func *f, uint32_t x, int64_t *v)
{
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

static bool same_target(const limba_func *f, uint32_t a, uint32_t b)
{
    uint32_t na = f->operands[a + 1];
    if (f->operands[a] != f->operands[b] || na != f->operands[b + 1])
        return false;
    for (uint32_t i = 0; i < na; i++)
        if (f->operands[a + 2 + i] != f->operands[b + 2 + i])
            return false;
    return true;
}

uint32_t limba_pass_cfg(limba_pass_ctx *x, limba_func *f)
{
    uint32_t changes = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        limba_inst *in = &f->insts[bl->insts[bl->ninsts - 1]];
        int64_t c;
        if (in->op == LIMBA_OP_CBR) {
            uint32_t t = in->first + 1;
            uint32_t e = t + 2 + f->operands[t + 1];
            if (is_iconst(f, f->operands[in->first], &c)) {
                limba_inst_set_br(f, in, c ? t : e);
                changes++;
            } else if (same_target(f, t, e)) {
                limba_inst_set_br(f, in, t);
                changes++;
            }
        } else if (in->op == LIMBA_OP_SWITCH &&
                   is_iconst(f, f->operands[in->first], &c)) {
            const uint32_t *o = f->operands + in->first;
            limba_id to = o[1];
            for (uint32_t k = 0; k < o[2]; k++) {
                int64_t v =
                    (int64_t)((uint64_t)o[4 + 3 * k] << 32 | o[3 + 3 * k]);
                if (limba_int_norm(v, f->insts[o[0]].type) == c) {
                    to = o[5 + 3 * k];
                    break;
                }
            }
            jump(f, in, to);
            changes++;
        }
    }

    /* blocks the entry does not reach */
    if (changes)
        limba_pass_cfg_drop(x);
    const limba_cfg *cfg = limba_pass_cfg_of(x, f);
    uint32_t dead = 0;
    for (uint32_t b = 1; b < f->nblocks; b++)
        dead += !limba_cfg_reachable(cfg, b);
    if (changes || dead) {
        limba_edit e;
        limba_edit_begin(&e, f);
        for (uint32_t b = 1; b < f->nblocks; b++)
            if (!limba_cfg_reachable(cfg, b)) {
                e.dead_block[b] = 1;
                for (uint32_t k = 0; k < f->blocks[b].ninsts; k++)
                    e.dead[f->blocks[b].insts[k]] = 1;
            }
        limba_edit_end(&e);
        if (dead) /* the blocks are numbered anew */
            limba_pass_cfg_drop(x);
    }
    return changes + dead;
}
