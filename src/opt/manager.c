/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * manager.c - the pipeline. One table, the only place that says which
 * passes run and in which order (SedaiBasic2 had five hand-made copies that
 * drifted apart). The whole pipeline repeats on a function until a round
 * changes nothing in it, at most MAX_ROUNDS times: no pass looks at
 * another function, so a function at a time gives what the whole module
 * at a time gave, and a front end can optimise each as it completes it.
 */
#include "limba/opt.h"
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ROUNDS 4

static const struct {
    const char *name;
    limba_pass_fn run;
} pipeline[] = {
    {"cfg", limba_pass_cfg},   /* constant branches, unreachable blocks */
    {"fold", limba_pass_fold}, /* constants and identities */
    {"gvn", limba_pass_gvn},   /* equal pure values, one computation */
    {"dce", limba_pass_dce},   /* values nobody uses */
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
    return z;
}

int limba_optimizer_func(limba_optimizer *z, limba_module *m, limba_id fid,
                         limba_diag *d)
{
    if (z->verify && !z->v)
        z->v = limba_verifier_new(m);
    unsigned round;
    for (round = 0; round < MAX_ROUNDS; round++) {
        uint64_t changed = 0;
        for (size_t p = 0; p < NPASSES; p++) {
            if (z->skip[p])
                continue;
            uint32_t n = pipeline[p].run(m, &m->funcs[fid]);
            z->total[p] += n;
            changed += n;
            if (z->verify && limba_verifier_func(z->v, fid, d) != 0) {
                if (d) { /* prefix the pass; a long message is cut */
                    char msg[sizeof(d->msg) + 32];
                    snprintf(msg, sizeof(msg), "after pass %s: %s",
                             pipeline[p].name, d->msg);
                    memcpy(d->msg, msg, sizeof(d->msg) - 1);
                    d->msg[sizeof(d->msg) - 1] = 0;
                }
                return -1;
            }
        }
        if (!changed)
            break;
    }
    round = round < MAX_ROUNDS ? round + 1 : round;
    if (round > z->rounds)
        z->rounds = round;
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
