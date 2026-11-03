/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * objmap.c - the objects of a run by address (see objmap.h).
 */
#include "objmap.h"

#include "common/hash.h"
#include "common/xalloc.h"

#include <stdlib.h>

struct objnode {
    uintptr_t base;
    size_t size;
    uint64_t prio;
    objnode *kid[2];
};

static objnode *rotate(objnode *t, int d)
{
    objnode *k = t->kid[d];
    t->kid[d] = k->kid[!d];
    k->kid[!d] = t;
    return k;
}

static objnode *insert(objnode *t, objnode *n)
{
    if (!t)
        return n;
    int d = n->base > t->base;
    t->kid[d] = insert(t->kid[d], n);
    return t->kid[d]->prio > t->prio ? rotate(t, d) : t;
}

/* the treap without the node at base, which *gone receives */
static objnode *erase(objnode *t, uintptr_t base, objnode **gone)
{
    if (!t)
        return NULL;
    if (base != t->base) {
        int d = base > t->base;
        t->kid[d] = erase(t->kid[d], base, gone);
        return t;
    }
    if (!t->kid[0] || !t->kid[1]) {
        *gone = t;
        return t->kid[0] ? t->kid[0] : t->kid[1];
    }
    int d = t->kid[1]->prio > t->kid[0]->prio;
    t = rotate(t, d);
    t->kid[!d] = erase(t->kid[!d], base, gone);
    return t;
}

void objmap_add(objmap *m, uintptr_t base, size_t size)
{
    objnode *n = limba_xmalloc(sizeof(*n));
    *n = (objnode){base, size, limba_mix(LIMBA_FNV_SEED, base), {NULL, NULL}};
    m->root = insert(m->root, n);
}

void objmap_del(objmap *m, uintptr_t base)
{
    objnode *gone = NULL;
    m->root = erase(m->root, base, &gone);
    if (gone == m->last)
        m->last = NULL;
    free(gone);
}

static bool within(const objnode *o, uintptr_t p, uint64_t n)
{
    return p >= o->base && p - o->base <= o->size &&
           n <= o->size - (p - o->base);
}

bool objmap_holds(objmap *m, uintptr_t p, uint64_t n)
{
    if (m->last && within(m->last, p, n))
        return true;
    /* the object with the greatest base not above p */
    objnode *best = NULL;
    for (objnode *t = m->root; t;)
        if (t->base <= p) {
            best = t;
            t = t->kid[1];
        } else {
            t = t->kid[0];
        }
    if (!best || !within(best, p, n))
        return false;
    m->last = best;
    return true;
}

static void drop(objnode *t)
{
    while (t) {
        drop(t->kid[0]);
        objnode *r = t->kid[1];
        free(t);
        t = r;
    }
}

void objmap_free(objmap *m)
{
    drop(m->root);
    m->root = m->last = NULL;
}
