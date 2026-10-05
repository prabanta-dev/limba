/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower.c - a checked Luxia program in the IR (see lower.h): types,
 * functions, variables, statements.
 */
#include "lower.h"

#include "common/hash.h"
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
    case LIMBA_LTK_BIGINT:
    case LIMBA_LTK_POINTER:
    case LIMBA_LTK_OPAQUE:
    case LIMBA_LTK_NIL:
        return true;
    }
    return false;
}

/* the name in the IR of a symbol: a unit's after its name, one of the
   library after $std. too (§ 7 of the proposal), then suffix */
static void sym_text(lxl *L, limba_sym s, const char *suffix, char *buf,
                     size_t cap)
{
    limba_lxs *S = L->S;
    size_t n;
    const char *sp = lxs_spell(S, s, &n);
    bool impl;
    uint32_t u = S->nunits ? lxs_unit_of(S, S->st.sym[s].scope, &impl) : 0;
    const limba_lx_node *r = S->nunits ? &S->t->node[S->units[u].root] : NULL;
    if (r && r->kind == LXN_UNIT && r->a) {
        const char *un = lxs_text_at(S, S->t->node[r->a].loc);
        snprintf(buf, cap, "%s%.*s.%.*s%s", S->units[u].library ? "$std." : "",
                 (int)lxs_ident_len(un), un, (int)n, sp, suffix);
    } else {
        snprintf(buf, cap, "%.*s%s", (int)n, sp, suffix);
    }
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
    case LIMBA_LTK_BIGINT:
        r = LIMBA_T_REF;
        break;
    case LIMBA_LTK_POINTER:
    case LIMBA_LTK_OPAQUE:
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
        char name[320];
        snprintf(name, sizeof(name), "record.%u", t);
        if (x->name != UINT32_MAX)
            sym_text(L, x->name, "", name, sizeof(name));
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

#define LVN_SIZE 1024 /* a power of 2 */

limba_id lxl_emit(lxl *L, unsigned op, limba_id type, unsigned cc, int64_t imm,
                  int64_t imm2, const uint32_t *ops, uint32_t nops)
{
    limba_func *f = limba_ssa_func(L->ssa);
    if (limba_ops[op].flags & LIMBA_OPF_CALL)
        L->ncalls++; /* it may free: a dangling check is due again */
    if (!(limba_ops[op].flags & LIMBA_OPF_PURE) || op == LIMBA_OP_PARAM ||
        op == LIMBA_OP_UNDEF || type == LIMBA_T_VOID || nops > 3)
        return limba_inst_add(f, L->cur, op, type, cc, imm, imm2, ops, nops);
    uint64_t h = limba_mix(LIMBA_FNV_SEED,
                           (uint64_t)op << 40 ^ (uint64_t)cc << 32 ^ type);
    h = limba_mix(h, (uint64_t)imm);
    h = limba_mix(h, (uint64_t)imm2);
    for (uint32_t i = 0; i < nops; i++)
        h = limba_mix(h, ops[i]);
    h = limba_mix(h, L->cur);
    if (!L->lvn)
        L->lvn = limba_xcalloc(LVN_SIZE, sizeof(*L->lvn));
    struct lxl_lvn *e = &L->lvn[h & (LVN_SIZE - 1)];
    if (e->gen == L->gen && e->block == L->cur) {
        const limba_inst *in = &f->insts[e->id];
        if (in->op == op && in->type == type && in->cc == cc &&
            in->imm == imm && in->imm2 == imm2 && in->nops == nops &&
            in->block == L->cur &&
            (!nops ||
             !memcmp(f->operands + in->first, ops, nops * sizeof(*ops))))
            return e->id;
    }
    limba_id id = limba_inst_add(f, L->cur, op, type, cc, imm, imm2, ops, nops);
    *e = (struct lxl_lvn){id, L->cur, L->gen};
    return id;
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

/* the check a code of an error at run time belongs to (§ 9) */
static unsigned check_of(int64_t code)
{
    switch (code) {
    case LXR_INDEX:
        return LXS_CHECK_INDEX;
    case LXR_RANGE:
        return LXS_CHECK_RANGE;
    case LXR_OVERFLOW:
        return LXS_CHECK_OVERFLOW;
    case LXR_DIVZERO:
        return LXS_CHECK_DIVISION;
    case LXR_CONVERSION:
        return LXS_CHECK_CONVERSION;
    case LXR_SHIFT:
        return LXS_CHECK_SHIFT;
    case LXR_NIL:
        return LXS_CHECK_NIL;
    case LXR_DANGLING:
        return LXS_CHECK_DANGLING;
    }
    return 0;
}

void lxl_via_set(lxl *L, limba_id a, limba_id p)
{
    if (a >= L->capvia) {
        uint32_t cap = L->capvia ? L->capvia : 256;
        while (cap <= a)
            cap *= 2;
        L->via = limba_xrealloc(L->via, cap, sizeof(*L->via));
        memset(L->via + L->capvia, 0xff,
               (size_t)(cap - L->capvia) * sizeof(*L->via));
        L->capvia = cap;
    }
    L->via[a] = p;
}

limba_id lxl_via(const lxl *L, limba_id a)
{
    return a < L->capvia ? L->via[a] : LIMBA_NONE;
}

void lxl_live(lxl *L, limba_id a)
{
    limba_id p = lxl_via(L, a);
    if (p == LIMBA_NONE || (L->suppress & LXS_CHECK_DANGLING))
        return;
    if (p == L->live_ptr && L->live_calls == L->ncalls &&
        L->live_block == L->cur)
        return; /* checked here already, and nothing freed since */
    lxl_check(L, lxl_rt(L, LIMBA_RT_PTR_LIVE, LIMBA_T_I1, &p, 1), LXR_DANGLING);
    L->live_ptr = p;
    L->live_calls = L->ncalls;
    L->live_block = L->cur;
}

bool lxl_holds_str(lxl *L, limba_ltype t)
{
    return t && limba_type_holds_str(L->m, lxl_type(L, t));
}

void lxl_rc(lxl *L, unsigned op, limba_id p, limba_ltype t, limba_id n)
{
    if (!lxl_holds_str(L, t))
        return;
    limba_id it = lxl_type(L, t);
    const limba_typeinfo *x = ti(L, t);
    if (x->kind == LIMBA_LTK_ARRAY && !L->m->types[it].count) {
        /* too many elements for the type of the IR, or computed: its
           elements, n times as many */
        const limba_typeinfo *ix = ti(L, x->index);
        uint64_t k = (uint64_t)(ix->hi - ix->lo + 1);
        uint32_t o[2] = {n, lxl_iconst(L, LIMBA_T_I64, (int64_t)k)};
        n = lxl_emit(L, LIMBA_OP_MUL, LIMBA_T_I64, 0, 0, 0, o, 2);
        it = lxl_type(L, x->elem);
    }
    uint32_t o[2] = {p, n};
    lxl_emit(L, op, LIMBA_T_VOID, 0, it, 0, o, 2);
}

void lxl_copy(lxl *L, limba_id dst, limba_id src, limba_ltype t)
{
    lxl_live(L, dst);
    lxl_live(L, src);
    limba_id one = lxl_iconst(L, LIMBA_T_I64, 1);
    lxl_rc(L, LIMBA_OP_RETAIN, src, t, one);
    lxl_rc(L, LIMBA_OP_RELEASE, dst, t, one);
    uint32_t o[3] = {dst, src,
                     lxl_iconst(L, LIMBA_T_I64, (int64_t)ti(L, t)->size)};
    lxl_emit(L, LIMBA_OP_MEMCPY, LIMBA_T_VOID, 0, 0, 0, o, 3);
}

bool lxl_overflow_checked(const lxl *L)
{
    return !(L->suppress & LXS_CHECK_OVERFLOW);
}

/* a check turned off by a pragma is not made: where it would fail, the
   behaviour is undefined (§ 9) */
void lxl_check(lxl *L, limba_id cond, int64_t code)
{
    if (L->suppress & check_of(code))
        return;
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
        /* the name of the file, interned once while it does not change */
        if (w.file + 1 != L->pos_file) {
            const char *path = src->file[w.file].irname
                                   ? src->file[w.file].irname
                                   : src->file[w.file].path;
            L->pos_file = w.file + 1;
            L->pos_name = limba_str_intern(L->m, path, strlen(path));
        }
        L->node_pos[node] = limba_pos_add(L->m, L->pos_name, w.line, w.col) + 1;
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
    /* a slot that holds Strings is typed: released at every return */
    bool counted = size == x->size && lxl_holds_str(L, t);
    limba_id slot =
        limba_slot_add_typed(f, (uint32_t)size, x->align ? x->align : 1,
                             counted ? lxl_type(L, t) : LIMBA_NONE);
    /* the address is made in the entry block, which dominates all */
    limba_id cur = L->cur;
    L->cur = 0;
    st->kind = LXL_MEM;
    st->addr = lxl_emit(L, LIMBA_OP_SLOT, LIMBA_T_PTR, 0, slot, 0, NULL, 0);
    L->cur = cur;
    /* a slot is zeroed at the entry of its function (IR § 4): only a
       declaration inside a loop starts again from zero, its Strings of
       the round before released */
    if (!L->nloops)
        return;
    if (counted)
        lxl_rc(L, LIMBA_OP_RELEASE, st->addr, t, lxl_iconst(L, LIMBA_T_I64, 1));
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
    bool counted = size == x->size && lxl_holds_str(L, t);
    limba_id slot = limba_slot_add_typed(limba_ssa_func(L->ssa), (uint32_t)size,
                                         x->align ? x->align : 1,
                                         counted ? lxl_type(L, t) : LIMBA_NONE);
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
        const lxl_store *st = &L->store[L->dyns[i]];
        limba_id a = st->addr;
        lxl_rc(L, LIMBA_OP_RELEASE, a,
               ti(L, L->S->st.sym[L->dyns[i]].type)->elem, st->count);
        lxl_rt(L, LIMBA_RT_MEM_FREE, LIMBA_T_VOID, &a, 1);
    }
}

/* the elements of type et in the bytes at p, none assigned: the first
   made invalid (§ 4.5), then copies that double what is done */
void lxl_fill_dyn(lxl *L, limba_id p, limba_id bytes, limba_ltype et)
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
    /* an empty range holds no element (§ 4.5), a length past an Int64 is
       out of memory (§ 3.7) */
    uint32_t c[2] = {hi, lo};
    limba_id empty = lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1,
                              lxl_signed(L, lxs_base(L->S, it)) ? LIMBA_CC_SLT
                                                                : LIMBA_CC_ULT,
                              0, 0, c, 2);
    limba_id n = lxl_count(L, empty, lo, hi);
    st->count = n;
    /* n * size never past INT64_MAX: out of memory, as new (no check) */
    uint64_t esize = ti(L, x->elem)->size ? ti(L, x->elem)->size : 1;
    uint32_t r[2] = {n,
                     lxl_iconst(L, LIMBA_T_I64, (int64_t)(INT64_MAX / esize))};
    limba_id room =
        lxl_emit(L, LIMBA_OP_ICMP, LIMBA_T_I1, LIMBA_CC_ULE, 0, 0, r, 2);
    lxl_emit(L, LIMBA_OP_CHECK, LIMBA_T_VOID, 0, LIMBA_TRAP_NOMEM, 0, &room, 1);
    uint32_t o4[2] = {
        n, lxl_iconst(L, LIMBA_T_I64, (int64_t)ti(L, x->elem)->size)};
    limba_id bytes = lxl_emit(L, LIMBA_OP_MUL, LIMBA_T_I64, 0, 0, 0, o4, 2);
    st->addr = lxl_rt(L, LIMBA_RT_MEM_ALLOC, LIMBA_T_PTR, &bytes, 1);
    uint32_t o5[3] = {st->addr, lxl_iconst(L, LIMBA_T_I8, 0), bytes};
    lxl_emit(L, LIMBA_OP_MEMSET, LIMBA_T_VOID, 0, 0, 0, o5, 3);
    if (lxl_has_narrow(L, x->elem))
        lxl_fill_dyn(L, st->addr, bytes, x->elem);
    LIMBA_GROW(L->dyns, L->ndyns, L->capdyns);
    L->dyns[L->ndyns++] = s;
}

static bool ready_decl(lxl *L, uint32_t d);

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
            } else if (nd(L, init)->kind == LXN_AGG &&
                       (ti(L, t)->flags & LIMBA_TF_DYNAMIC)) {
                /* a declaration: straight into its elements (§ 6.8) */
                limba_ltype it = ti(L, t)->index;
                lxl_agg_fill_dyn(L, init, st->addr, t,
                                 lxl_to_i64(L, st->lo, it),
                                 lxl_to_i64(L, st->hi, it));
            } else if (nd(L, init)->kind == LXN_AGG &&
                       (st->kind != LXL_GLOBAL || ready_decl(L, d))) {
                /* a local just declared, or a variable ready at once:
                   nothing can see it yet, straight in (§ 6.8) */
                lxl_agg_fill(L, init, lxl_var_addr(L, s), t);
            } else {
                limba_id dst = lxl_var_addr(L, s);
                lxl_copy(L, dst, lxl_addr(L, init), t);
            }
            (void)tmp;
        }
    }
}

/* ---- statements ---- */

static void stmts(lxl *L, uint32_t list);
static void pragma(lxl *L, uint32_t node);

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

/* an array or a string variable whose bounds never change while a loop
   runs over them: an array keeps its bounds for all its life; a string
   variable is kept when nothing assigns it or takes its address, and it
   is no global (a routine called in the loop could assign it) */
static bool fixed_bounds(const lxl *L, uint32_t ref)
{
    if (nd(L, ref)->kind != LXN_REF || !L->S->sym[ref])
        return false;
    limba_sym x = L->S->sym[ref];
    const limba_symbol *y = &L->S->st.sym[x];
    if (y->kind != LIMBA_LSYM_VAR && y->kind != LIMBA_LSYM_PARAM)
        return false;
    switch (ti(L, y->type)->kind) {
    case LIMBA_LTK_ARRAY:
    case LIMBA_LTK_OPEN:
        return true;
    case LIMBA_LTK_STRING:
        return L->store[x].kind != LXL_GLOBAL && !L->taken[x] &&
               !L->assigned[x];
    }
    return false;
}

/* the variable whose low (low) or high bound expression e stays within,
   0 if none: low(x), high(x), length(s) (the high bound of a string), a
   constant from 1 up for any string, i + c and i - c with c >= 0 for a
   for variable i already known */
static limba_sym bound_of(const lxl *L, uint32_t e, bool low)
{
    limba_lxs *S = L->S;
    const limba_lx_node *x = nd(L, e);
    if (x->kind == LXN_CALL && S->sym[x->a] &&
        S->st.sym[S->sym[x->a]].kind == LIMBA_LSYM_BUILTIN &&
        list_n(L, x->b) == 1) {
        unsigned id = S->st.sym[S->sym[x->a]].value;
        uint32_t a = list_at(L, x->b, 0);
        if (!fixed_bounds(L, a))
            return 0;
        bool str = ti(L, S->type[a])->kind == LIMBA_LTK_STRING;
        if (low ? id == LXB_LOW : id == LXB_HIGH || (str && id == LXB_LENGTH))
            return S->sym[a];
        return 0;
    }
    if (low && S->val[e]) {
        __int128 v;
        if (lxs_value_to_int(S, S->val[e], &v) && v >= 1)
            return LXL_ONE;
    }
    if (x->kind == LXN_REF && S->sym[e])
        return low ? L->store[S->sym[e]].lo_of : L->store[S->sym[e]].hi_of;
    if (x->kind == LXN_BINARY && x->op == (low ? LX_PLUS : LX_MINUS) &&
        nd(L, x->a)->kind == LXN_REF && S->val[x->b]) {
        __int128 c;
        if (lxs_value_to_int(S, S->val[x->b], &c) && c >= 0)
            return bound_of(L, x->a, low);
    }
    return 0;
}

bool lxl_in_bounds(const lxl *L, uint32_t base, uint32_t idx)
{
    limba_lxs *S = L->S;
    if (nd(L, idx)->kind != LXN_REF || !S->sym[idx] || !fixed_bounds(L, base))
        return false;
    limba_sym x = S->sym[base];
    const lxl_store *v = &L->store[S->sym[idx]];
    bool str = ti(L, S->type[base])->kind == LIMBA_LTK_STRING;
    return v->hi_of == x && (v->lo_of == x || (str && v->lo_of == LXL_ONE));
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
    st->lo_of = bound_of(L, down ? tn : fn, true);
    st->hi_of = bound_of(L, down ? fn : tn, false);
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
        lxl_copy(L, L->ret_ptr, lxl_addr(L, e), L->result);
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
    case LXN_PRAGMA:
        pragma(L, node);
        break;
    }
}

/* a pragma: its checks off, or back on */
static void pragma(lxl *L, uint32_t node)
{
    unsigned op = nd(L, node)->flags;
    if (op & LXS_UNSUPPRESS)
        L->suppress &= ~(op & LXS_CHECK_ALL);
    else
        L->suppress |= op & LXS_CHECK_ALL;
}

/* the pragmas among the declarations of list */
static void pragmas(lxl *L, uint32_t list)
{
    for (uint32_t i = 0; i < list_n(L, list); i++)
        if (nd(L, list_at(L, list, i))->kind == LXN_PRAGMA)
            pragma(L, list_at(L, list, i));
}

static void stmts(lxl *L, uint32_t list)
{
    uint32_t keep = L->ndyns;
    unsigned suppress = L->suppress; /* a pragma here lasts to the end */
    for (uint32_t i = 0; i < list_n(L, list); i++)
        stmt(L, list_at(L, list, i));
    L->suppress = suppress;
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
        if (x->kind == LXN_ASSIGN && nd(L, x->a)->kind == LXN_REF &&
            S->sym[x->a])
            L->assigned[S->sym[x->a]] = 1;
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
            if (y->value == LXB_FREECSTRING && list_n(L, args) > 0)
                mark_taken(L, list_at(L, args, 0));
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

/* how C takes a narrow integer of type t: sext for a signed one, zext
   for one without a sign (a Bits type too); none for i1 (C's _Bool) and
   the rest (IR § 11e) */
static uint8_t ext_of(lxl *L, limba_ltype t)
{
    limba_id it = lxl_type(L, t);
    if (it != LIMBA_T_I8 && it != LIMBA_T_I16 && it != LIMBA_T_I32)
        return LIMBA_EXT_NONE;
    const limba_typeinfo *x = ti(L, t);
    return (x->flags & LIMBA_TF_SIGNED) && !(x->flags & LIMBA_TF_MODULAR)
               ? LIMBA_EXT_SEXT
               : LIMBA_EXT_ZEXT;
}

/* the C signature of an external routine (§ 8.5, IR § 11e): a record by
   value is its struct, a record result too; var, out and arrays are
   addresses; a narrow integer says its extension */
static limba_id ext_type(lxl *L, limba_sym s)
{
    const limba_typeinfo *sig = ti(L, L->S->st.sym[s].type);
    limba_id *ps = limba_xmalloc((sig->count + 1) * sizeof(*ps));
    uint8_t *exts = limba_xcalloc(sig->count + 1, 1);
    for (uint32_t i = 0; i < sig->count; i++) {
        limba_param p = L->S->ts.param[sig->first + i];
        const limba_typeinfo *x = ti(L, p.type);
        bool addr = p.mode != LXS_IN || x->kind == LIMBA_LTK_ARRAY ||
                    x->kind == LIMBA_LTK_OPEN;
        ps[i] = addr ? LIMBA_T_PTR : lxl_type(L, p.type);
        exts[i] = addr ? LIMBA_EXT_NONE : ext_of(L, p.type);
    }
    bool none = sig->elem == L->S->ts.void_;
    limba_id ret = none ? LIMBA_T_VOID : lxl_type(L, sig->elem);
    limba_id ft = limba_type_func_ext(
        L->m, ret, none ? LIMBA_EXT_NONE : ext_of(L, sig->elem), ps, exts,
        sig->count, false);
    free(exts);
    free(ps);
    return ft;
}

bool lxl_external(const lxl *L, limba_sym s)
{
    const limba_symbol *y = &L->S->st.sym[s];
    return y->node && nd(L, y->node)->kind == LXN_ROUTINE &&
           nd(L, nd(L, y->node)->d)->kind == LXN_EXTERNAL;
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
    L->gen++; /* what the cache of lxl_emit holds is of another function */
    L->cur = 0;
    L->nloops = 0;
    L->nouts = 0;
    L->nout_place = 0;
    L->ndyns = 0;
    L->ret_ptr = LIMBA_NONE;
    L->live_ptr = LIMBA_NONE;
    if (L->capvia)
        memset(L->via, 0xff, (size_t)L->capvia * sizeof(*L->via));
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
    limba_edit e;
    limba_ssa_finish_edit(L->ssa, undefined, L, &e);
    limba_ssa_free(L->ssa);
    L->ssa = NULL;
    if (L->dry) {
        /* a routine no initialisation reaches: made for its errors */
        limba_edit_end(&e);
        limba_func_clear(&L->m->funcs[L->fid]);
    } else if (L->done && !L->S->rep->errors) {
        L->stopped = !L->done(L->ctx, L->m, L->fid, &e);
    } else {
        limba_edit_end(&e);
    }
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
    unsigned file = L->suppress;
    pragmas(L, locals); /* for the whole routine */
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
    L->suppress = file;
}

/* ---- the program ---- */

/* the declarations of a part of a file: the interface of a unit (0 for
   the program), its implementation or the program's (§ 11) */
static uint32_t file_decls(lxl *L, uint32_t u, bool impl)
{
    const limba_lx_node *r = nd(L, L->S->units[u].root);
    if (r->kind == LXN_UNIT)
        return impl ? r->c : r->b;
    return impl ? r->b : 0;
}

/* a symbol reached by an initialisation (all are, without units) */
static bool reached(const lxl *L, limba_sym s)
{
    return !L->S->reached || L->S->reached[s];
}

/* a var declaration whose names are ready before any code runs: an
   initial value constant, of a scalar type or a String (§ 11.5) */
static bool ready_decl(lxl *L, uint32_t d)
{
    const limba_lx_node *x = nd(L, d);
    if (!x->c || !list_n(L, x->a))
        return false;
    limba_sym s = L->S->sym[list_at(L, x->a, 0)];
    limba_ltype t = s ? L->S->st.sym[s].type : 0;
    if (!t)
        return false;
    if (lxs_agg_ready(L->S, x->c, t))
        return true; /* an aggregate of constants (§ 11.5) */
    if (!L->S->val[x->c])
        return false;
    unsigned k = ti(L, t)->kind;
    return k != LIMBA_LTK_RECORD && k != LIMBA_LTK_ARRAY &&
           k != LIMBA_LTK_BIGINT;
}

/* the name in the IR of a symbol, interned */
static limba_id ir_name(lxl *L, limba_sym s, const char *suffix)
{
    char buf[320];
    sym_text(L, s, suffix, buf, sizeof(buf));
    return limba_str_intern(L->m, buf, strlen(buf));
}

void lxl_begin_function(lxl *L, limba_id fid)
{
    begin_function(L, fid);
}

void lxl_end_function(lxl *L, uint32_t node, bool function)
{
    end_function(L, node, function);
}

limba_id lxl_eq_declare(lxl *L, limba_ltype t, limba_id ftype)
{
    limba_lxs *S = L->S;
    limba_sym ts = 0;
    for (limba_sym s = 1; s < S->st.nsym && !ts; s++)
        if (S->st.sym[s].kind == LIMBA_LSYM_TYPE && S->st.sym[s].type == t)
            ts = s;
    char buf[340];
    if (!ts)
        snprintf(buf, sizeof(buf), "$eq%u", ++L->neq_anon);
    limba_id fid = LIMBA_NONE;
    for (unsigned k = 1; fid == LIMBA_NONE; k++) {
        if (ts) {
            char suffix[24];
            snprintf(suffix, sizeof(suffix), k > 1 ? "$eq%u" : "$eq", k);
            sym_text(L, ts, suffix, buf, sizeof(buf));
        } else if (k > 1) {
            snprintf(buf, sizeof(buf), "$eq%u", ++L->neq_anon);
        }
        fid = limba_func_add(L->m, limba_str_intern(L->m, buf, strlen(buf)),
                             ftype, 0);
    }
    return fid;
}

/* the routines (those reached, or not) and the globals (the first time)
   of a declaration list */
static void declare(lxl *L, uint32_t decls, bool unreached)
{
    limba_lxs *S = L->S;
    for (uint32_t i = 0; decls && i < list_n(L, decls); i++) {
        uint32_t d = list_at(L, decls, i);
        const limba_lx_node *x = nd(L, d);
        if (x->kind == LXN_ROUTINE && S->sym[x->a] && x->d) {
            limba_sym s = S->sym[x->a];
            if (reached(L, s) == unreached)
                continue;
            limba_id name = ir_name(L, s, "");
            if (lxl_external(L, s)) {
                /* a routine of C: an extern of the module, its symbol
                   the name as written unless name gives it */
                const limba_lx_node *e = nd(L, x->d);
                size_t ln, sn;
                const char *lib =
                    limba_strtab_get(L->S->lx->strings, e->a, &ln);
                const char *sym =
                    e->c ? limba_strtab_get(L->S->lx->strings, e->b, &sn)
                         : lxs_spell(S, s, &sn);
                limba_id eid = limba_extern_add(
                    L->m, name, ext_type(L, s), limba_str_intern(L->m, sym, sn),
                    limba_str_intern(L->m, lib, ln));
                if (eid == LIMBA_NONE) {
                    name = ir_name(L, s, ".external");
                    eid = limba_extern_add(L->m, name, ext_type(L, s),
                                           limba_str_intern(L->m, sym, sn),
                                           limba_str_intern(L->m, lib, ln));
                }
                L->func_of[s] = eid;
                continue;
            }
            limba_id fid = limba_func_add(L->m, name, func_type(L, s), 0);
            if (fid == LIMBA_NONE) {
                name = ir_name(L, s, ".routine");
                fid = limba_func_add(L->m, name, func_type(L, s), 0);
            }
            L->func_of[s] = fid;
        } else if (x->kind == LXN_VAR && !unreached) {
            uint32_t names = x->a;
            for (uint32_t k = 0; k < list_n(L, names); k++) {
                limba_sym s = S->sym[list_at(L, names, k)];
                if (!s || !S->st.sym[s].type)
                    continue;
                limba_id name = ir_name(L, s, "");
                limba_id g = limba_global_add(
                    L->m, name, lxl_type(L, S->st.sym[s].type), 0);
                if (g == LIMBA_NONE) {
                    name = ir_name(L, s, ".var");
                    g = limba_global_add(L->m, name,
                                         lxl_type(L, S->st.sym[s].type), 0);
                }
                L->store[s].kind = LXL_GLOBAL;
                L->store[s].global = g;
            }
        }
    }
}

/* the bodies of the routines of a declaration list of file u, reached
   or not */
static void bodies(lxl *L, uint32_t u, uint32_t decls, const unsigned *off,
                   bool reach)
{
    limba_lxs *S = L->S;
    for (uint32_t i = 0; decls && i < list_n(L, decls) && !L->stopped; i++) {
        uint32_t d = list_at(L, decls, i);
        const limba_lx_node *x = nd(L, d);
        if (x->kind != LXN_ROUTINE || !S->sym[x->a] || !x->d)
            continue;
        limba_sym s = S->sym[x->a];
        if (lxl_external(L, s) || reached(L, s) != reach)
            continue;
        L->suppress = off[u];
        routine_body(L, s);
    }
}

limba_module *limba_lxl_program(limba_lxs *S)
{
    return limba_lxl_program_each(S, NULL, NULL);
}

limba_module *limba_lxl_program_each(limba_lxs *S,
                                     bool (*done)(void *ctx, limba_module *m,
                                                  limba_id fid, limba_edit *e),
                                     void *ctx)
{
    if (S->rep->errors)
        return NULL;
    lxl L_ = {0}, *L = &L_;
    L->S = S;
    L->done = done;
    L->ctx = ctx;
    L->m = limba_module_new();
    /* its traps are worded as Luxia says them: the texts of traps.def */
    L->m->language = limba_str_intern(L->m, "luxia", 5);
    /* C types by name or the C layout: bound to the platform (§ 3.13) */
    if (S->c_bound) {
        const char *tn = lxs_target_name(S->target);
        L->m->target = limba_str_intern(L->m, tn, strlen(tn));
    }
    L->store = limba_xcalloc(S->st.nsym + 1, sizeof(*L->store));
    L->taken = limba_xcalloc(S->st.nsym + 1, 1);
    L->assigned = limba_xcalloc(S->st.nsym + 1, 1);
    L->func_of = limba_xcalloc(S->st.nsym + 1, sizeof(*L->func_of));
    L->tmap = limba_xcalloc(S->ts.n + 1, sizeof(*L->tmap));
    L->node_pos = limba_xcalloc(S->t->nnode + 1, sizeof(*L->node_pos));
    L->eq_of = limba_xcalloc(S->ts.n + 1, sizeof(*L->eq_of));
    scan_taken(L);
    /* the files in the order of their initialisations, the program last
       (§ 11.5); the checks each file turns off for the whole of it */
    uint32_t nu = S->nunits;
    uint32_t *files = limba_xcalloc(nu, sizeof(*files));
    unsigned *off = limba_xcalloc(nu, sizeof(*off));
    for (uint32_t u = 0; u < nu; u++)
        files[S->units[u].order < nu ? S->units[u].order : u] = u;
    for (uint32_t u = 0; u < nu; u++) {
        L->suppress = S->suppress;
        for (int part = 0; part < 2; part++)
            pragmas(L, file_decls(L, u, part == 1));
        off[u] = L->suppress;
    }
    limba_id main_name = limba_str_intern(L->m, "main", 4);
    limba_id main_type = limba_type_func(L->m, LIMBA_T_VOID, NULL, 0, false);
    limba_id main_fid =
        limba_func_add(L->m, main_name, main_type, LIMBA_SYM_EXPORT);
    /* the routines and the globals, named as declared, a unit's after its
       name: the routines reached first, those no initialisation reaches
       after, to be dropped once their errors are known (§ 7 of the
       proposal: only what is reached goes in the IR) */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t k = 0; k < nu; k++)
            for (int part = 0; part < 2; part++)
                declare(L, file_decls(L, files[k], part == 1), pass == 1);
        if (pass == 0) {
            L->kept_funcs = L->m->nfuncs;
            L->kept_externs = L->m->nexterns;
        }
    }
    /* the typed constants of records and arrays, those of routines too:
       globals, filled first in main (§ 4.3) */
    for (uint32_t i = 0; i < S->ntconsts; i++) {
        limba_sym s = S->sym[nd(L, S->tconsts[i])->a];
        limba_ltype t = s ? S->st.sym[s].type : 0;
        if (!t)
            continue;
        limba_id g = LIMBA_NONE;
        for (unsigned k = 0; g == LIMBA_NONE && k < 1000; k++) {
            char suffix[24];
            snprintf(suffix, sizeof(suffix), k ? ".const%u" : "", k);
            g = limba_global_add(L->m, ir_name(L, s, suffix), lxl_type(L, t),
                                 0);
        }
        L->store[s].kind = LXL_GLOBAL;
        L->store[s].global = g;
    }
    /* the routines not reached: their errors (a missing return, a
       variable read before a value), then they go */
    L->dry = true;
    for (uint32_t k = 0; k < nu && !L->stopped; k++)
        for (int part = 0; part < 2; part++)
            bodies(L, files[k], file_decls(L, files[k], part == 1), off, false);
    L->dry = false;
    limba_module_truncate(L->m, L->kept_funcs, L->kept_externs);
    /* the comparison functions, before the routines that call them: the
       short ones go in line at -O1 */
    lxl_eq_functions(L);
    for (uint32_t k = 0; k < nu && !L->stopped; k++)
        for (int part = 0; part < 2; part++)
            bodies(L, files[k], file_decls(L, files[k], part == 1), off, true);
    if (L->stopped)
        goto done;
    /* main: the records and arrays without a value (§ 4.5) and the
       variables ready at once, of every file; then each initialisation:
       the initial values computed, the begin ... end of a unit; the
       program's body last */
    L->result = 0;
    begin_function(L, main_fid);
    for (uint32_t i = 0; i < S->ntconsts; i++) {
        const limba_lx_node *x = nd(L, S->tconsts[i]);
        limba_sym s = S->sym[x->a];
        if (s && S->st.sym[s].type && L->store[s].kind == LXL_GLOBAL)
            lxl_agg_fill(L, x->c, lxl_var_addr(L, s), S->st.sym[s].type);
    }
    for (uint32_t k = 0; k < nu; k++) {
        uint32_t u = files[k];
        L->suppress = off[u];
        for (int part = 0; part < 2; part++) {
            uint32_t decls = file_decls(L, u, part == 1);
            for (uint32_t i = 0; decls && i < list_n(L, decls); i++) {
                uint32_t d = list_at(L, decls, i);
                const limba_lx_node *x = nd(L, d);
                if (x->kind != LXN_VAR)
                    continue;
                if (x->c) {
                    if (ready_decl(L, d))
                        var_decl(L, d);
                    continue;
                }
                for (uint32_t m = 0; m < list_n(L, x->a); m++) {
                    limba_sym s = S->sym[list_at(L, x->a, m)];
                    limba_ltype t = s ? S->st.sym[s].type : 0;
                    if (t && !lxl_scalar(L, t) && lxl_has_narrow(L, t))
                        lxl_invalidate(L, lxl_var_addr(L, s), 0, t);
                }
            }
        }
    }
    uint32_t last = 0;
    for (uint32_t k = 0; k < nu; k++) {
        uint32_t u = files[k];
        L->suppress = off[u];
        for (int part = 0; part < 2; part++) {
            uint32_t decls = file_decls(L, u, part == 1);
            for (uint32_t i = 0; decls && i < list_n(L, decls); i++) {
                uint32_t d = list_at(L, decls, i);
                const limba_lx_node *x = nd(L, d);
                if (x->kind == LXN_VAR && x->c && !ready_decl(L, d))
                    var_decl(L, d);
            }
        }
        const limba_lx_node *r = nd(L, S->units[u].root);
        uint32_t body = r->kind == LXN_UNIT ? r->d : r->c;
        if (body)
            stmts(L, body);
        last = S->units[u].root;
    }
    end_function(L, last, false);

done:
    free(L->store);
    free(L->taken);
    free(L->assigned);
    free(L->func_of);
    free(L->tmap);
    free(L->loops);
    free(L->outs);
    free(L->out_place);
    free(L->dyns);
    free(L->node_pos);
    free(L->eq_of);
    free(L->lvn);
    free(L->via);
    free(files);
    free(off);
    if (S->rep->errors) {
        limba_module_free(L->m);
        return NULL;
    }
    return L->m;
}
