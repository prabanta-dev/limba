/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * bounds.c - the "bounds" pass: a check whose condition the facts before
 * it prove goes, after ABCD (Bodik, Gupta, Sarkar, "ABCD: Eliminating
 * Array Bounds Checks on Demand", PLDI 2000), in a small form.
 *
 * A fact is a difference a - b <= c between two integer values, or a
 * value and zero (node Z). Some hold wherever their values are: a
 * constant, a sign extension (the same value), an add.ov or sub.ov of a
 * constant (it traps rather than wrap, so the sum is exact), and the
 * parameter of a loop that starts at a value defined before the loop and
 * only grows by add.ov of a constant from 0 up (or only shrinks). The
 * others hold in part of the function: the condition of a cbr in the
 * blocks its edge dominates, and the condition of a check after it,
 * both with and seen through; the walk down the dominator tree adds them
 * and drops them on the way back.
 *
 * A check of a signed comparison (or an and of them) goes when a path of
 * facts proves it, and a check of a and b keeps only the one not proved;
 * the search is short, and what it does not find stays checked. Nothing
 * moves: a check that stays traps where it is written.
 */
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

/* v - u <= w: an edge from u to v */
typedef struct {
    uint32_t u, v;
    int64_t w;
} fact;

#define DEPTH 6    /* edges on a path of proof */
#define EXPAND 256 /* nodes a search may relax */

typedef struct {
    limba_func *f;
    limba_edit *e;
    const limba_cfg *cfg;
    uint32_t z; /* node Z: ninsts */
    /* the facts that hold everywhere, by their u (compressed rows) */
    uint32_t *gfirst;
    fact *g;
    /* the facts of the walk, a stack */
    fact *s;
    uint32_t ns, caps;
    /* the search: dist[v] is set when stamp[v] is now; a node joins a
       layer once, when layer[v] is the layer's number */
    int64_t *dist;
    uint32_t *stamp, now;
    uint32_t *layer, nlayer;
    uint32_t *front, *next;
} bctx;

static uint32_t val(bctx *c, uint32_t x)
{
    return limba_edit_resolve(c->e, x);
}

static const limba_inst *def(bctx *c, uint32_t x)
{
    return &c->f->insts[x];
}

static bool is_int(limba_id t)
{
    return t == LIMBA_T_I8 || t == LIMBA_T_I16 || t == LIMBA_T_I32 ||
           t == LIMBA_T_I64;
}

/* the value of the constant x, if it is one */
static bool konst(bctx *c, uint32_t x, int64_t *k)
{
    const limba_inst *in = def(c, x);
    if (in->op != LIMBA_OP_ICONST || !is_int(in->type))
        return false;
    *k = limba_int_norm(in->imm, in->type);
    return true;
}

/* ---- the facts that hold everywhere ---- */

typedef struct {
    fact *a;
    uint32_t n, cap;
} facts;

static void put(facts *fs, uint32_t u, uint32_t v, int64_t w)
{
    LIMBA_GROW(fs->a, fs->n, fs->cap);
    fs->a[fs->n++] = (fact){u, v, w};
}

/* a == b + k */
static void equal(facts *fs, uint32_t a, uint32_t b, int64_t k)
{
    put(fs, b, a, k); /* a - b <= k */
    if (k != INT64_MIN)
        put(fs, a, b, -k); /* b - a <= -k */
}

/* the argument block b gets from the terminator of p for parameter i;
   false if p passes none (a switch) or passes two different ones */
static bool arg_from(bctx *c, uint32_t p, uint32_t b, uint32_t i, uint32_t *arg)
{
    const limba_func *f = c->f;
    const limba_block *bl = &f->blocks[p];
    const limba_inst *in = &f->insts[bl->insts[bl->ninsts - 1]];
    if (in->op != LIMBA_OP_BR && in->op != LIMBA_OP_CBR)
        return false;
    bool found = false;
    uint32_t k = in->first + (in->op == LIMBA_OP_CBR);
    for (int t = 0; t < (in->op == LIMBA_OP_CBR ? 2 : 1); t++) {
        limba_id to;
        uint32_t n;
        uint32_t a = limba_target(f, k, &to, &n);
        if (to == b) {
            if (i >= n)
                return false;
            uint32_t x = val(c, f->operands[a + i]);
            if (found && x != *arg)
                return false;
            *arg = x;
            found = true;
        }
        k = a + n;
    }
    return found;
}

/* x is p plus (up) or minus (down) a constant from 0 */
static bool steps(bctx *c, uint32_t x, uint32_t p, bool up)
{
    const limba_inst *in = def(c, x);
    const uint32_t *o = c->f->operands + in->first;
    int64_t k;
    if (in->op == LIMBA_OP_ADDOV && in->nops == 2) {
        uint32_t a = val(c, o[0]), b = val(c, o[1]);
        if (a == p && konst(c, b, &k))
            return up ? k >= 0 : k <= 0;
        if (b == p && konst(c, a, &k))
            return up ? k >= 0 : k <= 0;
    }
    if (in->op == LIMBA_OP_SUBOV && in->nops == 2 && val(c, o[0]) == p &&
        konst(c, val(c, o[1]), &k))
        return up ? k <= 0 : k >= 0;
    return false;
}

/* parameter i of block b, value p: from init, or p stepping one way */
static void induction(bctx *c, facts *fs, uint32_t b, uint32_t i, uint32_t p)
{
    const limba_cfg *g = c->cfg;
    uint32_t init = LIMBA_NONE;
    bool up = true, down = true;
    for (uint32_t k = g->pfirst[b]; k < g->pfirst[b + 1]; k++) {
        uint32_t q = g->pred[k], a;
        if (!limba_cfg_reachable(g, q))
            continue;
        if (!arg_from(c, q, b, i, &a))
            return;
        if (a == p)
            continue;
        bool su = steps(c, a, p, true), sd = steps(c, a, p, false);
        if (su || sd) {
            up = up && su;
            down = down && sd;
            continue;
        }
        if (init != LIMBA_NONE && init != a)
            return;
        init = a;
    }
    if (init == LIMBA_NONE || init >= c->f->ninsts)
        return;
    /* the start is the same value on every trip through the loop: it is
       defined in a block that strictly dominates the loop's */
    uint32_t db = def(c, init)->block;
    if (db == b || !limba_cfg_reachable(g, db) ||
        !limba_cfg_dominates(g, db, b))
        return;
    if (up)
        put(fs, p, init, 0); /* init - p <= 0 */
    else if (down)
        put(fs, init, p, 0); /* p - init <= 0 */
}

static void everywhere(bctx *c)
{
    limba_func *f = c->f;
    facts fs = {NULL, 0, 0};
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (!limba_cfg_reachable(c->cfg, b))
            continue;
        const limba_block *bl = &f->blocks[b];
        for (uint32_t k = 0; k < bl->ninsts; k++) {
            uint32_t id = bl->insts[k];
            if (c->e->dead[id])
                continue;
            const limba_inst *in = &f->insts[id];
            const uint32_t *o = f->operands + in->first;
            int64_t v;
            if (!is_int(in->type))
                continue;
            switch (in->op) {
            case LIMBA_OP_ICONST:
                equal(&fs, id, c->z, limba_int_norm(in->imm, in->type));
                break;
            case LIMBA_OP_SEXT:
                if (is_int(def(c, val(c, o[0]))->type))
                    equal(&fs, id, val(c, o[0]), 0);
                break;
            case LIMBA_OP_ADDOV:
                if (konst(c, val(c, o[1]), &v))
                    equal(&fs, id, val(c, o[0]), v);
                else if (konst(c, val(c, o[0]), &v))
                    equal(&fs, id, val(c, o[1]), v);
                break;
            case LIMBA_OP_SUBOV:
                if (konst(c, val(c, o[1]), &v) && v != INT64_MIN)
                    equal(&fs, id, val(c, o[0]), -v);
                break;
            case LIMBA_OP_PARAM:
                induction(c, &fs, b, k, id); /* params come first */
                break;
            }
        }
    }
    /* by u, for the search */
    uint32_t n = c->z + 1;
    c->gfirst = limba_xcalloc((size_t)n + 1, sizeof(uint32_t));
    for (uint32_t i = 0; i < fs.n; i++)
        c->gfirst[fs.a[i].u + 1]++;
    for (uint32_t i = 0; i < n; i++)
        c->gfirst[i + 1] += c->gfirst[i];
    c->g = limba_xmalloc(((size_t)fs.n + 1) * sizeof(fact));
    uint32_t *fill = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    memcpy(fill, c->gfirst, (size_t)n * sizeof(uint32_t));
    for (uint32_t i = 0; i < fs.n; i++)
        c->g[fill[fs.a[i].u]++] = fs.a[i];
    free(fill);
    free(fs.a);
}

/* ---- the facts of the walk ---- */

static void push(bctx *c, uint32_t u, uint32_t v, int64_t w)
{
    LIMBA_GROW(c->s, c->ns, c->caps);
    c->s[c->ns++] = (fact){u, v, w};
}

/* the comparison cc of a and b holds: as differences */
static void compare(bctx *c, unsigned cc, uint32_t a, uint32_t b)
{
    switch (cc) {
    case LIMBA_CC_EQ:
        push(c, b, a, 0);
        push(c, a, b, 0);
        break;
    case LIMBA_CC_SLE:
        push(c, b, a, 0); /* a - b <= 0 */
        break;
    case LIMBA_CC_SLT:
        push(c, b, a, -1);
        break;
    case LIMBA_CC_SGE:
        push(c, a, b, 0); /* b - a <= 0 */
        break;
    case LIMBA_CC_SGT:
        push(c, a, b, -1);
        break;
    }
}

/* the negation of a signed comparison, or EQ for none (ne is no fact) */
static unsigned negate(unsigned cc)
{
    switch (cc) {
    case LIMBA_CC_NE:
        return LIMBA_CC_EQ;
    case LIMBA_CC_SLE:
        return LIMBA_CC_SGT;
    case LIMBA_CC_SLT:
        return LIMBA_CC_SGE;
    case LIMBA_CC_SGE:
        return LIMBA_CC_SLT;
    case LIMBA_CC_SGT:
        return LIMBA_CC_SLE;
    }
    return LIMBA_CC_NE; /* no fact */
}

/* condition x is known to be truth */
static void know(bctx *c, uint32_t x, bool truth, int depth)
{
    x = val(c, x);
    const limba_inst *in = def(c, x);
    const uint32_t *o = c->f->operands + in->first;
    if (depth > 4)
        return;
    if (in->op == LIMBA_OP_AND && truth) {
        know(c, o[0], true, depth + 1);
        know(c, o[1], true, depth + 1);
    } else if (in->op == LIMBA_OP_OR && !truth) {
        know(c, o[0], false, depth + 1);
        know(c, o[1], false, depth + 1);
    } else if (in->op == LIMBA_OP_ICMP) {
        uint32_t a = val(c, o[0]), b = val(c, o[1]);
        if (!is_int(def(c, a)->type))
            return;
        compare(c, truth ? in->cc : negate(in->cc), a, b);
    }
}

/* ---- proofs ---- */

/* the least w found with dst - src <= w along at most DEPTH facts, or
   INT64_MAX */
static int64_t reach(bctx *c, uint32_t src, uint32_t dst)
{
    c->now++;
    uint32_t nf = 0, budget = EXPAND;
    c->stamp[src] = c->now;
    c->dist[src] = 0;
    c->front[nf++] = src;
    for (int d = 0; d < DEPTH && nf && budget; d++) {
        uint32_t nn = 0;
        c->nlayer++;
        for (uint32_t i = 0; i < nf && budget; i++, budget--) {
            uint32_t u = c->front[i];
            int64_t du = c->dist[u];
            /* the facts out of u: those of everywhere, then the walk's */
            const fact *a = c->g + c->gfirst[u];
            uint32_t n = c->gfirst[u + 1] - c->gfirst[u];
            for (int part = 0; part < 2; part++) {
                for (uint32_t k = 0; k < n; k++) {
                    int64_t w;
                    uint32_t v = a[k].v;
                    if (a[k].u != u || __builtin_add_overflow(du, a[k].w, &w))
                        continue;
                    if (c->stamp[v] == c->now && c->dist[v] <= w)
                        continue;
                    c->stamp[v] = c->now;
                    c->dist[v] = w;
                    if (c->layer[v] != c->nlayer) {
                        c->layer[v] = c->nlayer;
                        c->next[nn++] = v;
                    }
                }
                a = c->s;
                n = c->ns;
            }
        }
        uint32_t *t = c->front;
        c->front = c->next;
        c->next = t;
        nf = nn;
    }
    return c->stamp[dst] == c->now ? c->dist[dst] : INT64_MAX;
}

/* condition x holds for certain */
static bool proved(bctx *c, uint32_t x, int depth)
{
    x = val(c, x);
    const limba_inst *in = def(c, x);
    const uint32_t *o = c->f->operands + in->first;
    if (in->op == LIMBA_OP_ICONST)
        return limba_int_norm(in->imm, in->type) != 0;
    if (depth > 4)
        return false;
    if (in->op == LIMBA_OP_AND)
        return proved(c, o[0], depth + 1) && proved(c, o[1], depth + 1);
    if (in->op != LIMBA_OP_ICMP)
        return false;
    uint32_t a = val(c, o[0]), b = val(c, o[1]);
    if (!is_int(def(c, a)->type))
        return false;
    switch (in->cc) {
    case LIMBA_CC_SLE: /* a - b <= 0 */
        return reach(c, b, a) <= 0;
    case LIMBA_CC_SLT:
        return reach(c, b, a) <= -1;
    case LIMBA_CC_SGE: /* b - a <= 0 */
        return reach(c, a, b) <= 0;
    case LIMBA_CC_SGT:
        return reach(c, a, b) <= -1;
    case LIMBA_CC_EQ:
        return reach(c, b, a) <= 0 && reach(c, a, b) <= 0;
    }
    return false;
}

/* ---- the walk ---- */

/* the fact the edge from the single predecessor of b gives */
static void edge_fact(bctx *c, uint32_t b)
{
    const limba_cfg *g = c->cfg;
    if (g->pfirst[b + 1] - g->pfirst[b] != 1)
        return;
    uint32_t p = g->pred[g->pfirst[b]];
    const limba_func *f = c->f;
    const limba_block *bl = &f->blocks[p];
    const limba_inst *in = &f->insts[bl->insts[bl->ninsts - 1]];
    if (in->op != LIMBA_OP_CBR)
        return;
    uint32_t t = in->first + 1;
    uint32_t e = t + 2 + f->operands[t + 1];
    limba_id tb = f->operands[t], eb = f->operands[e];
    if (tb == eb)
        return;
    know(c, f->operands[in->first], tb == b, 0);
}

static uint32_t visit(bctx *c, uint32_t b)
{
    const limba_block *bl = &c->f->blocks[b];
    uint32_t changes = 0;
    edge_fact(c, b);
    for (uint32_t k = 0; k < bl->ninsts; k++) {
        uint32_t id = bl->insts[k];
        const limba_inst *in = &c->f->insts[id];
        if (c->e->dead[id] || in->op != LIMBA_OP_CHECK)
            continue;
        uint32_t cond = c->f->operands[in->first];
        if (proved(c, cond, 0)) {
            c->e->dead[id] = 1;
            changes++;
            continue;
        }
        /* a and b with one of them proved: the check keeps the other */
        const limba_inst *ci = def(c, val(c, cond));
        if (ci->op == LIMBA_OP_AND) {
            const uint32_t *o = c->f->operands + ci->first;
            uint32_t a = val(c, o[0]), b = val(c, o[1]);
            bool pa = proved(c, a, 0);
            if (pa || proved(c, b, 0)) {
                c->f->operands[in->first] = pa ? b : a;
                changes++;
            }
        }
        know(c, cond, true, 0); /* past it, it holds */
    }
    return changes;
}

uint32_t limba_pass_bounds(limba_pass_ctx *x, limba_func *f)
{
    bctx c = {.f = f, .e = &x->e, .cfg = limba_pass_cfg_of(x, f)};
    uint32_t n = f->nblocks, nodes = f->ninsts + 1;
    /* nothing to do without a check of a comparison */
    bool any = false;
    for (uint32_t i = 0; i < f->ninsts && !any; i++) {
        const limba_inst *in = &f->insts[i];
        if (in->op != LIMBA_OP_CHECK || x->e.dead[i])
            continue;
        unsigned op =
            f->insts[limba_edit_resolve(&x->e, f->operands[in->first])].op;
        any = op == LIMBA_OP_ICMP || op == LIMBA_OP_AND;
    }
    if (!any)
        return 0;
    c.z = f->ninsts;
    everywhere(&c);
    c.dist = limba_xmalloc((size_t)nodes * sizeof(int64_t));
    c.stamp = limba_xcalloc(nodes, sizeof(uint32_t));
    c.layer = limba_xcalloc(nodes, sizeof(uint32_t));
    c.front = limba_xmalloc(((size_t)nodes + 1) * sizeof(uint32_t));
    c.next = limba_xmalloc(((size_t)nodes + 1) * sizeof(uint32_t));

    /* children in the dominator tree */
    const limba_cfg *g = c.cfg;
    uint32_t *cfirst = limba_xcalloc((size_t)n + 2, sizeof(uint32_t));
    uint32_t *child = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *fill = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    for (uint32_t b = 1; b < n; b++)
        if (limba_cfg_reachable(g, b))
            cfirst[g->idom[b] + 1]++;
    for (uint32_t b = 0; b < n; b++)
        cfirst[b + 1] += cfirst[b];
    memcpy(fill, cfirst, n * sizeof(uint32_t));
    for (uint32_t b = 1; b < n; b++)
        if (limba_cfg_reachable(g, b))
            child[fill[g->idom[b]]++] = b;

    /* down the tree: a block's facts hold in the blocks it dominates */
    uint32_t *stack = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *next = limba_xcalloc((size_t)n + 1, sizeof(uint32_t));
    uint32_t *mark = limba_xmalloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t sp = 0, changes = 0;
    stack[sp++] = 0;
    mark[0] = 0;
    changes += visit(&c, 0);
    while (sp) {
        uint32_t b = stack[sp - 1];
        if (cfirst[b] + next[b] < cfirst[b + 1]) {
            uint32_t s = child[cfirst[b] + next[b]++];
            mark[s] = c.ns;
            stack[sp++] = s;
            changes += visit(&c, s);
        } else {
            c.ns = mark[b];
            sp--;
        }
    }
    free(stack);
    free(next);
    free(mark);
    free(cfirst);
    free(child);
    free(fill);
    free(c.dist);
    free(c.stamp);
    free(c.layer);
    free(c.front);
    free(c.next);
    free(c.gfirst);
    free(c.g);
    free(c.s);
    return changes;
}
