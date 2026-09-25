/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * leb128.h - variable-length integers of the binary form: a byte buffer
 * that grows for writing, a bounded cursor for reading that never trusts
 * its input.
 */
#ifndef LIMBA_LEB128_H
#define LIMBA_LEB128_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} limba_wbuf;

void limba_w_bytes(limba_wbuf *w, const void *p, size_t n);
void limba_w_u64(limba_wbuf *w, uint64_t v); /* 8 bytes, little endian */
/* room for n more bytes */
void limba_w_reserve(limba_wbuf *w, size_t n);

/* the small writes are inline: the binary form is made of them */
static inline void limba_w_byte(limba_wbuf *w, uint8_t b)
{
    if (w->len == w->cap)
        limba_w_reserve(w, 1);
    w->buf[w->len++] = b;
}

/* v at p, where there is room for 10 bytes; past it */
static inline uint8_t *limba_put_uleb(uint8_t *p, uint64_t v)
{
    while (v >= 0x80) {
        *p++ = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

static inline uint8_t *limba_put_sleb(uint8_t *p, int64_t v)
{
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7f);
        v >>= 7; /* arithmetic shift: gcc and clang define it */
        if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40))) {
            *p++ = x;
            return p;
        }
        *p++ = (uint8_t)(x | 0x80);
    }
}

static inline void limba_w_uleb(limba_wbuf *w, uint64_t v)
{
    if (w->cap - w->len < 10)
        limba_w_reserve(w, 10);
    w->len = (size_t)(limba_put_uleb(w->buf + w->len, v) - w->buf);
}

static inline void limba_w_sleb(limba_wbuf *w, int64_t v)
{
    if (w->cap - w->len < 10)
        limba_w_reserve(w, 10);
    w->len = (size_t)(limba_put_sleb(w->buf + w->len, v) - w->buf);
}

typedef struct {
    const uint8_t *p, *end;
    bool bad; /* set on the first read past the end or malformed value */
} limba_rbuf;

uint8_t limba_r_byte(limba_rbuf *r);
uint64_t limba_r_uleb(limba_rbuf *r);
int64_t limba_r_sleb(limba_rbuf *r);
uint64_t limba_r_u64(limba_rbuf *r);
/* a count that must fit 32 bits and, times min_bytes each, the input */
uint32_t limba_r_count(limba_rbuf *r, size_t min_bytes);
/* n bytes in place, NULL (and bad) if they are not there */
const uint8_t *limba_r_bytes(limba_rbuf *r, size_t n);

#endif
