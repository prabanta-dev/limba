/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * manager.c - the pipeline. One table, the only place that says which
 * passes run and in which order (SedaiBasic2 had five hand-made copies that
 * drifted apart). The pipeline repeats on a function until every pass has
 * run once in a row without changing it, or changing only what gives the
 * others no work (no round more just to see that nothing changes), at
 * most MAX_ROUNDS rounds: no pass looks at
 * another function, so a function at a time gives what the whole module
 * at a time gave, and a front end can optimise each as it completes it.
 */
#include "limba/opt.h"
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ROUNDS 4

/* wakes: what the pass changes may give work to the others. dce only
   drops values nobody uses: no branch, constant or equal value is new
   after it, and its own work list leaves nothing dead behind */
static const struct {
    const char *name;
    limba_pass_fn run;
    bool wakes;
} pipeline[] = {
    {"cfg", limba_pass_cfg, true},  /* constant branches, unreachable blocks */
    {"gvn", limba_pass_gvn, true},  /* folding, then equal values once */
    {"dce", limba_pass_dce, false}, /* values nobody uses */
};
#define NPASSES (sizeof(pipeline) / sizeof(pipeline[0]))

const char *limba_pass_name(unsigned i)
{
    return i < NPASSES ? pipeline[i].name : NULL;
}

/* is name in the comma-separated list? */
static bool listed(const char *list, const char *name)
{
    size_t n = strlen(name);
    while (list && *list) {
        const char *end = strchr(list, ',');
        size_t k = end ? (size_t)(end - list) : strlen(list);
        if (k == n && !strncmp(list, name, n))
            return true;
        list = end ? end + 1 : NULL;
    }
    return false;
}

struct limba_optimizer {
    limba_opt_options o;
    bool verify;
    bool skip[NPASSES];
    bool no_fold; /* "fold" skipped: gvn does not fold */
    uint64_t total[NPASSES];
    unsigned rounds; /* the most a function took */
    limba_verifier *v;
};

limba_optimizer *limba_optimizer_new(const limba_opt_options *o)
{
    limba_optimizer *z = limba_xcalloc(1, sizeof(*z));
    if (o)
        z->o = *o;
    z->verify = z->o.verify_each;
#ifndef NDEBUG
    z->verify = true;
#endif
    const char *env = getenv("LIMBA_OPTSKIP");
    for (size_t p = 0; p < NPASSES; p++)
        z->skip[p] = listed(z->o.skip, pipeline[p].name) ||
                     listed(env, pipeline[p].name);
    z->no_fold = listed(z->o.skip, "fold") || listed(env, "fold");
    return z;
}

#ifndef NDEBUG
/* the builds for testing check that a CFG kept is the one f has */
static void cfg_same(const limba_cfg *a, const limba_func *f)
{
    limba_cfg b;
    limba_cfg_build(f, &b);
    uint32_t n = a->n;
    bool ok = a->n == b.n && a->sfirst[n] == b.sfirst[n] &&
              !memcmp(a->sfirst, b.sfirst, (n + 1) * sizeof(uint32_t)) &&
              !memcmp(a->succ, b.succ, b.sfirst[n] * sizeof(uint32_t)) &&
              !memcmp(a->idom, b.idom, n * sizeof(uint32_t)) &&
              !memcmp(a->pre, b.pre, n * sizeof(uint32_t)) &&
              !memcmp(a->post, b.post, n * sizeof(uint32_t));
    limba_cfg_free(&b);
    if (!ok) {
        fprintf(stderr, "limba: internal error: a pass changed the "
                        "branches and kept the old CFG\n");
        abort();
    }
}
#endif

const limba_cfg *limba_pass_cfg_of(limba_pass_ctx *x, const limba_func *f)
{
    if (!x->have_cfg) {
        limba_cfg_build(f, &x->cfg);
        x->have_cfg = true;
    }
#ifndef NDEBUG
    else
        cfg_same(&x->cfg, f);
#endif
    return &x->cfg;
}

void limba_pass_cfg_drop(limba_pass_ctx *x)
{
    if (x->have_cfg)
        limba_cfg_free(&x->cfg);
    x->have_cfg = false;
}

int limba_optimizer_func(limba_optimizer *z, limba_module *m, limba_id fid,
                         limba_diag *d)
{
    limba_pass_ctx x = {.m = m, .fold = !z->no_fold};
    int r = 0;
    if (z->verify && !z->v)
        z->v = limba_verifier_new(m);
    /* to the fixed point: every pass has run on the function as it is
       and changed nothing, or nothing that wakes the others; at most
       MAX_ROUNDS rounds of runs */
    unsigned active = 0, runs = 0, quiet = 0;
    for (size_t p = 0; p < NPASSES; p++)
        active += !z->skip[p];
    for (size_t p = 0; quiet < active && runs < MAX_ROUNDS * active;
         p = (p + 1) % NPASSES) {
        if (z->skip[p])
            continue;
        runs++;
        uint32_t n = pipeline[p].run(&x, &m->funcs[fid]);
        z->total[p] += n;
        quiet = n && pipeline[p].wakes ? 0 : quiet + 1;
        if (z->verify && limba_verifier_func(z->v, fid, d) != 0) {
            if (d) { /* prefix the pass; a long message is cut */
                char msg[sizeof(d->msg) + 32];
                snprintf(msg, sizeof(msg), "after pass %s: %s",
                         pipeline[p].name, d->msg);
                memcpy(d->msg, msg, sizeof(d->msg) - 1);
                d->msg[sizeof(d->msg) - 1] = 0;
            }
            r = -1;
            break;
        }
    }
    limba_pass_cfg_drop(&x);
    if (r)
        return r;
    unsigned rounds = active ? (runs + active - 1) / active : 0;
    if (rounds > z->rounds)
        z->rounds = rounds;
    return 0;
}

void limba_optimizer_free(limba_optimizer *z)
{
    if (!z)
        return;
    if (z->o.stats) {
        for (size_t p = 0; p < NPASSES; p++)
            fprintf(z->o.stats, "pass %-5s %s%llu changes\n", pipeline[p].name,
                    z->skip[p] ? "skipped, " : "",
                    (unsigned long long)z->total[p]);
        fprintf(z->o.stats, "rounds %u\n", z->rounds);
    }
    limba_verifier_free(z->v);
    free(z);
}

int limba_optimize(limba_module *m, const limba_opt_options *o, limba_diag *d)
{
    limba_optimizer *z = limba_optimizer_new(o);
    int r = 0;
    for (uint32_t i = 0; i < m->nfuncs && r == 0; i++)
        r = limba_optimizer_func(z, m, i, d);
    limba_optimizer_free(z);
    return r;
}
