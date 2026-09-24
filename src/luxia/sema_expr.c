/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_expr.c - the expressions of Luxia 0 (see sema.h and luxia_0.md
 * § 4.3-4.4, § 6): both operands of an operator have the same type, a
 * constant without a type takes the type of the other operand or of the
 * place it goes to, no conversion is implicit.
 */
#include "sema.h"

#include <stdio.h>
#include <string.h>

static limba_type set(limba_lxs *S, uint32_t node, limba_type t)
{
    S->type[node] = t;
    return t;
}

static bool untyped(const limba_lxs *S, limba_type t)
{
    return t == S->ts.uint || t == S->ts.ureal;
}

static unsigned kind(const limba_lxs *S, limba_type t)
{
    return lxs_ty(S, t)->kind;
}

static bool is_int(const limba_lxs *S, limba_type t)
{
    return t == S->ts.uint || kind(S, t) == LIMBA_TK_INT;
}

static bool is_numeric(const limba_lxs *S, limba_type t)
{
    return untyped(S, t) || kind(S, t) == LIMBA_TK_INT ||
           kind(S, t) == LIMBA_TK_FLOAT;
}

static bool is_float(const limba_lxs *S, limba_type t)
{
    return t == S->ts.ureal || kind(S, t) == LIMBA_TK_FLOAT;
}

static bool is_modular(const limba_lxs *S, limba_type t)
{
    return kind(S, t) == LIMBA_TK_INT &&
           (lxs_ty(S, t)->flags & LIMBA_TF_MODULAR);
}

static bool is_bool(const limba_lxs *S, limba_type t)
{
    return kind(S, t) == LIMBA_TK_BOOL;
}

/* significant bits of an integer: 5 has 3, 40 has 3 too */
static uint32_t sig_bits(const limba_big *b)
{
    uint32_t bits = limba_big_bits(b), tz = 0;
    if (!bits)
        return 0;
    for (uint32_t i = 0; i < b->n; i++) {
        if (b->w[i]) {
            tz += (uint32_t)__builtin_ctz(b->w[i]);
            break;
        }
        tz += 32;
    }
    return bits - tz;
}

/* a constant without a type takes target */
static bool convert_const(limba_lxs *S, uint32_t node, limba_type target)
{
    limba_type t = S->type[node];
    unsigned tk = kind(S, target);
    char tb[128];
    if (tk == LIMBA_TK_ERROR)
        return true;
    if (t == S->ts.uint && tk == LIMBA_TK_FLOAT) {
        uint32_t v = S->val[node];
        if (v && sig_bits(&S->v[v].num.num) >
                     (lxs_ty(S, target)->bits == 32 ? 24u : 53u)) {
            lxs_error(S, LXE_CONST_RANGE, node,
                      "this integer is not exact in %s: convert it "
                      "explicitly",
                      lxs_tname(S, target, tb));
            return false;
        }
    } else if (!(t == S->ts.uint && tk == LIMBA_TK_INT) &&
               !(t == S->ts.ureal && tk == LIMBA_TK_FLOAT)) {
        lxs_error(S, LXE_TYPE_MISMATCH, node, "%s where %s is expected",
                  t == S->ts.uint ? "an integer constant" : "a real constant",
                  lxs_tname(S, target, tb));
        set(S, node, 0);
        return false;
    }
    set(S, node, target);
    return lxs_fit(S, node, &S->val[node], target);
}

bool lxs_assign_to(limba_lxs *S, uint32_t node, limba_type target,
                   const char *what)
{
    limba_type t = S->type[node];
    if (!t || !target)
        return true;
    if (untyped(S, t))
        return convert_const(S, node, target);
    char ta[128], tb[128];
    if (t == S->ts.nil) {
        if (kind(S, target) == LIMBA_TK_POINTER) {
            set(S, node, target);
            return true;
        }
        lxs_error(S, LXE_TYPE_MISMATCH, node, "nil where %s is expected",
                  lxs_tname(S, target, tb));
        return false;
    }
    if (!lxs_compatible(S, t, target)) {
        lxs_error(S, LXE_TYPE_MISMATCH, node, "%s is %s, %s wants %s", "this",
                  lxs_tname(S, t, ta), what, lxs_tname(S, target, tb));
        return false;
    }
    if (S->val[node] && limba_types_is_discrete(&S->ts, target) &&
        (lxs_ty(S, target)->flags & LIMBA_TF_RANGE))
        return lxs_fit(S, node, &S->val[node], target);
    return true;
}

/* make the operands of a binary operator agree; false if they cannot */
static bool unify(limba_lxs *S, uint32_t node, uint32_t l, uint32_t r,
                  limba_type *lt, limba_type *rt)
{
    if (!*lt || !*rt)
        return false;
    if (untyped(S, *lt) && untyped(S, *rt)) {
        if (*lt != *rt) {
            set(S, l, S->ts.ureal);
            set(S, r, S->ts.ureal);
            *lt = *rt = S->ts.ureal;
        }
        return true;
    }
    if (untyped(S, *lt)) {
        if (!convert_const(S, l, *rt))
            return false;
        *lt = *rt;
        return true;
    }
    if (untyped(S, *rt)) {
        if (!convert_const(S, r, *lt))
            return false;
        *rt = *lt;
        return true;
    }
    if (*lt == S->ts.nil && kind(S, *rt) == LIMBA_TK_POINTER) {
        set(S, l, *rt);
        *lt = *rt;
        return true;
    }
    if (*rt == S->ts.nil && kind(S, *lt) == LIMBA_TK_POINTER) {
        set(S, r, *lt);
        *rt = *lt;
        return true;
    }
    if (!lxs_compatible(S, *lt, *rt)) {
        char ta[128], tb[128];
        lxs_error(S, LXE_TYPE_MISMATCH, node,
                  "'%s' between %s and %s: convert one of them",
                  limba_lx_kind_text(lxs_node(S, node)->op),
                  lxs_tname(S, *lt, ta), lxs_tname(S, *rt, tb));
        return false;
    }
    return true;
}

static limba_type op_error(limba_lxs *S, uint32_t node, limba_type t,
                           const char *what)
{
    char tb[128];
    lxs_error(S, LXE_OPERATOR_TYPE, node, "'%s' %s, not on %s",
              limba_lx_kind_text(lxs_node(S, node)->op), what,
              lxs_tname(S, t, tb));
    return set(S, node, 0);
}

/* the type an operation on t yields: the base of a range */
static limba_type result_of(limba_lxs *S, limba_type t)
{
    return untyped(S, t) ? t : lxs_base(S, t);
}

static limba_type binary(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    unsigned op = x->op;
    uint32_t l = x->a, r = x->b;
    limba_type lt = lxs_expr(S, l, scope, 0);
    limba_type rt = lxs_expr(S, r, scope, 0);
    limba_type res;
    switch (op) {
    case LX_POWER:
        if (!lt || !rt)
            return set(S, node, 0);
        if (!is_numeric(S, lt))
            return op_error(S, node, lt, "raises numbers");
        if (!is_int(S, rt))
            return op_error(S, r, rt, "takes an integer exponent");
        if (untyped(S, rt) && !untyped(S, lt))
            convert_const(S, r, S->ty_int[3]);
        res = result_of(S, lt);
        break;
    case LX_KW_SHL:
    case LX_KW_SHR:
        if (!lt || !rt)
            return set(S, node, 0);
        if (untyped(S, lt)) {
            lxs_error(S, LXE_NEED_TYPE, l,
                      "a shift needs a Bits type: write Bits32(1) shl n");
            return set(S, node, 0);
        }
        if (!is_modular(S, lt))
            return op_error(S, node, lt, "shifts Bits types only");
        if (!is_int(S, rt))
            return op_error(S, r, rt, "shifts by an integer");
        if (untyped(S, rt))
            convert_const(S, r, S->ty_int[3]);
        res = result_of(S, lt);
        break;
    case LX_AMP:
        /* strings and characters, in any mix */
        if (!lt || !rt)
            return set(S, node, 0);
        if (kind(S, lt) != LIMBA_TK_STRING && kind(S, lt) != LIMBA_TK_CHAR)
            return op_error(S, node, lt, "joins strings and characters");
        if (kind(S, rt) != LIMBA_TK_STRING && kind(S, rt) != LIMBA_TK_CHAR)
            return op_error(S, node, rt, "joins strings and characters");
        res = S->ts.string;
        break;
    default:
        if (!unify(S, node, l, r, &lt, &rt))
            return set(S, node, 0);
        switch (op) {
        case LX_KW_AND:
        case LX_KW_OR:
        case LX_KW_XOR:
            if (untyped(S, lt)) {
                lxs_error(S, LXE_NEED_TYPE, node,
                          "'%s' on constants needs a Bits type",
                          limba_lx_kind_text(op));
                return set(S, node, 0);
            }
            if (!is_bool(S, lt) && !is_modular(S, lt))
                return op_error(S, node, lt, "takes Boolean or Bits values");
            res = result_of(S, lt);
            break;
        case LX_EQ:
        case LX_NE:
        case LX_LT:
        case LX_LE:
        case LX_GT:
        case LX_GE: {
            unsigned k = kind(S, lt);
            bool ordered = is_numeric(S, lt) || k == LIMBA_TK_CHAR ||
                           k == LIMBA_TK_ENUM || k == LIMBA_TK_BOOL ||
                           k == LIMBA_TK_STRING;
            bool equal = ordered || k == LIMBA_TK_POINTER || k == LIMBA_TK_NIL;
            if (!(op == LX_EQ || op == LX_NE ? equal : ordered))
                return op_error(S, node, lt, "compares scalars and strings");
            res = S->ts.bool_;
            break;
        }
        case LX_SLASH:
            if (!is_float(S, lt))
                return op_error(S, node, lt,
                                "divides reals: for integers use div");
            res = result_of(S, lt);
            break;
        case LX_KW_DIV:
        case LX_KW_MOD:
        case LX_KW_REM:
            if (!is_int(S, lt))
                return op_error(S, node, lt,
                                "divides integers: for reals "
                                "use /");
            res = result_of(S, lt);
            break;
        default: /* + - * */
            if (!is_numeric(S, lt))
                return op_error(S, node, lt, "computes on numbers");
            res = result_of(S, lt);
        }
    }
    set(S, node, res);
    if (S->val[l] && S->val[r])
        S->val[node] = lxs_fold_binary(S, node, op, S->val[l], S->val[r],
                                       untyped(S, res) ? 0 : res);
    return res;
}

static limba_type unary(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    unsigned op = x->op;
    uint32_t e = x->a;
    limba_type t = lxs_expr(S, e, scope, 0);
    if (!t)
        return set(S, node, 0);
    switch (op) {
    case LX_MINUS:
        if (!is_numeric(S, t))
            return op_error(S, node, t, "negates numbers");
        if (kind(S, t) == LIMBA_TK_INT &&
            !(lxs_ty(S, t)->flags & (LIMBA_TF_SIGNED | LIMBA_TF_MODULAR)))
            return op_error(S, node, t,
                            "negates signed numbers and Bits values");
        break;
    case LX_PLUS:
        if (!is_numeric(S, t))
            return op_error(S, node, t, "applies to numbers");
        break;
    case LX_KW_ABS:
        if (!is_numeric(S, t) || is_modular(S, t))
            return op_error(S, node, t, "applies to numbers");
        break;
    case LX_KW_NOT:
        if (untyped(S, t)) {
            lxs_error(S, LXE_NEED_TYPE, node,
                      "'not' on a constant needs a Bits type");
            return set(S, node, 0);
        }
        if (!is_bool(S, t) && !is_modular(S, t))
            return op_error(S, node, t, "takes Boolean or Bits values");
        break;
    }
    limba_type res = result_of(S, t);
    set(S, node, res);
    if (S->val[e])
        S->val[node] =
            lxs_fold_unary(S, node, op, S->val[e], untyped(S, res) ? 0 : res);
    return res;
}

static limba_type reference(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_sym s = lxs_lookup(S, scope, node);
    if (!s)
        return set(S, node, 0);
    lxs_force(S, s);
    limba_symbol *y = &S->st.sym[s];
    size_t n;
    const char *sp;
    switch (y->kind) {
    case LIMBA_SYM_CONST:
        S->val[node] = y->value;
        return set(S, node, y->type);
    case LIMBA_SYM_VAR:
    case LIMBA_SYM_PARAM:
        return set(S, node, y->type);
    case LIMBA_SYM_TYPE:
        sp = lxs_spell(S, s, &n);
        lxs_error(S, LXE_NOT_A_VALUE, node,
                  "'%.*s' is a type: to convert a value write %.*s(x)", (int)n,
                  sp, (int)n, sp);
        return set(S, node, 0);
    default:
        sp = lxs_spell(S, s, &n);
        lxs_error(S, LXE_NOT_A_VALUE, node,
                  "'%.*s' is a routine: call it with %.*s(...)", (int)n, sp,
                  (int)n, sp);
        return set(S, node, 0);
    }
}

/* the record or array behind a pointer, which . and [] reach alone */
static limba_type through_pointer(limba_lxs *S, limba_type t)
{
    if (t && kind(S, t) == LIMBA_TK_POINTER)
        return lxs_ty(S, t)->elem;
    return t;
}

static limba_type select(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t base = x->a, fname = x->b;
    limba_type t = through_pointer(S, lxs_expr(S, base, scope, 0));
    if (!t)
        return set(S, node, 0);
    const limba_typeinfo *ti = lxs_ty(S, t);
    char tb[128];
    if (ti->kind != LIMBA_TK_RECORD) {
        lxs_error(S, LXE_NO_FIELD, node, "%s has no fields",
                  lxs_tname(S, t, tb));
        return set(S, node, 0);
    }
    for (uint32_t i = 0; i < ti->count; i++) {
        const limba_field *f = &S->ts.field[ti->first + i];
        if (f->name == fname)
            return set(S, node, f->type);
    }
    size_t n;
    const char *nm = lxs_name(S, fname, &n);
    lxs_error(S, LXE_NO_FIELD, node, "%s has no field '%.*s'",
              lxs_tname(S, t, tb), (int)n, nm);
    return set(S, node, 0);
}

static limba_type index_expr(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t base = x->a, idx = x->b;
    limba_type t = through_pointer(S, lxs_expr(S, base, scope, 0));
    limba_type index, elem;
    if (!t) {
        lxs_expr(S, idx, scope, 0);
        return set(S, node, 0);
    }
    const limba_typeinfo *ti = lxs_ty(S, t);
    char tb[128];
    switch (ti->kind) {
    case LIMBA_TK_ARRAY:
        index = ti->index;
        elem = ti->elem;
        break;
    case LIMBA_TK_OPEN:
        index = S->ty_int[3];
        elem = ti->elem;
        break;
    case LIMBA_TK_STRING:
        index = S->ty_int[3];
        elem = S->ty_bits[0];
        break;
    default:
        lxs_error(S, LXE_NOT_INDEXABLE, node, "%s has no index",
                  lxs_tname(S, t, tb));
        lxs_expr(S, idx, scope, 0);
        return set(S, node, 0);
    }
    lxs_expr(S, idx, scope, index);
    if (ti->flags & LIMBA_TF_DYNAMIC)
        index = lxs_base(S, index);
    lxs_assign_to(S, idx, index, "the index");
    return set(S, node, elem);
}

/* x rounded to the nearest integer, halves away from zero (Ada) */
static void round_away(limba_rat *out, const limba_rat *x)
{
    limba_big q, m, two_m;
    limba_big_init(&q);
    limba_big_init(&m);
    limba_big_init(&two_m);
    limba_big_divmod(&q, &m, &x->num, &x->den);
    limba_big_abs(&two_m, &m);
    limba_big_shl(&two_m, &two_m, 1);
    if (limba_big_cmp(&two_m, &x->den) >= 0) {
        limba_big one;
        limba_big_init(&one);
        limba_big_set_i64(&one, x->num.neg ? -1 : 1);
        limba_big_add(&q, &q, &one);
        limba_big_free(&one);
    }
    limba_rat_set_big(out, &q);
    limba_big_free(&q);
    limba_big_free(&m);
    limba_big_free(&two_m);
}

/* T(x) */
static limba_type conversion(limba_lxs *S, uint32_t node, uint32_t scope,
                             limba_type target)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t args = x->b;
    char ta[128], tb[128];
    if (lxs_node(S, args)->b != 1) {
        lxs_error(S, LXE_ARG_COUNT, node, "a conversion takes one value");
        for (uint32_t i = 0; i < lxs_node(S, args)->b; i++)
            lxs_expr(S, limba_lx_list_at(S->t, args, i), scope, 0);
        return set(S, node, target);
    }
    uint32_t a = limba_lx_list_at(S->t, args, 0);
    limba_type t = lxs_expr(S, a, scope, 0);
    set(S, node, target);
    if (!t || !target)
        return target;
    bool numeric =
        is_numeric(S, t) && is_numeric(S, target) && !untyped(S, target);
    if (!numeric && !lxs_compatible(S, t, target)) {
        lxs_error(S, LXE_BAD_CONVERSION, node, "%s does not convert to %s",
                  lxs_tname(S, t, ta), lxs_tname(S, target, tb));
        return target;
    }
    uint32_t v = S->val[a];
    if (!v || S->v[v].kind != LXV_NUM)
        return target;
    /* a constant converts now: a real to an integer rounds away from
       zero, and every value must fit */
    if (!is_float(S, target) && !limba_rat_is_int(&S->v[v].num)) {
        uint32_t r = lxs_value_new(S, LXV_NUM);
        round_away(&S->v[r].num, &S->v[v].num);
        v = r;
    }
    if (lxs_fit(S, node, &v, target))
        S->val[node] = v;
    return target;
}

static void check_format(limba_lxs *S, uint32_t list)
{
    for (uint32_t i = 0; i < lxs_node(S, list)->b; i++) {
        uint32_t a = limba_lx_list_at(S->t, list, i);
        if (lxs_node(S, a)->kind == LXN_FMT && !S->sym[a]) {
            lxs_error(S, LXE_FORMAT_PLACE, a,
                      "x:width:decimals is for write and writeln only");
            S->sym[a] = 1; /* reported: not again when it is evaluated */
        }
    }
}

static limba_type routine_call(limba_lxs *S, uint32_t node, uint32_t scope,
                               limba_sym s)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t args = x->b, nargs = lxs_node(S, args)->b;
    check_format(S, args);
    limba_type sig = S->st.sym[s].type;
    if (!sig) {
        for (uint32_t i = 0; i < nargs; i++)
            lxs_expr(S, limba_lx_list_at(S->t, args, i), scope, 0);
        return set(S, node, 0);
    }
    const limba_typeinfo *ti = lxs_ty(S, sig);
    uint32_t first = ti->first, count = ti->count;
    limba_type result = ti->elem;
    size_t n;
    const char *sp = lxs_spell(S, s, &n);
    if (nargs != count)
        lxs_error(S, LXE_ARG_COUNT, node, "'%.*s' takes %u argument%s, not %u",
                  (int)n, sp, count, count == 1 ? "" : "s", nargs);
    for (uint32_t i = 0; i < nargs; i++) {
        uint32_t a = limba_lx_list_at(S->t, args, i);
        if (i >= count) {
            lxs_expr(S, a, scope, 0);
            continue;
        }
        limba_param p = S->ts.param[first + i];
        char ta[128], tb[128];
        if (kind(S, p.type) == LIMBA_TK_OPEN) {
            limba_type at = lxs_expr(S, a, scope, 0);
            unsigned ak = at ? kind(S, at) : 0;
            if (at && !((ak == LIMBA_TK_ARRAY || ak == LIMBA_TK_OPEN) &&
                        lxs_compatible(S, lxs_ty(S, at)->elem,
                                       lxs_ty(S, p.type)->elem)))
                lxs_error(S, LXE_TYPE_MISMATCH, a,
                          "this is %s, the parameter is %s",
                          lxs_tname(S, at, ta), lxs_tname(S, p.type, tb));
        } else if (p.mode == LXS_IN) {
            lxs_expr(S, a, scope, p.type);
            lxs_assign_to(S, a, p.type, "the parameter");
            continue;
        } else {
            limba_type at = lxs_expr(S, a, scope, 0);
            if (at && !untyped(S, at) && !lxs_compatible(S, at, p.type))
                lxs_error(S, LXE_TYPE_MISMATCH, a,
                          "this is %s, the parameter is %s",
                          lxs_tname(S, at, ta), lxs_tname(S, p.type, tb));
        }
        if (p.mode != LXS_IN)
            lxs_writable(S, a, true);
    }
    return set(S, node, result);
}

/* ---- the routines of the language ---- */

static uint32_t arg_at(limba_lxs *S, uint32_t node, uint32_t i)
{
    return limba_lx_list_at(S->t, lxs_node(S, node)->b, i);
}

static bool arity(limba_lxs *S, uint32_t node, uint32_t want, const char *name,
                  uint32_t scope)
{
    uint32_t args = lxs_node(S, node)->b, n = lxs_node(S, args)->b;
    if (n == want)
        return true;
    lxs_error(S, LXE_ARG_COUNT, node, "%s takes %u argument%s, not %u", name,
              want, want == 1 ? "" : "s", n);
    for (uint32_t i = 0; i < n; i++)
        lxs_expr(S, limba_lx_list_at(S->t, args, i), scope, 0);
    return false;
}

/* a value of type want (a constant without a type is given it) */
static limba_type arg_of(limba_lxs *S, uint32_t a, uint32_t scope,
                         limba_type want)
{
    limba_type t = lxs_expr(S, a, scope, want);
    if (t && want)
        lxs_assign_to(S, a, want, "the argument");
    return want ? want : t;
}

/* a value that write can print; a constant without a type prints as an
   Int64 or a Float64 */
static void printable(limba_lxs *S, uint32_t a, uint32_t scope)
{
    limba_type t = lxs_expr(S, a, scope, 0);
    if (!t)
        return;
    if (untyped(S, t)) {
        convert_const(S, a, t == S->ts.uint ? S->ty_int[3] : S->ty_f64);
        return;
    }
    unsigned k = kind(S, t);
    if (k != LIMBA_TK_INT && k != LIMBA_TK_FLOAT && k != LIMBA_TK_BOOL &&
        k != LIMBA_TK_CHAR && k != LIMBA_TK_STRING) {
        char tb[128];
        lxs_error(S, LXE_TYPE_MISMATCH, a,
                  "write prints numbers, Booleans, characters and strings, "
                  "not %s",
                  lxs_tname(S, t, tb));
    }
}

static limba_type builtin(limba_lxs *S, uint32_t node, uint32_t scope,
                          limba_sym s, limba_type expected)
{
    unsigned id = S->st.sym[s].value;
    uint32_t args = lxs_node(S, node)->b, n = lxs_node(S, args)->b;
    size_t len;
    const char *name = lxs_spell(S, s, &len);
    char nm[32];
    snprintf(nm, sizeof(nm), "%.*s", (int)len, name);
    char tb[128];
    if (id != LXB_WRITE && id != LXB_WRITELN)
        check_format(S, args);
    switch (id) {
    case LXB_WRITE:
    case LXB_WRITELN:
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = arg_at(S, node, i);
            limba_lx_node *ax = lxs_node(S, a);
            if (ax->kind != LXN_FMT) {
                printable(S, a, scope);
                continue;
            }
            uint32_t v = ax->a, w = ax->b, d = ax->c;
            printable(S, v, scope);
            arg_of(S, w, scope, S->ty_int[2]);
            if (d) {
                arg_of(S, d, scope, S->ty_int[2]);
                if (S->type[v] && !is_float(S, S->type[v]))
                    lxs_error(S, LXE_FORMAT_PLACE, d,
                              "decimals are for real numbers");
            }
        }
        return set(S, node, S->ts.void_);
    case LXB_WRITEBYTE:
        if (arity(S, node, 1, nm, scope))
            arg_of(S, arg_at(S, node, 0), scope, S->ty_bits[0]);
        return set(S, node, S->ts.void_);
    case LXB_READLINE:
        if (arity(S, node, 1, nm, scope)) {
            uint32_t a = arg_at(S, node, 0);
            limba_type t = lxs_expr(S, a, scope, 0);
            if (t && kind(S, t) != LIMBA_TK_STRING)
                lxs_error(S, LXE_TYPE_MISMATCH, a,
                          "readline reads into a String variable");
            lxs_writable(S, a, true);
        }
        return set(S, node, S->ts.bool_);
    case LXB_LENGTH:
    case LXB_LOW:
    case LXB_HIGH: {
        if (!arity(S, node, 1, nm, scope))
            return set(S, node, 0);
        uint32_t a = arg_at(S, node, 0);
        limba_type t = lxs_expr(S, a, scope, 0);
        if (!t)
            return set(S, node, 0);
        const limba_typeinfo *ti = lxs_ty(S, t);
        if (ti->kind == LIMBA_TK_STRING || ti->kind == LIMBA_TK_OPEN)
            return set(S, node, S->ty_int[3]);
        if (ti->kind != LIMBA_TK_ARRAY) {
            lxs_error(S, LXE_TYPE_MISMATCH, a,
                      "%s takes an array or a "
                      "string, not %s",
                      nm, lxs_tname(S, t, tb));
            return set(S, node, 0);
        }
        limba_type it = lxs_base(S, ti->index);
        if (!(ti->flags & LIMBA_TF_DYNAMIC)) {
            const limba_typeinfo *ix = lxs_ty(S, ti->index);
            __int128 v = id == LXB_LOW    ? ix->lo
                         : id == LXB_HIGH ? ix->hi
                                          : ix->hi - ix->lo + 1;
            uint32_t val = lxs_value_int(S, v);
            if (id == LXB_LENGTH && !lxs_fit(S, node, &val, it))
                return set(S, node, it);
            S->val[node] = val;
        }
        return set(S, node, it);
    }
    case LXB_COPY:
        if (arity(S, node, 3, nm, scope)) {
            arg_of(S, arg_at(S, node, 0), scope, S->ts.string);
            arg_of(S, arg_at(S, node, 1), scope, S->ty_int[3]);
            arg_of(S, arg_at(S, node, 2), scope, S->ty_int[3]);
        }
        return set(S, node, S->ts.string);
    case LXB_CHR: {
        if (!arity(S, node, 1, nm, scope))
            return set(S, node, S->ts.char_);
        uint32_t a = arg_at(S, node, 0);
        limba_type t = lxs_expr(S, a, scope, 0);
        if (t && !is_int(S, t))
            lxs_error(S, LXE_TYPE_MISMATCH, a, "chr takes an integer");
        set(S, node, S->ts.char_);
        uint32_t v = S->val[a];
        if (v && lxs_fit(S, a, &v, S->ts.char_))
            S->val[node] = v;
        return S->ts.char_;
    }
    case LXB_ORD:
    case LXB_SUCC:
    case LXB_PRED: {
        if (!arity(S, node, 1, nm, scope))
            return set(S, node, 0);
        uint32_t a = arg_at(S, node, 0);
        limba_type t = lxs_expr(S, a, scope, 0);
        if (!t)
            return set(S, node, 0);
        if (untyped(S, t) || !limba_types_is_discrete(&S->ts, t)) {
            lxs_error(S, LXE_TYPE_MISMATCH, a,
                      "%s takes a discrete value with a type", nm);
            return set(S, node, 0);
        }
        if (id == LXB_ORD) {
            set(S, node, S->ty_uint[2]);
            uint32_t v = S->val[a];
            if (v && lxs_fit(S, node, &v, S->ty_uint[2]))
                S->val[node] = v;
            return S->ty_uint[2];
        }
        return set(S, node, lxs_base(S, t));
    }
    case LXB_STR:
        if (arity(S, node, 1, nm, scope))
            printable(S, arg_at(S, node, 0), scope);
        return set(S, node, S->ts.string);
    case LXB_VAL:
        if (arity(S, node, 2, nm, scope)) {
            arg_of(S, arg_at(S, node, 0), scope, S->ts.string);
            uint32_t a = arg_at(S, node, 1);
            limba_type t = lxs_expr(S, a, scope, 0);
            if (t && !is_numeric(S, t))
                lxs_error(S, LXE_TYPE_MISMATCH, a, "val reads a number");
            lxs_writable(S, a, true);
        }
        return set(S, node, S->ts.bool_);
    case LXB_SQRT:
    case LXB_SIN:
    case LXB_COS:
    case LXB_TAN:
    case LXB_ARCTAN:
    case LXB_EXP:
    case LXB_LN:
    case LXB_TRUNC:
    case LXB_ROUND:
    case LXB_FLOOR:
    case LXB_CEIL: {
        if (!arity(S, node, 1, nm, scope))
            return set(S, node, 0);
        uint32_t a = arg_at(S, node, 0);
        limba_type t = lxs_expr(S, a, scope, 0);
        if (!t)
            return set(S, node, 0);
        if (untyped(S, t)) {
            if (!expected || !is_float(S, expected) || untyped(S, expected)) {
                lxs_error(S, LXE_NEED_TYPE, a,
                          "%s of a constant needs a type: write "
                          "%s(Float64(x))",
                          nm, nm);
                return set(S, node, 0);
            }
            convert_const(S, a, lxs_base(S, expected));
            t = lxs_base(S, expected);
        }
        if (!is_float(S, t)) {
            lxs_error(S, LXE_TYPE_MISMATCH, a,
                      "%s takes a real number, not %s: convert it", nm,
                      lxs_tname(S, t, tb));
            return set(S, node, 0);
        }
        return set(S, node, lxs_base(S, t));
    }
    case LXB_DISPOSE:
        if (arity(S, node, 1, nm, scope)) {
            uint32_t a = arg_at(S, node, 0);
            limba_type t = lxs_expr(S, a, scope, 0);
            if (t && kind(S, t) != LIMBA_TK_POINTER)
                lxs_error(S, LXE_NOT_POINTER, a, "dispose frees a pointer");
        }
        return set(S, node, S->ts.void_);
    case LXB_ARGCOUNT:
        arity(S, node, 0, nm, scope);
        return set(S, node, S->ty_int[2]);
    case LXB_ARG:
        if (arity(S, node, 1, nm, scope))
            arg_of(S, arg_at(S, node, 0), scope, S->ty_int[2]);
        return set(S, node, S->ts.string);
    case LXB_HALT:
        if (arity(S, node, 1, nm, scope))
            arg_of(S, arg_at(S, node, 0), scope, S->ty_int[2]);
        return set(S, node, S->ts.void_);
    }
    return set(S, node, 0);
}

static limba_type call(limba_lxs *S, uint32_t node, uint32_t scope,
                       limba_type expected)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t callee = x->a;
    if (lxs_node(S, callee)->kind != LXN_REF) {
        lxs_expr(S, callee, scope, 0);
        lxs_error(S, LXE_NOT_CALLABLE, node, "only routines are called");
        return set(S, node, 0);
    }
    limba_sym s = lxs_lookup(S, scope, callee);
    if (!s) {
        uint32_t args = x->b;
        for (uint32_t i = 0; i < lxs_node(S, args)->b; i++)
            lxs_expr(S, limba_lx_list_at(S->t, args, i), scope, 0);
        return set(S, node, 0);
    }
    lxs_force(S, s);
    switch (S->st.sym[s].kind) {
    case LIMBA_SYM_TYPE:
        return conversion(S, node, scope, S->st.sym[s].type);
    case LIMBA_SYM_ROUTINE:
        return routine_call(S, node, scope, s);
    case LIMBA_SYM_BUILTIN:
        return builtin(S, node, scope, s, expected);
    }
    size_t n;
    const char *sp = lxs_spell(S, s, &n);
    lxs_error(S, LXE_NOT_CALLABLE, callee, "'%.*s' is not a routine", (int)n,
              sp);
    uint32_t args = x->b;
    for (uint32_t i = 0; i < lxs_node(S, args)->b; i++)
        lxs_expr(S, limba_lx_list_at(S->t, args, i), scope, 0);
    return set(S, node, 0);
}

static limba_type membership(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t v = x->a, lo = x->b, hi = x->c;
    limba_type t = lxs_expr(S, v, scope, 0);
    set(S, node, S->ts.bool_);
    /* x in T: the range of a type */
    limba_lx_node *lx_ = lxs_node(S, lo);
    if (!hi && lx_->kind == LXN_REF) {
        limba_sym s = limba_sym_lookup(&S->st, scope, lx_->a);
        if (s && S->st.sym[s].kind == LIMBA_SYM_TYPE) {
            lxs_lookup(S, scope, lo); /* the spelling */
            lxs_force(S, s);
            limba_type rt = S->st.sym[s].type;
            if (t && untyped(S, t))
                convert_const(S, v, lxs_base(S, rt));
            else if (t && !lxs_compatible(S, t, rt)) {
                char ta[128], tb[128];
                lxs_error(S, LXE_TYPE_MISMATCH, node, "%s is not a range of %s",
                          lxs_tname(S, rt, tb), lxs_tname(S, t, ta));
            }
            return S->ts.bool_;
        }
    }
    if (!hi) {
        lxs_error(S, LXE_TYPE_MISMATCH, lo,
                  "'in' takes a range a..b or a type");
        lxs_expr(S, lo, scope, 0);
        return S->ts.bool_;
    }
    limba_type lt = lxs_expr(S, lo, scope, 0);
    limba_type ht = lxs_expr(S, hi, scope, 0);
    limba_type tt = t;
    unify(S, node, v, lo, &tt, &lt);
    tt = S->type[v];
    unify(S, node, v, hi, &tt, &ht);
    if (S->type[v] && !limba_types_is_discrete(&S->ts, S->type[v]) &&
        !untyped(S, S->type[v]))
        op_error(S, node, S->type[v], "tests discrete values");
    return set(S, node, S->ts.bool_);
}

limba_type lxs_expr(limba_lxs *S, uint32_t node, uint32_t scope,
                    limba_type expected)
{
    limba_lx_node *x = lxs_node(S, node);
    switch (x->kind) {
    case LXN_INT:
        S->val[node] = lxs_literal(S, node);
        return set(S, node, S->ts.uint);
    case LXN_REAL:
        S->val[node] = lxs_literal(S, node);
        return set(S, node, S->ts.ureal);
    case LXN_CHAR:
        S->val[node] = lxs_value_int(S, x->a);
        return set(S, node, S->ts.char_);
    case LXN_STRING: {
        uint32_t v = lxs_value_new(S, LXV_STR);
        S->v[v].str = x->a;
        S->val[node] = v;
        return set(S, node, S->ts.string);
    }
    case LXN_BOOL:
        S->val[node] = lxs_value_int(S, x->a);
        return set(S, node, S->ts.bool_);
    case LXN_NIL:
        S->val[node] = lxs_value_new(S, LXV_NIL);
        return set(S, node, S->ts.nil);
    case LXN_REF:
        return reference(S, node, scope);
    case LXN_UNARY:
        return unary(S, node, scope);
    case LXN_BINARY:
        return binary(S, node, scope);
    case LXN_IN:
        return membership(S, node, scope);
    case LXN_SEL:
        return select(S, node, scope);
    case LXN_INDEX:
        return index_expr(S, node, scope);
    case LXN_DEREF: {
        limba_type t = lxs_expr(S, x->a, scope, 0);
        if (t && kind(S, t) != LIMBA_TK_POINTER) {
            char tb[128];
            lxs_error(S, LXE_NOT_POINTER, node, "%s is not a pointer",
                      lxs_tname(S, t, tb));
            return set(S, node, 0);
        }
        return set(S, node, t ? lxs_ty(S, t)->elem : 0);
    }
    case LXN_CALL: {
        limba_type t = call(S, node, scope, expected);
        if (t == S->ts.void_) {
            lxs_error(S, LXE_NO_RESULT, node,
                      "a procedure gives no value: call it as a statement");
            return set(S, node, 0);
        }
        return t;
    }
    case LXN_NEW: {
        limba_type t = lxs_type(S, x->a, scope, 0);
        return set(S, node, t ? limba_types_pointer(&S->ts, t) : 0);
    }
    case LXN_FMT:
        if (!S->sym[node])
            lxs_error(S, LXE_FORMAT_PLACE, node,
                      "x:width:decimals is for write and writeln only");
        lxs_expr(S, x->a, scope, 0);
        return set(S, node, 0);
    }
    return set(S, node, 0);
}

limba_type lxs_call_stmt(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_type t = call(S, node, scope, 0);
    if (t && t != S->ts.void_)
        lxs_error(S, LXE_RESULT_IGNORED, node,
                  "the result of a function cannot be ignored: use it");
    return t;
}

bool lxs_writable(limba_lxs *S, uint32_t node, bool report)
{
    limba_lx_node *x = lxs_node(S, node);
    const char *why = "this is not a variable";
    switch (x->kind) {
    case LXN_REF: {
        limba_sym s = S->sym[node];
        if (!s)
            return true; /* reported */
        limba_symbol *y = &S->st.sym[s];
        if (y->kind == LIMBA_SYM_VAR && !(y->flags & LXS_LOOPVAR))
            return true;
        if (y->kind == LIMBA_SYM_PARAM && y->mode != LXS_IN)
            return true;
        why = y->kind == LIMBA_SYM_VAR ? "the variable of a for is constant "
                                         "in the loop"
              : y->kind == LIMBA_SYM_PARAM
                  ? "a parameter without var or out is read-only"
              : y->kind == LIMBA_SYM_CONST ? "a constant cannot change"
                                           : why;
        break;
    }
    case LXN_SEL:
    case LXN_INDEX: {
        limba_type bt = S->type[x->a];
        if (bt && kind(S, bt) == LIMBA_TK_POINTER)
            return true;
        if (x->kind == LXN_INDEX && bt && kind(S, bt) == LIMBA_TK_STRING) {
            why = "a String does not change: build a new one";
            break;
        }
        return lxs_writable(S, x->a, report);
    }
    case LXN_DEREF:
        return true;
    case LXN_ERROR:
        return true;
    }
    if (report)
        lxs_error(S, LXE_NOT_ASSIGNABLE, node, "%s", why);
    return false;
}
