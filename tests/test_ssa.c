/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_ssa.c - the SSA builder of the front ends (src/ir/ssa.c) on the
 * handlers: a handler has for predecessors those of the blocks it
 * protects (progetto_ir.md § 11i), so it reads the values a variable has
 * where they begin, and the check of definite assignment sees the same.
 * The mistakes of the front end stop the program: each is run in a child.
 */
#include "ir/ssa.h"
#include "limba/ir.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "test_ssa: line %d: ", __LINE__);                  \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* the tags reported as read without a definition */
static uint32_t told[8], ntold;

static void undefined(void *ctx, uint32_t tag)
{
    (void)ctx;
    if (ntold < 8)
        told[ntold++] = tag;
}

static bool was_told(uint32_t tag)
{
    for (uint32_t i = 0; i < ntold; i++)
        if (told[i] == tag)
            return true;
    return false;
}

typedef struct {
    limba_module *m;
    limba_ssa *s;
    uint32_t x, y; /* two variables of type i64 */
} fn;

/* @f(i64 c, i1 k) -> i64, its entry block 0 */
static fn begin(void)
{
    fn F;
    F.m = limba_module_new();
    limba_id p[2] = {LIMBA_T_I64, LIMBA_T_I1};
    limba_id t = limba_type_func(F.m, LIMBA_T_I64, p, 2, false);
    limba_id fid = limba_func_add(F.m, limba_str_intern(F.m, "f", 1), t, 0);
    limba_func *f = &F.m->funcs[fid];
    limba_block_add(f);
    limba_param_add(f, 0, LIMBA_T_I64);
    limba_param_add(f, 0, LIMBA_T_I1);
    F.s = limba_ssa_new(F.m, fid);
    F.x = limba_ssa_var(F.s, LIMBA_T_I64);
    F.y = limba_ssa_var(F.s, LIMBA_T_I64);
    ntold = 0;
    return F;
}

static limba_id iconst(fn *F, limba_id b, int64_t v)
{
    return limba_inst_add(limba_ssa_func(F->s), b, LIMBA_OP_ICONST, LIMBA_T_I64,
                          0, v, 0, NULL, 0);
}

/* a block protected by h */
static limba_id protected_block(fn *F, limba_id h)
{
    limba_id b = limba_ssa_block(F->s);
    limba_block_set_handler(limba_ssa_func(F->s), b, h);
    return b;
}

/* a handler, with the three parameters of the occurrence */
static limba_id handler(fn *F)
{
    limba_id h = limba_ssa_block(F->s);
    limba_func *f = limba_ssa_func(F->s);
    limba_param_add(f, h, LIMBA_T_I32);
    limba_param_add(f, h, LIMBA_T_I32);
    limba_param_add(f, h, LIMBA_T_STR);
    return h;
}

/* an instruction that may fail: c / c */
static void may_trap(fn *F, limba_id b)
{
    limba_func *f = limba_ssa_func(F->s);
    uint32_t ops[2] = {0, 0}; /* the parameter c is value 0 */
    limba_inst_add(f, b, LIMBA_OP_SDIV, LIMBA_T_I64, 0, 0, 0, ops, 2);
}

static void finish(fn *F)
{
    limba_ssa_finish(F->s, undefined, NULL);
    limba_ssa_free(F->s);
    limba_diag d;
    if (limba_verify(F->m, &d)) {
        CHECK(false, "the function does not verify: %s", d.msg);
        limba_print(F->m, stderr);
    }
}

/* the instruction a ret of block b returns, NULL if none */
static const limba_inst *returned(const fn *F, limba_id b)
{
    const limba_func *f = &F->m->funcs[0];
    if (b >= f->nblocks || !f->blocks[b].ninsts)
        return NULL;
    const limba_block *bl = &f->blocks[b];
    const limba_inst *r = &f->insts[bl->insts[bl->ninsts - 1]];
    if (r->op != LIMBA_OP_RET || r->nops != 1)
        return NULL;
    return &f->insts[f->operands[r->first]];
}

/* the operand k of in */
static const limba_inst *operand(const fn *F, const limba_inst *in, uint32_t k)
{
    const limba_func *f = &F->m->funcs[0];
    return &f->insts[f->operands[in->first + k]];
}

static bool is_iconst(const limba_inst *in, int64_t v)
{
    return in && in->op == LIMBA_OP_ICONST && in->imm == v;
}

/* x = 1; try { y = c / c; x = 2 } except { return x + y }; return x:
   the handler sees x = 1, and y unassigned */
static void test_entry_values(void)
{
    fn F = begin();
    limba_ssa_def(F.s, F.x, 0, iconst(&F, 0, 1));
    limba_id h = handler(&F);
    limba_id b1 = protected_block(&F, h);
    limba_id b2 = limba_ssa_block(F.s);
    limba_ssa_br(F.s, 0, b1);
    limba_ssa_seal(F.s, b1);
    may_trap(&F, b1);
    limba_ssa_def(F.s, F.y, b1, iconst(&F, b1, 5));
    limba_ssa_def(F.s, F.x, b1, iconst(&F, b1, 2));
    limba_ssa_br(F.s, b1, b2);
    limba_ssa_seal(F.s, b2);
    limba_ssa_ret(F.s, b2, limba_ssa_use(F.s, F.x, b2, 1));
    /* read before the handler is sealed, and after */
    limba_id hx = limba_ssa_use(F.s, F.x, h, 2);
    limba_ssa_seal(F.s, h);
    limba_id hy = limba_ssa_use(F.s, F.y, h, 3);
    uint32_t ops[2] = {hx, hy};
    limba_id sum = limba_inst_add(limba_ssa_func(F.s), h, LIMBA_OP_ADD,
                                  LIMBA_T_I64, 0, 0, 0, ops, 2);
    limba_ssa_ret(F.s, h, sum);
    finish(&F);
    CHECK(is_iconst(returned(&F, b2), 2), "after the try, x is not 2");
    const limba_inst *r = returned(&F, h);
    CHECK(r && r->op == LIMBA_OP_ADD && is_iconst(operand(&F, r, 0), 1),
          "the handler does not see x = 1");
    CHECK(r && r->op == LIMBA_OP_ADD && is_iconst(operand(&F, r, 1), 0),
          "the handler does not see y as zero");
    CHECK(was_told(3) && !was_told(2) && !was_told(1) && ntold == 1,
          "definite assignment: %u tags reported, y not alone", ntold);
    CHECK(F.m->funcs[0].blocks[h].nparams == 3, "the handler has %u parameters",
          F.m->funcs[0].blocks[h].nparams);
    limba_module_free(F.m);
}

/* x = 1; a loop in the try, x not assigned there: the parameter of the
   loop head is trivial, and so is the handler's */
static void test_loop_in_try(void)
{
    fn F = begin();
    limba_ssa_def(F.s, F.x, 0, iconst(&F, 0, 1));
    limba_id h = handler(&F);
    limba_id head = protected_block(&F, h);
    limba_id body = protected_block(&F, h);
    limba_id out = limba_ssa_block(F.s);
    limba_ssa_br(F.s, 0, head);
    may_trap(&F, head);
    limba_ssa_use(F.s, F.x, head, 1);
    limba_ssa_cbr(F.s, head, 1, body, out); /* k */
    limba_ssa_seal(F.s, body);
    may_trap(&F, body);
    limba_ssa_br(F.s, body, head);
    limba_ssa_seal(F.s, head);
    limba_ssa_seal(F.s, out);
    limba_ssa_ret(F.s, out, limba_ssa_use(F.s, F.x, out, 2));
    limba_ssa_seal(F.s, h);
    limba_ssa_ret(F.s, h, limba_ssa_use(F.s, F.x, h, 3));
    finish(&F);
    CHECK(is_iconst(returned(&F, h), 1), "in the loop, the handler: not 1");
    CHECK(ntold == 0, "in the loop, %u tags reported", ntold);
    limba_module_free(F.m);
}

/* a handler protected by another: the outer one has for predecessors
   those of the inner one, the predecessors of the try */
static void test_nested(void)
{
    fn F = begin();
    limba_ssa_def(F.s, F.x, 0, iconst(&F, 0, 1));
    limba_id outer = handler(&F);
    limba_id inner = handler(&F);
    limba_block_set_handler(limba_ssa_func(F.s), inner, outer);
    limba_id b1 = protected_block(&F, inner);
    limba_ssa_br(F.s, 0, b1);
    limba_ssa_seal(F.s, b1);
    may_trap(&F, b1);
    limba_ssa_def(F.s, F.x, b1, iconst(&F, b1, 2));
    limba_ssa_ret(F.s, b1, limba_ssa_use(F.s, F.x, b1, 1));
    limba_ssa_seal(F.s, inner);
    may_trap(&F, inner);
    limba_ssa_def(F.s, F.x, inner, iconst(&F, inner, 3));
    limba_ssa_ret(F.s, inner, limba_ssa_use(F.s, F.x, inner, 2));
    limba_ssa_seal(F.s, outer);
    limba_ssa_ret(F.s, outer, limba_ssa_use(F.s, F.x, outer, 3));
    finish(&F);
    CHECK(is_iconst(returned(&F, outer), 1), "the outer handler: not 1");
    CHECK(is_iconst(returned(&F, inner), 3), "the inner handler: not 3");
    CHECK(ntold == 0, "nested, %u tags reported", ntold);
    limba_module_free(F.m);
}

/* the mistakes of the front end, each in a child that must abort */

/* the handler sealed before the block it protects */
static void early_handler(void)
{
    fn F = begin();
    limba_id h = handler(&F);
    limba_ssa_seal(F.s, h);
    limba_id b1 = protected_block(&F, h);
    limba_ssa_br(F.s, 0, b1);
    limba_ssa_seal(F.s, b1);
}

/* the handler given after the block was sealed */
static void late_protection(void)
{
    fn F = begin();
    limba_id h = handler(&F);
    limba_id b1 = limba_ssa_block(F.s);
    limba_ssa_br(F.s, 0, b1);
    limba_ssa_seal(F.s, b1);
    limba_block_set_handler(limba_ssa_func(F.s), b1, h);
    may_trap(&F, b1);
    limba_ssa_ret(F.s, b1, iconst(&F, b1, 0));
    limba_ssa_seal(F.s, h);
    limba_ssa_ret(F.s, h, iconst(&F, h, 0));
    limba_ssa_finish(F.s, undefined, NULL);
}

/* x is 1 or 2 where the protected block begins: the handler would need
   a parameter */
static void handler_parameter(void)
{
    fn F = begin();
    limba_id h = handler(&F);
    limba_id a = limba_ssa_block(F.s);
    limba_id b = limba_ssa_block(F.s);
    limba_id j = protected_block(&F, h);
    limba_ssa_cbr(F.s, 0, 1, a, b);
    limba_ssa_seal(F.s, a);
    limba_ssa_seal(F.s, b);
    limba_ssa_def(F.s, F.x, a, iconst(&F, a, 1));
    limba_ssa_br(F.s, a, j);
    limba_ssa_def(F.s, F.x, b, iconst(&F, b, 2));
    limba_ssa_br(F.s, b, j);
    limba_ssa_seal(F.s, j);
    may_trap(&F, j);
    limba_ssa_ret(F.s, j, limba_ssa_use(F.s, F.x, j, 1));
    limba_ssa_seal(F.s, h);
    limba_ssa_ret(F.s, h, limba_ssa_use(F.s, F.x, h, 2));
    limba_ssa_finish(F.s, undefined, NULL);
}

static void expect_abort(void (*mistake)(void), const char *what)
{
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        /* the message of the builder is expected: not on the output */
        if (!freopen("/dev/null", "w", stderr))
            _exit(2);
        mistake();
        _exit(0);
    }
    int st = 0;
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) &&
              WTERMSIG(st) == SIGABRT,
          "%s: the builder did not stop", what);
}

int main(void)
{
    limba_set_program("test_ssa");
    test_entry_values();
    test_loop_in_try();
    test_nested();
    expect_abort(early_handler, "handler sealed first");
    expect_abort(late_protection, "handler given after sealing");
    expect_abort(handler_parameter, "handler with a parameter");
    printf("test_ssa: 3 functions with handlers, 3 mistakes of the front "
           "end, %d failures\n",
           failures);
    return failures ? 1 : 0;
}
