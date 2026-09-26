/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_gen.c - the OPTDIFF net on random programs. For each seed the
 * generator's program must verify, and it must behave the same not
 * optimised, fully optimised and with each pass alone. It writes nothing:
 * a disagreement prints the seed, the configuration and the program
 * (lir_gen SEED gives it back).
 *
 *   LIMBA_GEN_SEEDS  how many seeds (default 200)
 *   LIMBA_GEN_FIRST  the first seed (default 1)
 *
 * A run that reaches the step limit is not compared: the optimised program
 * executes fewer instructions, so the limit falls elsewhere.
 */
#define _GNU_SOURCE
#include "eval/eval.h"
#include "eval/gen.h"
#include "limba/ir.h"
#include "limba/opt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static const limba_eval_limits limits = {5000000, 0, 0, NULL, NULL};

static char *print(const limba_module *m)
{
    char *s = NULL;
    size_t len;
    FILE *f = open_memstream(&s, &len);
    limba_print(m, f);
    fclose(f);
    return s;
}

static bool same(const limba_eval_result *a, const limba_eval_result *b)
{
    return a->status == b->status && a->code == b->code && a->ret == b->ret &&
           a->outlen == b->outlen && !memcmp(a->out, b->out, a->outlen);
}

static uint64_t env(const char *name, uint64_t dflt)
{
    const char *s = getenv(name);
    return s && *s ? strtoull(s, NULL, 0) : dflt;
}

int main(void)
{
    uint64_t first = env("LIMBA_GEN_FIRST", 1);
    uint64_t count = env("LIMBA_GEN_SEEDS", 200);
    uint64_t ended[6] = {0}, compared = 0, limited = 0, insts = 0;
    uint64_t traps[3] = {0}; /* overflow 6, check 9, division 11 */
    int reported = 0;

    for (uint64_t seed = first; seed < first + count; seed++) {
        limba_module *m = limba_gen(seed);
        limba_diag d = {{0}, 0};
        if (limba_verify(m, &d) != 0) {
            fprintf(stderr, "FAIL seed %" PRIu64 ": does not verify: %s\n",
                    seed, d.msg);
            failures++;
            limba_module_free(m);
            continue;
        }
        for (uint32_t i = 0; i < m->nfuncs; i++)
            insts += m->funcs[i].ninsts;
        limba_eval_result base;
        limba_eval(m, "main", &limits, &base);
        ended[base.status]++;
        if (base.status == LIMBA_EVAL_TRAP)
            traps[base.code == 6 ? 0 : base.code == 9 ? 1 : 2]++;
        limba_module_free(m);
        if (base.status == LIMBA_EVAL_LIMIT) {
            limited++;
            limba_eval_result_free(&base);
            continue;
        }

        /* every pass, then each pass alone */
        for (int k = -1; k < 0 || limba_pass_name((unsigned)k); k++) {
            char skip[256] = "";
            for (unsigned p = 0; k >= 0 && limba_pass_name(p); p++)
                if ((int)p != k) {
                    strcat(skip, skip[0] ? "," : "");
                    strcat(skip, limba_pass_name(p));
                }
            const char *label =
                k < 0 ? "all passes" : limba_pass_name((unsigned)k);
            /* verified after every pass on odd seeds; on even ones the
               edit waits to the end, as in a release, and the module is
               verified after */
            limba_opt_options o = {seed & 1, skip, NULL, "bounds"};
            m = limba_gen(seed);
            if (limba_optimize(m, &o, &d) != 0 ||
                (!(seed & 1) && limba_verify(m, &d) != 0)) {
                fprintf(stderr, "FAIL seed %" PRIu64 " (%s): %s\n", seed, label,
                        d.msg);
                failures++;
                limba_module_free(m);
                continue;
            }
            limba_eval_result r;
            limba_eval(m, "main", &limits, &r);
            limba_module_free(m);
            if (r.status == LIMBA_EVAL_LIMIT) {
                limited++;
            } else if (!same(&r, &base)) {
                failures++;
                fprintf(stderr,
                        "FAIL seed %" PRIu64 " (%s): status %d/%d, code "
                        "%" PRId64 "/%" PRId64 ", result %" PRId64 "/%" PRId64
                        "\n",
                        seed, label, r.status, base.status, r.code, base.code,
                        (int64_t)r.ret, (int64_t)base.ret);
                if (reported++ < 3) {
                    limba_module *orig = limba_gen(seed);
                    char *text = print(orig);
                    fprintf(stderr,
                            "%s--- output optimised:\n%s--- "
                            "not optimised:\n%s---\n",
                            text, r.out, base.out);
                    free(text);
                    limba_module_free(orig);
                }
            } else {
                compared++;
            }
            limba_eval_result_free(&r);
        }
        limba_eval_result_free(&base);
    }
    printf("test_gen: %" PRIu64 " seeds from %" PRIu64 ", %" PRIu64
           " instructions; ended: %" PRIu64 " returned, %" PRIu64
           " trapped (%" PRIu64 " overflow, %" PRIu64 " check, %" PRIu64
           " division), %" PRIu64 " unreachable, %" PRIu64
           " at the limit; %" PRIu64 " optimised runs agreed, %" PRIu64
           " not compared (limit), %d failures\n",
           count, first, insts, ended[LIMBA_EVAL_OK], ended[LIMBA_EVAL_TRAP],
           traps[0], traps[1], traps[2], ended[LIMBA_EVAL_UNREACHABLE],
           ended[LIMBA_EVAL_LIMIT], compared, limited, failures);
    return failures ? 1 : 0;
}
