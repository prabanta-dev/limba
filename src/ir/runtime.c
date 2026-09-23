/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * runtime.c - the table of the run-time library (runtime.def): names,
 * signatures read from their text, fingerprint.
 */
#include "internal.h"

#include "common/hash.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

const limba_rt_info limba_rts[LIMBA_RT_COUNT] = {
#define LIMBA_RT(name, text, sig, attrs) {text, sig, attrs},
#include "limba/runtime.def"
#undef LIMBA_RT
};

uint64_t limba_rt_fingerprint(void)
{
    uint64_t h = LIMBA_FNV_SEED;
    for (unsigned i = 0; i < LIMBA_RT_COUNT; i++) {
        h = limba_fnv(limba_rts[i].name, strlen(limba_rts[i].name) + 1, h);
        h = limba_fnv(limba_rts[i].sig, strlen(limba_rts[i].sig) + 1, h);
        h = limba_fnv(&limba_rts[i].attrs, sizeof(limba_rts[i].attrs), h);
    }
    return h;
}

limba_id limba_rt_find(const char *name, size_t len)
{
    for (unsigned i = 0; i < LIMBA_RT_COUNT; i++)
        if (strlen(limba_rts[i].name) == len &&
            memcmp(limba_rts[i].name, name, len) == 0)
            return i;
    return LIMBA_NONE;
}

static const char *skip(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

static const char *word(const char *s, limba_id *t)
{
    size_t n = 0;
    while (isalnum((unsigned char)s[n]))
        n++;
    *t = limba_scalar_find(s, n);
    return s + n;
}

bool limba_rt_sig(limba_id rt, limba_sig *sig)
{
    if (rt >= LIMBA_RT_COUNT)
        return false;
    sig->n = 0;
    sig->variadic = false;
    sig->mem = NULL;
    const char *s = skip(limba_rts[rt].sig);
    if (*s++ != '(')
        return false;
    s = skip(s);
    while (*s != ')') {
        if (strncmp(s, "...", 3) == 0) {
            sig->variadic = true;
            s = skip(s + 3);
            continue;
        }
        if (sig->n == LIMBA_RT_MAXPARAMS)
            return false;
        limba_id t;
        s = word(s, &t);
        if (t == LIMBA_NONE || t == LIMBA_T_VOID)
            return false;
        sig->rt[sig->n++] = t;
        s = skip(s);
        if (*s == ',')
            s = skip(s + 1);
        else if (*s != ')')
            return false;
    }
    s = skip(s + 1);
    if (strncmp(s, "->", 2) != 0)
        return false;
    s = word(skip(s + 2), &sig->ret);
    return sig->ret != LIMBA_NONE && !*skip(s);
}
