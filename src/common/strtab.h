/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * strtab.h - interned byte strings: equal bytes, equal id. Ids are dense,
 * in order of first appearance.
 */
#ifndef LIMBA_STRTAB_H
#define LIMBA_STRTAB_H

#include <stddef.h>
#include <stdint.h>

typedef struct limba_strtab limba_strtab;

limba_strtab *limba_strtab_new(void);
void limba_strtab_free(limba_strtab *t);
uint32_t limba_strtab_intern(limba_strtab *t, const char *s, size_t len);
/* the same, with hash = limba_fnv(s, len, LIMBA_FNV_SEED) made already */
uint32_t limba_strtab_intern_hashed(limba_strtab *t, const char *s, size_t len,
                                    uint64_t hash);
/* UINT32_MAX if the bytes were never interned */
uint32_t limba_strtab_find(const limba_strtab *t, const char *s, size_t len);
/* the bytes of id, NUL-terminated for convenience; *len may be NULL */
const char *limba_strtab_get(const limba_strtab *t, uint32_t id, size_t *len);
uint32_t limba_strtab_count(const limba_strtab *t);

#endif
