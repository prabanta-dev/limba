/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * verify.c - is a module well formed? Types, symbols and layouts; in every
 * function the shape of the blocks, the shape and types of the operands,
 * and that every definition dominates its uses. It trusts nothing: a
 * module read from a broken file must make it say no, never crash.
 */
#include "internal.h"

#include "common/xalloc.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct limba_verifier {
    const limba_module *m;
    limba_diag *d;
    /* the function being checked */
    const limba_func *f;
    uint32_t fid;
    limba_cfg cfg;
    uint32_t *pos;  /* position of each instruction in its block */
    uint8_t *kinds; /* operand kinds of the current instruction */
    uint32_t capkinds;
} vctx;

#define FAIL(...)                                                              \
    do {                                                                       \
        limba_diag_set(v->d, 0, __VA_ARGS__);                                  \
        return false;                                                          \
    } while (0)

static const char *name_of(const limba_module *m, limba_id s)
{
    return s < limba_str_count(m) ? limba_str(m, s, NULL) : "?";
}

static bool type_ok(const limba_module *m, limba_id t)
{
    return t < m->ntypes;
}

/* a type a value may have: scalar, not void */
static bool is_value_type(const limba_module *m, limba_id t)
{
    return t > LIMBA_T_VOID && t < LIMBA_T_FIRST_USER && type_ok(m, t);
}

static bool is_pow2(uint32_t x)
{
    return x && !(x & (x - 1));
}

/* ---- module level ---- */

static bool check_types(vctx *v)
{
    const limba_module *m = v->m;
    for (limba_id t = LIMBA_T_FIRST_USER; t < m->ntypes; t++) {
        const limba_type *ty = &m->types[t];
        uint64_t last = (uint64_t)ty->first + ty->count;
        switch (ty->kind) {
        case LIMBA_TK_STRUCT:
            if (ty->name >= limba_str_count(m))
                FAIL("type %" PRIu32 ": struct without a name", t);
            if (!is_pow2(ty->align) || ty->size % ty->align)
                FAIL("type %%%s: size %" PRIu32 " and alignment %" PRIu32
                     " do not agree",
                     name_of(m, ty->name), ty->size, ty->align);
            if (last > m->nmembers)
                FAIL("type %%%s: fields out of range", name_of(m, ty->name));
            for (uint32_t i = 0; i < ty->count; i++) {
                const limba_member *f = &m->members[ty->first + i];
                if (f->type >= t || f->type == LIMBA_T_VOID ||
                    m->types[f->type].kind == LIMBA_TK_FUNC)
                    FAIL("type %%%s: field %" PRIu32
                         " has no valid type (a struct may only use types "
                         "defined before it)",
                         name_of(m, ty->name), i);
                const limba_type *ft = &m->types[f->type];
                if ((uint64_t)f->offset + ft->size > ty->size ||
                    (ft->align && f->offset % ft->align))
                    FAIL("type %%%s: field %" PRIu32 " at offset %" PRIu32
                         " does not fit or is misaligned",
                         name_of(m, ty->name), i, f->offset);
            }
            break;
        case LIMBA_TK_ARRAY:
            if (ty->elem >= t || ty->elem == LIMBA_T_VOID ||
                m->types[ty->elem].kind == LIMBA_TK_FUNC)
                FAIL("type %" PRIu32 ": array of an invalid type", t);
            break;
        case LIMBA_TK_FUNC:
            if (ty->elem >= m->ntypes ||
                (ty->elem != LIMBA_T_VOID && !is_value_type(m, ty->elem)))
                FAIL("type %" PRIu32 ": a function returns a scalar or void",
                     t);
            if (last > m->nmembers)
                FAIL("type %" PRIu32 ": parameters out of range", t);
            for (uint32_t i = 0; i < ty->count; i++)
                if (!is_value_type(m, m->members[ty->first + i].type))
                    FAIL("type %" PRIu32 ": parameter %" PRIu32
                         " is not a scalar",
                         t, i);
            break;
        default:
            FAIL("type %" PRIu32 ": kind %u is not a user type", t, ty->kind);
        }
    }
    return true;
}

static bool check_symbols(vctx *v)
{
    const limba_module *m = v->m;
    uint32_t nstr = limba_str_count(m);
    if (m->memory != LIMBA_MEM_STRICT && m->memory != LIMBA_MEM_FB)
        FAIL("memory model %" PRIu32 " is unknown", m->memory);
    if (m->name != LIMBA_NONE && m->name >= nstr)
        FAIL("module name out of range");
    for (uint32_t i = 0; i < m->nglobals; i++) {
        const limba_global *g = &m->globals[i];
        if (g->name >= nstr)
            FAIL("global %" PRIu32 ": no name", i);
        if (!type_ok(m, g->type) || g->type == LIMBA_T_VOID ||
            m->types[g->type].kind == LIMBA_TK_FUNC)
            FAIL("global @%s: invalid type", name_of(m, g->name));
        bool ok;
        switch (g->init) {
        case LIMBA_INIT_ZERO:
            ok = g->value == 0;
            break;
        case LIMBA_INIT_INT:
            ok = limba_type_is_int(g->type) || g->type == LIMBA_T_PTR;
            break;
        case LIMBA_INIT_FLOAT:
            ok = limba_type_is_float(g->type);
            break;
        case LIMBA_INIT_STR:
            ok = g->type == LIMBA_T_STR && (uint64_t)g->value < nstr;
            break;
        default:
            ok = false;
        }
        if (!ok)
            FAIL("global @%s: the initial value does not suit its type",
                 name_of(m, g->name));
    }
    for (uint32_t i = 0; i < m->nexterns; i++) {
        const limba_extern *e = &m->externs[i];
        if (e->name >= nstr || e->symbol >= nstr ||
            (e->library != LIMBA_NONE && e->library >= nstr))
            FAIL("extern %" PRIu32 ": names out of range", i);
        if (!type_ok(m, e->type) || m->types[e->type].kind != LIMBA_TK_FUNC)
            FAIL("extern @%s: not a function type", name_of(m, e->name));
    }
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        const limba_func *f = &m->funcs[i];
        if (f->name >= nstr)
            FAIL("function %" PRIu32 ": no name", i);
        if (!type_ok(m, f->type) || m->types[f->type].kind != LIMBA_TK_FUNC ||
            m->types[f->type].variadic)
            FAIL("function @%s: not a (non-variadic) function type",
                 name_of(m, f->name));
    }
    return true;
}

/* ---- inside a function ---- */

#define IFAIL(in_id, ...)                                                      \
    do {                                                                       \
        char where_[96];                                                       \
        snprintf(where_, sizeof(where_), "@%s, b%" PRIu32 ", inst %" PRIu32,   \
                 name_of(v->m, v->f->name), v->f->insts[in_id].block, in_id);  \
        char what_[160];                                                       \
        snprintf(what_, sizeof(what_), __VA_ARGS__);                           \
        limba_diag_set(v->d, 0, "%s: %s", where_, what_);                      \
        return false;                                                          \
    } while (0)

/* the type of value x used by instruction u, or LIMBA_NONE (and a
   diagnostic) if x is not a value that dominates u */
static bool use(vctx *v, uint32_t u, uint32_t x, limba_id *type)
{
    const limba_func *f = v->f;
    if (x >= f->ninsts)
        IFAIL(u, "operand v%" PRIu32 " does not exist", x);
    const limba_inst *d = &f->insts[x];
    if ((limba_ops[d->op].flags & LIMBA_OPF_NO_RESULT) ||
        d->type == LIMBA_T_VOID)
        IFAIL(u, "operand v%" PRIu32 " produces no value", x);
    limba_id bu = f->insts[u].block, bd = d->block;
    if (limba_cfg_reachable(&v->cfg, bu)) {
        bool ok = bu == bd ? v->pos[x] < v->pos[u]
                           : limba_cfg_reachable(&v->cfg, bd) &&
                                 limba_cfg_dominates(&v->cfg, bd, bu);
        if (!ok)
            IFAIL(u, "operand v%" PRIu32 " does not dominate this use", x);
    }
    *type = d->type;
    return true;
}

/* values of in, by operand index */
static bool vals(vctx *v, uint32_t u, limba_id *t, uint32_t n)
{
    const limba_inst *in = &v->f->insts[u];
    for (uint32_t i = 0; i < n; i++)
        if (!use(v, u, v->f->operands[in->first + i], &t[i]))
            return false;
    return true;
}

/* the arguments passed to target block at operand k */
static bool target(vctx *v, uint32_t u, uint32_t k, bool allow_args)
{
    const limba_func *f = v->f;
    limba_id b;
    uint32_t n;
    uint32_t a = limba_target(f, k, &b, &n);
    if (b >= f->nblocks)
        IFAIL(u, "branch to b%" PRIu32 ", which does not exist", b);
    if (b == 0)
        IFAIL(u, "branch to the entry block");
    const limba_block *bl = &f->blocks[b];
    if (n != bl->nparams)
        IFAIL(u, "b%" PRIu32 " takes %" PRIu32 " arguments, given %" PRIu32, b,
              bl->nparams, n);
    if (n && !allow_args)
        IFAIL(u, "switch targets take no arguments");
    for (uint32_t i = 0; i < n; i++) {
        limba_id t;
        if (!use(v, u, f->operands[a + i], &t))
            return false;
        if (t != f->insts[bl->insts[i]].type)
            IFAIL(u, "argument %" PRIu32 " to b%" PRIu32 " has the wrong type",
                  i, b);
    }
    return true;
}

static bool check_call(vctx *v, uint32_t u, const limba_inst *in)
{
    limba_sig s;
    if (!limba_call_sig(v->m, in, &s))
        IFAIL(u, "the callee does not exist");
    uint32_t first = 0;
    if (in->op == LIMBA_OP_CALLIND) {
        limba_id t;
        if (!use(v, u, v->f->operands[in->first], &t))
            return false;
        if (t != LIMBA_T_PTR)
            IFAIL(u, "an indirect callee is a ptr");
        first = 1;
    }
    uint32_t nargs = in->nops - first;
    if (nargs < s.n || (nargs > s.n && !s.variadic))
        IFAIL(u, "%" PRIu32 " arguments for %" PRIu32 " parameters", nargs,
              s.n);
    for (uint32_t i = 0; i < nargs; i++) {
        limba_id t;
        if (!use(v, u, v->f->operands[in->first + first + i], &t))
            return false;
        if (i < s.n && t != limba_sig_param(&s, i))
            IFAIL(u, "argument %" PRIu32 " has the wrong type", i);
    }
    if (in->type != s.ret)
        IFAIL(u, "the result type differs from the callee's");
    return true;
}

static bool check_conv(vctx *v, uint32_t u, const limba_inst *in, limba_id a)
{
    limba_id r = in->type;
    unsigned ba = limba_type_bits(a), br = limba_type_bits(r);
    bool ok;
    switch (in->op) {
    case LIMBA_OP_TRUNC:
        ok = ba && br && br < ba;
        break;
    case LIMBA_OP_ZEXT:
    case LIMBA_OP_SEXT:
        ok = ba && br && br > ba;
        break;
    case LIMBA_OP_FPTRUNC:
        ok = a == LIMBA_T_F64 && r == LIMBA_T_F32;
        break;
    case LIMBA_OP_FPEXT:
        ok = a == LIMBA_T_F32 && r == LIMBA_T_F64;
        break;
    case LIMBA_OP_SITOFP:
    case LIMBA_OP_UITOFP:
        ok = ba && limba_type_is_float(r);
        break;
    case LIMBA_OP_FPTOSI:
    case LIMBA_OP_FPTOUI:
        ok = limba_type_is_float(a) && br;
        break;
    case LIMBA_OP_BITCAST:
        ok = (a == LIMBA_T_I32 && r == LIMBA_T_F32) ||
             (a == LIMBA_T_F32 && r == LIMBA_T_I32) ||
             (a == LIMBA_T_I64 && r == LIMBA_T_F64) ||
             (a == LIMBA_T_F64 && r == LIMBA_T_I64);
        break;
    case LIMBA_OP_PTRTOINT:
        ok = a == LIMBA_T_PTR && br;
        break;
    case LIMBA_OP_INTTOPTR:
        ok = ba && r == LIMBA_T_PTR;
        break;
    default:
        ok = false;
    }
    if (!ok)
        IFAIL(u, "%s cannot turn %s into %s", limba_ops[in->op].text,
              limba_scalar_name(a) ? limba_scalar_name(a) : "?",
              limba_scalar_name(r) ? limba_scalar_name(r) : "?");
    return true;
}

static bool fits(int64_t x, unsigned bits)
{
    if (bits >= 64)
        return true;
    if (bits == 1)
        return x == 0 || x == 1;
    return x >= -((int64_t)1 << (bits - 1)) && x < ((int64_t)1 << bits);
}

static bool check_inst(vctx *v, uint32_t u)
{
    const limba_module *m = v->m;
    const limba_func *f = v->f;
    const limba_inst *in = &f->insts[u];
    const limba_op_info *op = &limba_ops[in->op];
    limba_id t[3];
    limba_id r = in->type;

    if (!type_ok(m, r) || (r != LIMBA_T_VOID && !is_value_type(m, r)))
        IFAIL(u, "result type out of range or not a scalar");
    if ((op->flags & LIMBA_OPF_NO_RESULT) && r != LIMBA_T_VOID)
        IFAIL(u, "%s has no result", op->text);
    if (!(op->flags & (LIMBA_OPF_NO_RESULT | LIMBA_OPF_CALL)) &&
        r == LIMBA_T_VOID)
        IFAIL(u, "%s needs a result type", op->text);
    if (in->cc && op->format != LIMBA_F_CMP)
        IFAIL(u, "a condition on %s", op->text);

    switch (op->format) {
    case LIMBA_F_ICONST:
        if (!limba_type_is_int(r) || !fits(in->imm, limba_type_bits(r)))
            IFAIL(u, "iconst %" PRId64 " does not fit its type", in->imm);
        break;
    case LIMBA_F_FCONST:
        if (!limba_type_is_float(r))
            IFAIL(u, "fconst of a type that is not a float");
        break;
    case LIMBA_F_SCONST:
        if (r != LIMBA_T_STR || (uint64_t)in->imm >= limba_str_count(m))
            IFAIL(u, "sconst: not a str, or no such string");
        break;
    case LIMBA_F_TYPED:
        if (in->op == LIMBA_OP_NULLV && r != LIMBA_T_PTR && r != LIMBA_T_STR &&
            r != LIMBA_T_REF)
            IFAIL(u, "null of a type that is not a handle or a pointer");
        break;
    case LIMBA_F_UN:
        if (!vals(v, u, t, 1))
            return false;
        if (t[0] != r ||
            (in->op == LIMBA_OP_FNEG || in->op == LIMBA_OP_FROUND ||
                     in->op == LIMBA_OP_FROUNDA
                 ? !limba_type_is_float(r)
                 : !limba_type_is_int(r)))
            IFAIL(u, "%s: operand and result types", op->text);
        break;
    case LIMBA_F_BIN: {
        if (!vals(v, u, t, 2))
            return false;
        bool fl = in->op >= LIMBA_OP_FADD && in->op <= LIMBA_OP_FDIV;
        if (t[0] != r || t[1] != r ||
            (fl ? !limba_type_is_float(r) : !limba_type_is_int(r)))
            IFAIL(u, "%s: operand and result types", op->text);
        break;
    }
    case LIMBA_F_TERN:
        if (!vals(v, u, t, 3))
            return false;
        if (in->op == LIMBA_OP_SELECT) {
            if (t[0] != LIMBA_T_I1 || t[1] != r || t[2] != r)
                IFAIL(u, "select: an i1 and two values of the result type");
        } else if (!limba_type_is_float(r) || t[0] != r || t[1] != r ||
                   t[2] != r) {
            IFAIL(u, "fma: three floats of the result type");
        }
        break;
    case LIMBA_F_CMP: {
        if (!vals(v, u, t, 2))
            return false;
        bool fl = in->op == LIMBA_OP_FCMP;
        if (in->cc >= LIMBA_CC_COUNT || fl != limba_cc_is_float(in->cc))
            IFAIL(u, "%s with a condition of the other kind", op->text);
        if (r != LIMBA_T_I1 || t[0] != t[1] ||
            (fl ? !limba_type_is_float(t[0])
                : !limba_type_is_int(t[0]) && t[0] != LIMBA_T_PTR))
            IFAIL(u, "%s: operand and result types", op->text);
        break;
    }
    case LIMBA_F_CONV:
        if (!vals(v, u, t, 1))
            return false;
        return check_conv(v, u, in, t[0]);
    case LIMBA_F_LOAD:
        if (!vals(v, u, t, 1))
            return false;
        if (t[0] != LIMBA_T_PTR)
            IFAIL(u, "load from a value that is not a ptr");
        break;
    case LIMBA_F_STORE:
        if (!vals(v, u, t, 2))
            return false;
        if (t[1] != LIMBA_T_PTR)
            IFAIL(u, "store to a value that is not a ptr");
        break;
    case LIMBA_F_SLOT:
        if ((uint64_t)in->imm >= f->nslots || r != LIMBA_T_PTR)
            IFAIL(u, "slot $%" PRId64 " does not exist", in->imm);
        break;
    case LIMBA_F_GADDR:
        if ((uint64_t)in->imm >= m->nglobals || r != LIMBA_T_PTR)
            IFAIL(u, "gaddr of a global that does not exist");
        break;
    case LIMBA_F_FADDR:
        if ((uint64_t)in->imm >= m->nfuncs || r != LIMBA_T_PTR)
            IFAIL(u, "faddr of a function that does not exist");
        break;
    case LIMBA_F_ADDR:
        if (!vals(v, u, t, 2))
            return false;
        if (t[0] != LIMBA_T_PTR || !limba_type_is_int(t[1]) ||
            r != LIMBA_T_PTR || in->imm < 0 || in->imm > INT32_MAX)
            IFAIL(u, "addr: a ptr base, an integer index, a scale >= 0");
        break;
    case LIMBA_F_MEM3:
        if (!vals(v, u, t, 3))
            return false;
        if (t[0] != LIMBA_T_PTR || !limba_type_is_int(t[2]) ||
            (in->op == LIMBA_OP_MEMCPY ? t[1] != LIMBA_T_PTR
                                       : !limba_type_is_int(t[1])))
            IFAIL(u, "%s: operand types", op->text);
        break;
    case LIMBA_F_CALL:
    case LIMBA_F_CALL_IND:
    case LIMBA_F_CALL_EXT:
    case LIMBA_F_CALL_RT:
        return check_call(v, u, in);
    case LIMBA_F_BR:
        return target(v, u, in->first, true);
    case LIMBA_F_CBR: {
        if (!vals(v, u, t, 1))
            return false;
        if (t[0] != LIMBA_T_I1)
            IFAIL(u, "cbr on a value that is not an i1");
        if (!target(v, u, in->first + 1, true))
            return false;
        uint32_t skip = f->operands[in->first + 2];
        return target(v, u, in->first + 3 + skip, true);
    }
    case LIMBA_F_SWITCH: {
        if (!vals(v, u, t, 1))
            return false;
        if (!limba_type_is_int(t[0]))
            IFAIL(u, "switch on a value that is not an integer");
        uint32_t k = in->first + 1;
        /* the default, then (lo, hi, block) per case; targets are written
           as block, count so they reuse target() */
        if (f->operands[k] >= f->nblocks || f->operands[k] == 0)
            IFAIL(u, "switch default is not a valid block");
        if (f->blocks[f->operands[k]].nparams)
            IFAIL(u, "switch targets take no arguments");
        uint32_t cases = f->operands[k + 1];
        const uint32_t *o = f->operands + k + 2;
        for (uint32_t c = 0; c < cases; c++) {
            int64_t val = (int64_t)((uint64_t)o[3 * c + 1] << 32 | o[3 * c]);
            limba_id b = o[3 * c + 2];
            if (b >= f->nblocks || b == 0 || f->blocks[b].nparams)
                IFAIL(u, "switch case %" PRIu32 " goes to an invalid block", c);
            if (!fits(val, limba_type_bits(t[0])))
                IFAIL(u, "switch case %" PRId64 " does not fit", val);
            for (uint32_t e = 0; e < c; e++)
                if (o[3 * e] == o[3 * c] && o[3 * e + 1] == o[3 * c + 1])
                    IFAIL(u, "switch case %" PRId64 " twice", val);
        }
        break;
    }
    case LIMBA_F_RET: {
        limba_sig s;
        limba_type_sig(m, f->type, &s);
        if (in->nops == 0) {
            if (s.ret != LIMBA_T_VOID)
                IFAIL(u, "ret without the value the function returns");
        } else {
            if (!vals(v, u, t, 1))
                return false;
            if (t[0] != s.ret)
                IFAIL(u, "ret of the wrong type");
        }
        break;
    }
    case LIMBA_F_NONE:
        break;
    case LIMBA_F_TRAP:
        if (in->imm < 0)
            IFAIL(u, "trap with a negative code");
        break;
    case LIMBA_F_CHECK:
        if (!vals(v, u, t, 1))
            return false;
        if (t[0] != LIMBA_T_I1 || in->imm < 0)
            IFAIL(u, "check: an i1 condition and a code >= 0");
        break;
    case LIMBA_F_PARAM:
        break;
    }
    return true;
}

static bool check_func(vctx *v)
{
    const limba_module *m = v->m;
    const limba_func *f = v->f;
    const char *fname = name_of(m, f->name);

    if (!f->nblocks)
        FAIL("@%s: a function has at least one block", fname);
    for (uint32_t s = 0; s < f->nslots; s++)
        if (!is_pow2(f->slots[s].align))
            FAIL("@%s: slot $%" PRIu32 " has an alignment that is not a "
                 "power of two",
                 fname, s);

    for (uint32_t i = 0; i < f->ninsts; i++)
        if (limba_inst_pos(f, i) > m->npos)
            FAIL("@%s: an instruction at position %" PRIu32 ", which does "
                 "not exist",
                 fname, limba_inst_pos(f, i));

    /* every instruction in exactly one block, at a known position */
    free(v->pos);
    v->pos = limba_xmalloc(((size_t)f->ninsts + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < f->ninsts; i++)
        v->pos[i] = UINT32_MAX;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        if (!bl->ninsts)
            FAIL("@%s, b%" PRIu32 ": empty block", fname, b);
        if (bl->nparams > bl->ninsts)
            FAIL("@%s, b%" PRIu32 ": more parameters than instructions", fname,
                 b);
        for (uint32_t k = 0; k < bl->ninsts; k++) {
            uint32_t id = bl->insts[k];
            if (id >= f->ninsts || v->pos[id] != UINT32_MAX ||
                f->insts[id].block != b)
                FAIL("@%s, b%" PRIu32 ": instruction list is corrupt", fname,
                     b);
            v->pos[id] = k;
            const limba_inst *in = &f->insts[id];
            if (in->op >= LIMBA_OP_COUNT)
                FAIL("@%s, b%" PRIu32 ": unknown operation %u", fname, b,
                     in->op);
            bool param = in->op == LIMBA_OP_PARAM;
            if (param != (k < bl->nparams))
                FAIL("@%s, b%" PRIu32 ": parameters must come first", fname, b);
            bool term = limba_ops[in->op].flags & LIMBA_OPF_TERMINATOR;
            if (term != (k == bl->ninsts - 1))
                FAIL("@%s, b%" PRIu32 ": %s", fname, b,
                     term ? "a terminator before the end"
                          : "the block does not end with a terminator");
            if ((uint64_t)in->first + in->nops > f->noperands)
                FAIL("@%s, b%" PRIu32 ": operands out of range", fname, b);
            if (in->nops > v->capkinds) {
                v->capkinds = in->nops;
                v->kinds = limba_xrealloc(v->kinds, v->capkinds, 1);
            }
            if (!limba_operand_kinds(f, in, v->kinds))
                FAIL("@%s, b%" PRIu32 ": %s with operands of the wrong shape",
                     fname, b, limba_ops[in->op].text);
        }
    }
    for (uint32_t i = 0; i < f->ninsts; i++)
        if (v->pos[i] == UINT32_MAX)
            FAIL("@%s: instruction %" PRIu32 " belongs to no block", fname, i);

    /* the entry: the function's parameters, and nothing jumps to it */
    limba_sig s;
    limba_type_sig(m, f->type, &s);
    const limba_block *e = &f->blocks[0];
    if (e->nparams != s.n)
        FAIL("@%s: b0 has %" PRIu32 " parameters, the function %" PRIu32, fname,
             e->nparams, s.n);
    for (uint32_t i = 0; i < s.n; i++)
        if (f->insts[e->insts[i]].type != limba_sig_param(&s, i))
            FAIL("@%s: parameter %" PRIu32 " of b0 has the wrong type", fname,
                 i);

    limba_cfg_free(&v->cfg);
    if (!limba_cfg_build(f, &v->cfg))
        FAIL("@%s: a branch to a block that does not exist", fname);
    for (uint32_t b = 0; b < f->nblocks; b++)
        for (uint32_t k = 0; k < f->blocks[b].ninsts; k++)
            if (!check_inst(v, f->blocks[b].insts[k]))
                return false;
    return true;
}

int limba_verify_decls(const limba_module *m, limba_diag *d)
{
    vctx v = {.m = m, .d = d};
    bool ok = check_types(&v) && check_symbols(&v);
    for (uint32_t k = 0; ok && k < m->npos; k++)
        if (m->pos[k].file >= limba_str_count(m)) {
            limba_diag_set(d, 0, "position %" PRIu32 ": no such file string",
                           k + 1);
            ok = false;
        }
    limba_cfg_free(&v.cfg);
    free(v.pos);
    free(v.kinds);
    return ok ? 0 : -1;
}

limba_verifier *limba_verifier_new(const limba_module *m)
{
    vctx *v = limba_xcalloc(1, sizeof(*v));
    v->m = m;
    return v;
}

int limba_verifier_func(limba_verifier *v, limba_id fid, limba_diag *d)
{
    v->d = d;
    v->f = &v->m->funcs[fid];
    v->fid = fid;
    return check_func(v) ? 0 : -1;
}

void limba_verifier_free(limba_verifier *v)
{
    if (!v)
        return;
    limba_cfg_free(&v->cfg);
    free(v->pos);
    free(v->kinds);
    free(v);
}

int limba_verify(const limba_module *m, limba_diag *d)
{
    if (limba_verify_decls(m, d) != 0)
        return -1;
    limba_verifier *v = limba_verifier_new(m);
    int r = 0;
    for (uint32_t i = 0; r == 0 && i < m->nfuncs; i++)
        r = limba_verifier_func(v, i, d);
    limba_verifier_free(v);
    return r;
}
