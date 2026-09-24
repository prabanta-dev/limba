/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * print.c - the text form (.lit). The output is canonical: values are
 * numbered v0, v1, ... in the order they appear, blocks b0, b1, ... in
 * their order, so printing what parse() read from this output gives the
 * same text again. Floating constants are written in hexadecimal (%a), so
 * the value survives exactly. Assumes a module that limba_verify accepts.
 */
#include "internal.h"

#include "common/xalloc.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void print_bytes(FILE *out, const char *s, size_t n)
{
    fputc('"', out);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (c < 0x20 || c == 0x7f)
                fprintf(out, "\\x%02x", c);
            else
                fputc(c, out);
        }
    }
    fputc('"', out);
}

/* @name, quoted when it is not a plain identifier */
static void print_sym(FILE *out, char sigil, const limba_module *m,
                      limba_id name)
{
    size_t n;
    const char *s = limba_str(m, name, &n);
    bool plain = n > 0 && !isdigit((unsigned char)s[0]);
    for (size_t i = 0; i < n && plain; i++) {
        unsigned char c = (unsigned char)s[i];
        plain = isalnum(c) || c == '_' || c == '.' || c == '$';
    }
    fputc(sigil, out);
    if (plain)
        fwrite(s, 1, n, out);
    else
        print_bytes(out, s, n);
}

static void print_type(FILE *out, const limba_module *m, limba_id t)
{
    const limba_type *ty = &m->types[t];
    if (t < LIMBA_T_FIRST_USER) {
        fputs(limba_scalar_name(t), out);
        return;
    }
    switch (ty->kind) {
    case LIMBA_TK_STRUCT:
        print_sym(out, '%', m, ty->name);
        return;
    case LIMBA_TK_ARRAY:
        fprintf(out, "[%" PRIu32 " x ", ty->count);
        print_type(out, m, ty->elem);
        fputc(']', out);
        return;
    case LIMBA_TK_FUNC:
        fputs("fn(", out);
        for (uint32_t i = 0; i < ty->count; i++) {
            if (i)
                fputs(", ", out);
            print_type(out, m, m->members[ty->first + i].type);
        }
        if (ty->variadic)
            fputs(ty->count ? ", ..." : "...", out);
        fputs(") -> ", out);
        print_type(out, m, ty->elem);
        return;
    }
}

static void print_double(FILE *out, int64_t bits)
{
    double x;
    memcpy(&x, &bits, sizeof(x));
    if (isnan(x)) /* the payload and the sign must survive */
        fprintf(out, "nan.0x%016" PRIx64, (uint64_t)bits);
    else
        fprintf(out, "%a", x);
}

typedef struct {
    FILE *out;
    const limba_module *m;
    const limba_func *f;
    uint32_t *num; /* canonical number of every value */
} pctx;

static void val(pctx *p, uint32_t x)
{
    fprintf(p->out, "v%" PRIu32, p->num[x]);
}

static void args(pctx *p, uint32_t k, uint32_t n)
{
    fputc('(', p->out);
    for (uint32_t i = 0; i < n; i++) {
        if (i)
            fputs(", ", p->out);
        val(p, p->f->operands[k + i]);
    }
    fputc(')', p->out);
}

/* a branch target at operand k: b3 or b3(v1, v2); returns the next k */
static uint32_t target(pctx *p, uint32_t k)
{
    limba_id b;
    uint32_t n;
    uint32_t a = limba_target(p->f, k, &b, &n);
    fprintf(p->out, "b%" PRIu32, b);
    if (n)
        args(p, a, n);
    return a + n;
}

static void print_inst(pctx *p, uint32_t id)
{
    FILE *out = p->out;
    const limba_module *m = p->m;
    const limba_func *f = p->f;
    const limba_inst *in = &f->insts[id];
    const limba_op_info *op = &limba_ops[in->op];
    const uint32_t *o = f->operands + in->first;

    fputs("  ", out);
    if (in->type != LIMBA_T_VOID) {
        val(p, id);
        fputs(" = ", out);
    }
    fputs(op->text, out);
    if (op->format == LIMBA_F_CMP)
        fprintf(out, ".%s", limba_cc_text[in->cc]);
    /* the type: always for a value, and for calls also when void */
    if (in->type != LIMBA_T_VOID || (op->flags & LIMBA_OPF_CALL)) {
        fputc(' ', out);
        print_type(out, m, in->type);
    }
    switch (op->format) {
    case LIMBA_F_ICONST:
        fprintf(out, " %" PRId64, in->imm);
        break;
    case LIMBA_F_FCONST:
        fputc(' ', out);
        print_double(out, in->imm);
        break;
    case LIMBA_F_SCONST: {
        size_t n;
        const char *s = limba_str(m, (limba_id)in->imm, &n);
        fputc(' ', out);
        print_bytes(out, s, n);
        break;
    }
    case LIMBA_F_SLOT:
        fprintf(out, " $%" PRId64, in->imm);
        break;
    case LIMBA_F_GADDR:
        fputc(' ', out);
        print_sym(out, '@', m, m->globals[in->imm].name);
        break;
    case LIMBA_F_FADDR:
        fputc(' ', out);
        print_sym(out, '@', m, m->funcs[in->imm].name);
        break;
    case LIMBA_F_ADDR:
        fputc(' ', out);
        val(p, o[0]);
        fputs(", ", out);
        val(p, o[1]);
        fprintf(out, ", %" PRId64 ", %" PRId64, in->imm, in->imm2);
        break;
    case LIMBA_F_CALL:
        fputc(' ', out);
        print_sym(out, '@', m, m->funcs[in->imm].name);
        args(p, in->first, in->nops);
        break;
    case LIMBA_F_CALL_EXT:
        fputc(' ', out);
        print_sym(out, '@', m, m->externs[in->imm].name);
        args(p, in->first, in->nops);
        break;
    case LIMBA_F_CALL_RT:
        fprintf(out, " %s", limba_rts[in->imm].name);
        args(p, in->first, in->nops);
        break;
    case LIMBA_F_CALL_IND:
        fputc(' ', out);
        print_type(out, m, (limba_id)in->imm);
        fputc(' ', out);
        val(p, o[0]);
        args(p, in->first + 1, in->nops - 1);
        break;
    case LIMBA_F_BR:
        fputc(' ', out);
        target(p, in->first);
        break;
    case LIMBA_F_CBR: {
        fputc(' ', out);
        val(p, o[0]);
        fputs(", ", out);
        uint32_t k = target(p, in->first + 1);
        fputs(", ", out);
        target(p, k);
        break;
    }
    case LIMBA_F_SWITCH: {
        fputc(' ', out);
        val(p, o[0]);
        fprintf(out, ", b%" PRIu32 " [", o[1]);
        for (uint32_t c = 0; c < o[2]; c++) {
            const uint32_t *cs = o + 3 + 3 * c;
            int64_t v = (int64_t)((uint64_t)cs[1] << 32 | cs[0]);
            fprintf(out, "%s%" PRId64 ": b%" PRIu32, c ? ", " : "", v, cs[2]);
        }
        fputc(']', out);
        break;
    }
    case LIMBA_F_TRAP:
        fprintf(out, " %" PRId64, in->imm);
        break;
    case LIMBA_F_CHECK:
        fputc(' ', out);
        val(p, o[0]);
        fprintf(out, ", %" PRId64, in->imm);
        break;
    default: /* values only: un, bin, tern, cmp, conv, load, store, mem3,
                ret */
        for (uint32_t i = 0; i < in->nops; i++) {
            fputs(i ? ", " : " ", out);
            val(p, o[i]);
        }
        break;
    }
    if (limba_inst_pos(f, id))
        fprintf(out, " !%" PRIu32, limba_inst_pos(f, id));
    fputc('\n', out);
}

static void print_func(FILE *out, const limba_module *m, const limba_func *f)
{
    pctx p = {out, m, f, limba_xmalloc(((size_t)f->ninsts + 1) * 4)};
    uint32_t next = 0;
    for (uint32_t b = 0; b < f->nblocks; b++)
        for (uint32_t k = 0; k < f->blocks[b].ninsts; k++) {
            uint32_t id = f->blocks[b].insts[k];
            if (f->insts[id].type != LIMBA_T_VOID)
                p.num[id] = next++;
        }

    fputs("\nfunc ", out);
    print_sym(out, '@', m, f->name);
    fputs(" : ", out);
    print_type(out, m, f->type);
    if (f->flags & LIMBA_SYM_EXPORT)
        fputs(" export", out);
    fputs(" {\n", out);
    for (uint32_t s = 0; s < f->nslots; s++)
        fprintf(out, "  slot %" PRIu32 " align %" PRIu32 "\n", f->slots[s].size,
                f->slots[s].align);
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const limba_block *bl = &f->blocks[b];
        fprintf(out, "b%" PRIu32, b);
        if (bl->nparams) {
            fputc('(', out);
            for (uint32_t k = 0; k < bl->nparams; k++) {
                uint32_t id = bl->insts[k];
                fputs(k ? ", " : "", out);
                val(&p, id);
                fputs(": ", out);
                print_type(out, m, f->insts[id].type);
            }
            fputc(')', out);
        }
        fputs(":\n", out);
        for (uint32_t k = bl->nparams; k < bl->ninsts; k++)
            print_inst(&p, bl->insts[k]);
    }
    fputs("}\n", out);
    free(p.num);
}

void limba_print(const limba_module *m, FILE *out)
{
    fprintf(out, "; Limba IR, version %d\n", LIMBA_IR_VERSION);
    if (m->name != LIMBA_NONE) {
        size_t n;
        const char *s = limba_str(m, m->name, &n);
        fputs("module ", out);
        print_bytes(out, s, n);
        fputc('\n', out);
    }
    fprintf(out, "memory %s\n", m->memory == LIMBA_MEM_FB ? "fb" : "strict");

    for (limba_id t = LIMBA_T_FIRST_USER; t < m->ntypes; t++) {
        const limba_type *ty = &m->types[t];
        if (ty->kind != LIMBA_TK_STRUCT)
            continue;
        fputs("type ", out);
        print_sym(out, '%', m, ty->name);
        fputs(" = {", out);
        for (uint32_t i = 0; i < ty->count; i++) {
            const limba_member *f = &m->members[ty->first + i];
            fputs(i ? ", " : " ", out);
            print_type(out, m, f->type);
            fprintf(out, " @%" PRIu32, f->offset);
        }
        fprintf(out, " } size %" PRIu32 " align %" PRIu32 "\n", ty->size,
                ty->align);
    }

    for (uint32_t i = 0; i < m->nglobals; i++) {
        const limba_global *g = &m->globals[i];
        fputs("global ", out);
        print_sym(out, '@', m, g->name);
        fputs(" : ", out);
        print_type(out, m, g->type);
        switch (g->init) {
        case LIMBA_INIT_INT:
            fprintf(out, " = %" PRId64, g->value);
            break;
        case LIMBA_INIT_FLOAT:
            fputs(" = ", out);
            print_double(out, g->value);
            break;
        case LIMBA_INIT_STR: {
            size_t n;
            const char *s = limba_str(m, (limba_id)g->value, &n);
            fputs(" = ", out);
            print_bytes(out, s, n);
            break;
        }
        }
        if (g->flags & LIMBA_SYM_EXPORT)
            fputs(" export", out);
        if (g->flags & LIMBA_SYM_CONST)
            fputs(" const", out);
        fputc('\n', out);
    }

    for (uint32_t i = 0; i < m->nexterns; i++) {
        const limba_extern *e = &m->externs[i];
        size_t n;
        const char *s;
        fputs("extern ", out);
        print_sym(out, '@', m, e->name);
        fputs(" : ", out);
        print_type(out, m, e->type);
        fputs(" = ", out);
        s = limba_str(m, e->symbol, &n);
        print_bytes(out, s, n);
        if (e->library != LIMBA_NONE) {
            fputs(" in ", out);
            s = limba_str(m, e->library, &n);
            print_bytes(out, s, n);
        }
        fputc('\n', out);
    }

    /* the positions in the source, which instructions refer to as !k */
    for (uint32_t k = 0; k < m->npos; k++) {
        size_t n;
        const char *s = limba_str(m, m->pos[k].file, &n);
        fprintf(out, "pos %" PRIu32 " ", k + 1);
        print_bytes(out, s, n);
        fprintf(out, " %" PRIu32 " %" PRIu32 "\n", m->pos[k].line,
                m->pos[k].col);
    }

    for (uint32_t i = 0; i < m->nfuncs; i++)
        print_func(out, m, &m->funcs[i]);
}
