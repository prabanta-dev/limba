/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower.c - a checked Luxia program in the IR (see lower.h): types,
 * functions, variables, statements.
 */
#include "lower.h"

#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const limba_typeinfo *ti(const lxl *L, limba_ltype t)
{
    return &L->S->ts.t[t];
}

static limba_lx_node *nd(const lxl *L, uint32_t node)
{
    return &L->S->t->node[node];
}

static uint32_t list_n(const lxl *L, uint32_t list)
{
    return list ? nd(L, list)->b : 0;
}

static uint32_t list_at(const lxl *L, uint32_t list, uint32_t i)
{
    return limba_lx_list_at(L->S->t, list, i);
}

/* ---- types ---- */

bool lxl_scalar(const lxl *L, limba_ltype t)
{
    switch (ti(L, t)->kind) {
    case LIMBA_LTK_INT:
    case LIMBA_LTK_FLOAT:
    case LIMBA_LTK_BOOL:
    case LIMBA_LTK_CHAR:
    case LIMBA_LTK_ENUM:
    case LIMBA_LTK_STRING:
    case LIMBA_LTK_POINTER:
    case LIMBA_LTK_NIL:
        return true;
    }
    return false;
}

limba_id lxl_type(lxl *L, limba_ltype t)
{
    if (L->tmap[t])
        return L->tmap[t];
    const limba_typeinfo *x = ti(L, t);
    limba_id r = LIMBA_T_VOID;
    switch (x->kind) {
    case LIMBA_LTK_INT:
        r = x->bits == 8    ? LIMBA_T_I8
            : x->bits == 16 ? LIMBA_T_I16
            : x->bits == 32 ? LIMBA_T_I32
                            : LIMBA_T_I64;
        break;
    case LIMBA_LTK_ENUM:
        r = x->size == 1   ? LIMBA_T_I8
            : x->size == 2 ? LIMBA_T_I16
                           : LIMBA_T_I32;
        break;
    case LIMBA_LTK_BOOL:
        r = LIMBA_T_I1;
        break;
    case LIMBA_LTK_CHAR:
        r = LIMBA_T_I32;
        break;
    case LIMBA_LTK_FLOAT:
        r = x->bits == 32 ? LIMBA_T_F32 : LIMBA_T_F64;
        break;
    case LIMBA_LTK_STRING:
        r = LIMBA_T_STR;
        break;
    case LIMBA_LTK_POINTER:
    case LIMBA_LTK_NIL:
    case LIMBA_LTK_OPEN:
        r = LIMBA_T_PTR;
        break;
    case LIMBA_LTK_ARRAY: {
        limba_id e = lxl_type(L, x->elem);
        const limba_typeinfo *ix = ti(L, x->index);
        uint64_t n = (x->flags & LIMBA_TF_DYNAMIC) || ix->hi < ix->lo
                         ? 0
                         : (uint64_t)(ix->hi - ix->lo + 1);
        r = limba_type_array(L->m, e, n > UINT32_MAX ? 0 : (uint32_t)n);
        break;
    }
    case LIMBA_LTK_RECORD: {
        limba_member *mem = limba_xmalloc((x->count + 1) * sizeof(*mem));
        for (uint32_t i = 0; i < x->count; i++) {
            const limba_field *f = &L->S->ts.field[x->first + i];
            mem[i].type = lxl_type(L, f->type);
            mem[i].offset = (uint32_t)f->offset;
        }
        char name[64];
        snprintf(name, sizeof(name), "record.%u", t);
        if (x->name != UINT32_MAX) {
            size_t n;
            const char *s = lxs_spell(L->S, x->name, &n);
            snprintf(name, sizeof(name), "%.*s", (int)n, s);
        }
        limba_id sn = limba_str_intern(L->m, name, strlen(name));
        r = limba_type_struct(L->m, sn, mem, x->count, (uint32_t)x->size,
                              x->align);
        if (r == LIMBA_NONE) {
            snprintf(name, sizeof(name), "record.%u", t);
            sn = limba_str_intern(L->m, name, strlen(name));
            r = limba_type_struct(L->m, sn, mem, x->count, (uint32_t)x->size,
                                  x->align);
        }
        free(mem);
        break;
    }
    }
    L->tmap[t] = r;
    return r;
}

/* ---- emitting ---- */

limba_id lxl_emit(lxl *L, unsigned op, limba_id type, unsigned cc, int64_t imm,
                  int64_t imm2, const uint32_t *ops, uint32_t nops)
{
    return limba_inst_add(limba_ssa_func(L->ssa), L->cur, op, type, cc, imm,
                          imm2, ops, nops);
}

limba_id lxl_iconst(lxl *L, limba_id type, int64_t v)
{
    return lxl_emit(L, LIMBA_OP_ICONST, type, 0, v, 0, NULL, 0);
}

limba_id lxl_rt(lxl *L, unsigned rt, limba_id type, const uint32_t *args,
                uint32_t n)
{
    return lxl_emit(L, LIMBA_OP_CALLRT, type, 0, rt, 0, args, n);
}

void lxl_check(lxl *L, limba_id cond, int64_t code)
{
    lxl_emit(L, LIMBA_OP_CHECK, LIMBA_T_VOID, 0, code, 0, &cond, 1);
}

void lxl_at(lxl *L, uint32_t node)
{
    if (!L->ssa)
        return;
    /* found once per node: the same nodes come back many times */
    if (!L->node_pos[node]) {
        limba_where w;
        const limba_source *src = L->S->src;
        if (!limba_source_where(src, L->S->t->node[node].loc, &w))
            return;
        const char *path = src->file[w.file].path;
        limba_id file = limba_str_intern(L->m, path, strlen(path));
        L->node_pos[node] = limba_pos_add(L->m, file, w.line, w.col) + 1;
    }
    limba_ssa_func(L->ssa)->pos_cur = L->node_pos[node] - 1;
}

void lxl_goto_new(lxl *L, limba_id b)
{
    limba_ssa_br(L->ssa, L->cur, b);
    L->cur = b;
}

/* after return, exit, continue: the code that follows is unreachable */
static void dead_end(lxl *L)
{
    limba_id b = limba_ssa_block(L->ssa);
    limba_ssa_seal(L->ssa, b);
    L->cur = b;
}

/* ---- variables ---- */

/* a variable in the current function: SSA or a zeroed slot */
static void local(lxl *L, limba_sym s)
{
    limba_ltype t = L->S->st.sym[s].type;
    lxl_store *st = &L->store[s];
    if (!t)
        return;
    const limba_typeinfo *x = ti(L, t);
    if (lxl_scalar(L, t) && !L->taken[s]) {
        st->kind = LXL_SSA;
        st->var = limba_ssa_var(L->ssa, lxl_type(L, t));
        return;
    }
    limba_func *f = limba_ssa_func(L->ssa);
    uint64_t size = x->size ? x->size : 8;
    if (size > UINT32_MAX) {
        lxs_note(L->S, L->S->st.sym[s].loc, L->S->st.sym[s].len,
                 "too large for the stack");
        size = 8;
    }
    limba_id slot = limba_slot_add(f, (uint32_t)size, x->align ? x->align : 1);
    /* the address is made in the entry block, which dominates all */
    limba_id cur = L->cur;
    L->cur = 0;
    st->kind = LXL_MEM;
    st->addr = lxl_emit(L, LIMBA_OP_SLOT, LIMBA_T_PTR, 0, slot, 0, NULL, 0);
    L->cur = cur;
    uint32_t o[3] = {st->addr, lxl_iconst(L, LIMBA_T_I8, 0),
                     lxl_iconst(L, LIMBA_T_I64, (int64_t)size)};
    lxl_emit(L, LIMBA_OP_MEMSET, LIMBA_T_VOID, 0, 0, 0, o, 3);
}

limba_id lxl_temp(lxl *L, limba_ltype t, uint32_t node)
{
    const limba_typeinfo *x = ti(L, t);
    uint64_t size = x->size ? x->size : 8;
    if (size > UINT32_MAX) {
        lxs_error(L->S, LXE_UNSUPPORTED, node,
                  "a value too large for the stack is not translated yet");
        size = 8;
    }
    limba_id slot = limba_slot_add(limba_ssa_func(L->ssa), (uint32_t)size,
                                   x->align ? x->align : 1);
    limba_id cur = L->cur;
    L->cur = 0;
    limba_id a = lxl_emit(L, LIMBA_OP_SLOT, LIMBA_T_PTR, 0, slot, 0, NULL, 0);
    L->cur = cur;
    return a;
}

/* free the computed arrays alive from the n-th on, innermost first */
static void free_dyns(lxl *L, uint32_t n)
{
    for (uint32_t i = L->ndyns; i-- > n;) {
        limba_id a = L->store[L->dyns[i]].addr;
        lxl_rt(L, LIMBA_RT_MEM_FREE, LIMBA_T_VOID, &a, 1);
    }
}

/* the elements of type et in the bytes at p, none assigned: the first
   made invalid (§ 4.5), then copies that double what is done */
static void fill_dyn(lxl *L, limba_id p, limba_id bytes, limba_ltype et)
{
    int64_t esize = (int64_t)ti(L, et)->size;
    limba_id first = limba_ssa_block(L->ssa), head = limba_ssa_block(L->ssa),
             body = limba_ssa_block(L->ssa), out = limba_ssa_block(L->ssa);
    uint32_t c[2] = {bytes, lxl_iconst(L, LIMBA_T_I64, 0)};
    limba_id some =
        lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_NE, 0, 0, c, 2);
    limba_ssa_cbr(L->ssa, L->cur, some, first, out);
    limba_ssa_seal(L->ssa, first);
    L->cur = first;
    lxl_invalidate(L, p, 0, et);
    uint32_t done = limba_ssa_var(L->ssa, LIMBA_T_I64);
    limba_ssa_def(L->ssa, done, first, lxl_iconst(L, LIMBA_T_I64, esize));
    limba_ssa_br(L->ssa, first, head);
    L->cur = head;
    limba_id d = limba_ssa_use(L->ssa, done, head, UINT32_MAX);
    uint32_t c2[2] = {d, bytes};
    limba_id more =
        lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_ULT, 0, 0, c2, 2);
    limba_ssa_cbr(L->ssa, head, more, body, out);
    limba_ssa_seal(L->ssa, body);
    L->cur = body;
    uint32_t r[2] = {bytes, d};
    limba_id rest = lxl_emit(L, LIMBA_OP_SUB, LIMBA_T_I64, 0, 0, 0, r, 2);
    uint32_t lt[2] = {d, rest};
    uint32_t sel[3] = {
        lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_ULT, 0, 0, lt, 2), d,
        rest};
    limba_id chunk = lxl_emit(L, LIMBA_OP_SELECT, LIMBA_T_I64, 0, 0, 0, sel, 3);
    uint32_t a[2] = {p, d};
    uint32_t m[3] = {lxl_emit(L, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, 1, 0, a, 2), p,
                     chunk};
    lxl_emit(L, LIMBA_OP_MEMCPY, LIMBA_T_VOID, 0, 0, 0, m, 3);
    uint32_t nx[2] = {d, chunk};
    limba_ssa_def(L->ssa, done, body,
                  lxl_emit(L, LIMBA_OP_ADD, LIMBA_T_I64, 0, 0, 0, nx, 2));
    limba_ssa_br(L->ssa, body, head);
    limba_ssa_seal(L->ssa, head);
    limba_ssa_seal(L->ssa, out);
    L->cur = out;
}

/* an array whose bounds are computed: on the heap */
static void dynamic(lxl *L, limba_sym s, uint32_t tnode)
{
    limba_ltype t = L->S->st.sym[s].type;
    const limba_typeinfo *x = ti(L, t);
    lxl_store *st = &L->store[s];
    const limba_lx_node *in = nd(L, nd(L, tnode)->a);
    limba_ltype it = x->index;
    st->kind = LXL_DYN;
    st->lo = lxl_value(L, in->b);
    st->hi = lxl_value(L, in->c);
    limba_id lo = lxl_to_i64(L, st->lo, it), hi = lxl_to_i64(L, st->hi, it);
    uint32_t o2[2] = {hi, lo};
    limba_id d = lxl_emit(L, LIMBA_OP_SUB, LIMBA_T_I64, 0, 0, 0, o2, 2);
    uint32_t o3[2] = {d, lxl_iconst(L, LIMBA_T_I64, 1)};
    limba_id n = lxl_emit(L, LIMBA_OP_ADD, LIMBA_T_I64, 0, 0, 0, o3, 2);
    /* an empty range holds no element (§ 4.5) */
    uint32_t c[2] = {n, lxl_iconst(L, LIMBA_T_I64, 0)};
    uint32_t sel[3] = {
        lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_SLT, 0, 0, c, 2), c[1],
        n};
    n = lxl_emit(L, LIMBA_OP_SELECT, LIMBA_T_I64, 0, 0, 0, sel, 3);
    uint32_t o4[2] = {
        n, lxl_iconst(L, LIMBA_T_I64, (int64_t)ti(L, x->elem)->size)};
    limba_id bytes = lxl_emit(L, LIMBA_OP_MULOV, LIMBA_T_I64, 0, 0, 0, o4, 2);
    st->addr = lxl_rt(L, LIMBA_RT_MEM_ALLOC, LIMBA_T_PTR, &bytes, 1);
    uint32_t o5[3] = {st->addr, lxl_iconst(L, LIMBA_T_I8, 0), bytes};
    lxl_emit(L, LIMBA_OP_MEMSET, LIMBA_T_VOID, 0, 0, 0, o5, 3);
    if (lxl_has_narrow(L, x->elem))
        fill_dyn(L, st->addr, bytes, x->elem);
    LIMBA_GROW(L->dyns, L->ndyns, L->capdyns);
    L->dyns[L->ndyns++] = s;
}

/* the names of a VAR node: storage, then the initial value */
static void var_decl(lxl *L, uint32_t d)
{
    const limba_lx_node *x = nd(L, d);
    uint32_t names = x->a, init = x->c, tnode = x->b;
    for (uint32_t k = 0; k < list_n(L, names); k++) {
        limba_sym s = L->S->sym[list_at(L, names, k)];
        if (!s)
            continue;
        limba_ltype t = L->S->st.sym[s].type;
        if (t && (ti(L, t)->flags & LIMBA_TF_DYNAMIC))
            dynamic(L, s, tnode);
        else if (L->store[s].kind == LXL_NONE)
            local(L, s);
        if (!init && t && !lxl_scalar(L, t) &&
            !(ti(L, t)->flags & LIMBA_TF_DYNAMIC) && lxl_has_narrow(L, t))
            lxl_invalidate(L, lxl_var_addr(L, s), 0, t); /* § 4.5 */
        if (init) {
            uint32_t tmp = list_at(L, names, k);
            /* a REF-like assignment to the name just declared */
            lxl_store *st = &L->store[s];
            limba_id v;
            if (lxl_scalar(L, t)) {
                v = lxl_value(L, init);
                lxl_at(L, tmp); /* a range is checked at the name */
                v = lxl_coerce(L, v, L->S->type[init], t);
                if (st->kind == LXL_SSA)
                    limba_ssa_def(L->ssa, st->var, L->cur, v);
                else {
                    uint32_t o[2] = {v, lxl_var_addr(L, s)};
                    lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, o, 2);
                }
            } else {
                uint32_t o[3] = {
                    lxl_var_addr(L, s), lxl_addr(L, init),
                    lxl_iconst(L, LIMBA_T_I64, (int64_t)ti(L, t)->size)};
                lxl_emit(L, LIMBA_OP_MEMCPY, LIMBA_T_VOID, 0, 0, 0, o, 3);
            }
            (void)tmp;
        }
    }
}

/* ---- statements ---- */

static void stmts(lxl *L, uint32_t list);

static void push_loop(lxl *L, limba_id exit, limba_id cont)
{
    LIMBA_GROW(L->loops, L->nloops, L->caploops);
    L->loops[L->nloops++] = (lxl_loop){exit, cont, L->ndyns};
}

static void if_stmt(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    uint32_t arms = x->a, other = x->b;
    limba_id join = limba_ssa_block(L->ssa);
    for (uint32_t i = 0; i < list_n(L, arms); i++) {
        uint32_t arm = list_at(L, arms, i);
        limba_id yes = limba_ssa_block(L->ssa), no = limba_ssa_block(L->ssa);
        lxl_branch(L, nd(L, arm)->a, yes, no);
        limba_ssa_seal(L->ssa, yes);
        limba_ssa_seal(L->ssa, no);
        L->cur = yes;
        stmts(L, nd(L, arm)->b);
        if (!limba_ssa_terminated(L->ssa, L->cur))
            limba_ssa_br(L->ssa, L->cur, join);
        L->cur = no;
    }
    if (other)
        stmts(L, other);
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, join);
    limba_ssa_seal(L->ssa, join);
    L->cur = join;
}

static void while_stmt(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    uint32_t cond = x->a, body = x->b;
    limba_id head = limba_ssa_block(L->ssa), in = limba_ssa_block(L->ssa),
             out = limba_ssa_block(L->ssa);
    lxl_goto_new(L, head);
    lxl_branch(L, cond, in, out);
    limba_ssa_seal(L->ssa, in);
    L->cur = in;
    push_loop(L, out, head);
    stmts(L, body);
    L->nloops--;
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, head);
    limba_ssa_seal(L->ssa, head);
    limba_ssa_seal(L->ssa, out);
    L->cur = out;
}

static void repeat_stmt(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    uint32_t body = x->a, cond = x->b;
    limba_id top = limba_ssa_block(L->ssa), test = limba_ssa_block(L->ssa),
             out = limba_ssa_block(L->ssa);
    lxl_goto_new(L, top);
    push_loop(L, out, test);
    stmts(L, body);
    L->nloops--;
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, test);
    limba_ssa_seal(L->ssa, test);
    L->cur = test;
    lxl_branch(L, cond, out, top);
    limba_ssa_seal(L->ssa, top);
    limba_ssa_seal(L->ssa, out);
    L->cur = out;
}

static void loop_stmt(lxl *L, uint32_t node)
{
    limba_id top = limba_ssa_block(L->ssa), out = limba_ssa_block(L->ssa);
    lxl_goto_new(L, top);
    push_loop(L, out, top);
    stmts(L, nd(L, node)->a);
    L->nloops--;
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, top);
    limba_ssa_seal(L->ssa, top);
    limba_ssa_seal(L->ssa, out);
    L->cur = out;
}

/* for var i := a to b: the bounds once, no step past the last value */
static void for_stmt(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    uint32_t name = x->a, range = x->c, body = x->d;
    bool down = x->op == LX_KW_DOWNTO;
    limba_sym s = L->S->sym[name];
    limba_ltype t = s ? L->S->st.sym[s].type : 0;
    if (!t)
        return;
    limba_id it = lxl_type(L, t);
    uint32_t fn = nd(L, range)->a, tn = nd(L, range)->b;
    limba_id from = lxl_value(L, fn);
    lxl_at(L, node); /* a range is checked at the for */
    from = lxl_coerce(L, from, L->S->type[fn], t);
    limba_id to = lxl_value(L, tn);
    lxl_at(L, node);
    to = lxl_coerce(L, to, L->S->type[tn], t);
    bool sg = lxl_signed(L, t);
    unsigned cc = down ? (sg ? LIMBA_CC_SGE : LIMBA_CC_UGE)
                       : (sg ? LIMBA_CC_SLE : LIMBA_CC_ULE);
    uint32_t c[2] = {from, to};
    limba_id enter = lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, cc, 0, 0, c, 2);
    limba_id bodyb = limba_ssa_block(L->ssa), latch = limba_ssa_block(L->ssa),
             step = limba_ssa_block(L->ssa), out = limba_ssa_block(L->ssa);
    lxl_store *st = &L->store[s];
    st->kind = LXL_SSA;
    st->var = limba_ssa_var(L->ssa, it);
    limba_ssa_def(L->ssa, st->var, L->cur, from);
    limba_ssa_cbr(L->ssa, L->cur, enter, bodyb, out);
    L->cur = bodyb;
    push_loop(L, out, latch);
    stmts(L, body);
    L->nloops--;
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, latch);
    limba_ssa_seal(L->ssa, latch);
    L->cur = latch;
    limba_id i = limba_ssa_use(L->ssa, st->var, latch, UINT32_MAX);
    uint32_t e[2] = {i, to};
    limba_id last =
        lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_EQ, 0, 0, e, 2);
    limba_ssa_cbr(L->ssa, latch, last, out, step);
    limba_ssa_seal(L->ssa, step);
    L->cur = step;
    uint32_t o[2] = {i, lxl_iconst(L, it, 1)};
    limba_id next =
        lxl_emit(L, down ? LIMBA_OP_SUB : LIMBA_OP_ADD, it, 0, 0, 0, o, 2);
    limba_ssa_def(L->ssa, st->var, step, next);
    limba_ssa_br(L->ssa, step, bodyb);
    limba_ssa_seal(L->ssa, bodyb);
    limba_ssa_seal(L->ssa, out);
    L->cur = out;
}

static void case_stmt(lxl *L, uint32_t node)
{
    limba_lxs *S = L->S;
    const limba_lx_node *x = nd(L, node);
    uint32_t sel = x->a, whens = x->b, other = x->c;
    limba_id v = lxl_value(L, sel);
    limba_id from = L->cur;
    limba_id join = limba_ssa_block(L->ssa), dflt = limba_ssa_block(L->ssa);
    int64_t *vals = NULL;
    limba_id *targets = NULL;
    uint32_t n = 0, capv = 0, capt = 0, nt = 0;
    limba_id *arm = limba_xmalloc((list_n(L, whens) + 1) * sizeof(*arm));
    for (uint32_t i = 0; i < list_n(L, whens); i++) {
        uint32_t w = list_at(L, whens, i), labels = nd(L, w)->a;
        arm[i] = limba_ssa_block(L->ssa);
        for (uint32_t k = 0; k < list_n(L, labels); k++) {
            uint32_t lab = list_at(L, labels, k);
            __int128 lo = 0, hi;
            lxs_value_to_int(S, S->val[nd(L, lab)->a], &lo);
            hi = lo;
            if (nd(L, lab)->b)
                lxs_value_to_int(S, S->val[nd(L, lab)->b], &hi);
            if (hi - lo > 4096) {
                lxs_error(S, LXE_UNSUPPORTED, lab,
                          "a range of more than 4096 values in a case is not "
                          "translated yet");
                hi = lo;
            }
            for (__int128 c = lo; c <= hi; c++) {
                LIMBA_GROW(vals, n, capv);
                vals[n++] = (int64_t)(uint64_t)(unsigned __int128)c;
                LIMBA_GROW(targets, nt, capt);
                targets[nt++] = arm[i];
            }
        }
    }
    limba_ssa_switch(L->ssa, from, v, dflt, vals, targets, n);
    for (uint32_t i = 0; i < list_n(L, whens); i++) {
        limba_ssa_seal(L->ssa, arm[i]);
        L->cur = arm[i];
        stmts(L, nd(L, list_at(L, whens, i))->b);
        if (!limba_ssa_terminated(L->ssa, L->cur))
            limba_ssa_br(L->ssa, L->cur, join);
    }
    limba_ssa_seal(L->ssa, dflt);
    L->cur = dflt;
    if (other)
        stmts(L, other);
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, join);
    limba_ssa_seal(L->ssa, join);
    L->cur = join;
    free(vals);
    free(targets);
    free(arm);
}

static void exit_stmt(lxl *L, uint32_t node, bool exit)
{
    uint32_t cond = nd(L, node)->a;
    if (!L->nloops)
        return;
    const lxl_loop *lp = &L->loops[L->nloops - 1];
    limba_id target = exit ? lp->exit : lp->cont;
    uint32_t keep = lp->ndyn;
    if (!cond) {
        free_dyns(L, keep);
        limba_ssa_br(L->ssa, L->cur, target);
        dead_end(L);
        return;
    }
    limba_id go_on = limba_ssa_block(L->ssa);
    if (L->ndyns > keep) {
        /* the arrays of the loop body are freed on the way out */
        limba_id leave = limba_ssa_block(L->ssa);
        lxl_branch(L, cond, leave, go_on);
        limba_ssa_seal(L->ssa, leave);
        L->cur = leave;
        free_dyns(L, keep);
        limba_ssa_br(L->ssa, leave, target);
    } else {
        lxl_branch(L, cond, target, go_on);
    }
    limba_ssa_seal(L->ssa, go_on);
    L->cur = go_on;
}

static void store_outs(lxl *L, uint32_t node);

static void return_stmt(lxl *L, uint32_t node)
{
    uint32_t e = nd(L, node)->a;
    limba_id v = LIMBA_NONE;
    if (e && L->ret_ptr != LIMBA_NONE) {
        /* a record or an array: into the slot of the caller */
        uint32_t o[3] = {
            L->ret_ptr, lxl_addr(L, e),
            lxl_iconst(L, LIMBA_T_I64, (int64_t)ti(L, L->result)->size)};
        lxl_emit(L, LIMBA_OP_MEMCPY, LIMBA_T_VOID, 0, 0, 0, o, 3);
    } else if (e) {
        v = lxl_value(L, e);
        lxl_at(L, node); /* a range is checked at the return */
        v = lxl_coerce(L, v, L->S->type[e], L->result);
    }
    store_outs(L, node);
    free_dyns(L, 0);
    limba_ssa_ret(L->ssa, L->cur, v);
    dead_end(L);
}

static void const_stmt(lxl *L, uint32_t node)
{
    (void)L;
    (void)node; /* its value is folded into every use */
}

static void stmt(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    limba_id r;
    lxl_at(L, node);
    switch (x->kind) {
    case LXN_ASSIGN:
        lxl_assign(L, node, x->a, x->b);
        break;
    case LXN_CALLST:
        lxl_call(L, x->a, &r);
        break;
    case LXN_IF:
        if_stmt(L, node);
        break;
    case LXN_CASE:
        case_stmt(L, node);
        break;
    case LXN_WHILE:
        while_stmt(L, node);
        break;
    case LXN_REPEAT:
        repeat_stmt(L, node);
        break;
    case LXN_FOR:
        for_stmt(L, node);
        break;
    case LXN_LOOP:
        loop_stmt(L, node);
        break;
    case LXN_EXIT:
        exit_stmt(L, node, true);
        break;
    case LXN_CONTINUE:
        exit_stmt(L, node, false);
        break;
    case LXN_RETURN:
        return_stmt(L, node);
        break;
    case LXN_VAR:
        var_decl(L, node);
        break;
    case LXN_CONST:
        const_stmt(L, node);
        break;
    }
}

static void stmts(lxl *L, uint32_t list)
{
    uint32_t keep = L->ndyns;
    for (uint32_t i = 0; i < list_n(L, list); i++)
        stmt(L, list_at(L, list, i));
    if (L->ndyns > keep) {
        if (!limba_ssa_terminated(L->ssa, L->cur))
            free_dyns(L, keep);
        L->ndyns = keep;
    }
}

/* ---- address taken: var and out arguments, readline, val ---- */

static void mark_taken(lxl *L, uint32_t a)
{
    if (nd(L, a)->kind == LXN_REF && L->S->sym[a])
        L->taken[L->S->sym[a]] = 1;
}

static void scan_taken(lxl *L)
{
    limba_lxs *S = L->S;
    for (uint32_t n = 1; n < S->t->nnode; n++) {
        const limba_lx_node *x = nd(L, n);
        if (x->kind != LXN_CALL)
            continue;
        limba_sym s = S->sym[x->a];
        if (!s)
            continue;
        const limba_symbol *y = &S->st.sym[s];
        uint32_t args = x->b;
        if (y->kind == LIMBA_LSYM_BUILTIN) {
            if (y->value == LXB_READLINE && list_n(L, args) > 0)
                mark_taken(L, list_at(L, args, 0));
            if (y->value == LXB_VAL && list_n(L, args) > 1)
                mark_taken(L, list_at(L, args, 1));
            continue;
        }
        if (y->kind != LIMBA_LSYM_ROUTINE || !y->type)
            continue;
        const limba_typeinfo *sig = ti(L, y->type);
        for (uint32_t i = 0; i < sig->count && i < list_n(L, args); i++)
            if (S->ts.param[sig->first + i].mode != LXS_IN)
                mark_taken(L, list_at(L, args, i));
    }
}

/* ---- functions ---- */

static void undefined(void *ctx, uint32_t tag)
{
    lxl *L = ctx;
    if ((tag & 0xc0000000u) == 0x40000000u) {
        uint32_t k = tag & 0x3fffffffu;
        limba_sym s = L->out_place[2 * k];
        size_t n = 0;
        const char *sp = lxs_spell(L->S, s, &n);
        lxs_error(L->S, LXE_OUT_UNASSIGNED, L->out_place[2 * k + 1],
                  "the out parameter '%.*s' may leave without a value", (int)n,
                  sp);
        return;
    }
    if (tag & 0x80000000u) {
        lxs_error(L->S, LXE_MISSING_RETURN, tag & 0x7fffffffu,
                  "a path reaches the end of the function without return");
        return;
    }
    limba_sym s = L->S->sym[tag];
    size_t n = 0;
    const char *sp = s ? lxs_spell(L->S, s, &n) : "";
    lxs_error(L->S, LXE_UNASSIGNED, tag,
              "'%.*s' may be read before it is given a value", (int)n, sp);
}

static limba_id func_type(lxl *L, limba_sym s)
{
    const limba_typeinfo *sig = ti(L, L->S->st.sym[s].type);
    limba_id *ps = limba_xmalloc((3 * sig->count + 2) * sizeof(*ps));
    uint32_t n = 0;
    bool agg = sig->elem != L->S->ts.void_ && !lxl_scalar(L, sig->elem);
    if (agg)
        ps[n++] = LIMBA_T_PTR; /* where the result goes */
    for (uint32_t i = 0; i < sig->count; i++) {
        limba_param p = L->S->ts.param[sig->first + i];
        if (ti(L, p.type)->kind == LIMBA_LTK_OPEN) {
            ps[n++] = LIMBA_T_PTR; /* address, low, high */
            ps[n++] = LIMBA_T_I64;
            ps[n++] = LIMBA_T_I64;
        } else if (p.mode != LXS_IN || !lxl_scalar(L, p.type)) {
            ps[n++] = LIMBA_T_PTR;
        } else {
            ps[n++] = lxl_type(L, p.type);
        }
    }
    limba_id ret = sig->elem == L->S->ts.void_ || agg ? LIMBA_T_VOID
                                                      : lxl_type(L, sig->elem);
    limba_id ft = limba_type_func(L->m, ret, ps, n, false);
    free(ps);
    return ft;
}

static void begin_function(lxl *L, limba_id fid)
{
    limba_func *f = &L->m->funcs[fid];
    limba_block_add(f);
    const limba_type *ft = &L->m->types[f->type];
    for (uint32_t i = 0; i < ft->count; i++)
        limba_param_add(f, 0, L->m->members[ft->first + i].type);
    L->ssa = limba_ssa_new(L->m, fid);
    L->fid = fid;
    L->cur = 0;
    L->nloops = 0;
    L->nouts = 0;
    L->nout_place = 0;
    L->ndyns = 0;
    L->ret_ptr = LIMBA_NONE;
}

/* before a return: every out parameter goes back to its argument, and
   must have a value on every path that gets here (§ 8) */
static void store_outs(lxl *L, uint32_t node)
{
    for (uint32_t i = 0; i < L->nouts; i++) {
        const lxl_store *st = &L->store[L->outs[i]];
        uint32_t k = L->nout_place / 2;
        LIMBA_GROW(L->out_place, L->nout_place, L->capout_place);
        L->out_place[L->nout_place++] = L->outs[i];
        LIMBA_GROW(L->out_place, L->nout_place, L->capout_place);
        L->out_place[L->nout_place++] = node;
        limba_id v;
        if (st->kind == LXL_SSA) {
            v = limba_ssa_use(L->ssa, st->var, L->cur, 0x40000000u | k);
        } else {
            /* its address was needed: a cell, not checked */
            uint32_t a = st->addr;
            limba_ltype t = L->S->st.sym[L->outs[i]].type;
            v = lxl_emit(L, LIMBA_OP_LOAD, lxl_type(L, t), 0, 0, 0, &a, 1);
        }
        uint32_t o[2] = {v, st->back};
        lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, o, 2);
    }
}

static void end_function(lxl *L, uint32_t node, bool function)
{
    if (!limba_ssa_terminated(L->ssa, L->cur)) {
        store_outs(L, node);
        free_dyns(L, 0);
        if (function) {
            /* a use that no definition reaches is a missing return */
            bool agg = L->ret_ptr != LIMBA_NONE;
            uint32_t var = limba_ssa_var(L->ssa, agg ? LIMBA_T_I1
                                                     : lxl_type(L, L->result));
            limba_id v = limba_ssa_use(L->ssa, var, L->cur, node | 0x80000000u);
            limba_ssa_ret(L->ssa, L->cur, agg ? LIMBA_NONE : v);
        } else {
            limba_ssa_ret(L->ssa, L->cur, LIMBA_NONE);
        }
    }
    /* the jumps belong to no single place */
    limba_ssa_func(L->ssa)->pos_cur = 0;
    limba_ssa_finish(L->ssa, undefined, L);
    limba_ssa_free(L->ssa);
    L->ssa = NULL;
}

static void routine_body(lxl *L, limba_sym s)
{
    limba_lxs *S = L->S;
    const limba_symbol *y = &S->st.sym[s];
    const limba_lx_node *r = nd(L, y->node);
    uint32_t params = r->b, body = r->d;
    uint32_t locals = nd(L, body)->a, list = nd(L, body)->b;
    const limba_typeinfo *sig = ti(L, y->type);
    L->result = sig->elem == S->ts.void_ ? 0 : sig->elem;
    begin_function(L, L->func_of[s]);
    limba_func *f = limba_ssa_func(L->ssa);
    uint32_t k = 0, v = 0;
    if (L->result && !lxl_scalar(L, L->result))
        L->ret_ptr = f->blocks[0].insts[v++];
    /* assign the entry parameters to the storage, in order */
    for (uint32_t i = 0; i < list_n(L, params); i++) {
        uint32_t p = list_at(L, params, i);
        uint32_t names = nd(L, p)->a;
        for (uint32_t j = 0; j < list_n(L, names); j++) {
            limba_sym ps = S->sym[list_at(L, names, j)];
            limba_param pp = S->ts.param[sig->first + (k++)];
            lxl_store *st = &L->store[ps];
            f = limba_ssa_func(L->ssa);
            limba_id first = f->blocks[0].insts[v];
            if (ti(L, pp.type)->kind == LIMBA_LTK_OPEN) {
                st->kind = LXL_OPEN;
                st->addr = first;
                st->lo = f->blocks[0].insts[v + 1];
                st->hi = f->blocks[0].insts[v + 2];
                v += 3;
            } else if (pp.mode == LXS_OUT && lxl_scalar(L, pp.type)) {
                /* copied back at every return, as Ada does with scalars:
                   an SSA variable with no value yet, where a missing value
                   is found, or a cell when its address is needed */
                if (L->taken[ps]) {
                    local(L, ps);
                } else {
                    st->kind = LXL_SSA;
                    st->var = limba_ssa_var(L->ssa, lxl_type(L, pp.type));
                }
                st->back = first;
                LIMBA_GROW(L->outs, L->nouts, L->capouts);
                L->outs[L->nouts++] = ps;
                v++;
            } else if (pp.mode != LXS_IN || !lxl_scalar(L, pp.type)) {
                st->kind = LXL_MEM;
                st->addr = first;
                v++;
            } else if (L->taken[ps]) {
                local(L, ps);
                uint32_t o[2] = {first, st->addr};
                lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, o, 2);
                v++;
            } else {
                st->kind = LXL_SSA;
                st->var = limba_ssa_var(L->ssa, lxl_type(L, pp.type));
                limba_ssa_def(L->ssa, st->var, 0, first);
                v++;
            }
        }
    }
    for (uint32_t i = 0; i < list_n(L, locals); i++) {
        uint32_t d = list_at(L, locals, i);
        if (nd(L, d)->kind == LXN_VAR)
            var_decl(L, d);
    }
    L->routine_node = r->a;
    stmts(L, list);
    end_function(L, r->a, L->result != 0);
}

/* ---- the program ---- */

limba_module *limba_lxl_program(limba_lxs *S)
{
    if (S->rep->errors)
        return NULL;
    lxl L_ = {0}, *L = &L_;
    L->S = S;
    L->m = limba_module_new();
    L->store = limba_xcalloc(S->st.nsym + 1, sizeof(*L->store));
    L->taken = limba_xcalloc(S->st.nsym + 1, 1);
    L->func_of = limba_xcalloc(S->st.nsym + 1, sizeof(*L->func_of));
    L->tmap = limba_xcalloc(S->ts.n + 1, sizeof(*L->tmap));
    L->node_pos = limba_xcalloc(S->t->nnode + 1, sizeof(*L->node_pos));
    scan_taken(L);

    limba_lx_node *p = nd(L, S->t->root);
    uint32_t decls = p->b, body = p->c;
    limba_id main_name = limba_str_intern(L->m, "main", 4);
    limba_id main_type = limba_type_func(L->m, LIMBA_T_VOID, NULL, 0, false);
    limba_id main_fid =
        limba_func_add(L->m, main_name, main_type, LIMBA_SYM_EXPORT);
    /* the routines and the globals, named as declared */
    for (uint32_t i = 0; i < list_n(L, decls); i++) {
        uint32_t d = list_at(L, decls, i);
        const limba_lx_node *x = nd(L, d);
        if (x->kind == LXN_ROUTINE && S->sym[x->a]) {
            limba_sym s = S->sym[x->a];
            size_t n;
            const char *sp = lxs_spell(S, s, &n);
            limba_id name = limba_str_intern(L->m, sp, n);
            limba_id fid = limba_func_add(L->m, name, func_type(L, s), 0);
            if (fid == LIMBA_NONE) {
                char buf[160];
                snprintf(buf, sizeof(buf), "%.*s.routine", (int)n, sp);
                name = limba_str_intern(L->m, buf, strlen(buf));
                fid = limba_func_add(L->m, name, func_type(L, s), 0);
            }
            L->func_of[s] = fid;
        } else if (x->kind == LXN_VAR) {
            uint32_t names = x->a;
            for (uint32_t k = 0; k < list_n(L, names); k++) {
                limba_sym s = S->sym[list_at(L, names, k)];
                if (!s || !S->st.sym[s].type)
                    continue;
                size_t n;
                const char *sp = lxs_spell(S, s, &n);
                limba_id name = limba_str_intern(L->m, sp, n);
                limba_id g = limba_global_add(
                    L->m, name, lxl_type(L, S->st.sym[s].type), 0);
                if (g == LIMBA_NONE) {
                    char buf[160];
                    snprintf(buf, sizeof(buf), "%.*s.var", (int)n, sp);
                    name = limba_str_intern(L->m, buf, strlen(buf));
                    g = limba_global_add(L->m, name,
                                         lxl_type(L, S->st.sym[s].type), 0);
                }
                L->store[s].kind = LXL_GLOBAL;
                L->store[s].global = g;
            }
        }
    }
    for (uint32_t i = 0; i < list_n(L, decls); i++) {
        uint32_t d = list_at(L, decls, i);
        const limba_lx_node *x = nd(L, d);
        if (x->kind == LXN_ROUTINE && S->sym[x->a])
            routine_body(L, S->sym[x->a]);
    }
    /* main: the records and arrays without a value (§ 4.5), the initial
       values of the globals, then the body */
    L->result = 0;
    begin_function(L, main_fid);
    for (uint32_t i = 0; i < list_n(L, decls); i++) {
        uint32_t d = list_at(L, decls, i);
        const limba_lx_node *x = nd(L, d);
        if (x->kind != LXN_VAR || x->c)
            continue;
        for (uint32_t k = 0; k < list_n(L, x->a); k++) {
            limba_sym s = S->sym[list_at(L, x->a, k)];
            limba_ltype t = s ? S->st.sym[s].type : 0;
            if (t && !lxl_scalar(L, t) && lxl_has_narrow(L, t))
                lxl_invalidate(L, lxl_var_addr(L, s), 0, t);
        }
    }
    for (uint32_t i = 0; i < list_n(L, decls); i++) {
        uint32_t d = list_at(L, decls, i);
        const limba_lx_node *x = nd(L, d);
        if (x->kind == LXN_VAR && x->c)
            var_decl(L, d);
    }
    stmts(L, body);
    end_function(L, S->t->root, false);

    free(L->store);
    free(L->taken);
    free(L->func_of);
    free(L->tmap);
    free(L->loops);
    free(L->outs);
    free(L->out_place);
    free(L->dyns);
    free(L->node_pos);
    if (S->rep->errors) {
        limba_module_free(L->m);
        return NULL;
    }
    return L->m;
}
