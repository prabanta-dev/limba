/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * manager.c - the pipeline. One table, the only place that says which
 * passes run and in which order (SedaiBasic2 had five hand-made copies that
 * drifted apart). The whole pipeline repeats until a round changes nothing,
 * at most MAX_ROUNDS times.
 */
#include "limba/opt.h"
#include "pass.h"

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

int limba_optimize(limba_module *m, const limba_opt_options *o, limba_diag *d)
{
    limba_opt_options none = {false, NULL, NULL};
    if (!o)
        o = &none;
    bool verify = o->verify_each;
#ifndef NDEBUG
    verify = true;
#endif
    const char *env = getenv("LIMBA_OPTSKIP");
    uint64_t total[NPASSES] = {0};
    bool skip[NPASSES];
    for (size_t p = 0; p < NPASSES; p++)
        skip[p] =
            listed(o->skip, pipeline[p].name) || listed(env, pipeline[p].name);

    unsigned round;
    for (round = 0; round < MAX_ROUNDS; round++) {
        uint64_t changed = 0;
        for (size_t p = 0; p < NPASSES; p++) {
            if (skip[p])
                continue;
            for (uint32_t i = 0; i < m->nfuncs; i++) {
                uint32_t n = pipeline[p].run(m, &m->funcs[i]);
                total[p] += n;
                changed += n;
            }
            if (verify && limba_verify(m, d) != 0) {
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
    if (o->stats) {
        for (size_t p = 0; p < NPASSES; p++)
            fprintf(o->stats, "pass %-5s %s%llu changes\n", pipeline[p].name,
                    skip[p] ? "skipped, " : "", (unsigned long long)total[p]);
        fprintf(o->stats, "rounds %u\n",
                round < MAX_ROUNDS ? round + 1 : round);
    }
    return 0;
}
