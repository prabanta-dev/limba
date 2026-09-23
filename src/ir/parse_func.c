/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * parse_func.c - reading the text form: a function body. Values and
 * blocks may be used before their definition: operands are resolved when
 * the function ends.
 */
#include "parse.h"

#include "common/xalloc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ---- inside a function ---- */

static bool vnum_same(const void *ctx, uint32_t id)
{
    const P *p = ctx;
    return p->vnum[id] == p->vnum[p->capvnum - 1];
}

/* lookups compare against a probe kept in the last slot of the array:
   grow_nums keeps that slot past every defined id */
static uint32_t find_value(P *p, uint64_t num)
{
    p->vnum[p->capvnum - 1] = num;
    return limba_hash_find(p->vidx, num * 0x9e3779b97f4a7c15ull, vnum_same, p);
}

static void grow_nums(uint64_t **arr, uint32_t *cap, uint32_t need)
{
    if (need + 1 <= *cap)
        return;
    uint32_t c = *cap ? *cap : 64;
    while (c < need + 1)
        c *= 2;
    *arr = limba_xrealloc(*arr, c, sizeof(**arr));
    *cap = c;
}

/* the hash of blocks uses bnum; same trick */
static bool bnum_same(const void *ctx, uint32_t id)
{
    const P *p = ctx;
    return p->bnum[id] == p->bnum[p->capbnum - 1];
}

static uint32_t find_block(P *p, uint64_t num)
{
    p->bnum[p->capbnum - 1] = num;
    return limba_hash_find(p->bidx, num * 0x9e3779b97f4a7c15ull, bnum_same, p);
}

/* record value id as the definition of v<num> */
static bool define_value(P *p, uint64_t num, limba_id id, unsigned line)
{
    grow_nums(&p->vnum, &p->capvnum, id + 1);
    if (find_value(p, num) != UINT32_MAX) {
        p->err = false;
        limba_diag_set(p->d, line, "line %u: v%" PRIu64 " defined twice", line,
                       num);
        p->err = true;
        return false;
    }
    p->vnum[id] = num;
    limba_hash_put(p->vidx, num * 0x9e3779b97f4a7c15ull, id);
    return true;
}

static void operand(P *p, uint32_t v)
{
    LIMBA_GROW(p->ops, p->nops, p->capops);
    p->ops[p->nops++] = v;
}

/* v<num> or b<num>, to be resolved when the function ends */
static void pending(P *p, uint64_t num, bool block)
{
    LIMBA_GROW(p->pend, p->npend, p->cappend);
    p->pend[p->npend].rel = p->nops;
    p->pend[p->npend].num = num;
    p->pend[p->npend].line = lp_peek(p)->line;
    p->pend[p->npend].block = block;
    p->npend++;
    operand(p, 0);
}

static bool value(P *p)
{
    if (lp_peek(p)->kind != TK_VALUE)
        return lp_fail(p, "%s expected", "a value v<n>");
    pending(p, lp_peek(p)->num, false);
    p->i++;
    return true;
}

static bool values(P *p, int n)
{
    for (int k = 0; k < n; k++)
        if ((k && !lp_expect(p, TK_COMMA, "','")) || !value(p))
            return false;
    return true;
}

/* (v1, v2, ...) */
static bool arglist(P *p, uint32_t *count)
{
    *count = 0;
    if (!lp_expect(p, TK_LPAREN, "'('"))
        return false;
    while (!lp_accept(p, TK_RPAREN)) {
        if (*count && !lp_expect(p, TK_COMMA, "',' or ')'"))
            return false;
        if (!value(p))
            return false;
        (*count)++;
    }
    return true;
}

/* b3 or b3(v1, v2) */
static bool target(P *p)
{
    if (lp_peek(p)->kind != TK_BLOCK)
        return lp_fail(p, "%s expected", "a block b<n>");
    pending(p, lp_peek(p)->num, true);
    p->i++;
    uint32_t at = p->nops;
    operand(p, 0);
    uint32_t n = 0;
    if (lp_peek(p)->kind == TK_LPAREN && !arglist(p, &n))
        return false;
    p->ops[at] = n;
    return true;
}

/* the operation named by an identifier, with its condition */
static bool operation(P *p, unsigned *op, unsigned *cc)
{
    const limba_tok *k = lp_peek(p);
    *cc = 0;
    if (k->kind != TK_IDENT)
        return lp_fail(p, "%s expected", "an operation");
    for (unsigned o = 0; o < LIMBA_OP_COUNT; o++)
        if (strlen(limba_ops[o].text) == k->n &&
            memcmp(limba_ops[o].text, k->s, k->n) == 0 &&
            limba_ops[o].format != LIMBA_F_CMP && o != LIMBA_OP_PARAM) {
            *op = o;
            p->i++;
            return true;
        }
    const char *dot = memchr(k->s, '.', k->n);
    if (dot) {
        size_t base = (size_t)(dot - k->s);
        bool fl = base == 4 && memcmp(k->s, "fcmp", 4) == 0;
        bool in = base == 4 && memcmp(k->s, "icmp", 4) == 0;
        size_t cn = k->n - base - 1;
        for (unsigned c = 0; (fl || in) && c < LIMBA_CC_COUNT; c++)
            if (limba_cc_is_float(c) == fl && strlen(limba_cc_text[c]) == cn &&
                memcmp(limba_cc_text[c], dot + 1, cn) == 0) {
                *op = fl ? LIMBA_OP_FCMP : LIMBA_OP_ICMP;
                *cc = c;
                p->i++;
                return true;
            }
    }
    return lp_fail(p, "%s", "unknown operation");
}

static bool statement(P *p)
{
    limba_func *f = p->f;
    uint64_t defnum = 0;
    bool def = false;
    unsigned line = lp_peek(p)->line;
    unsigned op = 0, cc = 0;
    limba_id ty = LIMBA_T_VOID;
    int64_t imm = 0, imm2 = 0;

    if (lp_peek(p)->kind == TK_VALUE) {
        def = true;
        defnum = lp_peek(p)->num;
        p->i++;
        if (!lp_expect(p, TK_EQ, "'='"))
            return false;
    }
    if (!operation(p, &op, &cc))
        return false;
    const limba_op_info *info = &limba_ops[op];
    if (!(info->flags & LIMBA_OPF_NO_RESULT) || (info->flags & LIMBA_OPF_CALL))
        if (!lp_type(p, &ty))
            return false;
    if (def != (ty != LIMBA_T_VOID))
        return lp_fail(p, "%s",
                       def ? "this operation produces no value"
                           : "a value needs a name: v<n> = ...");
    p->nops = 0;
    p->npend = 0;

    switch (info->format) {
    case LIMBA_F_ICONST:
        if (!lp_integer(p, &imm))
            return false;
        break;
    case LIMBA_F_FCONST:
        if (!lp_real(p, &imm))
            return false;
        break;
    case LIMBA_F_SCONST: {
        limba_id s;
        if (!lp_string(p, &s))
            return false;
        imm = s;
        break;
    }
    case LIMBA_F_TYPED:
    case LIMBA_F_NONE:
        break;
    case LIMBA_F_UN:
    case LIMBA_F_CONV:
    case LIMBA_F_LOAD:
        if (!value(p))
            return false;
        break;
    case LIMBA_F_BIN:
    case LIMBA_F_CMP:
    case LIMBA_F_STORE:
        if (!values(p, 2))
            return false;
        break;
    case LIMBA_F_TERN:
    case LIMBA_F_MEM3:
        if (!values(p, 3))
            return false;
        break;
    case LIMBA_F_SLOT:
        if (lp_peek(p)->kind != TK_SLOT)
            return lp_fail(p, "%s expected", "a slot $<n>");
        imm = (int64_t)lp_peek(p)->num;
        p->i++;
        break;
    case LIMBA_F_GADDR:
    case LIMBA_F_FADDR:
    case LIMBA_F_CALL:
    case LIMBA_F_CALL_EXT: {
        limba_id n;
        if (!lp_sym(p, &n))
            return false;
        limba_id id = info->format == LIMBA_F_GADDR ? limba_global_find(p->m, n)
                      : info->format == LIMBA_F_CALL_EXT
                          ? limba_extern_find(p->m, n)
                          : limba_func_find(p->m, n);
        if (id == LIMBA_NONE)
            return lp_fail(p, "%s",
                           info->format == LIMBA_F_GADDR ? "unknown global"
                           : info->format == LIMBA_F_CALL_EXT
                               ? "unknown extern"
                               : "unknown function");
        imm = id;
        uint32_t n2;
        if ((info->format == LIMBA_F_CALL ||
             info->format == LIMBA_F_CALL_EXT) &&
            !arglist(p, &n2))
            return false;
        break;
    }
    case LIMBA_F_CALL_RT: {
        const limba_tok *k = lp_peek(p);
        if (k->kind != TK_IDENT)
            return lp_fail(p, "%s expected", "a runtime function");
        limba_id id = limba_rt_find(k->s, k->n);
        if (id == LIMBA_NONE)
            return lp_fail(p, "%s", "unknown runtime function");
        p->i++;
        imm = id;
        uint32_t n2;
        if (!arglist(p, &n2))
            return false;
        break;
    }
    case LIMBA_F_CALL_IND: {
        limba_id sig;
        uint32_t n2;
        if (!lp_type(p, &sig) || !value(p) || !arglist(p, &n2))
            return false;
        imm = sig;
        break;
    }
    case LIMBA_F_ADDR:
        if (!values(p, 2) || !lp_expect(p, TK_COMMA, "','") ||
            !lp_integer(p, &imm) || !lp_expect(p, TK_COMMA, "','") ||
            !lp_integer(p, &imm2))
            return false;
        break;
    case LIMBA_F_BR:
        if (!target(p))
            return false;
        break;
    case LIMBA_F_CBR:
        if (!value(p) || !lp_expect(p, TK_COMMA, "','") || !target(p) ||
            !lp_expect(p, TK_COMMA, "','") || !target(p))
            return false;
        break;
    case LIMBA_F_SWITCH: {
        if (!value(p) || !lp_expect(p, TK_COMMA, "','"))
            return false;
        if (lp_peek(p)->kind != TK_BLOCK)
            return lp_fail(p, "%s expected", "a default block");
        pending(p, lp_peek(p)->num, true);
        p->i++;
        uint32_t at = p->nops;
        operand(p, 0);
        uint32_t cases = 0;
        if (!lp_expect(p, TK_LBRACK, "'['"))
            return false;
        while (!lp_accept(p, TK_RBRACK)) {
            int64_t v;
            if ((cases && !lp_expect(p, TK_COMMA, "',' or ']'")) ||
                !lp_integer(p, &v) || !lp_expect(p, TK_COLON, "':'"))
                return false;
            if (lp_peek(p)->kind != TK_BLOCK)
                return lp_fail(p, "%s expected", "a block");
            operand(p, (uint32_t)(uint64_t)v);
            operand(p, (uint32_t)((uint64_t)v >> 32));
            pending(p, lp_peek(p)->num, true);
            p->i++;
            cases++;
        }
        p->ops[at] = cases;
        break;
    }
    case LIMBA_F_RET:
        if (lp_peek(p)->kind == TK_VALUE && !value(p))
            return false;
        break;
    case LIMBA_F_TRAP:
        if (!lp_integer(p, &imm))
            return false;
        break;
    case LIMBA_F_CHECK:
        if (!value(p) || !lp_expect(p, TK_COMMA, "','") || !lp_integer(p, &imm))
            return false;
        break;
    case LIMBA_F_PARAM:
        return lp_fail(p, "%s", "parameters go after the block name");
    }

    if (p->cur == LIMBA_NONE)
        return lp_fail(p, "%s", "an instruction before the first block");
    limba_id id =
        limba_inst_add(f, p->cur, op, ty, cc, imm, imm2, p->ops, p->nops);
    uint32_t first = f->insts[id].first;
    for (uint32_t k = 0; k < p->npend; k++) {
        fixup x = {first + p->pend[k].rel, p->pend[k].num, p->pend[k].line};
        if (p->pend[k].block) {
            LIMBA_GROW(p->bfix, p->nbfix, p->capbfix);
            p->bfix[p->nbfix++] = x;
        } else {
            LIMBA_GROW(p->vfix, p->nvfix, p->capvfix);
            p->vfix[p->nvfix++] = x;
        }
    }
    if (def && !define_value(p, defnum, id, line))
        return false;
    /* after a terminator only a new block may follow */
    if (info->flags & LIMBA_OPF_TERMINATOR)
        p->cur = LIMBA_NONE;
    return true;
}

/* b3(v1: i64, v2: f64): */
static bool label(P *p)
{
    uint64_t num = lp_peek(p)->num;
    unsigned line = lp_peek(p)->line;
    p->i++;
    grow_nums(&p->bnum, &p->capbnum, p->f->nblocks + 1);
    if (find_block(p, num) != UINT32_MAX) {
        limba_diag_set(p->d, line, "line %u: b%" PRIu64 " defined twice", line,
                       num);
        p->err = true;
        return false;
    }
    if (p->cur != LIMBA_NONE)
        return lp_fail(p, "%s", "the previous block has no terminator");
    limba_id b = limba_block_add(p->f);
    grow_nums(&p->bnum, &p->capbnum, b + 1);
    p->bnum[b] = num;
    limba_hash_put(p->bidx, num * 0x9e3779b97f4a7c15ull, b);
    p->cur = b;
    if (lp_accept(p, TK_LPAREN)) {
        bool firstp = true;
        while (!lp_accept(p, TK_RPAREN)) {
            if (!firstp && !lp_expect(p, TK_COMMA, "',' or ')'"))
                return false;
            firstp = false;
            if (lp_peek(p)->kind != TK_VALUE)
                return lp_fail(p, "%s expected", "a parameter v<n>");
            uint64_t vn = lp_peek(p)->num;
            unsigned vl = lp_peek(p)->line;
            limba_id t;
            p->i++;
            if (!lp_expect(p, TK_COLON, "':'") || !lp_type(p, &t))
                return false;
            if (!define_value(p, vn, limba_param_add(p->f, b, t), vl))
                return false;
        }
    }
    return lp_expect(p, TK_COLON, "':' after the block");
}

static bool resolve(P *p)
{
    limba_func *f = p->f;
    for (uint32_t k = 0; k < p->nvfix; k++) {
        uint32_t id = find_value(p, p->vfix[k].num);
        if (id == UINT32_MAX) {
            limba_diag_set(p->d, p->vfix[k].line,
                           "line %u: v%" PRIu64 " is never defined",
                           p->vfix[k].line, p->vfix[k].num);
            return false;
        }
        f->operands[p->vfix[k].index] = id;
    }
    for (uint32_t k = 0; k < p->nbfix; k++) {
        uint32_t id = find_block(p, p->bfix[k].num);
        if (id == UINT32_MAX) {
            limba_diag_set(p->d, p->bfix[k].line,
                           "line %u: b%" PRIu64 " is never defined",
                           p->bfix[k].line, p->bfix[k].num);
            return false;
        }
        f->operands[p->bfix[k].index] = id;
    }
    return true;
}

bool lp_func(P *p)
{
    limba_id n, t;
    if (!lp_sym(p, &n) || !lp_expect(p, TK_COLON, "':'") || !lp_type(p, &t))
        return false;
    limba_func *f = &p->m->funcs[limba_func_find(p->m, n)];
    if (p->m->types[t].kind != LIMBA_TK_FUNC)
        return lp_fail(p, "%s", "a function needs a function type");
    f->type = t;
    if (lp_accept_word(p, "export"))
        f->flags |= LIMBA_SYM_EXPORT;
    if (!lp_expect(p, TK_LBRACE, "'{'"))
        return false;

    limba_hash_free(p->vidx);
    limba_hash_free(p->bidx);
    p->vidx = limba_hash_new();
    p->bidx = limba_hash_new();
    p->f = f;
    p->cur = LIMBA_NONE;
    p->nvfix = p->nbfix = 0;
    grow_nums(&p->vnum, &p->capvnum, 1);
    grow_nums(&p->bnum, &p->capbnum, 1);

    while (lp_accept_word(p, "slot")) {
        uint32_t size, align;
        if (!lp_uinteger32(p, &size) || !lp_expect_word(p, "align") ||
            !lp_uinteger32(p, &align))
            return false;
        limba_slot_add(f, size, align);
    }
    while (!lp_accept(p, TK_RBRACE)) {
        if (lp_peek(p)->kind == TK_EOF)
            return lp_fail(p, "%s", "'}' expected at the end of the function");
        if (lp_peek(p)->kind == TK_BLOCK) {
            if (!label(p))
                return false;
        } else if (!statement(p)) {
            return false;
        }
    }
    if (p->cur != LIMBA_NONE)
        return lp_fail(p, "%s", "the last block has no terminator");
    return resolve(p);
}
