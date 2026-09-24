/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * edit.c - changing a function in one go (see internal.h): replacements,
 * dead instructions and blocks, canonical renumbering.
 */
#include "internal.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

void limba_edit_begin(limba_edit *e, limba_func *f)
{
    e->f = f;
    e->ninsts = f->ninsts;
    e->nblocks = f->nblocks;
    e->map = limba_xmalloc(((size_t)f->ninsts + 1) * sizeof(*e->map));
    for (uint32_t i = 0; i < f->ninsts; i++)
        e->map[i] = i;
    e->dead = limba_xcalloc((size_t)f->ninsts + 1, 1);
    e->dead_block = limba_xcalloc((size_t)f->nblocks + 1, 1);
}

uint32_t limba_edit_resolve(limba_edit *e, uint32_t x)
{
    if (x >= e->ninsts)
        return x;
    uint32_t r = x;
    while (e->map[r] != r)
        r = e->map[r];
    while (e->map[x] != r) { /* shorten the chain for next time */
        uint32_t next = e->map[x];
        e->map[x] = r;
        x = next;
    }
    return r;
}

void limba_edit_replace(limba_edit *e, uint32_t from, uint32_t to)
{
    to = limba_edit_resolve(e, to);
    if (from != to) {
        e->map[from] = to;
        e->dead[from] = 1;
    }
}

void limba_edit_end(limba_edit *e)
{
    limba_func *f = e->f;
    uint32_t n = e->ninsts, nb = e->nblocks;
    uint32_t *newid = limba_xmalloc(((size_t)n + 1) * sizeof(*newid));
    uint32_t *newblock = limba_xmalloc(((size_t)nb + 1) * sizeof(*newblock));
    uint32_t next = 0, nextb = 0;

    for (uint32_t i = 0; i < n; i++)
        newid[i] = UINT32_MAX; /* a use of it makes the verifier say no */
    for (uint32_t b = 0; b < nb; b++) {
        newblock[b] = UINT32_MAX;
        if (e->dead_block[b])
            continue;
        newblock[b] = nextb++;
        const limba_block *bl = &f->blocks[b];
        for (uint32_t k = 0; k < bl->ninsts; k++)
            if (!e->dead[bl->insts[k]])
                newid[bl->insts[k]] = next++;
    }

    limba_inst *insts = limba_xmalloc(((size_t)next + 1) * sizeof(*insts));
    uint32_t *locs =
        f->locs ? limba_xcalloc((size_t)next + 1, sizeof(*locs)) : NULL;
    uint32_t *ops = NULL, nops = 0, capops = 0;
    limba_block *blocks = limba_xcalloc((size_t)nextb + 1, sizeof(*blocks));
    uint8_t *kinds = NULL;
    uint32_t capkinds = 0;

    for (uint32_t b = 0; b < nb; b++) {
        if (e->dead_block[b])
            continue;
        const limba_block *bl = &f->blocks[b];
        limba_block *nbl = &blocks[newblock[b]];
        nbl->insts = limba_xmalloc(((size_t)bl->ninsts + 1) * sizeof(uint32_t));
        nbl->cap = bl->ninsts + 1;
        for (uint32_t k = 0; k < bl->ninsts; k++) {
            uint32_t id = bl->insts[k];
            if (e->dead[id])
                continue;
            const limba_inst *in = &f->insts[id];
            limba_inst *out = &insts[newid[id]];
            *out = *in;
            out->block = newblock[b];
            if (locs)
                locs[newid[id]] = limba_inst_pos(f, id);
            out->first = nops;
            if (in->nops > capkinds) {
                capkinds = in->nops;
                kinds = limba_xrealloc(kinds, capkinds, 1);
            }
            limba_operand_kinds(f, in, kinds);
            for (uint32_t i = 0; i < in->nops; i++) {
                uint32_t o = f->operands[in->first + i];
                if (kinds[i] == LIMBA_OK_VALUE) {
                    o = limba_edit_resolve(e, o);
                    o = o < n ? newid[o] : UINT32_MAX;
                } else if (kinds[i] == LIMBA_OK_BLOCK) {
                    o = o < nb ? newblock[o] : UINT32_MAX;
                }
                LIMBA_GROW(ops, nops, capops);
                ops[nops++] = o;
            }
            nbl->insts[nbl->ninsts++] = newid[id];
            if (in->op == LIMBA_OP_PARAM)
                nbl->nparams++;
        }
    }

    for (uint32_t b = 0; b < nb; b++)
        free(f->blocks[b].insts);
    free(f->blocks);
    free(f->insts);
    free(f->operands);
    f->blocks = blocks;
    f->nblocks = f->capblocks = nextb;
    f->insts = insts;
    f->ninsts = f->capinsts = next;
    free(f->locs);
    f->locs = locs;
    f->caplocs = locs ? next : 0;
    f->operands = ops;
    f->noperands = nops;
    f->capoperands = capops;
    /* the arrays stay growable: LIMBA_GROW doubles from a capacity of 0 */
    free(kinds);
    free(newid);
    free(newblock);
    limba_edit_cancel(e);
}

void limba_edit_cancel(limba_edit *e)
{
    free(e->map);
    free(e->dead);
    free(e->dead_block);
    memset(e, 0, sizeof(*e));
}

int64_t limba_int_norm(int64_t v, limba_id t)
{
    unsigned bits = limba_type_bits(t);
    if (bits == 1)
        return v & 1;
    if (bits == 0 || bits >= 64)
        return v;
    uint64_t u = (uint64_t)v << (64 - bits);
    return (int64_t)u >> (64 - bits); /* arithmetic: gcc and clang */
}

void limba_inst_set_iconst(limba_inst *in, int64_t v)
{
    in->op = LIMBA_OP_ICONST;
    in->cc = 0;
    in->nops = 0;
    in->imm = limba_int_norm(v, in->type);
    in->imm2 = 0;
}

void limba_inst_set_fconst(limba_inst *in, int64_t bits)
{
    in->op = LIMBA_OP_FCONST;
    in->cc = 0;
    in->nops = 0;
    in->imm = bits;
    in->imm2 = 0;
}

void limba_inst_set_br(limba_func *f, limba_inst *in, uint32_t k)
{
    limba_id b;
    uint32_t n;
    uint32_t a = limba_target(f, k, &b, &n);
    uint32_t first = f->noperands;
    LIMBA_GROW(f->operands, f->noperands, f->capoperands);
    f->operands[f->noperands++] = b;
    LIMBA_GROW(f->operands, f->noperands, f->capoperands);
    f->operands[f->noperands++] = n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = f->operands[a + i]; /* read before the pool may move */
        LIMBA_GROW(f->operands, f->noperands, f->capoperands);
        f->operands[f->noperands++] = v;
    }
    in->op = LIMBA_OP_BR;
    in->cc = 0;
    in->type = LIMBA_T_VOID;
    in->first = first;
    in->nops = 2 + n;
    in->imm = in->imm2 = 0;
}
