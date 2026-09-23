/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ops.c - the table of operations (ir_ops.def) and of conditions.
 */
#include "internal.h"

const limba_op_info limba_ops[LIMBA_OP_COUNT] = {
#define LIMBA_OP(name, text, format, flags) {text, format, flags},
#include "limba/ir_ops.def"
#undef LIMBA_OP
};

const char *const limba_cc_text[LIMBA_CC_COUNT] = {
    "eq",  "ne",  "slt", "sle", "sgt", "sge", "ult", "ule",
    "ugt", "uge", "oeq", "one", "olt", "ole", "ogt", "oge",
    "ord", "uno", "ueq", "une", "ult", "ule", "ugt", "uge",
};

bool limba_cc_is_float(unsigned cc)
{
    return cc >= LIMBA_CC_OEQ && cc < LIMBA_CC_COUNT;
}
