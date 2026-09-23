/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * strtab.c - interned byte strings (see strtab.h).
 */
#include "strtab.h"

#include "hash.h"
#include "xalloc.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct limba_strtab {
    char *bytes; /* every string followed by a NUL */
    size_t nbytes, capbytes;
    uint32_t *start;
    uint32_t *len;
    uint32_t count, cap;
    limba_hash *index;
};

limba_strtab *limba_strtab_new(void)
{
    limba_strtab *t = limba_xcalloc(1, sizeof(*t));
    t->index = limba_hash_new();
    return t;
}

void limba_strtab_free(limba_strtab *t)
{
    if (!t)
        return;
    free(t->bytes);
    free(t->start);
    free(t->len);
    limba_hash_free(t->index);
    free(t);
}

struct probe {
    const limba_strtab *t;
    const char *s;
    size_t len;
};

static bool same(const void *ctx, uint32_t id)
{
    const struct probe *p = ctx;
    return p->t->len[id] == p->len &&
           memcmp(p->t->bytes + p->t->start[id], p->s, p->len) == 0;
}

uint32_t limba_strtab_find(const limba_strtab *t, const char *s, size_t len)
{
    struct probe p = {t, s, len};
    return limba_hash_find(t->index, limba_fnv(s, len, LIMBA_FNV_SEED), same,
                           &p);
}

uint32_t limba_strtab_intern(limba_strtab *t, const char *s, size_t len)
{
    uint32_t id = limba_strtab_find(t, s, len);
    if (id != UINT32_MAX)
        return id;
    if (len > UINT32_MAX - 1 || t->nbytes + len + 1 > UINT32_MAX) {
        /* ids and offsets are 32-bit: 4 GB of names is not a program */
        abort();
    }
    while (t->nbytes + len + 1 > t->capbytes) {
        t->capbytes = t->capbytes ? 2 * t->capbytes : 4096;
        t->bytes = limba_xrealloc(t->bytes, t->capbytes, 1);
    }
    if (t->count == t->cap) {
        t->cap = t->cap ? 2 * t->cap : 64;
        t->start = limba_xrealloc(t->start, t->cap, sizeof(*t->start));
        t->len = limba_xrealloc(t->len, t->cap, sizeof(*t->len));
    }
    id = t->count++;
    t->start[id] = (uint32_t)t->nbytes;
    t->len[id] = (uint32_t)len;
    if (len)
        memcpy(t->bytes + t->nbytes, s, len);
    t->bytes[t->nbytes + len] = 0;
    t->nbytes += len + 1;
    limba_hash_put(t->index, limba_fnv(s, len, LIMBA_FNV_SEED), id);
    return id;
}

const char *limba_strtab_get(const limba_strtab *t, uint32_t id, size_t *len)
{
    if (len)
        *len = t->len[id];
    return t->bytes + t->start[id];
}

uint32_t limba_strtab_count(const limba_strtab *t)
{
    return t->count;
}
