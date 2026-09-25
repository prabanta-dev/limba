/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * binary.c - the binary form (.lir): what Limba writes and Meri reads.
 *
 *   "LIR\0"  version  runtime-fingerprint  ops-fingerprint  memory  name
 *   strings  types  globals  externs  functions
 *
 * Integers are LEB128 (signed where they may be negative), fingerprints 8
 * bytes little endian. Instructions are written block by block, parameters
 * first, and a value operand is the index of its instruction in that order:
 * reading creates the instructions in the same order, so the index is the
 * id. Writing is deterministic: write(read(write(m))) == write(m), byte for
 * byte. Reading trusts nothing: every count is checked against the bytes
 * left, every id that is dereferenced here is checked, and the rest is for
 * limba_verify to judge.
 */
#include "internal.h"

#include "common/hash.h"
#include "common/leb128.h"
#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t magic[4] = {'L', 'I', 'R', 0};

static uint64_t ops_fingerprint(void)
{
    uint64_t h = LIMBA_FNV_SEED;
    for (unsigned i = 0; i < LIMBA_OP_COUNT; i++) {
        h = limba_fnv(limba_ops[i].text, strlen(limba_ops[i].text) + 1, h);
        h = limba_fnv(&limba_ops[i].format, sizeof(limba_ops[i].format), h);
        h = limba_fnv(&limba_ops[i].flags, sizeof(limba_ops[i].flags), h);
    }
    return h;
}

/* ---- writing ---- */

static void w_func(limba_wbuf *w, const limba_func *f)
{
    /* canonical index of every instruction: block order, parameters first
       (they are first in each block already) */
    uint32_t *num = limba_xmalloc(((size_t)f->ninsts + 1) * sizeof(*num));
    uint32_t next = 0, cap = 0;
    uint8_t *kinds = NULL;
    for (uint32_t b = 0; b < f->nblocks; b++)
        for (uint32_t k = 0; k < f->blocks[b].ninsts; k++)
            num[f->blocks[b].insts[k]] = next++;

    limba_w_uleb(w, f->name);
    limba_w_uleb(w, f->type);
    limba_w_uleb(w, f->flags);
    limba_w_uleb(w, f->nslots);
    for (uint32_t s = 0; s < f->nslots; s++) {
        limba_w_uleb(w, f->slots[s].size);
        limba_w_uleb(w, f->slots[s].align);
    }
    limba_w_uleb(w, f->nblocks);
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        limba_w_uleb(w, bl->nparams);
        for (uint32_t k = 0; k < bl->nparams; k++)
            limba_w_uleb(w, f->insts[bl->insts[k]].type);
        limba_w_uleb(w, bl->ninsts - bl->nparams);
        for (uint32_t k = bl->nparams; k < bl->ninsts; k++) {
            const limba_inst *in = &f->insts[bl->insts[k]];
            const uint32_t *o = f->operands + in->first;
            /* room for all of it, then no more checks */
            limba_w_reserve(w, 60 + 10 * (size_t)in->nops);
            uint8_t *p = w->buf + w->len;
            p = limba_put_uleb(p, in->op);
            p = limba_put_uleb(p, in->type);
            *p++ = in->cc;
            p = limba_put_sleb(p, in->imm);
            p = limba_put_sleb(p, in->imm2);
            p = limba_put_uleb(p, in->nops);
            if (limba_only_values(in)) {
                for (uint32_t i = 0; i < in->nops; i++)
                    p = limba_put_uleb(p, num[o[i]]);
                w->len = (size_t)(p - w->buf);
                continue;
            }
            w->len = (size_t)(p - w->buf);
            if (in->nops > cap) {
                cap = in->nops;
                kinds = limba_xrealloc(kinds, cap, 1);
            }
            limba_operand_kinds(f, in, kinds);
            for (uint32_t i = 0; i < in->nops; i++)
                limba_w_uleb(w, kinds[i] == LIMBA_OK_VALUE ? num[o[i]] : o[i]);
        }
    }
    free(num);
    free(kinds);
}

/* the positions of the instructions of f, in canonical order, after a
   flag: whether it has any */
static void w_locs(limba_wbuf *w, const limba_func *f)
{
    limba_w_byte(w, f->locs != NULL);
    if (!f->locs)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++)
        for (uint32_t k = 0; k < f->blocks[b].ninsts; k++)
            limba_w_uleb(w, limba_inst_pos(f, f->blocks[b].insts[k]));
}

/* the bytes of the functions given one at a time: body and positions,
   where each begins and ends, by function id */
struct limba_writer {
    limba_wbuf body, locs;
    size_t *bat, *bend, *lat, *lend;
    uint8_t *done;
    uint32_t cap;
};

limba_writer *limba_writer_new(void)
{
    return limba_xcalloc(1, sizeof(limba_writer));
}

void limba_writer_func(limba_writer *w, const limba_module *m, limba_id fid)
{
    if (fid >= w->cap) {
        uint32_t cap = w->cap ? w->cap : 16;
        while (cap <= fid || cap < m->nfuncs)
            cap *= 2;
        w->bat = limba_xrealloc(w->bat, cap, sizeof(size_t));
        w->bend = limba_xrealloc(w->bend, cap, sizeof(size_t));
        w->lat = limba_xrealloc(w->lat, cap, sizeof(size_t));
        w->lend = limba_xrealloc(w->lend, cap, sizeof(size_t));
        w->done = limba_xrealloc(w->done, cap, 1);
        memset(w->done + w->cap, 0, cap - w->cap);
        w->cap = cap;
    }
    const limba_func *f = &m->funcs[fid];
    w->bat[fid] = w->body.len;
    w_func(&w->body, f);
    w->bend[fid] = w->body.len;
    w->lat[fid] = w->locs.len;
    w_locs(&w->locs, f);
    w->lend[fid] = w->locs.len;
    w->done[fid] = 1;
}

int limba_write(const limba_module *m, uint8_t **buf, size_t *len)
{
    return limba_writer_end(limba_writer_new(), m, buf, len);
}

int limba_writer_end(limba_writer *wr, const limba_module *m, uint8_t **buf,
                     size_t *len)
{
    limba_wbuf w = {0};
    limba_w_bytes(&w, magic, sizeof(magic));
    limba_w_uleb(&w, LIMBA_IR_VERSION);
    limba_w_u64(&w, limba_rt_fingerprint());
    limba_w_u64(&w, ops_fingerprint());
    limba_w_uleb(&w, m->memory);
    limba_w_uleb(&w, m->name == LIMBA_NONE ? 0 : (uint64_t)m->name + 1);

    uint32_t nstr = limba_str_count(m);
    limba_w_uleb(&w, nstr);
    for (uint32_t i = 0; i < nstr; i++) {
        size_t n;
        const char *s = limba_str(m, i, &n);
        limba_w_uleb(&w, n);
        limba_w_bytes(&w, s, n);
    }

    limba_w_uleb(&w, m->ntypes - LIMBA_T_FIRST_USER);
    for (limba_id t = LIMBA_T_FIRST_USER; t < m->ntypes; t++) {
        const limba_type *ty = &m->types[t];
        limba_w_byte(&w, ty->kind);
        switch (ty->kind) {
        case LIMBA_TK_STRUCT:
            limba_w_uleb(&w, ty->name);
            limba_w_uleb(&w, ty->size);
            limba_w_uleb(&w, ty->align);
            limba_w_uleb(&w, ty->count);
            for (uint32_t i = 0; i < ty->count; i++) {
                limba_w_uleb(&w, m->members[ty->first + i].type);
                limba_w_uleb(&w, m->members[ty->first + i].offset);
            }
            break;
        case LIMBA_TK_ARRAY:
            limba_w_uleb(&w, ty->elem);
            limba_w_uleb(&w, ty->count);
            break;
        case LIMBA_TK_FUNC:
            limba_w_byte(&w, ty->variadic);
            limba_w_uleb(&w, ty->elem);
            limba_w_uleb(&w, ty->count);
            for (uint32_t i = 0; i < ty->count; i++)
                limba_w_uleb(&w, m->members[ty->first + i].type);
            break;
        }
    }

    limba_w_uleb(&w, m->nglobals);
    for (uint32_t i = 0; i < m->nglobals; i++) {
        const limba_global *g = &m->globals[i];
        limba_w_uleb(&w, g->name);
        limba_w_uleb(&w, g->type);
        limba_w_uleb(&w, g->flags);
        limba_w_byte(&w, g->init);
        limba_w_sleb(&w, g->value);
    }

    limba_w_uleb(&w, m->nexterns);
    for (uint32_t i = 0; i < m->nexterns; i++) {
        const limba_extern *e = &m->externs[i];
        limba_w_uleb(&w, e->name);
        limba_w_uleb(&w, e->type);
        limba_w_uleb(&w, e->symbol);
        limba_w_uleb(&w,
                     e->library == LIMBA_NONE ? 0 : (uint64_t)e->library + 1);
    }

    limba_w_uleb(&w, m->nfuncs);
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        if (i < wr->cap && wr->done[i])
            limba_w_bytes(&w, wr->body.buf + wr->bat[i],
                          wr->bend[i] - wr->bat[i]);
        else
            w_func(&w, &m->funcs[i]);
    }

    /* positions in the source: the table, then for each function a flag
       and, if set, the position of every instruction in canonical order */
    limba_w_uleb(&w, m->npos);
    for (uint32_t k = 0; k < m->npos; k++) {
        limba_w_uleb(&w, m->pos[k].file);
        limba_w_uleb(&w, m->pos[k].line);
        limba_w_uleb(&w, m->pos[k].col);
    }
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        if (i < wr->cap && wr->done[i])
            limba_w_bytes(&w, wr->locs.buf + wr->lat[i],
                          wr->lend[i] - wr->lat[i]);
        else
            w_locs(&w, &m->funcs[i]);
    }

    *buf = w.buf;
    *len = w.len;
    limba_writer_free(wr);
    return 0;
}

void limba_writer_free(limba_writer *w)
{
    if (!w)
        return;
    free(w->body.buf);
    free(w->locs.buf);
    free(w->bat);
    free(w->bend);
    free(w->lat);
    free(w->lend);
    free(w->done);
    free(w);
}

/* ---- reading ---- */

/* a 32-bit id; ids are not checked here unless dereferenced */
static uint32_t r_id(limba_rbuf *r)
{
    uint64_t v = limba_r_uleb(r);
    if (v > UINT32_MAX) {
        r->bad = true;
        return 0;
    }
    return (uint32_t)v;
}

/* 0 = none, else id + 1 */
static uint32_t r_opt(limba_rbuf *r)
{
    uint64_t v = limba_r_uleb(r);
    if (v > (uint64_t)UINT32_MAX) {
        r->bad = true;
        return 0;
    }
    return v ? (uint32_t)(v - 1) : LIMBA_NONE;
}

static bool r_func(limba_rbuf *r, limba_module *m, limba_diag *d)
{
    uint32_t *ops = NULL, cap = 0;
    limba_id name = r_id(r), type = r_id(r);
    uint32_t flags = r_id(r);
    if (r->bad)
        return false;
    limba_id fid = limba_func_add(m, name, type, flags);
    if (fid == LIMBA_NONE) {
        limba_diag_set(d, 0, "a function name used twice");
        return false;
    }
    limba_func *f = &m->funcs[fid];
    uint32_t nslots = limba_r_count(r, 2);
    for (uint32_t s = 0; s < nslots && !r->bad; s++) {
        uint32_t size = r_id(r), align = r_id(r);
        limba_slot_add(f, size, align);
    }
    uint32_t nblocks = limba_r_count(r, 2);
    for (uint32_t b = 0; b < nblocks && !r->bad; b++)
        limba_block_add(f);
    for (uint32_t b = 0; b < nblocks && !r->bad; b++) {
        uint32_t np = limba_r_count(r, 1);
        for (uint32_t k = 0; k < np && !r->bad; k++)
            limba_param_add(f, b, r_id(r));
        uint32_t ni = limba_r_count(r, 6);
        for (uint32_t k = 0; k < ni && !r->bad; k++) {
            uint32_t op = r_id(r), ty = r_id(r);
            uint8_t cc = limba_r_byte(r);
            int64_t imm = limba_r_sleb(r), imm2 = limba_r_sleb(r);
            uint32_t n = limba_r_count(r, 1);
            if (r->bad)
                break;
            if (op >= LIMBA_OP_COUNT || op == LIMBA_OP_PARAM) {
                limba_diag_set(d, 0, "an unknown operation %u", op);
                free(ops);
                return false;
            }
            if (n > cap) {
                cap = n;
                ops = limba_xrealloc(ops, cap, sizeof(*ops));
            }
            for (uint32_t i = 0; i < n; i++)
                ops[i] = r_id(r);
            if (r->bad)
                break;
            limba_inst_add(f, b, op, ty, cc, imm, imm2, ops, n);
        }
    }
    free(ops);
    return !r->bad;
}

limba_module *limba_read(const uint8_t *buf, size_t len, limba_diag *d)
{
    limba_rbuf r = {buf, buf + len, false};
    const uint8_t *mg = limba_r_bytes(&r, sizeof(magic));
    if (!mg || memcmp(mg, magic, sizeof(magic)) != 0) {
        limba_diag_set(d, 0, "not a Limba IR file");
        return NULL;
    }
    uint64_t version = limba_r_uleb(&r);
    uint64_t rtfp = limba_r_u64(&r), opfp = limba_r_u64(&r);
    if (r.bad || version != LIMBA_IR_VERSION) {
        limba_diag_set(d, 0, "IR version %llu, this program reads %d",
                       (unsigned long long)version, LIMBA_IR_VERSION);
        return NULL;
    }
    if (rtfp != limba_rt_fingerprint() || opfp != ops_fingerprint()) {
        limba_diag_set(d, 0,
                       "written against another runtime or operation "
                       "table: rebuild it with this Limba");
        return NULL;
    }

    limba_module *m = limba_module_new();
    m->memory = r_id(&r);
    m->name = r_opt(&r);

    uint32_t nstr = limba_r_count(&r, 1);
    for (uint32_t i = 0; i < nstr && !r.bad; i++) {
        uint32_t n = limba_r_count(&r, 1);
        const uint8_t *s = limba_r_bytes(&r, n);
        if (!s)
            break;
        if (limba_str_intern(m, (const char *)s, n) != i) {
            limba_diag_set(d, 0, "a string written twice");
            goto fail;
        }
    }

    uint32_t ntypes = limba_r_count(&r, 3);
    limba_member *mem = NULL;
    uint32_t capmem = 0;
    for (uint32_t i = 0; i < ntypes && !r.bad; i++) {
        limba_id expect = m->ntypes, got = LIMBA_NONE;
        uint8_t kind = limba_r_byte(&r);
        if (kind == LIMBA_TK_STRUCT) {
            limba_id nm = r_id(&r);
            uint32_t size = r_id(&r), align = r_id(&r);
            uint32_t n = limba_r_count(&r, 2);
            if (n > capmem) {
                capmem = n;
                mem = limba_xrealloc(mem, capmem, sizeof(*mem));
            }
            for (uint32_t k = 0; k < n; k++) {
                mem[k].type = r_id(&r);
                mem[k].offset = r_id(&r);
            }
            if (!r.bad)
                got = limba_type_struct(m, nm, mem, n, size, align);
        } else if (kind == LIMBA_TK_ARRAY) {
            limba_id elem = r_id(&r);
            uint32_t count = r_id(&r);
            if (!r.bad && elem < m->ntypes)
                got = limba_type_array(m, elem, count);
        } else if (kind == LIMBA_TK_FUNC) {
            uint8_t variadic = limba_r_byte(&r);
            limba_id ret = r_id(&r);
            uint32_t n = limba_r_count(&r, 1);
            limba_id *params = limba_xmalloc((size_t)n * sizeof(*params) + 1);
            for (uint32_t k = 0; k < n; k++)
                params[k] = r_id(&r);
            if (!r.bad && variadic <= 1)
                got = limba_type_func(m, ret, params, n, variadic);
            free(params);
        }
        /* written types are distinct, so each one is new here */
        if (!r.bad && got != expect) {
            free(mem);
            limba_diag_set(d, 0, "type %u is malformed or a duplicate", i);
            goto fail;
        }
    }
    free(mem);

    uint32_t nglobals = limba_r_count(&r, 5);
    for (uint32_t i = 0; i < nglobals && !r.bad; i++) {
        limba_id nm = r_id(&r), ty = r_id(&r);
        uint32_t flags = r_id(&r);
        uint8_t init = limba_r_byte(&r);
        int64_t value = limba_r_sleb(&r);
        if (r.bad)
            break;
        limba_id g = limba_global_add(m, nm, ty, flags);
        if (g == LIMBA_NONE) {
            limba_diag_set(d, 0, "a global name used twice");
            goto fail;
        }
        m->globals[g].init = init;
        m->globals[g].value = value;
    }

    uint32_t nexterns = limba_r_count(&r, 4);
    for (uint32_t i = 0; i < nexterns && !r.bad; i++) {
        limba_id nm = r_id(&r), ty = r_id(&r), sy = r_id(&r);
        limba_id lib = r_opt(&r);
        if (r.bad)
            break;
        if (limba_extern_add(m, nm, ty, sy, lib) == LIMBA_NONE) {
            limba_diag_set(d, 0, "an extern name used twice");
            goto fail;
        }
    }

    uint32_t nfuncs = limba_r_count(&r, 5);
    for (uint32_t i = 0; i < nfuncs && !r.bad; i++)
        if (!r_func(&r, m, d)) {
            if (!r.bad)
                goto fail;
            break;
        }

    uint32_t npos = limba_r_count(&r, 3);
    for (uint32_t k = 0; k < npos && !r.bad; k++) {
        limba_id file = r_id(&r);
        uint32_t line = r_id(&r), col = r_id(&r);
        LIMBA_GROW(m->pos, m->npos, m->cappos);
        m->pos[m->npos++] = (limba_pos){file, line, col};
    }
    for (uint32_t i = 0; i < m->nfuncs && !r.bad; i++) {
        limba_func *f = &m->funcs[i];
        uint8_t has = limba_r_byte(&r);
        if (has > 1)
            r.bad = true;
        if (!has || r.bad)
            continue;
        /* instructions were made in canonical order: id = index */
        f->locs = limba_xcalloc((size_t)f->capinsts + 1, sizeof(*f->locs));
        f->caplocs = f->capinsts;
        for (uint32_t k = 0; k < f->ninsts && !r.bad; k++)
            f->locs[k] = r_id(&r);
    }

    if (r.bad || r.p != r.end) {
        limba_diag_set(d, 0,
                       r.bad ? "the file is truncated or malformed"
                             : "bytes after the end of the module");
        goto fail;
    }
    return m;
fail:
    limba_module_free(m);
    return NULL;
}
