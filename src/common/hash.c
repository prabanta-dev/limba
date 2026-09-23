/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * hash.c - hash to ids, linear probing, at most half full (see hash.h).
 */
#include "hash.h"

#include "xalloc.h"

#include <stdlib.h>

struct limba_hash {
    uint64_t *keys;
    uint32_t *ids; /* UINT32_MAX = empty */
    uint32_t cap;  /* a power of two */
    uint32_t count;
};

static void alloc_table(limba_hash *h, uint32_t cap)
{
    h->cap = cap;
    h->keys = limba_xcalloc(cap, sizeof(*h->keys));
    h->ids = limba_xmalloc((size_t)cap * sizeof(*h->ids));
    for (uint32_t i = 0; i < cap; i++)
        h->ids[i] = UINT32_MAX;
}

limba_hash *limba_hash_new(void)
{
    limba_hash *h = limba_xcalloc(1, sizeof(*h));
    alloc_table(h, 64);
    return h;
}

void limba_hash_free(limba_hash *h)
{
    if (!h)
        return;
    free(h->keys);
    free(h->ids);
    free(h);
}

uint32_t limba_hash_find(const limba_hash *h, uint64_t key,
                         bool (*eq)(const void *ctx, uint32_t id),
                         const void *ctx)
{
    uint32_t mask = h->cap - 1;
    for (uint32_t i = (uint32_t)key & mask;; i = (i + 1) & mask) {
        if (h->ids[i] == UINT32_MAX)
            return UINT32_MAX;
        if (h->keys[i] == key && eq(ctx, h->ids[i]))
            return h->ids[i];
    }
}

static void insert(limba_hash *h, uint64_t key, uint32_t id)
{
    uint32_t mask = h->cap - 1;
    uint32_t i = (uint32_t)key & mask;
    while (h->ids[i] != UINT32_MAX)
        i = (i + 1) & mask;
    h->keys[i] = key;
    h->ids[i] = id;
}

void limba_hash_put(limba_hash *h, uint64_t key, uint32_t id)
{
    if (2 * (h->count + 1) > h->cap) {
        uint64_t *keys = h->keys;
        uint32_t *ids = h->ids;
        uint32_t cap = h->cap;
        alloc_table(h, 2 * cap);
        for (uint32_t i = 0; i < cap; i++)
            if (ids[i] != UINT32_MAX)
                insert(h, keys[i], ids[i]);
        free(keys);
        free(ids);
    }
    insert(h, key, id);
    h->count++;
}
