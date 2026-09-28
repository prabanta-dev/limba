/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * inline.c - the "inline" pass: a call of a short function optimised
 * before its caller becomes a copy of its body.
 *
 * Once a function is optimised the manager keeps a copy of its body when
 * it may go in line (limba_inline_record):
 *
 *   - at most INLINE_MAX instructions, its block parameters not counted;
 *   - it calls (call) only functions optimised before it, never itself:
 *     no chain of calls leads back to it;
 *   - no typed slot: its strings would have to be released at every exit
 *     of each copy;
 *   - no address of one of its slots leaves its body, by ret or as the
 *     value of a store: in line, its slots outlive the "return", and a
 *     dangling address would become a valid one. Given to a call, it is
 *     used before the call ends, and may.
 *
 * In the caller the block of the call splits: what came before, then the
 * copy, whose entry block continues the first part (its parameters are
 * the arguments), then a new block with what came after, which takes the
 * value of each ret as its parameter. The slots of the callee become slots
 * of the caller, zeroed at the entry of the copy each time it runs (a
 * frame would be zeroed at each call); a copied instruction keeps the
 * position it has in the callee, where a trap of it is in the source.
 * One level only: the calls in a copy stay calls (the copy is optimised,
 * its short calls in line already). A caller does not grow past
 * n0 + max(n0, INLINE_GROW) instructions, n0 its size before the pass,
 * taking the calls in the order of the program. progetto_ir.md § 8.
 */
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

#define INLINE_MAX 30
#define INLINE_GROW 60

/* ---- the copies ---- */

static limba_func *copy_func(const limba_func *g)
{
    limba_func *c = limba_xcalloc(1, sizeof(*c));
    c->name = g->name;
    c->type = g->type;
    c->ninsts = c->capinsts = g->ninsts;
    c->insts = limba_xmalloc(((size_t)g->ninsts + 1) * sizeof(*c->insts));
    memcpy(c->insts, g->insts, g->ninsts * sizeof(*c->insts));
    c->noperands = c->capoperands = g->noperands;
    c->operands =
        limba_xmalloc(((size_t)g->noperands + 1) * sizeof(*c->operands));
    if (g->noperands) /* a bare ret has none */
        memcpy(c->operands, g->operands, g->noperands * sizeof(*c->operands));
    c->nblocks = c->capblocks = g->nblocks;
    c->blocks = limba_xcalloc((size_t)g->nblocks + 1, sizeof(*c->blocks));
    for (uint32_t b = 0; b < g->nblocks; b++) {
        const limba_block *s = &g->blocks[b];
        limba_block *d = &c->blocks[b];
        d->ninsts = d->cap = s->ninsts;
        d->nparams = s->nparams;
        d->insts = limba_xmalloc(((size_t)s->ninsts + 1) * sizeof(uint32_t));
        if (s->ninsts)
            memcpy(d->insts, s->insts, s->ninsts * sizeof(uint32_t));
    }
    c->nslots = c->capslots = g->nslots;
    c->slots = limba_xmalloc(((size_t)g->nslots + 1) * sizeof(*c->slots));
    if (g->nslots) /* none: g->slots may be NULL */
        memcpy(c->slots, g->slots, g->nslots * sizeof(*c->slots));
    if (g->locs) {
        c->caplocs = g->ninsts;
        c->locs = limba_xcalloc((size_t)g->ninsts + 1, sizeof(*c->locs));
        for (uint32_t i = 0; i < g->ninsts; i++)
            c->locs[i] = limba_inst_pos(g, i);
    }
    return c;
}

/* an address of a slot of g reaches a ret or is the value of a store:
   derived from a slot is every value computed from one, but what a load
   reads from it; a call given one may give it back */
static bool slot_escapes(const limba_func *g)
{
    uint8_t *der = limba_xcalloc((size_t)g->ninsts + 1, 1);
    uint8_t *kinds = NULL;
    uint32_t capkinds = 0;
    bool escapes = false, changed = true;
    while (changed && !escapes) {
        changed = false;
        for (uint32_t b = 0; b < g->nblocks && !escapes; b++)
            for (uint32_t k = 0; k < g->blocks[b].ninsts && !escapes; k++) {
                uint32_t id = g->blocks[b].insts[k];
                const limba_inst *in = &g->insts[id];
                const uint32_t *o = g->operands + in->first;
                if (in->nops > capkinds) {
                    capkinds = in->nops;
                    kinds = limba_xrealloc(kinds, capkinds, 1);
                }
                limba_operand_kinds(g, in, kinds);
                bool any = false;
                for (uint32_t i = 0; i < in->nops; i++)
                    any |= kinds[i] == LIMBA_OK_VALUE && o[i] < g->ninsts &&
                           der[o[i]];
                if (in->op == LIMBA_OP_RET) {
                    escapes = any;
                } else if (in->op == LIMBA_OP_STORE) {
                    escapes = o[0] < g->ninsts && der[o[0]];
                } else if (in->op == LIMBA_OP_BR || in->op == LIMBA_OP_CBR) {
                    /* an argument derived: the parameter too */
                    for (uint32_t i = 0; i < in->nops; i++) {
                        if (kinds[i] != LIMBA_OK_BLOCK)
                            continue;
                        const limba_block *t = &g->blocks[o[i]];
                        for (uint32_t a = 0; a < o[i + 1]; a++) {
                            uint32_t v = o[i + 2 + a], p = t->insts[a];
                            if (v < g->ninsts && der[v] && !der[p])
                                der[p] = changed = true;
                        }
                    }
                } else if (in->type != LIMBA_T_VOID && !der[id] &&
                           (in->op == LIMBA_OP_SLOT ||
                            (any && in->op != LIMBA_OP_LOAD &&
                             in->op != LIMBA_OP_LOADINV))) {
                    der[id] = changed = true;
                }
            }
    }
    free(kinds);
    free(der);
    return escapes;
}

/* may function fid of m, optimised now, go in line? */
static bool eligible(const limba_inline_lib *lib, const limba_module *m,
                     limba_id fid, uint32_t *size)
{
    const limba_func *g = &m->funcs[fid];
    /* the parameters of b0 would not hold what a variadic call adds */
    if (g->type >= m->ntypes || m->types[g->type].variadic)
        return false;
    uint32_t n = 0;
    for (uint32_t b = 0; b < g->nblocks; b++)
        n += g->blocks[b].ninsts - g->blocks[b].nparams;
    if (n > INLINE_MAX || !g->nblocks)
        return false;
    for (uint32_t s = 0; s < g->nslots; s++)
        if (g->slots[s].type != LIMBA_NONE)
            return false;
    for (uint32_t i = 0; i < g->ninsts; i++) {
        const limba_inst *in = &g->insts[i];
        if (in->op == LIMBA_OP_CALL &&
            ((limba_id)in->imm == fid || (uint64_t)in->imm >= lib->n ||
             !lib->done[in->imm]))
            return false;
    }
    if (g->nslots && slot_escapes(g))
        return false;
    *size = n;
    return true;
}

void limba_inline_record(limba_inline_lib *lib, const limba_module *m,
                         limba_id fid)
{
    if (m->nfuncs > lib->n) {
        lib->body = limba_xrealloc(lib->body, m->nfuncs, sizeof(*lib->body));
        lib->size = limba_xrealloc(lib->size, m->nfuncs, sizeof(*lib->size));
        lib->done = limba_xrealloc(lib->done, m->nfuncs, 1);
        for (uint32_t i = lib->n; i < m->nfuncs; i++) {
            lib->body[i] = NULL;
            lib->size[i] = 0;
            lib->done[i] = 0;
        }
        lib->n = m->nfuncs;
    }
    uint32_t size;
    if (!lib->done[fid] && eligible(lib, m, fid, &size)) {
        lib->body[fid] = copy_func(&m->funcs[fid]);
        lib->size[fid] = size;
    }
    lib->done[fid] = 1;
}

void limba_inline_lib_free(limba_inline_lib *lib)
{
    for (uint32_t i = 0; i < lib->n; i++)
        if (lib->body[i]) {
            limba_func_clear(lib->body[i]);
            free(lib->body[i]);
        }
    free(lib->body);
    free(lib->size);
    free(lib->done);
    memset(lib, 0, sizeof(*lib));
}

/* ---- the pass ---- */

/* the position of instruction id of f */
static void set_pos(limba_func *f, uint32_t id, uint32_t pos)
{
    if (!pos && !f->locs)
        return;
    if (f->caplocs < f->capinsts) {
        f->locs = limba_xrealloc(f->locs, f->capinsts, sizeof(*f->locs));
        memset(f->locs + f->caplocs, 0,
               (f->capinsts - f->caplocs) * sizeof(*f->locs));
        f->caplocs = f->capinsts;
    }
    f->locs[id] = pos;
}

static limba_id add(limba_func *f, limba_id b, unsigned op, limba_id type,
                    int64_t imm, const uint32_t *ops, uint32_t nops,
                    uint32_t pos)
{
    limba_id id = limba_inst_add(f, b, op, type, 0, imm, 0, ops, nops);
    set_pos(f, id, pos);
    return id;
}

/* the call at place k of block cur replaced by a copy of g; the block of
   what came after it is the return */
static limba_id expand(limba_pass_ctx *x, limba_func *f, limba_id cur,
                       uint32_t k, const limba_func *g)
{
    limba_edit *e = &x->e;
    uint32_t call = f->blocks[cur].insts[k];
    limba_inst ci = f->insts[call];
    uint32_t pos = limba_inst_pos(f, call);
    uint32_t *args = limba_xmalloc(((size_t)ci.nops + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < ci.nops; i++)
        args[i] = limba_edit_resolve(e, f->operands[ci.first + i]);

    /* after: the value of the call, then what followed it */
    limba_id after = limba_block_add(f);
    limba_id r = LIMBA_NONE;
    if (ci.type != LIMBA_T_VOID)
        r = add(f, after, LIMBA_OP_PARAM, ci.type, 0, NULL, 0, 0);
    limba_block *from = &f->blocks[cur], *to = &f->blocks[after];
    for (uint32_t j = k + 1; j < from->ninsts; j++) {
        uint32_t id = from->insts[j];
        LIMBA_GROW(to->insts, to->ninsts, to->cap);
        to->insts[to->ninsts++] = id;
        f->insts[id].block = after;
    }
    from->ninsts = k;
    limba_edit_sync(e);
    if (r != LIMBA_NONE)
        limba_edit_replace(e, call, r);
    else
        e->dead[call] = 1;

    /* the blocks and slots of the copy; its entry continues cur */
    uint32_t *bmap = limba_xmalloc(((size_t)g->nblocks + 1) * sizeof(*bmap));
    bmap[0] = cur;
    for (uint32_t b = 1; b < g->nblocks; b++)
        bmap[b] = limba_block_add(f);
    uint32_t s0 = f->nslots;
    for (uint32_t s = 0; s < g->nslots; s++)
        limba_slot_add(f, g->slots[s].size, g->slots[s].align);
    if (g->nslots) { /* zeroed each time the copy runs, as a frame */
        limba_id zero =
            add(f, cur, LIMBA_OP_ICONST, LIMBA_T_I8, 0, NULL, 0, pos);
        for (uint32_t s = 0; s < g->nslots; s++) {
            uint32_t o[3];
            o[0] =
                add(f, cur, LIMBA_OP_SLOT, LIMBA_T_PTR, s0 + s, NULL, 0, pos);
            o[1] = zero;
            o[2] = add(f, cur, LIMBA_OP_ICONST, LIMBA_T_I64, g->slots[s].size,
                       NULL, 0, pos);
            add(f, cur, LIMBA_OP_MEMSET, LIMBA_T_VOID, 0, o, 3, pos);
        }
    }

    /* the values of g as they will be numbered: its parameters are the
       arguments, the rest follows in the order of its blocks */
    uint32_t *vmap = limba_xmalloc(((size_t)g->ninsts + 1) * sizeof(*vmap));
    uint32_t next = f->ninsts;
    for (uint32_t b = 0; b < g->nblocks; b++)
        for (uint32_t j = 0; j < g->blocks[b].ninsts; j++) {
            uint32_t id = g->blocks[b].insts[j];
            vmap[id] = b == 0 && j < g->blocks[0].nparams ? args[j] : next++;
        }
    uint8_t *kinds = NULL;
    uint32_t capkinds = 0, *ops = NULL, capops = 0;
    for (uint32_t b = 0; b < g->nblocks; b++)
        for (uint32_t j = b == 0 ? g->blocks[0].nparams : 0;
             j < g->blocks[b].ninsts; j++) {
            uint32_t id = g->blocks[b].insts[j];
            const limba_inst *in = &g->insts[id];
            const uint32_t *o = g->operands + in->first;
            if (in->nops + 3 > capkinds) {
                capkinds = in->nops + 3;
                kinds = limba_xrealloc(kinds, capkinds, 1);
                capops = capkinds;
                ops = limba_xrealloc(ops, capops, sizeof(*ops));
            }
            unsigned op = in->op;
            limba_id type = in->type;
            int64_t imm = in->imm;
            uint32_t n = 0;
            if (op == LIMBA_OP_RET) { /* a jump to after, with its value */
                ops[n++] = after;
                ops[n++] = in->nops;
                if (in->nops)
                    ops[n++] = vmap[o[0]];
                op = LIMBA_OP_BR;
                type = LIMBA_T_VOID;
                imm = 0;
            } else {
                limba_operand_kinds(g, in, kinds);
                for (uint32_t i = 0; i < in->nops; i++)
                    ops[n++] = kinds[i] == LIMBA_OK_VALUE   ? vmap[o[i]]
                               : kinds[i] == LIMBA_OK_BLOCK ? bmap[o[i]]
                                                            : o[i];
                if (op == LIMBA_OP_SLOT)
                    imm += s0;
            }
            limba_id nid = limba_inst_add(f, bmap[b], op, type, in->cc, imm,
                                          in->imm2, ops, n);
            set_pos(f, nid, limba_inst_pos(g, id));
        }
    limba_edit_sync(e);
    free(ops);
    free(kinds);
    free(vmap);
    free(bmap);
    free(args);
    return after;
}

uint32_t limba_pass_inline(limba_pass_ctx *x, limba_func *f)
{
    const limba_inline_lib *lib = x->lib;
    if (!lib)
        return 0;
    limba_edit *e = &x->e;
    uint64_t size = 0;
    for (uint32_t i = 0; i < f->ninsts; i++)
        size += !e->dead[i] && f->insts[i].op != LIMBA_OP_PARAM;
    uint64_t cap = size + (size > INLINE_GROW ? size : INLINE_GROW);
    uint32_t changes = 0, nb = f->nblocks;
    for (limba_id b = 0; b < nb; b++) {
        if (e->dead_block[b])
            continue;
        limba_id cur = b;
        for (uint32_t k = 0; k < f->blocks[cur].ninsts; k++) {
            uint32_t id = f->blocks[cur].insts[k];
            const limba_inst *in = &f->insts[id];
            if (e->dead[id] || in->op != LIMBA_OP_CALL ||
                (uint64_t)in->imm >= lib->n || !lib->body[in->imm] ||
                size + lib->size[in->imm] > cap)
                continue;
            size += lib->size[in->imm];
            cur = expand(x, f, cur, k, lib->body[in->imm]);
            k = f->blocks[cur].nparams - 1; /* on after the value */
            changes++;
        }
    }
    if (changes)
        limba_pass_cfg_drop(x);
    return changes;
}
