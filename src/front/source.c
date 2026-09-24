/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * source.c - source files and positions (see source.h).
 */
#include "source.h"

#include "common/xalloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void limba_source_init(limba_source *s)
{
    memset(s, 0, sizeof(*s));
    s->next = 1;
}

void limba_source_free(limba_source *s)
{
    for (uint32_t i = 0; i < s->count; i++) {
        free(s->file[i].path);
        free(s->file[i].text);
        free(s->file[i].line);
    }
    free(s->file);
    limba_source_init(s);
}

/* take text (len bytes and a NUL, allocated) as a new file */
static uint32_t adopt(limba_source *s, const char *path, char *text, size_t len)
{
    /* the file and the position just past its end must fit */
    if (len >= UINT32_MAX - s->next) {
        free(text);
        errno = EFBIG;
        return UINT32_MAX;
    }
    LIMBA_GROW(s->file, s->count, s->cap);
    limba_srcfile *f = &s->file[s->count];
    size_t plen = strlen(path);
    f->path = limba_xmalloc(plen + 1);
    memcpy(f->path, path, plen + 1);
    f->text = text;
    f->len = (uint32_t)len;
    f->base = s->next;
    s->next += (uint32_t)len + 1;

    uint32_t cap = 0;
    f->line = NULL;
    f->nlines = 0;
    LIMBA_GROW(f->line, f->nlines, cap);
    f->line[f->nlines++] = 0;
    const char *p = text, *end = text + len;
    while ((p = memchr(p, '\n', (size_t)(end - p))) != NULL) {
        p++;
        LIMBA_GROW(f->line, f->nlines, cap);
        f->line[f->nlines++] = (uint32_t)(p - text);
    }
    return s->count++;
}

uint32_t limba_source_add(limba_source *s, const char *path, const char *text,
                          size_t len)
{
    if (len >= UINT32_MAX) {
        errno = EFBIG;
        return UINT32_MAX;
    }
    char *copy = limba_xmalloc(len + 1);
    if (len)
        memcpy(copy, text, len);
    copy[len] = 0;
    return adopt(s, path, copy, len);
}

uint32_t limba_source_load(limba_source *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return UINT32_MAX;
    char *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            if (cap >= UINT32_MAX / 2) {
                free(buf);
                fclose(f);
                errno = EFBIG;
                return UINT32_MAX;
            }
            cap = cap ? 2 * cap : 65536;
            buf = limba_xrealloc(buf, cap + 1, 1);
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    int err = ferror(f) ? errno : 0;
    fclose(f);
    if (err) {
        free(buf);
        errno = err;
        return UINT32_MAX;
    }
    if (!buf)
        buf = limba_xmalloc(1);
    buf[n] = 0;
    return adopt(s, path, buf, n);
}

bool limba_source_where(const limba_source *s, limba_loc loc, limba_where *w)
{
    if (loc == LIMBA_NOLOC || s->count == 0)
        return false;
    /* the last file whose base is <= loc */
    uint32_t lo = 0, hi = s->count;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->file[mid].base <= loc)
            lo = mid;
        else
            hi = mid;
    }
    const limba_srcfile *f = &s->file[lo];
    if (loc < f->base || loc - f->base > f->len)
        return false;
    uint32_t off = loc - f->base;
    /* the last line that starts at or before off */
    uint32_t a = 0, b = f->nlines;
    while (b - a > 1) {
        uint32_t mid = a + (b - a) / 2;
        if (f->line[mid] <= off)
            a = mid;
        else
            b = mid;
    }
    uint32_t col = 1;
    for (uint32_t i = f->line[a]; i < off; i++)
        if (((unsigned char)f->text[i] & 0xc0) != 0x80)
            col++;
    w->file = lo;
    w->off = off;
    w->line = a + 1;
    w->col = col;
    return true;
}

const char *limba_source_line(const limba_source *s, uint32_t file,
                              uint32_t line, uint32_t *len)
{
    const limba_srcfile *f = &s->file[file];
    if (line == 0 || line > f->nlines) {
        *len = 0;
        return "";
    }
    uint32_t start = f->line[line - 1];
    uint32_t end = line < f->nlines ? f->line[line] - 1 : f->len;
    if (end > start && f->text[end - 1] == '\r')
        end--;
    *len = end - start;
    return f->text + start;
}

size_t limba_utf8_check(const char *text, size_t len)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t i = 0;
    while (i < len) {
        unsigned c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        unsigned n;
        uint32_t cp, min;
        if (c >= 0xc2 && c <= 0xdf) {
            n = 2, cp = c & 0x1f, min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 3, cp = c & 0x0f, min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 4, cp = c & 0x07, min = 0x10000;
        } else {
            return i;
        }
        if (len - i < n)
            return i;
        for (unsigned k = 1; k < n; k++) {
            if ((p[i + k] & 0xc0) != 0x80)
                return i;
            cp = cp << 6 | (p[i + k] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return i;
        i += n;
    }
    return len;
}

uint32_t limba_utf8_decode(const char *text, unsigned *n)
{
    const unsigned char *p = (const unsigned char *)text;
    unsigned c = p[0];
    if (c < 0x80) {
        *n = 1;
        return c;
    }
    unsigned k = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
    uint32_t cp = c & (0x7f >> k);
    for (unsigned i = 1; i < k; i++)
        cp = cp << 6 | (p[i] & 0x3f);
    *n = k;
    return cp;
}
