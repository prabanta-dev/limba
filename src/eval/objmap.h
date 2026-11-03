/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * objmap.h - the objects of a run of the reference interpreter, by
 * address: slots, globals, blocks of mem_alloc, C strings, the bytes of
 * strings. It answers whether n bytes from p lie in one object alive
 * (progetto_ir.md § 4: an access outside is a wrong module). A treap
 * whose priorities are a hash of the address, so a run is the same each
 * time; the object found last is remembered, as most accesses fall in
 * the object of the one before.
 */
#ifndef LIMBA_EVAL_OBJMAP_H
#define LIMBA_EVAL_OBJMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct objnode objnode;

typedef struct {
    objnode *root;
    objnode *last; /* the object found last, or NULL */
} objmap;

/* an object of size bytes at base; two objects never overlap */
void objmap_add(objmap *m, uintptr_t base, size_t size);
/* the object at base is gone (nothing if there is none) */
void objmap_del(objmap *m, uintptr_t base);
/* do the n bytes from p (n > 0) lie in one object? */
bool objmap_holds(objmap *m, uintptr_t p, uint64_t n);
void objmap_free(objmap *m);

#endif
