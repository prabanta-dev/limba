/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * report.c - how a position and an exception no handler caught are
 * worded (doc/ir.md § 13.7), one text for every engine: lir_run, Meri,
 * the tests of the front ends.
 */
#include "limba/ir.h"

#include <inttypes.h>
#include <string.h>

size_t limba_pos_print(const limba_module *m, uint32_t k, char *buf, size_t cap)
{
    if (cap)
        buf[0] = 0;
    if (!k || k > m->npos)
        return 0;
    const limba_pos *p = &m->pos[k - 1];
    size_t n = 0;
    const char *file = limba_str(m, p->file, &n);
    int r = snprintf(buf, cap, "%.*s:%" PRIu32 ":%" PRIu32, (int)n, file,
                     p->line, p->col);
    return r < 0 ? 0 : (size_t)r;
}

void limba_report_uncaught(const limba_module *m, int64_t id, const char *msg,
                           size_t msglen, uint32_t pos, FILE *out)
{
    size_t pn = 0, nn = 0, tn = msglen;
    const char *prefix = m->language != LIMBA_NONE
                             ? limba_str(m, m->language, &pn)
                             : limba_program();
    if (m->language == LIMBA_NONE)
        pn = strlen(prefix);
    const char *name = limba_exception_name(m, id, &nn);
    const char *text = msg;
    /* a trap, or raised with no words: those of the module or traps.def */
    if (!text || (!tn && !name)) {
        tn = 0;
        text = limba_trap_message(m, id, &tn);
    }
    fprintf(out, "%.*s: ", (int)pn, prefix);
    if (name) {
        fprintf(out, "%.*s", (int)nn, name);
        if (text && tn)
            fprintf(out, ": %.*s", (int)tn, text);
    } else if (text && tn) {
        fprintf(out, "%.*s", (int)tn, text);
    } else {
        fprintf(out, "exception %" PRId64, id);
    }
    if (pos && pos <= m->npos) {
        const limba_pos *p = &m->pos[pos - 1];
        size_t fn = 0;
        const char *file = limba_str(m, p->file, &fn);
        fprintf(out, " at %.*s:%" PRIu32 ":%" PRIu32, (int)fn, file, p->line,
                p->col);
    }
    fputc('\n', out);
}
