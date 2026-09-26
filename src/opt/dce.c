/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * dce.c - the "dce" pass: an instruction whose value nobody uses, and that
 * has no other effect, goes; then its operands may be unused in turn. What
 * may trap stays (a division by zero must still raise its error), what
 * reads memory stays (the read may fault), what writes or talks to the
 * outside stays. A call to the run-time library goes only when its table
 * entry says it has no effect but its result.
 */
#include "pass.h"

#include "common/xalloc.h"

#include <stdlib.h>

static bool removable(const limba_inst *in)
{
    const limba_op_info *op = &limba_ops[in->op];
    if (in->op == LIMBA_OP_PARAM || in->type == LIMBA_T_VOID)
        return false;
    if (op->flags & LIMBA_OPF_PURE)
        return true;
    if (in->op == LIMBA_OP_CALLRT) {
        uint32_t a = limba_rts[in->imm].attrs;
        return !(a & (LIMBA_RTA_IO | LIMBA_RTA_WRITES_MEM | LIMBA_RTA_MAY_TRAP |
                      LIMBA_RTA_NORETURN));
    }
    return false;
}

uint32_t limba_pass_dce(limba_pass_ctx *x, limba_func *f)
{
    /* no branch changes: the CFG holds */
    limba_edit *e = &x->e;
    uint32_t n = f->ninsts, changes = 0;
    uint32_t *uses = limba_xcalloc((size_t)n + 1, sizeof(*uses));
    uint32_t *work = limba_xmalloc(((size_t)n + 1) * sizeof(*work));
    uint8_t *kinds = NULL;
    uint32_t capkinds = 0, nwork = 0;

    /* the uses by the instructions still there, of the values they stand
       for now */
    for (uint32_t i = 0; i < n; i++) {
        if (e->dead[i])
            continue;
        const limba_inst *in = &f->insts[i];
        if (in->nops > capkinds) {
            capkinds = in->nops;
            kinds = limba_xrealloc(kinds, capkinds, 1);
        }
        limba_operand_kinds(f, in, kinds);
        for (uint32_t k = 0; k < in->nops; k++)
            if (kinds[k] == LIMBA_OK_VALUE)
                uses[limba_edit_resolve(e, f->operands[in->first + k])]++;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!e->dead[i] && !uses[i] && removable(&f->insts[i]))
            work[nwork++] = i;
    while (nwork) {
        uint32_t id = work[--nwork];
        if (e->dead[id])
            continue;
        e->dead[id] = 1;
        changes++;
        const limba_inst *in = &f->insts[id];
        if (in->nops > capkinds) {
            capkinds = in->nops;
            kinds = limba_xrealloc(kinds, capkinds, 1);
        }
        limba_operand_kinds(f, in, kinds);
        for (uint32_t k = 0; k < in->nops; k++) {
            if (kinds[k] != LIMBA_OK_VALUE)
                continue;
            uint32_t o = limba_edit_resolve(e, f->operands[in->first + k]);
            if (--uses[o] == 0 && removable(&f->insts[o]))
                work[nwork++] = o;
        }
    }

    free(uses);
    free(work);
    free(kinds);
    return changes;
}
