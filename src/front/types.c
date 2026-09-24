/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * types.c - the types of a source language (see types.h).
 */
#include "types.h"

#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pointers, strings and open arrays: the sizes of a 64-bit target; the
   IR lowers them, these serve sizeof and the layout of records */
#define PTR_SIZE 8

static limba_ltype add(limba_types *ts, unsigned kind, unsigned flags,
                       uint64_t size, uint32_t align)
{
    LIMBA_GROW(ts->t, ts->n, ts->cap);
    limba_ltype id = ts->n++;
    limba_typeinfo *t = &ts->t[id];
    memset(t, 0, sizeof(*t));
    t->kind = (uint8_t)kind;
    t->flags = (uint8_t)flags;
    t->root = t->base = id;
    t->name = UINT32_MAX;
    t->size = size;
    t->align = align;
    return id;
}

void limba_types_init(limba_types *ts)
{
    memset(ts, 0, sizeof(*ts));
    add(ts, LIMBA_LTK_ERROR, 0, 0, 1);
    ts->void_ = add(ts, LIMBA_LTK_VOID, 0, 0, 1);
    ts->uint = add(ts, LIMBA_LTK_UINT, 0, 0, 1);
    ts->ureal = add(ts, LIMBA_LTK_UREAL, 0, 0, 1);
    ts->nil = add(ts, LIMBA_LTK_NIL, 0, PTR_SIZE, PTR_SIZE);
    ts->bool_ = add(ts, LIMBA_LTK_BOOL, 0, 1, 1);
    ts->t[ts->bool_].hi = 1;
    ts->char_ = add(ts, LIMBA_LTK_CHAR, 0, 4, 4);
    ts->t[ts->char_].hi = 0x10ffff;
    ts->string = add(ts, LIMBA_LTK_STRING, 0, PTR_SIZE, PTR_SIZE);
}

void limba_types_free(limba_types *ts)
{
    free(ts->t);
    free(ts->field);
    free(ts->param);
    memset(ts, 0, sizeof(*ts));
}

limba_ltype limba_types_int(limba_types *ts, unsigned bits, unsigned flags)
{
    limba_ltype id = add(ts, LIMBA_LTK_INT, flags, bits / 8, bits / 8);
    limba_typeinfo *t = &ts->t[id];
    t->bits = (uint16_t)bits;
    if (flags & LIMBA_TF_SIGNED) {
        t->hi = ((__int128)1 << (bits - 1)) - 1;
        t->lo = -t->hi - 1;
    } else {
        t->lo = 0;
        t->hi = ((__int128)1 << bits) - 1;
    }
    return id;
}

limba_ltype limba_types_float(limba_types *ts, unsigned bits)
{
    limba_ltype id =
        add(ts, LIMBA_LTK_FLOAT, LIMBA_TF_SIGNED, bits / 8, bits / 8);
    ts->t[id].bits = (uint16_t)bits;
    return id;
}

limba_ltype limba_types_enum(limba_types *ts, uint32_t count)
{
    uint32_t size = count <= 256 ? 1 : count <= 65536 ? 2 : 4;
    limba_ltype id = add(ts, LIMBA_LTK_ENUM, 0, size, size);
    ts->t[id].count = count;
    ts->t[id].lo = 0;
    ts->t[id].hi = (__int128)count - 1;
    return id;
}

limba_ltype limba_types_range(limba_types *ts, limba_ltype base, __int128 lo,
                              __int128 hi)
{
    limba_ltype id = add(ts, 0, 0, 0, 1);
    limba_typeinfo *t = &ts->t[id];
    *t = ts->t[base];
    t->flags |= LIMBA_TF_RANGE;
    t->base = base;
    t->lo = lo;
    t->hi = hi;
    t->name = UINT32_MAX;
    return id;
}

limba_ltype limba_types_distinct(limba_types *ts, limba_ltype base)
{
    /* new T range a..b: the range of a new copy of T, as in Ada */
    if (ts->t[base].flags & LIMBA_TF_RANGE) {
        __int128 lo = ts->t[base].lo, hi = ts->t[base].hi;
        limba_ltype d = limba_types_distinct(ts, ts->t[base].base);
        return limba_types_range(ts, d, lo, hi);
    }
    limba_ltype id = add(ts, 0, 0, 0, 1);
    limba_typeinfo *t = &ts->t[id];
    *t = ts->t[base];
    t->root = t->base = id;
    t->name = UINT32_MAX;
    return id;
}

limba_ltype limba_types_array(limba_types *ts, limba_ltype index,
                              limba_ltype elem, bool dynamic, bool *ok)
{
    *ok = true;
    uint64_t size = 0;
    if (!dynamic) {
        __int128 n = ts->t[index].hi - ts->t[index].lo + 1;
        if (n < 0)
            n = 0;
        if (n > (__int128)UINT64_MAX ||
            __builtin_mul_overflow((uint64_t)n, ts->t[elem].size, &size)) {
            *ok = false;
            size = 0;
        }
    }
    limba_ltype id = add(ts, LIMBA_LTK_ARRAY, dynamic ? LIMBA_TF_DYNAMIC : 0,
                         size, ts->t[elem].align);
    ts->t[id].index = index;
    ts->t[id].elem = elem;
    return id;
}

limba_ltype limba_types_open(limba_types *ts, limba_ltype index,
                             limba_ltype elem)
{
    limba_ltype id = add(ts, LIMBA_LTK_OPEN, 0, 3 * PTR_SIZE, PTR_SIZE);
    ts->t[id].index = index;
    ts->t[id].elem = elem;
    return id;
}

limba_ltype limba_types_pointer(limba_types *ts, limba_ltype target)
{
    limba_ltype id = add(ts, LIMBA_LTK_POINTER, 0, PTR_SIZE, PTR_SIZE);
    ts->t[id].elem = target;
    return id;
}

void limba_types_set_target(limba_types *ts, limba_ltype p, limba_ltype target)
{
    ts->t[p].elem = target;
}

limba_ltype limba_types_routine(limba_types *ts, const limba_param *params,
                                uint32_t n, limba_ltype result)
{
    limba_ltype id = add(ts, LIMBA_LTK_ROUTINE, 0, PTR_SIZE, PTR_SIZE);
    ts->t[id].first = ts->nparam;
    ts->t[id].count = n;
    ts->t[id].elem = result;
    for (uint32_t i = 0; i < n; i++) {
        LIMBA_GROW(ts->param, ts->nparam, ts->capparam);
        ts->param[ts->nparam++] = params[i];
    }
    return id;
}

limba_ltype limba_types_record_begin(limba_types *ts)
{
    limba_ltype id = add(ts, LIMBA_LTK_RECORD, LIMBA_TF_INCOMPLETE, 0, 1);
    ts->t[id].first = ts->nfield;
    return id;
}

void limba_types_record_field(limba_types *ts, limba_ltype r, uint32_t name,
                              limba_ltype type)
{
    LIMBA_GROW(ts->field, ts->nfield, ts->capfield);
    ts->field[ts->nfield++] = (limba_field){name, type, 0};
    ts->t[r].count++;
}

bool limba_types_record_end(limba_types *ts, limba_ltype r)
{
    limba_typeinfo *t = &ts->t[r];
    uint64_t off = 0;
    uint32_t align = 1;
    bool ok = true;
    for (uint32_t i = 0; i < t->count; i++) {
        limba_field *f = &ts->field[t->first + i];
        const limba_typeinfo *ft = &ts->t[f->type];
        uint32_t a = ft->align ? ft->align : 1;
        off = (off + a - 1) / a * a;
        f->offset = off;
        if (__builtin_add_overflow(off, ft->size, &off))
            ok = false;
        if (a > align)
            align = a;
    }
    t->size = ok ? (off + align - 1) / align * align : 0;
    t->align = align;
    t->flags &= (uint8_t)~LIMBA_TF_INCOMPLETE;
    return ok;
}

void limba_types_set_name(limba_types *ts, limba_ltype t, uint32_t name)
{
    if (ts->t[t].name == UINT32_MAX)
        ts->t[t].name = name;
}

bool limba_types_same(const limba_types *ts, limba_ltype a, limba_ltype b)
{
    if (a == b)
        return true;
    const limba_typeinfo *x = &ts->t[a], *y = &ts->t[b];
    if (x->kind != y->kind)
        return false;
    switch (x->kind) {
    case LIMBA_LTK_OPEN:
        return limba_types_same(ts, x->index, y->index) &&
               limba_types_same(ts, x->elem, y->elem);
    case LIMBA_LTK_POINTER:
        return limba_types_same(ts, x->elem, y->elem);
    case LIMBA_LTK_ROUTINE:
        if (x->count != y->count || !limba_types_same(ts, x->elem, y->elem))
            return false;
        for (uint32_t i = 0; i < x->count; i++) {
            const limba_param *p = &ts->param[x->first + i];
            const limba_param *q = &ts->param[y->first + i];
            if (p->mode != q->mode || !limba_types_same(ts, p->type, q->type))
                return false;
        }
        return true;
    }
    return false;
}

bool limba_types_is_discrete(const limba_types *ts, limba_ltype t)
{
    unsigned k = ts->t[t].kind;
    return k == LIMBA_LTK_INT || k == LIMBA_LTK_BOOL || k == LIMBA_LTK_CHAR ||
           k == LIMBA_LTK_ENUM;
}

/* a 128-bit value in decimal */
static void show_int(char *buf, size_t size, __int128 v)
{
    char tmp[48];
    int n = 0;
    unsigned __int128 m = v < 0 ? -(unsigned __int128)v : (unsigned __int128)v;
    do {
        tmp[n++] = (char)('0' + (int)(m % 10));
        m /= 10;
    } while (m);
    size_t len = 0;
    if (v < 0 && len + 1 < size)
        buf[len++] = '-';
    while (n && len + 1 < size)
        buf[len++] = tmp[--n];
    if (size)
        buf[len] = 0;
}

static size_t put(char *buf, size_t size, size_t len, const char *s, size_t n)
{
    for (size_t i = 0; i < n && len + 1 < size; i++)
        buf[len++] = s[i];
    if (size)
        buf[len < size ? len : size - 1] = 0;
    return len;
}

static size_t show(const limba_types *ts, limba_ltype id, limba_name_fn name,
                   const void *ctx, char *buf, size_t size, size_t len,
                   int depth)
{
    const limba_typeinfo *t = &ts->t[id];
    if (t->name != UINT32_MAX && !(t->flags & LIMBA_TF_RANGE && depth == 0)) {
        size_t n;
        const char *s = name(ctx, t->name, &n);
        return put(buf, size, len, s, n);
    }
    if (depth > 8)
        return put(buf, size, len, "...", 3);
    char num[48];
    switch (t->kind) {
    case LIMBA_LTK_ERROR:
        return put(buf, size, len, "?", 1);
    case LIMBA_LTK_VOID:
        return put(buf, size, len, "no value", 8);
    case LIMBA_LTK_UINT:
        return put(buf, size, len, "an integer constant", 19);
    case LIMBA_LTK_UREAL:
        return put(buf, size, len, "a real constant", 15);
    case LIMBA_LTK_NIL:
        return put(buf, size, len, "nil", 3);
    case LIMBA_LTK_ARRAY:
        len = put(buf, size, len, "array[", 6);
        len = show(ts, t->index, name, ctx, buf, size, len, depth + 1);
        len = put(buf, size, len, "] of ", 5);
        return show(ts, t->elem, name, ctx, buf, size, len, depth + 1);
    case LIMBA_LTK_OPEN:
        len = put(buf, size, len, "array[", 6);
        len = show(ts, t->index, name, ctx, buf, size, len, depth + 1);
        len = put(buf, size, len, " range <>] of ", 14);
        return show(ts, t->elem, name, ctx, buf, size, len, depth + 1);
    case LIMBA_LTK_POINTER:
        len = put(buf, size, len, "^", 1);
        return show(ts, t->elem, name, ctx, buf, size, len, depth + 1);
    case LIMBA_LTK_RECORD:
        return put(buf, size, len, "a record", 8);
    case LIMBA_LTK_ENUM:
        return put(buf, size, len, "an enumeration", 14);
    case LIMBA_LTK_ROUTINE:
        return put(buf, size, len, "a routine", 9);
    }
    if (t->flags & LIMBA_TF_RANGE) {
        len = show(ts, t->base, name, ctx, buf, size, len, depth + 1);
        len = put(buf, size, len, " range ", 7);
        show_int(num, sizeof(num), t->lo);
        len = put(buf, size, len, num, strlen(num));
        len = put(buf, size, len, "..", 2);
        show_int(num, sizeof(num), t->hi);
        return put(buf, size, len, num, strlen(num));
    }
    snprintf(num, sizeof(num), "%s%u",
             t->kind == LIMBA_LTK_FLOAT ? "a float of " : "an integer of ",
             t->bits);
    len = put(buf, size, len, num, strlen(num));
    return put(buf, size, len, " bits", 5);
}

const char *limba_types_show(const limba_types *ts, limba_ltype t,
                             limba_name_fn name, const void *ctx, char *buf,
                             size_t size)
{
    if (size)
        buf[0] = 0;
    show(ts, t, name, ctx, buf, size, 0, 0);
    return buf;
}
