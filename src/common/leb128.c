/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * leb128.c - variable-length integers (see leb128.h).
 */
#include "leb128.h"

#include "xalloc.h"

#include <string.h>

void limba_w_bytes(limba_wbuf *w, const void *p, size_t n)
{
    while (w->len + n > w->cap) {
        w->cap = w->cap ? 2 * w->cap : 4096;
        w->buf = limba_xrealloc(w->buf, w->cap, 1);
    }
    if (n)
        memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void limba_w_byte(limba_wbuf *w, uint8_t b)
{
    limba_w_bytes(w, &b, 1);
}

void limba_w_uleb(limba_wbuf *w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        limba_w_byte(w, (uint8_t)(b | (v ? 0x80 : 0)));
    } while (v);
}

void limba_w_sleb(limba_wbuf *w, int64_t v)
{
    for (;;) {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7; /* arithmetic shift: gcc and clang define it */
        bool done = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
        limba_w_byte(w, (uint8_t)(b | (done ? 0 : 0x80)));
        if (done)
            return;
    }
}

void limba_w_u64(limba_wbuf *w, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    limba_w_bytes(w, b, 8);
}

uint8_t limba_r_byte(limba_rbuf *r)
{
    if (r->bad || r->p >= r->end) {
        r->bad = true;
        return 0;
    }
    return *r->p++;
}

uint64_t limba_r_uleb(limba_rbuf *r)
{
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        uint8_t b = limba_r_byte(r);
        if (r->bad)
            return 0;
        if (shift == 63 && (b & 0x7e)) {
            r->bad = true; /* more than 64 bits */
            return 0;
        }
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
    }
    r->bad = true;
    return 0;
}

int64_t limba_r_sleb(limba_rbuf *r)
{
    uint64_t v = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
        if (shift >= 64) {
            r->bad = true;
            return 0;
        }
        b = limba_r_byte(r);
        if (r->bad)
            return 0;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= ~(uint64_t)0 << shift;
    return (int64_t)v;
}

uint64_t limba_r_u64(limba_rbuf *r)
{
    const uint8_t *p = limba_r_bytes(r, 8);
    uint64_t v = 0;
    if (!p)
        return 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

uint32_t limba_r_count(limba_rbuf *r, size_t min_bytes)
{
    uint64_t n = limba_r_uleb(r);
    size_t need;
    if (r->bad || n > UINT32_MAX ||
        __builtin_mul_overflow((size_t)n, min_bytes, &need) ||
        need > (size_t)(r->end - r->p)) {
        r->bad = true;
        return 0;
    }
    return (uint32_t)n;
}

const uint8_t *limba_r_bytes(limba_rbuf *r, size_t n)
{
    if (r->bad || n > (size_t)(r->end - r->p)) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *p = r->p;
    r->p += n;
    return p;
}
