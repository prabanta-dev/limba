/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * hash.h - an index from a 64-bit hash to ids, open addressing. It stores
 * no keys: the caller says with a callback whether a candidate id is the
 * one it looks for, so the same table serves strings, types and names.
 */
#ifndef LIMBA_HASH_H
#define LIMBA_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct limba_hash limba_hash;

limba_hash *limba_hash_new(void);
void limba_hash_free(limba_hash *h);
/* the id with hash key for which eq(ctx, id) holds, UINT32_MAX if none */
uint32_t limba_hash_find(const limba_hash *h, uint64_t key,
                         bool (*eq)(const void *ctx, uint32_t id),
                         const void *ctx);
/* add id under key; the caller has checked it is not there */
void limba_hash_put(limba_hash *h, uint64_t key, uint32_t id);

/* FNV-1a */
static inline uint64_t limba_fnv(const void *p, size_t n, uint64_t h)
{
    const unsigned char *s = p;
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
#define LIMBA_FNV_SEED 0xcbf29ce484222325ull

#endif
