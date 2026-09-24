/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ast.h - the syntax tree of a Luxia file: nodes of one size in an array,
 * children by index, lists in a pool. Node 0 is "none". The tree is
 * faithful to the source: nothing is rewritten while parsing.
 */
#ifndef LIMBA_LUXIA_AST_H
#define LIMBA_LUXIA_AST_H

#include "front/source.h"
#include "lex.h"

#include <stdint.h>
#include <stdio.h>

enum {
#define LX_NODE(name, text, fields) LXN_##name,
#include "nodes.def"
#undef LX_NODE
    LXN_NKINDS
};

#define LXN_F_BIG 1u /* INT: past 64 bits, read the text again */

typedef struct {
    uint8_t kind;
    uint8_t op; /* a token kind, see nodes.def */
    uint16_t flags;
    limba_loc loc;
    uint32_t a, b, c, d;
} limba_lx_node;

typedef struct {
    limba_lx_node *node;
    uint32_t nnode, capnode;
    uint32_t *pool; /* the members of the lists */
    uint32_t npool, cappool;
    uint32_t *stack; /* members of the lists being built */
    uint32_t nstack, capstack;
    uint32_t root;
} limba_lx_ast;

void limba_lx_ast_init(limba_lx_ast *t);
void limba_lx_ast_free(limba_lx_ast *t);

uint32_t limba_lx_node_new(limba_lx_ast *t, unsigned kind, limba_loc loc,
                           uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* a list is built on a stack: mark = limba_lx_list_begin, push the
   members, then limba_lx_list_end makes the LIST node */
uint32_t limba_lx_list_begin(const limba_lx_ast *t);
void limba_lx_list_push(limba_lx_ast *t, uint32_t node);
uint32_t limba_lx_list_end(limba_lx_ast *t, uint32_t mark, limba_loc loc);

/* the i-th member of a LIST node */
static inline uint32_t limba_lx_list_at(const limba_lx_ast *t, uint32_t list,
                                        uint32_t i)
{
    return t->pool[t->node[list].a + i];
}

const char *limba_lx_node_text(unsigned kind);

/* a subtree: on one line if indent < 0, else one node per line */
void limba_lx_ast_show(FILE *out, const limba_lx_ast *t, const limba_lx *lx,
                       uint32_t node, int indent);

#endif
