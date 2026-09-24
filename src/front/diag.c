/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * diag.c - diagnoses (see diag.h).
 */
#include "diag.h"

#include "common/xalloc.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void limba_report_init(limba_report *r, const limba_source *src, char letter,
                       uint32_t max_errors)
{
    memset(r, 0, sizeof(*r));
    r->src = src;
    r->letter = letter;
    r->max_errors = max_errors;
}

void limba_report_free(limba_report *r)
{
    for (uint32_t i = 0; i < r->count; i++)
        free(r->item[i].msg);
    free(r->item);
    r->item = NULL;
    r->count = r->cap = 0;
}

bool limba_report_add(limba_report *r, limba_severity sev, unsigned code,
                      limba_loc loc, uint32_t len, const char *fmt, ...)
{
    if (r->full)
        return false;
    if (sev == LIMBA_ERROR && r->max_errors && r->errors == r->max_errors) {
        r->full = true;
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *msg = limba_xmalloc(n > 0 ? (size_t)n + 1 : 1);
    msg[0] = 0;
    if (n > 0) {
        va_start(ap, fmt);
        vsnprintf(msg, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    LIMBA_GROW(r->item, r->count, r->cap);
    r->item[r->count++] =
        (limba_report_item){(uint8_t)sev, (uint16_t)code, loc, len, msg};
    if (sev == LIMBA_ERROR)
        r->errors++;
    return true;
}

static const char *const sev_name[] = {"error", "warning", "note"};

/* the source line under the message, and a mark under the columns */
static void show_line(const limba_report *r, const limba_where *w, uint32_t len,
                      FILE *out)
{
    uint32_t n;
    const char *line = limba_source_line(r->src, w->file, w->line, &n);
    char num[16];
    int width = snprintf(num, sizeof(num), "%u", w->line);
    fprintf(out, " %s | ", num);
    for (uint32_t i = 0; i < n; i++)
        fputc(line[i] == '\t' ? ' ' : line[i], out);
    fprintf(out, "\n %*s | ", width, "");
    for (uint32_t c = 1; c < w->col; c++)
        fputc(' ', out);
    fputc('^', out);
    /* one ~ for every further character of the span, on this line only */
    uint32_t start = w->off - (uint32_t)(line - r->src->file[w->file].text);
    uint32_t end = start + len < n ? start + len : n;
    for (uint32_t i = start + 1; i < end; i++)
        if (((unsigned char)line[i] & 0xc0) != 0x80)
            fputc('~', out);
    fputc('\n', out);
}

void limba_report_print(const limba_report *r, FILE *out)
{
    for (uint32_t i = 0; i < r->count; i++) {
        const limba_report_item *it = &r->item[i];
        limba_where w;
        bool at = r->src && limba_source_where(r->src, it->loc, &w);
        if (at)
            fprintf(out, "%s:%u:%u: ", r->src->file[w.file].path, w.line,
                    w.col);
        else
            fputs("limba: ", out);
        fputs(sev_name[it->sev], out);
        if (it->code)
            fprintf(out, "[%c%04u]", r->letter, it->code);
        fprintf(out, ": %s\n", it->msg);
        if (at)
            show_line(r, &w, it->len, out);
    }
    if (r->full)
        fprintf(out, "limba: stopped after %u errors\n", r->errors);
}
