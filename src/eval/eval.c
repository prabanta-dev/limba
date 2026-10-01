/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * eval.c - the reference interpreter of the IR (see eval.h).
 *
 * Every value is 64 bits: integers in their canonical form (sign-extended
 * from their width, 0 or 1 for i1), floats as the bits of a double (an f32
 * is a double holding a float value, computed in float), pointers as the
 * address, strings as a pointer to an immutable string of this file, a
 * BigInt (ref) as a pointer to an immutable number in the same box (0 is
 * the number 0, as it is the string "").
 * Memory is real: slots and globals are allocated, load and store touch
 * them. The arithmetic here is written again on purpose rather than shared
 * with the constant folder: an oracle that reuses the code it judges would
 * agree with its mistakes.
 */
#include "eval.h"

#include "limba/fmt.h"
#include "limba/val.h"
#include "common/leb128.h"
#include "common/xalloc.h"
#include "front/bigint.h"
#include "ir/internal.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a string of the program: length, the counts of check_mem, bytes, a NUL
   for str_ptr. A BigInt is one too, its bytes a limba_big (read with
   memcpy: they are not aligned), so that check_mem counts both alike */
typedef struct {
    size_t len;
    int64_t mrefs;    /* references from memory: store str, retain... */
    int64_t seen;     /* at the end: the words of memory that hold it */
    uint8_t immortal; /* sconst, an initial value: never counted */
    uint8_t listed;   /* in E.counted */
    char data[];
} estr;

/* a map of addresses, open addressing: key 0 is empty, 1 removed */
typedef struct {
    uintptr_t *key;
    size_t *val;
    size_t cap, used, live;
} amap;

#define AMAP_NONE SIZE_MAX

typedef struct {
    const limba_module *m;
    limba_eval_limits lim;
    uint64_t steps;
    uint32_t depth;
    limba_wbuf out;
    void **arena; /* everything allocated, freed at the end */
    size_t narena, caparena;
    void **globals;
    amap heap;      /* the blocks of mem_alloc not yet freed, with their size */
    amap cstrs;     /* the C strings of cstr_new not yet freed: memory of C,
                       out of the budget */
    amap words;     /* check_mem: the words of memory that hold a str */
    estr **counted; /* check_mem: the strings whose count ever moved */
    size_t ncounted, capcounted;
    int status;
    int64_t code;
    uint32_t pos; /* where the run stopped, innermost call first */
    /* the C stack the run may use: a call of the program deeper than
       budget bytes from base is the trap STACK, never a crash of the
       interpreter itself */
    const char *stack_base;
    size_t stack_budget;
    /* the memory of the program against max_memory; a string past it in
       the runtime jumps back to the call with the trap NOMEM */
    uint64_t used, budget;
    jmp_buf nomem;
    bool armed;
    estr **sconst; /* the string of each sconst, made once: immortal */
    bool checked;  /* the counts checked once, at the end or at a stop */
} E;

/* the run has a thread of its own, with a stack for max_depth calls of
   any build (a sanitizer grows each frame); reserved, not touched */
#define EVAL_STACK ((size_t)256 << 20)
#define EVAL_STACK_MARGIN ((size_t)1 << 20)

static void *keep(E *e, void *p)
{
    LIMBA_GROW(e->arena, e->narena, e->caparena);
    e->arena[e->narena++] = p;
    return p;
}

static size_t amap_hash(uintptr_t p, size_t cap)
{
    return (size_t)(((p >> 3) * 0x9e3779b97f4a7c15ull) >> 24) & (cap - 1);
}

/* where p is, AMAP_NONE if absent */
static size_t amap_find(const amap *a, uintptr_t p)
{
    if (!a->live)
        return AMAP_NONE;
    for (size_t k = amap_hash(p, a->cap); a->key[k]; k = (k + 1) & (a->cap - 1))
        if (a->key[k] == p)
            return k;
    return AMAP_NONE;
}

static void amap_put(amap *a, uintptr_t p, size_t val)
{
    size_t k = amap_find(a, p);
    if (k != AMAP_NONE) {
        a->val[k] = val;
        return;
    }
    if (2 * (a->used + 1) > a->cap) {
        /* rehash the live entries, without the removed ones */
        size_t cap = a->cap ? a->cap : 64;
        while (4 * (a->live + 1) > cap)
            cap *= 2;
        uintptr_t *key = limba_xcalloc(cap, sizeof(*key));
        size_t *vals = limba_xcalloc(cap, sizeof(*vals));
        for (size_t i = 0; i < a->cap; i++) {
            if (a->key[i] <= 1)
                continue;
            size_t j = amap_hash(a->key[i], cap);
            while (key[j])
                j = (j + 1) & (cap - 1);
            key[j] = a->key[i];
            vals[j] = a->val[i];
        }
        free(a->key);
        free(a->val);
        a->key = key;
        a->val = vals;
        a->cap = cap;
        a->used = a->live;
    }
    k = amap_hash(p, a->cap);
    while (a->key[k] > 1)
        k = (k + 1) & (a->cap - 1);
    if (!a->key[k])
        a->used++;
    a->key[k] = p;
    a->val[k] = val;
    a->live++;
}

/* false if p was not there */
static bool amap_del(amap *a, uintptr_t p)
{
    size_t k = amap_find(a, p);
    if (k == AMAP_NONE)
        return false;
    a->key[k] = 1;
    a->live--;
    return true;
}

static void amap_free(amap *a)
{
    free(a->key);
    free(a->val);
}

/* n bytes more for the program, if the budget has them */
static bool take(E *e, uint64_t n)
{
    if (n > e->budget - e->used)
        return false;
    e->used += n;
    return true;
}

/* a string of n bytes, its contents to be written by the caller; past the
   budget, in the runtime, the trap NOMEM */
static estr *str_alloc(E *e, size_t n)
{
    if (!take(e, sizeof(estr) + (uint64_t)n + 1)) {
        if (e->armed)
            longjmp(e->nomem, 1);
        e->used += sizeof(estr) + (uint64_t)n + 1; /* a constant: made */
    }
    estr *x = keep(e, limba_xmalloc(sizeof(estr) + n + 1));
    x->len = n;
    x->mrefs = x->seen = 0;
    x->immortal = x->listed = 0;
    x->data[n] = 0;
    return x;
}

static estr *str_make(E *e, const char *s, size_t n)
{
    estr *x = str_alloc(e, n);
    if (n)
        memcpy(x->data, s, n);
    return x;
}

static const estr empty = {0};

static const estr *str_of(uint64_t v)
{
    return v ? (const estr *)(uintptr_t)v : &empty;
}

static uint64_t sv(const estr *s)
{
    return (uint64_t)(uintptr_t)s;
}

/* ---- numbers ---- */

static uint64_t norm(uint64_t v, limba_id t)
{
    return (uint64_t)limba_int_norm((int64_t)v, t);
}

static uint64_t width_mask(limba_id t)
{
    unsigned bits = limba_type_bits(t);
    return bits >= 64 || bits == 0 ? ~0ull : (1ull << bits) - 1;
}

static uint64_t uv(uint64_t v, limba_id t)
{
    return v & width_mask(t);
}

static double dv(uint64_t v)
{
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

/* an f32 is the 32 bits of its float, zero-extended, as in Meri: it goes
   through a double only where the IR converts (a double would quieten a
   signalling NaN, progetto_ir.md § 11b) */
static float fv32(uint64_t v)
{
    uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static uint64_t f32bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* the number a float value of type t holds, for arithmetic only */
static double fnum(uint64_t v, limba_id t)
{
    return t == LIMBA_T_F32 ? (double)fv32(v) : dv(v);
}

/* the value of type t for the result d of an arithmetic operation */
static uint64_t fbits(double d, limba_id t)
{
    if (t == LIMBA_T_F32)
        return f32bits((float)d);
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return b;
}

static void end_check(E *e);

/* a trap stops the program; with check_mem the counts are checked there,
   before the calls unwind: they hold between any two instructions, and
   most runs of a random program end in a trap */
static bool trap(E *e, int64_t code)
{
    e->status = LIMBA_EVAL_TRAP;
    e->code = code;
    if (e->lim.check_mem)
        end_check(e);
    return false;
}

/* ---- the run-time library ---- */

static void out_i64(E *e, int64_t v)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%" PRId64, v);
    limba_w_bytes(&e->out, buf, (size_t)n);
}

static void out_f64(E *e, double v)
{
    char buf[LIMBA_FMT_F64_MAX];
    limba_w_bytes(&e->out, buf, limba_fmt_f64(buf, v));
}

static void store(void *p, limba_id t, uint64_t v);
static bool mem_store(E *e, uintptr_t p, limba_id t, uint64_t v);
static bool forget(E *e, uintptr_t p, size_t n);
static bool no_str(E *e, uintptr_t p, size_t n);
static bool badmem(E *e);
static bool runtime_arrays(E *e, uint32_t rt, const uint64_t *a, uint64_t *r);

/* UTF-8 of a code point, into buf (4 bytes); its length */
static size_t utf8(uint32_t c, char *buf)
{
    if (c < 0x80) {
        buf[0] = (char)c;
        return 1;
    }
    size_t n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    for (size_t k = n; k-- > 1;) {
        buf[k] = (char)(0x80 | (c & 0x3f));
        c >>= 6;
    }
    buf[0] = (char)((0xf00 >> n) | c);
    return n;
}

/* ---- BigInt (luxia_0.md § 3.12) ---- */

/* the number of a handle: its words are the box's, read only */
static limba_big big_get(uint64_t v)
{
    limba_big b;
    limba_big_init(&b);
    if (v)
        memcpy(&b, ((const estr *)(uintptr_t)v)->data, sizeof(b));
    return b;
}

/* the bits the budget still has room for: how large a result may grow */
static uint64_t big_room(const E *e)
{
    uint64_t left = e->budget - e->used;
    return left > UINT64_MAX / 8 ? UINT64_MAX / 8 : left * 8;
}

/* t boxed as a new value (0 for zero), its words taken; past the budget
   the trap NOMEM, t freed */
static bool big_box(E *e, limba_big *t, uint64_t *r)
{
    if (!t->n) {
        limba_big_free(t);
        *r = 0;
        return true;
    }
    size_t box = sizeof(estr) + sizeof(limba_big) + 1;
    if (!take(e, box + (uint64_t)t->cap * sizeof(uint32_t))) {
        limba_big_free(t);
        return trap(e, LIMBA_TRAP_NOMEM);
    }
    estr *x = keep(e, limba_xmalloc(box));
    x->len = sizeof(limba_big);
    x->mrefs = x->seen = 0;
    x->immortal = x->listed = 0;
    x->data[x->len] = 0;
    keep(e, t->w);
    memcpy(x->data, t, sizeof(*t));
    *r = sv(x);
    return true;
}

/* the result of an operation of the _lim functions: false is NOMEM */
static bool big_done(E *e, bool ok, limba_big *t, uint64_t *r)
{
    if (!ok) {
        limba_big_free(t);
        return trap(e, LIMBA_TRAP_NOMEM);
    }
    return big_box(e, t, r);
}

/* the decimal digits of v, malloc'd */
static char *big_text(uint64_t v, size_t *len)
{
    limba_big b = big_get(v);
    /* a digit for every 3.3 bits, the sign, room for the NUL past it */
    size_t size = limba_big_bits(&b) / 3 + 5;
    char *buf = limba_xmalloc(size);
    limba_big_str(&b, buf, size);
    *len = strlen(buf);
    return buf;
}

static bool runtime_big(E *e, uint32_t rt, const uint64_t *a, uint64_t *r)
{
    limba_big t, x, y;
    limba_big_init(&t);
    /* the functions whose first operand is a BigInt */
    bool first_big = rt != LIMBA_RT_BIG_FROM_I64 &&
                     rt != LIMBA_RT_BIG_FROM_U64 && rt != LIMBA_RT_BIG_LIT &&
                     rt != LIMBA_RT_BIG_FROM_F64 && rt != LIMBA_RT_STR_TO_BIG;
    x = big_get(first_big ? a[0] : 0);
    switch (rt) {
    case LIMBA_RT_BIG_FROM_I64:
        limba_big_set_i64(&t, (int64_t)a[0]);
        return big_box(e, &t, r);
    case LIMBA_RT_BIG_FROM_U64:
        limba_big_set_u64(&t, a[0]);
        return big_box(e, &t, r);
    case LIMBA_RT_BIG_LIT: {
        const estr *s = str_of(a[0]);
        bool neg = s->len && s->data[0] == '-';
        bool ok = limba_big_parse_lim(&t, s->data + neg, s->len - neg, 10,
                                      big_room(e));
        if (ok && neg)
            limba_big_neg(&t, &t);
        return big_done(e, ok, &t, r);
    }
    case LIMBA_RT_BIG_TO_I64: {
        int64_t v;
        *r = limba_big_to_i64(&x, &v);
        return !*r || mem_store(e, a[1], LIMBA_T_I64, (uint64_t)v);
    }
    case LIMBA_RT_BIG_TO_U64: {
        uint64_t v;
        *r = limba_big_to_u64(&x, &v);
        return !*r || mem_store(e, a[1], LIMBA_T_I64, v);
    }
    case LIMBA_RT_BIG_TO_BITS: {
        uint64_t low = x.n == 0   ? 0
                       : x.n == 1 ? x.w[0]
                                  : (uint64_t)x.w[1] << 32 | x.w[0];
        *r = x.neg ? 0 - low : low;
        return true;
    }
    case LIMBA_RT_BIG_FROM_F64: {
        /* finite and integral, checked before (else the module is wrong):
           exact */
        double d = fabs(dv(a[0]));
        if (!isfinite(d) || d != trunc(d)) {
            e->status = LIMBA_EVAL_BAD;
            return false;
        }
        if (d < 18446744073709551616.0) {
            limba_big_set_u64(&t, (uint64_t)d);
        } else {
            int ex;
            double frac = frexp(d, &ex); /* d = frac 2^ex, frac in [0.5, 1) */
            limba_big_set_u64(&t, (uint64_t)ldexp(frac, 53));
            limba_big_shl_lim(&t, &t, (uint32_t)(ex - 53), UINT64_MAX);
        }
        if (signbit(dv(a[0])))
            limba_big_neg(&t, &t);
        return big_box(e, &t, r);
    }
    case LIMBA_RT_BIG_TO_F64:
    case LIMBA_RT_BIG_TO_F32: {
        /* once, straight to the type: limba_rat rounds to its bits */
        limba_rat q;
        limba_rat_init(&q);
        limba_rat_set_big(&q, &x);
        if (rt == LIMBA_RT_BIG_TO_F64) {
            double d;
            if (!limba_rat_to_f64(&q, &d))
                d = x.neg ? -INFINITY : INFINITY;
            *r = fbits(d, LIMBA_T_F64);
        } else {
            float f;
            if (!limba_rat_to_f32(&q, &f))
                f = x.neg ? -INFINITY : INFINITY;
            *r = f32bits(f);
        }
        limba_rat_free(&q);
        return true;
    }
    case LIMBA_RT_BIG_ADD:
        y = big_get(a[1]);
        return big_done(e, limba_big_add_lim(&t, &x, &y, big_room(e)), &t, r);
    case LIMBA_RT_BIG_SUB:
        y = big_get(a[1]);
        return big_done(e, limba_big_sub_lim(&t, &x, &y, big_room(e)), &t, r);
    case LIMBA_RT_BIG_MUL:
        y = big_get(a[1]);
        return big_done(e, limba_big_mul_lim(&t, &x, &y, big_room(e)), &t, r);
    case LIMBA_RT_BIG_NEG:
        limba_big_neg(&t, &x);
        return big_box(e, &t, r);
    case LIMBA_RT_BIG_ABS:
        limba_big_abs(&t, &x);
        return big_box(e, &t, r);
    case LIMBA_RT_BIG_DIV:
    case LIMBA_RT_BIG_REM:
    case LIMBA_RT_BIG_MOD: {
        y = big_get(a[1]);
        if (limba_big_is_zero(&y)) { /* the IR checks it before: a module
                                        that does not is wrong */
            e->status = LIMBA_EVAL_BAD;
            return false;
        }
        limba_big m;
        limba_big_init(&m);
        limba_big_divmod(&t, &m, &x, &y);
        if (rt == LIMBA_RT_BIG_DIV) {
            limba_big_free(&m);
            return big_box(e, &t, r);
        }
        limba_big_free(&t);
        /* mod: the sign of the divisor */
        if (rt == LIMBA_RT_BIG_MOD && !limba_big_is_zero(&m) &&
            limba_big_sign(&m) != limba_big_sign(&y))
            limba_big_add_lim(&m, &m, &y, UINT64_MAX / 8);
        return big_box(e, &m, r);
    }
    case LIMBA_RT_BIG_POW:
        return big_done(e, limba_big_pow_lim(&t, &x, a[1], big_room(e)), &t, r);
    case LIMBA_RT_BIG_CMP:
        y = big_get(a[1]);
        *r = norm((uint64_t)(int64_t)limba_big_cmp(&x, &y), LIMBA_T_I32);
        return true;
    case LIMBA_RT_BIG_SIGN:
        *r = norm((uint64_t)(int64_t)limba_big_sign(&x), LIMBA_T_I32);
        return true;
    case LIMBA_RT_PRINT_BIG:
    case LIMBA_RT_STR_FROM_BIG: {
        size_t n;
        char *text = big_text(a[0], &n);
        bool ok = true;
        if (rt == LIMBA_RT_PRINT_BIG) {
            limba_w_bytes(&e->out, text, n);
        } else if (!take(e, sizeof(estr) + (uint64_t)n + 1)) {
            ok = trap(e, LIMBA_TRAP_NOMEM); /* str_make would jump */
        } else {
            e->used -= sizeof(estr) + (uint64_t)n + 1;
            *r = sv(str_make(e, text, n));
        }
        free(text);
        return ok;
    }
    case LIMBA_RT_STR_TO_BIG: {
        const estr *s = str_of(a[0]);
        char *digits = limba_xmalloc(s->len + 1);
        size_t n;
        unsigned base;
        bool neg;
        *r = limba_val_big(s->data, s->len, digits, &n, &base, &neg);
        if (!*r) {
            free(digits);
            return true;
        }
        bool ok = limba_big_parse_lim(&t, digits, n, base, big_room(e));
        free(digits);
        if (ok && neg)
            limba_big_neg(&t, &t);
        uint64_t v;
        return big_done(e, ok, &t, &v) && mem_store(e, a[1], LIMBA_T_REF, v);
    }
    }
    e->status = LIMBA_EVAL_UNSUPPORTED;
    return false;
}

/* a call that skips a check the IR owes it: a wrong module */
static bool bad_module(E *e)
{
    e->status = LIMBA_EVAL_BAD;
    return false;
}

/* the C strings (luxia_0.md § 9.9): malloc and free of C, as a C library
   would have them; out of the budget, as memory of C */
static bool runtime_c(E *e, uint32_t rt, const uint64_t *a, uint64_t *r)
{
    switch (rt) {
    case LIMBA_RT_CSTR_NEW: {
        const estr *s = str_of(a[0]);
        if (memchr(s->data, 0, s->len))
            return trap(e, LIMBA_TRAP_RANGE); /* C would cut it short */
        char *p = malloc(s->len + 1);
        if (!p)
            return trap(e, LIMBA_TRAP_NOMEM);
        memcpy(p, s->data, s->len);
        p[s->len] = 0;
        amap_put(&e->cstrs, (uintptr_t)p, s->len + 1);
        *r = (uint64_t)(uintptr_t)p;
        return true;
    }
    case LIMBA_RT_CSTR_VALUE: {
        const char *p = (const char *)(uintptr_t)a[0];
        if (!p)
            return bad_module(e); /* the nil check comes before */
        *r = sv(str_make(e, p, strlen(p)));
        return true;
    }
    case LIMBA_RT_CSTR_VALUE_N: {
        const char *p = (const char *)(uintptr_t)a[0];
        if (!p || (int64_t)a[1] < 0)
            return bad_module(e);
        *r = sv(str_make(e, p, (size_t)a[1]));
        return true;
    }
    case LIMBA_RT_CSTR_FREE:
        if (!a[0])
            return true;
        /* only what cstr_new made: here there is no C to have made any */
        if (!amap_del(&e->cstrs, a[0]))
            return bad_module(e);
        free((void *)(uintptr_t)a[0]);
        return true;
    }
    return runtime_arrays(e, rt, a, r);
}

/* the routines of arrays (luxia_0.md § 9.5): the tract, the pattern,
   nil and dangling are checked before, in the IR */
static bool runtime_arrays(E *e, uint32_t rt, const uint64_t *a, uint64_t *r)
{
    uint8_t *p = (uint8_t *)(uintptr_t)a[0];
    int64_t n = (int64_t)a[1];
    switch (rt) {
    case LIMBA_RT_MEM_TRANSLATE: {
        const uint8_t *t = (const uint8_t *)(uintptr_t)a[2];
        if (n < 0 || (n && (!p || !t)))
            return bad_module(e);
        if (!n)
            return true;
        if (e->lim.check_mem && (!no_str(e, (uintptr_t)p, (size_t)n) ||
                                 !no_str(e, (uintptr_t)t, 256)))
            return false;
        uint8_t table[256]; /* first: the table may be the array itself */
        memcpy(table, t, sizeof(table));
        for (int64_t i = 0; i < n; i++)
            p[i] = table[p[i]];
        return true;
    }
    case LIMBA_RT_MEM_REVERSE: {
        size_t size = (size_t)a[2], len;
        if (n < 0 || !size || (n && !p) ||
            __builtin_mul_overflow((size_t)n, size, &len))
            return bad_module(e);
        if (n < 2)
            return true;
        if (e->lim.check_mem && e->words.live) {
            /* the str in the tract go with their elements */
            size_t *offs = NULL, k = 0, cap = 0;
            uintptr_t s = (uintptr_t)p;
            for (uintptr_t w = s & ~(uintptr_t)7; w < s + len; w += 8)
                if (amap_find(&e->words, w) != AMAP_NONE) {
                    LIMBA_GROW(offs, k, cap);
                    offs[k++] = w - s;
                }
            bool ok = forget(e, s, len);
            for (size_t i = 0; i < k && ok; i++) {
                size_t el = offs[i] / size, off = offs[i] % size;
                uintptr_t w = s + ((size_t)n - 1 - el) * size + off;
                if (w & 7)
                    ok = badmem(e);
                else
                    amap_put(&e->words, w, 0);
            }
            free(offs);
            if (!ok)
                return false;
        }
        for (size_t i = 0, j = (size_t)n - 1; i < j; i++, j--)
            for (size_t b = 0; b < size; b++) {
                uint8_t x = p[i * size + b];
                p[i * size + b] = p[j * size + b];
                p[j * size + b] = x;
            }
        return true;
    }
    case LIMBA_RT_MEM_COUNT: {
        const uint8_t *q = (const uint8_t *)(uintptr_t)a[2];
        int64_t m = (int64_t)a[3];
        if (n < 0 || m < 1 || !q || (n && !p))
            return bad_module(e);
        if (e->lim.check_mem && (!no_str(e, (uintptr_t)p, (size_t)n) ||
                                 !no_str(e, (uintptr_t)q, (size_t)m)))
            return false;
        int64_t c = 0;
        for (int64_t i = 0; i + m <= n;)
            if (!memcmp(p + i, q, (size_t)m)) {
                c++;
                i += m;
            } else {
                i++;
            }
        *r = (uint64_t)c;
        return true;
    }
    }
    return runtime_big(e, rt, a, r);
}

/* a whole string as a number: optional blanks around, nothing else */
static bool runtime_luxia(E *e, uint32_t rt, const uint64_t *a, uint64_t *r)
{
    char buf[512];
    int n;
    switch (rt) {
    case LIMBA_RT_PRINT_U64:
        n = snprintf(buf, sizeof(buf), "%" PRIu64, a[0]);
        limba_w_bytes(&e->out, buf, (size_t)n);
        return true;
    case LIMBA_RT_PRINT_BOOL:
        limba_w_bytes(&e->out, a[0] & 1 ? "true" : "false", a[0] & 1 ? 4 : 5);
        return true;
    case LIMBA_RT_PRINT_CHAR:
        limba_w_bytes(&e->out, buf, utf8((uint32_t)a[0], buf));
        return true;
    case LIMBA_RT_PRINT_BYTE:
        buf[0] = (char)a[0];
        limba_w_bytes(&e->out, buf, 1);
        return true;
    case LIMBA_RT_PRINT_STR_W: {
        const estr *s = str_of(a[0]);
        int64_t width = (int32_t)a[1], chars = 0;
        for (size_t i = 0; i < s->len; i++)
            chars += ((unsigned char)s->data[i] & 0xc0) != 0x80;
        for (; width > chars; width--)
            limba_w_bytes(&e->out, " ", 1);
        limba_w_bytes(&e->out, s->data, s->len);
        return true;
    }
    case LIMBA_RT_STR_FROM_U64:
        n = snprintf(buf, sizeof(buf), "%" PRIu64, a[0]);
        *r = sv(str_make(e, buf, (size_t)n));
        return true;
    case LIMBA_RT_STR_FROM_F64_FIXED: {
        char f[LIMBA_FMT_FIXED_MAX];
        *r =
            sv(str_make(e, f, limba_fmt_f64_fixed(f, dv(a[0]), (int32_t)a[1])));
        return true;
    }
    case LIMBA_RT_STR_FROM_CHAR:
        *r = sv(str_make(e, buf, utf8((uint32_t)a[0], buf)));
        return true;
    case LIMBA_RT_STR_FROM_BOOL:
        *r = sv(str_make(e, a[0] & 1 ? "true" : "false", a[0] & 1 ? 4 : 5));
        return true;
    case LIMBA_RT_READ_LINE: {
        char *line = NULL;
        size_t cap = 0;
        ssize_t got = e->lim.in ? getline(&line, &cap, e->lim.in) : -1;
        bool ok = got >= 0;
        size_t len = ok ? (size_t)got : 0;
        if (len && line[len - 1] == '\n')
            len--;
        if (len && line[len - 1] == '\r')
            len--;
        uint64_t s = sv(str_make(e, ok ? line : "", len));
        free(line);
        *r = ok;
        return mem_store(e, a[0], LIMBA_T_STR, s);
    }
    case LIMBA_RT_STR_TO_I64:
    case LIMBA_RT_STR_TO_U64: {
        uint64_t mag;
        const estr *s = str_of(a[0]);
        bool neg, ok = limba_val_int(s->data, s->len, &mag, &neg);
        if (rt == LIMBA_RT_STR_TO_I64)
            ok = ok && mag <= (uint64_t)INT64_MAX + neg &&
                 (int64_t)(neg ? 0 - mag : mag) >= (int64_t)a[2] &&
                 (int64_t)(neg ? 0 - mag : mag) <= (int64_t)a[3];
        else
            ok = ok && (!neg || mag == 0) && mag >= a[2] && mag <= a[3];
        *r = ok;
        return !ok || mem_store(e, a[1], LIMBA_T_I64, neg ? 0 - mag : mag);
    }
    case LIMBA_RT_STR_TO_F64:
    case LIMBA_RT_STR_TO_F32: {
        bool f32 = rt == LIMBA_RT_STR_TO_F32;
        uint64_t v;
        const estr *s = str_of(a[0]);
        bool ok = limba_val_real(s->data, s->len, f32, &v);
        *r = ok;
        return !ok || mem_store(e, a[1], f32 ? LIMBA_T_F32 : LIMBA_T_F64, v);
    }
    case LIMBA_RT_ARG_COUNT:
        *r = (uint64_t)(e->lim.argc > 0 ? e->lim.argc : 0);
        return true;
    case LIMBA_RT_ARG: {
        int32_t i = (int32_t)a[0];
        const char *s = i >= 1 && i <= e->lim.argc ? e->lim.argv[i - 1] : "";
        *r = sv(str_make(e, s, strlen(s)));
        return true;
    }
    case LIMBA_RT_HALT:
        e->status = LIMBA_EVAL_HALT;
        e->code = (int32_t)a[0];
        if (e->lim.check_mem)
            end_check(e); /* as at a trap */
        return false;
    case LIMBA_RT_MATH_TAN:
        *r = fbits(tan(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_ATAN:
        *r = fbits(atan(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_EXP:
        *r = fbits(exp(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_LN:
        *r = fbits(log(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_TRUNC:
        *r = fbits(trunc(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_FLOOR:
        *r = fbits(floor(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_CEIL:
        *r = fbits(ceil(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_INT_POW: {
        int64_t base = (int64_t)a[0], acc = 1;
        uint64_t ex = a[1]; /* without a sign */
        while (ex) {
            if ((ex & 1) && __builtin_mul_overflow(acc, base, &acc))
                return trap(e, LIMBA_TRAP_OVERFLOW);
            ex >>= 1;
            if (ex && __builtin_mul_overflow(base, base, &base))
                return trap(e, LIMBA_TRAP_OVERFLOW);
        }
        *r = (uint64_t)acc;
        return true;
    }
    case LIMBA_RT_UINT_POW:
    case LIMBA_RT_BITS_POW: {
        uint64_t base = a[0], acc = 1, ex = a[1];
        bool wrap = rt == LIMBA_RT_BITS_POW;
        while (ex) {
            if ((ex & 1) && __builtin_mul_overflow(acc, base, &acc) && !wrap)
                return trap(e, LIMBA_TRAP_OVERFLOW);
            ex >>= 1;
            if (ex && __builtin_mul_overflow(base, base, &base) && !wrap)
                return trap(e, LIMBA_TRAP_OVERFLOW);
        }
        *r = acc;
        return true;
    }
    default:
        return runtime_c(e, rt, a, r);
    }
}

static bool runtime(E *e, uint32_t rt, const uint64_t *a, uint64_t *r)
{
    char buf[40];
    *r = 0;
    switch (rt) {
    case LIMBA_RT_PRINT_I64:
        out_i64(e, (int64_t)a[0]);
        return true;
    case LIMBA_RT_PRINT_F64:
        out_f64(e, dv(a[0]));
        return true;
    case LIMBA_RT_PRINT_STR: {
        const estr *s = str_of(a[0]);
        limba_w_bytes(&e->out, s->data, s->len);
        return true;
    }
    case LIMBA_RT_PRINT_NL:
        limba_w_byte(&e->out, '\n');
        return true;
    case LIMBA_RT_STR_CONCAT: {
        const estr *x = str_of(a[0]), *y = str_of(a[1]);
        estr *s = str_alloc(e, x->len + y->len);
        memcpy(s->data, x->data, x->len);
        memcpy(s->data + x->len, y->data, y->len);
        *r = sv(s);
        return true;
    }
    case LIMBA_RT_STR_LEN:
        *r = str_of(a[0])->len;
        return true;
    case LIMBA_RT_STR_CMP: {
        const estr *x = str_of(a[0]), *y = str_of(a[1]);
        size_t n = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->data, y->data, n);
        if (!c)
            c = x->len < y->len ? -1 : x->len > y->len;
        *r = norm((uint64_t)(int64_t)(c < 0 ? -1 : c > 0), LIMBA_T_I32);
        return true;
    }
    case LIMBA_RT_STR_FROM_I64: {
        int n = snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)a[0]);
        *r = sv(str_make(e, buf, (size_t)n));
        return true;
    }
    case LIMBA_RT_STR_FROM_F64: {
        char f[LIMBA_FMT_F64_MAX];
        *r = sv(str_make(e, f, limba_fmt_f64(f, dv(a[0]))));
        return true;
    }
    case LIMBA_RT_STR_FROM_F32: {
        char f[LIMBA_FMT_F64_MAX];
        *r = sv(str_make(e, f, limba_fmt_f32(f, fv32(a[0]))));
        return true;
    }
    case LIMBA_RT_PRINT_F32: {
        char f[LIMBA_FMT_F64_MAX];
        limba_w_bytes(&e->out, f, limba_fmt_f32(f, fv32(a[0])));
        return true;
    }
    case LIMBA_RT_STR_MID: { /* (s, start from 0, length), clamped */
        const estr *s = str_of(a[0]);
        int64_t start = (int64_t)a[1], len = (int64_t)a[2];
        if (start < 0)
            start = 0;
        if ((uint64_t)start > s->len)
            start = (int64_t)s->len;
        if (len < 0 || (uint64_t)len > s->len - (uint64_t)start)
            len = (int64_t)(s->len - (uint64_t)start);
        *r = sv(str_make(e, s->data + start, (size_t)len));
        return true;
    }
    case LIMBA_RT_STR_PTR:
        *r = (uint64_t)(uintptr_t)str_of(a[0])->data;
        return true;
    case LIMBA_RT_MEM_ALLOC: {
        size_t size = a[0] ? a[0] : 1;
        if ((int64_t)a[0] < 0 || !take(e, size))
            return trap(e, LIMBA_TRAP_NOMEM);
        void *p = calloc(size, 1);
        if (!p) {
            e->used -= size;
            return trap(e, LIMBA_TRAP_NOMEM);
        }
        keep(e, p);
        amap_put(&e->heap, (uintptr_t)p, size);
        *r = (uint64_t)(uintptr_t)p;
        return true;
    }
    case LIMBA_RT_MEM_FREE: {
        /* the arena frees the bytes at the end, and never reuses them: a
           dangling pointer stays different from any new one */
        size_t k = amap_find(&e->heap, a[0]);
        if (!a[0])
            return true;
        if (k == AMAP_NONE)
            return trap(e, LIMBA_TRAP_INVALID_FREE);
        size_t size = e->heap.val[k];
        amap_del(&e->heap, a[0]);
        e->used -= size; /* out of the budget; the bytes stay reserved */
        return !e->lim.check_mem || forget(e, a[0], size);
    }
    case LIMBA_RT_PTR_LIVE:
        *r = a[0] && amap_find(&e->heap, a[0]) != AMAP_NONE;
        return true;
    case LIMBA_RT_MATH_SQRT:
        *r = fbits(sqrt(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_SIN:
        *r = fbits(sin(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_COS:
        *r = fbits(cos(dv(a[0])), LIMBA_T_F64);
        return true;
    case LIMBA_RT_MATH_POW:
        *r = fbits(pow(dv(a[0]), dv(a[1])), LIMBA_T_F64);
        return true;
    default:
        return runtime_luxia(e, rt, a, r);
    }
    e->status = LIMBA_EVAL_UNSUPPORTED;
    return false;
}

/* ---- memory ---- */

static uint64_t load(void *p, limba_id t)
{
    switch (t) {
    case LIMBA_T_I1:
        return *(uint8_t *)p & 1;
    case LIMBA_T_I8:
        return (uint64_t)(int64_t)*(int8_t *)p;
    case LIMBA_T_I16: {
        int16_t x;
        memcpy(&x, p, sizeof(x));
        return (uint64_t)(int64_t)x;
    }
    case LIMBA_T_I32: {
        int32_t x;
        memcpy(&x, p, sizeof(x));
        return (uint64_t)(int64_t)x;
    }
    case LIMBA_T_F32: { /* the 32 bits, as they are */
        uint32_t x;
        memcpy(&x, p, sizeof(x));
        return x;
    }
    default: {
        uint64_t x;
        memcpy(&x, p, sizeof(x));
        return x;
    }
    }
}

static void store(void *p, limba_id t, uint64_t v)
{
    switch (t) {
    case LIMBA_T_I1:
    case LIMBA_T_I8: {
        uint8_t x = (uint8_t)v;
        memcpy(p, &x, 1);
        return;
    }
    case LIMBA_T_I16: {
        uint16_t x = (uint16_t)v;
        memcpy(p, &x, 2);
        return;
    }
    case LIMBA_T_I32: {
        uint32_t x = (uint32_t)v;
        memcpy(p, &x, 4);
        return;
    }
    case LIMBA_T_F32: {
        uint32_t x = (uint32_t)v;
        memcpy(p, &x, 4);
        return;
    }
    default:
        memcpy(p, &v, 8);
    }
}

/* size zeroed bytes aligned to align, taken from the budget; NULL past
   it: the trap NOMEM, never a crash */
static void *zalloc_aligned(E *e, size_t size, size_t align)
{
    void *p;
    if (align < sizeof(void *))
        align = sizeof(void *);
    if (!size)
        size = 1;
    size = (size + align - 1) / align * align;
    if (!take(e, size))
        return NULL;
    if (posix_memalign(&p, align, size)) {
        e->used -= size;
        return NULL;
    }
    memset(p, 0, size);
    return p;
}

/* the bytes zalloc_aligned took for size */
static size_t zalloc_size(size_t size, size_t align)
{
    if (align < sizeof(void *))
        align = sizeof(void *);
    if (!size)
        size = 1;
    return (size + align - 1) / align * align;
}

/* ---- the strings in memory (check_mem, progetto_ir.md § 11c) ---- */

static bool badmem(E *e)
{
    e->status = LIMBA_EVAL_BADMEM;
    return false;
}

/* one reference from memory more (d = 1) or less (d = -1) on string v */
static bool count(E *e, uint64_t v, int d)
{
    if (!v)
        return true;
    estr *s = (estr *)(uintptr_t)v;
    if (s->immortal)
        return true;
    if (!s->listed) {
        s->listed = 1;
        LIMBA_GROW(e->counted, e->ncounted, e->capcounted);
        e->counted[e->ncounted++] = s;
    }
    s->mrefs += d;
    return s->mrefs >= 0 || badmem(e);
}

/* no str in the bytes [p, p + n): what a load or store of another type
   may touch */
static bool no_str(E *e, uintptr_t p, size_t n)
{
    if (!e->words.live)
        return true;
    for (uintptr_t w = p & ~(uintptr_t)7; w < p + n; w += 8)
        if (amap_find(&e->words, w) != AMAP_NONE)
            return badmem(e);
    return true;
}

/* the str in [p, p + n) are overwritten or gone, uncounted (memset,
   memcpy, mem_free, the end of a slot): they must be whole */
static bool forget(E *e, uintptr_t p, size_t n)
{
    if (!e->words.live)
        return true;
    for (uintptr_t w = p & ~(uintptr_t)7; w < p + n; w += 8)
        if (amap_del(&e->words, w) && (w < p || w + 8 > p + n))
            return badmem(e);
    return true;
}

static bool mem_load(E *e, uintptr_t p, limba_id t, uint64_t *r)
{
    *r = load((void *)p, t);
    if (!e->lim.check_mem)
        return true;
    if (t == LIMBA_T_STR || t == LIMBA_T_REF) /* zero: never written */
        return !*r || amap_find(&e->words, p) != AMAP_NONE || badmem(e);
    return no_str(e, p, e->m->types[t].size);
}

/* store str takes a reference on the new value, then releases the old */
static bool mem_store(E *e, uintptr_t p, limba_id t, uint64_t v)
{
    if (e->lim.check_mem) {
        if (t == LIMBA_T_STR || t == LIMBA_T_REF) {
            if (p & 7)
                return badmem(e);
            uint64_t old = amap_find(&e->words, p) != AMAP_NONE
                               ? load((void *)p, LIMBA_T_STR)
                               : 0;
            if (!count(e, v, 1) || !count(e, old, -1))
                return false;
            amap_put(&e->words, p, 0);
        } else if (!no_str(e, p, e->m->types[t].size)) {
            return false;
        }
    }
    store((void *)p, t, v);
    return true;
}

/* memcpy moves the str of the source with it, uncounted */
static bool mem_copy(E *e, uintptr_t dst, uintptr_t src, size_t len)
{
    if (!e->lim.check_mem || !e->words.live) {
        memmove((void *)dst, (void *)src, len);
        return true;
    }
    size_t *offs = NULL, n = 0, cap = 0;
    bool ok = true;
    for (uintptr_t w = src & ~(uintptr_t)7; w < src + len && ok; w += 8)
        if (amap_find(&e->words, w) != AMAP_NONE) {
            if (w < src || w + 8 > src + len || ((dst - src) & 7))
                ok = badmem(e);
            LIMBA_GROW(offs, n, cap);
            offs[n++] = w - src;
        }
    if (ok && (ok = forget(e, dst, len))) {
        memmove((void *)dst, (void *)src, len);
        for (size_t i = 0; i < n; i++)
            amap_put(&e->words, dst + offs[i], 0);
    }
    free(offs);
    return ok;
}

static bool mem_fill(E *e, uintptr_t dst, int byte, size_t len)
{
    if (e->lim.check_mem && !forget(e, dst, len))
        return false;
    memset((void *)dst, byte, len);
    return true;
}

/* retain (d = 1) or release (d = -1) every str of a value of type t at p */
static bool rc_walk(E *e, uintptr_t p, limba_id t, int d)
{
    const limba_module *m = e->m;
    const limba_type *ty = &m->types[t];
    switch (ty->kind) {
    case LIMBA_TK_STR:
    case LIMBA_TK_REF: {
        uint64_t v = load((void *)p, LIMBA_T_STR);
        if (v && amap_find(&e->words, p) == AMAP_NONE)
            return badmem(e);
        return count(e, v, d);
    }
    case LIMBA_TK_ARRAY: {
        uint32_t size = m->types[ty->elem].size;
        if (!limba_type_counted(m, ty->elem))
            return true;
        for (uint32_t i = 0; i < ty->count; i++)
            if (!rc_walk(e, p + (uintptr_t)i * size, ty->elem, d))
                return false;
        return true;
    }
    case LIMBA_TK_STRUCT:
        for (uint32_t i = 0; i < ty->count; i++) {
            const limba_member *f = &m->members[ty->first + i];
            if (limba_type_counted(m, f->type) &&
                !rc_walk(e, p + f->offset, f->type, d))
                return false;
        }
        return true;
    default:
        return true;
    }
}

/* retain T p, n and release T p, n: a negative count, or one whose bytes
   overflow, is RANGE in every engine; the counts only with check_mem */
static bool rc_inst(E *e, const limba_inst *in, uint64_t p, uint64_t nv)
{
    int64_t n = (int64_t)nv;
    uint64_t size = e->m->types[in->imm].size;
    if (n < 0 || (n && size > UINT64_MAX / (uint64_t)n))
        return trap(e, LIMBA_TRAP_RANGE);
    if (!e->lim.check_mem)
        return true;
    int d = in->op == LIMBA_OP_RETAIN ? 1 : -1;
    for (int64_t i = 0; i < n; i++)
        if (!rc_walk(e, p + (uint64_t)i * size, (limba_id)in->imm, d))
            return false;
    return true;
}

/* at the end of a run, or where it stops (a trap, halt): every counted
   string has as many references as words of memory still hold it */
static void end_check(E *e)
{
    if (e->checked)
        return;
    e->checked = true;
    amap *w = &e->words;
    for (int pass = 0; pass < 2; pass++)
        for (size_t k = 0; k < w->cap; k++) {
            if (w->key[k] <= 1)
                continue;
            uint64_t v = load((void *)w->key[k], LIMBA_T_STR);
            estr *s = (estr *)(uintptr_t)v;
            if (!v || s->immortal)
                continue;
            if (!pass)
                s->seen++;
            else if (s->seen != s->mrefs)
                e->status = LIMBA_EVAL_BADMEM;
        }
    for (size_t i = 0; i < e->ncounted; i++)
        if (e->counted[i]->seen != e->counted[i]->mrefs)
            e->status = LIMBA_EVAL_BADMEM;
}

/* ---- operations ---- */

static bool int_bin(E *e, unsigned op, limba_id t, uint64_t a, uint64_t b,
                    uint64_t *r)
{
    unsigned bits = limba_type_bits(t);
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    uint64_t ua = uv(a, t), ub = uv(b, t);
    int64_t smin = bits >= 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));
    __int128 w;
    switch (op) {
    case LIMBA_OP_ADD:
        *r = ua + ub;
        break;
    case LIMBA_OP_SUB:
        *r = ua - ub;
        break;
    case LIMBA_OP_MUL:
        *r = ua * ub;
        break;
    case LIMBA_OP_AND:
        *r = a & b;
        break;
    case LIMBA_OP_OR:
        *r = a | b;
        break;
    case LIMBA_OP_XOR:
        *r = a ^ b;
        break;
    case LIMBA_OP_SHL:
        *r = ua << (ub % bits);
        break;
    case LIMBA_OP_LSHR:
        *r = ua >> (ub % bits);
        break;
    case LIMBA_OP_ASHR:
        *r = (uint64_t)(sa >> (ub % bits));
        break;
    case LIMBA_OP_UDIV:
    case LIMBA_OP_UREM:
        if (!ub)
            return trap(e, LIMBA_TRAP_DIVZERO);
        *r = op == LIMBA_OP_UDIV ? ua / ub : ua % ub;
        break;
    case LIMBA_OP_SDIV:
    case LIMBA_OP_SREM:
        if (!sb || (sa == smin && sb == -1))
            return trap(e, LIMBA_TRAP_DIVZERO);
        *r = (uint64_t)(op == LIMBA_OP_SDIV ? sa / sb : sa % sb);
        break;
    case LIMBA_OP_ADDOV:
    case LIMBA_OP_SUBOV:
    case LIMBA_OP_MULOV:
        w = op == LIMBA_OP_ADDOV   ? (__int128)sa + sb
            : op == LIMBA_OP_SUBOV ? (__int128)sa - sb
                                   : (__int128)sa * sb;
        if (w < smin || w > -(__int128)smin - 1)
            return trap(e, LIMBA_TRAP_OVERFLOW);
        *r = (uint64_t)(int64_t)w;
        break;
    default:
        return false;
    }
    *r = norm(*r, t);
    return true;
}

/* rounding ties away from zero, written from the integer part so that
   it does not share its method with fold's round(): x - trunc(x) is exact */
static double round_away(double x)
{
    double t = trunc(x), d = x - t;
    if (isnan(x) || isinf(x))
        return x;
    return fabs(d) >= 0.5 ? t + copysign(1.0, x) : t;
}

static float round_awayf(float x)
{
    float t = truncf(x), d = x - t;
    if (isnan(x) || isinf(x))
        return x;
    return fabsf(d) >= 0.5f ? t + copysignf(1.0f, x) : t;
}

static uint64_t float_op(unsigned op, limba_id t, uint64_t a, uint64_t b,
                         uint64_t c)
{
    if (t == LIMBA_T_F32) {
        float x = fv32(a), y = fv32(b), z = fv32(c), r = 0;
        switch (op) {
        case LIMBA_OP_FADD:
            r = x + y;
            break;
        case LIMBA_OP_FSUB:
            r = x - y;
            break;
        case LIMBA_OP_FMUL:
            r = x * y;
            break;
        case LIMBA_OP_FDIV:
            r = x / y;
            break;
        case LIMBA_OP_FNEG:
            return a ^ 0x80000000u; /* the sign bit: a NaN keeps the rest */
        case LIMBA_OP_FROUND:
            r = nearbyintf(x); /* the default mode: to nearest, ties even */
            break;
        case LIMBA_OP_FROUNDA:
            r = round_awayf(x);
            break;
        case LIMBA_OP_FMA:
            r = fmaf(x, y, z);
            break;
        }
        return f32bits(r);
    }
    double x = dv(a), y = dv(b), z = dv(c), r = 0;
    switch (op) {
    case LIMBA_OP_FADD:
        r = x + y;
        break;
    case LIMBA_OP_FSUB:
        r = x - y;
        break;
    case LIMBA_OP_FMUL:
        r = x * y;
        break;
    case LIMBA_OP_FDIV:
        r = x / y;
        break;
    case LIMBA_OP_FNEG:
        r = -x;
        break;
    case LIMBA_OP_FROUND:
        r = nearbyint(x);
        break;
    case LIMBA_OP_FROUNDA:
        r = round_away(x);
        break;
    case LIMBA_OP_FMA:
        r = fma(x, y, z);
        break;
    }
    return fbits(r, t);
}

static bool compare(unsigned cc, limba_id t, uint64_t a, uint64_t b)
{
    if (limba_cc_is_float(cc)) {
        double x = fnum(a, t), y = fnum(b, t);
        bool u = isnan(x) || isnan(y);
        switch (cc) {
        case LIMBA_CC_OEQ:
            return !u && x == y;
        case LIMBA_CC_ONE:
            return !u && x != y;
        case LIMBA_CC_OLT:
            return !u && x < y;
        case LIMBA_CC_OLE:
            return !u && x <= y;
        case LIMBA_CC_OGT:
            return !u && x > y;
        case LIMBA_CC_OGE:
            return !u && x >= y;
        case LIMBA_CC_ORD:
            return !u;
        case LIMBA_CC_UNO:
            return u;
        case LIMBA_CC_UEQ:
            return u || x == y;
        case LIMBA_CC_UNE:
            return u || x != y;
        case LIMBA_CC_FULT:
            return u || x < y;
        case LIMBA_CC_FULE:
            return u || x <= y;
        case LIMBA_CC_FUGT:
            return u || x > y;
        default: /* FUGE */
            return u || x >= y;
        }
    }
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    uint64_t ua = uv(a, t), ub = uv(b, t);
    switch (cc) {
    case LIMBA_CC_EQ:
        return a == b;
    case LIMBA_CC_NE:
        return a != b;
    case LIMBA_CC_SLT:
        return sa < sb;
    case LIMBA_CC_SLE:
        return sa <= sb;
    case LIMBA_CC_SGT:
        return sa > sb;
    case LIMBA_CC_SGE:
        return sa >= sb;
    case LIMBA_CC_ULT:
        return ua < ub;
    case LIMBA_CC_ULE:
        return ua <= ub;
    case LIMBA_CC_UGT:
        return ua > ub;
    default: /* UGE */
        return ua >= ub;
    }
}

static bool convert(unsigned op, limba_id from, limba_id to, uint64_t a,
                    uint64_t *r)
{
    unsigned bits = limba_type_bits(to);
    double d = limba_type_is_float(from) ? fnum(a, from) : 0, tr;
    switch (op) {
    case LIMBA_OP_TRUNC:
        *r = norm(a, to);
        return true;
    case LIMBA_OP_ZEXT:
        *r = norm(uv(a, from), to);
        return true;
    case LIMBA_OP_SEXT:
        *r = from == LIMBA_T_I1 ? (a ? ~0ull : 0) : a;
        *r = norm(*r, to);
        return true;
    case LIMBA_OP_FPTRUNC:
    case LIMBA_OP_FPEXT:
        *r = fbits(d, to);
        return true;
    case LIMBA_OP_SITOFP:
        *r = to == LIMBA_T_F32 ? fbits((float)(int64_t)a, to)
                               : fbits((double)(int64_t)a, to);
        return true;
    case LIMBA_OP_UITOFP:
        *r = to == LIMBA_T_F32 ? fbits((float)uv(a, from), to)
                               : fbits((double)uv(a, from), to);
        return true;
    case LIMBA_OP_FPTOSI: { /* saturating: NaN is 0 */
        double lo = -ldexp(1, (int)bits - 1);
        tr = trunc(d);
        if (isnan(d))
            *r = 0;
        else if (tr < lo)
            *r = (uint64_t)(int64_t)lo;
        else if (tr >= -lo)
            *r = (uint64_t)(bits >= 64 ? INT64_MAX
                                       : ((int64_t)1 << (bits - 1)) - 1);
        else
            *r = (uint64_t)(int64_t)tr;
        *r = norm(*r, to);
        return true;
    }
    case LIMBA_OP_FPTOUI: /* saturating: NaN and negatives are 0 */
        tr = trunc(d);
        if (isnan(d) || tr <= 0)
            *r = 0;
        else if (tr >= ldexp(1, (int)bits))
            *r = width_mask(to);
        else
            *r = (uint64_t)tr;
        *r = norm(*r, to);
        return true;
    case LIMBA_OP_BITCAST:
        /* the bits, untouched: an f32 is already its 32 bits */
        *r = from == LIMBA_T_I32   ? (uint64_t)(uint32_t)a
             : from == LIMBA_T_F32 ? norm((uint32_t)a, LIMBA_T_I32)
                                   : a;
        return true;
    case LIMBA_OP_PTRTOINT:
        *r = norm(a, to);
        return true;
    case LIMBA_OP_INTTOPTR:
        *r = uv(a, from);
        return true;
    }
    return false;
}

/* ---- functions ---- */

static bool call(E *e, const limba_func *f, const uint64_t *args,
                 uint64_t *ret);

static bool call_inst(E *e, const limba_func *f, const limba_inst *in,
                      const uint64_t *v, uint64_t *r)
{
    const limba_module *m = e->m;
    const uint32_t *o = f->operands + in->first;
    uint32_t first = in->op == LIMBA_OP_CALLIND ? 1 : 0;
    uint32_t n = in->nops - first;
    uint64_t stack[16], *args = n <= 16 ? stack : limba_xmalloc(n * 8);
    bool ok;
    for (uint32_t i = 0; i < n; i++)
        args[i] = v[o[first + i]];
    *r = 0;
    switch (in->op) {
    case LIMBA_OP_CALL:
        ok = call(e, &m->funcs[in->imm], args, r);
        break;
    case LIMBA_OP_CALLIND: {
        uintptr_t p = (uintptr_t)v[o[0]], base = (uintptr_t)m->funcs;
        uintptr_t k = (p - base) / sizeof(limba_func);
        if (p < base || (p - base) % sizeof(limba_func) || k >= m->nfuncs ||
            m->funcs[k].type != (limba_id)in->imm) {
            e->status = LIMBA_EVAL_BAD;
            ok = false;
        } else {
            ok = call(e, &m->funcs[k], args, r);
        }
        break;
    }
    case LIMBA_OP_CALLRT:
        /* a string past the budget comes back here (str_alloc) */
        if (setjmp(e->nomem)) {
            e->armed = false;
            ok = trap(e, LIMBA_TRAP_NOMEM);
            break;
        }
        e->armed = true;
        ok = runtime(e, (uint32_t)in->imm, args, r);
        e->armed = false;
        break;
    default:
        e->status = LIMBA_EVAL_UNSUPPORTED; /* call.ext: no C here */
        ok = false;
    }
    if (args != stack)
        free(args);
    return ok;
}

/* jump to the target whose (block, count, args) start at operand k:
   arguments are read first, then written, as one parallel copy */
static const limba_block *jump(const limba_func *f, uint32_t k, uint64_t *v)
{
    limba_id b;
    uint32_t n;
    uint32_t a = limba_target(f, k, &b, &n);
    const limba_block *bl = &f->blocks[b];
    uint64_t stack[16], *tmp = n <= 16 ? stack : limba_xmalloc(n * 8);
    for (uint32_t i = 0; i < n; i++)
        tmp[i] = v[f->operands[a + i]];
    for (uint32_t i = 0; i < n; i++)
        v[bl->insts[i]] = tmp[i];
    if (tmp != stack)
        free(tmp);
    return bl;
}

static bool call(E *e, const limba_func *f, const uint64_t *args, uint64_t *ret)
{
    const limba_module *m = e->m;
    char here;
    size_t used = e->stack_base > &here ? (size_t)(e->stack_base - &here)
                                        : (size_t)(&here - e->stack_base);
    if (e->depth >= e->lim.max_depth || used > e->stack_budget)
        return trap(e, LIMBA_TRAP_STACK);
    e->depth++;
    uint64_t *v = limba_xcalloc((size_t)f->ninsts + 1, sizeof(*v));
    /* the slots live while the call does: zeroed at entry (IR § 4) */
    void **slots = limba_xcalloc((size_t)f->nslots + 1, sizeof(*slots));
    bool ok = true;
    for (uint32_t s = 0; s < f->nslots && ok; s++)
        if (!(slots[s] =
                  zalloc_aligned(e, f->slots[s].size, f->slots[s].align)))
            ok = trap(e, LIMBA_TRAP_NOMEM);
    const limba_block *bl = &f->blocks[0];
    for (uint32_t i = 0; i < bl->nparams; i++)
        v[bl->insts[i]] = args[i];
    if (!ok)
        goto done;

    for (;;) {
        const limba_block *next = NULL;
        for (uint32_t k = bl->nparams; k < bl->ninsts && ok && !next; k++) {
            if (++e->steps > e->lim.max_steps) {
                e->status = LIMBA_EVAL_LIMIT;
                ok = false;
                break;
            }
            uint32_t id = bl->insts[k];
            const limba_inst *in = &f->insts[id];
            const uint32_t *o = f->operands + in->first;
            limba_id t = in->type;
            uint64_t r = 0;
            switch (limba_ops[in->op].format) {
            case LIMBA_F_ICONST:
                r = norm((uint64_t)in->imm, t);
                break;
            case LIMBA_F_FCONST:
                /* the bits of the float of t, as the IR carries them */
                r = t == LIMBA_T_F32 ? (uint64_t)(uint32_t)in->imm
                                     : (uint64_t)in->imm;
                break;
            case LIMBA_F_SCONST: {
                /* immortal: made once, however often it runs */
                estr **x = &e->sconst[in->imm];
                if (!*x) {
                    size_t n;
                    const char *s = limba_str(m, (limba_id)in->imm, &n);
                    *x = str_make(e, s, n);
                    (*x)->immortal = 1;
                }
                r = sv(*x);
                break;
            }
            case LIMBA_F_TYPED:
                r = 0;
                break;
            case LIMBA_F_UN:
                if (in->op == LIMBA_OP_FNEG || in->op == LIMBA_OP_FROUND ||
                    in->op == LIMBA_OP_FROUNDA)
                    r = float_op(in->op, t, v[o[0]], 0, 0);
                else if (in->op == LIMBA_OP_NEG)
                    r = norm(0 - uv(v[o[0]], t), t);
                else
                    r = norm(~v[o[0]], t);
                break;
            case LIMBA_F_BIN:
                if (limba_type_is_float(t))
                    r = float_op(in->op, t, v[o[0]], v[o[1]], 0);
                else
                    ok = int_bin(e, in->op, t, v[o[0]], v[o[1]], &r);
                break;
            case LIMBA_F_TERN:
                if (in->op == LIMBA_OP_SELECT)
                    r = v[o[0]] ? v[o[1]] : v[o[2]];
                else
                    r = float_op(in->op, t, v[o[0]], v[o[1]], v[o[2]]);
                break;
            case LIMBA_F_CMP:
                r = compare(in->cc, f->insts[o[0]].type, v[o[0]], v[o[1]]);
                break;
            case LIMBA_F_CONV:
                ok = convert(in->op, f->insts[o[0]].type, t, v[o[0]], &r);
                break;
            case LIMBA_F_LOAD:
                ok = mem_load(e, v[o[0]], t, &r);
                break;
            case LIMBA_F_STORE:
                ok = mem_store(e, v[o[1]], f->insts[o[0]].type, v[o[0]]);
                break;
            case LIMBA_F_SLOT:
                r = (uint64_t)(uintptr_t)slots[in->imm];
                break;
            case LIMBA_F_GADDR:
                r = (uint64_t)(uintptr_t)e->globals[in->imm];
                break;
            case LIMBA_F_FADDR:
                r = (uint64_t)(uintptr_t)&m->funcs[in->imm];
                break;
            case LIMBA_F_ADDR:
                /* modular arithmetic: an address wraps like the machine */
                r = v[o[0]] + v[o[1]] * (uint64_t)in->imm + (uint64_t)in->imm2;
                break;
            case LIMBA_F_MEM3: {
                size_t len = (size_t)uv(v[o[2]], f->insts[o[2]].type);
                if (in->op == LIMBA_OP_MEMCPY)
                    ok = mem_copy(e, v[o[0]], v[o[1]], len);
                else
                    ok = mem_fill(e, v[o[0]], (int)(uint8_t)v[o[1]], len);
                break;
            }
            case LIMBA_F_RC:
                ok = rc_inst(e, in, v[o[0]], v[o[1]]);
                break;
            case LIMBA_F_CALL:
            case LIMBA_F_CALL_IND:
            case LIMBA_F_CALL_EXT:
            case LIMBA_F_CALL_RT:
                ok = call_inst(e, f, in, v, &r);
                break;
            case LIMBA_F_BR:
                next = jump(f, in->first, v);
                break;
            case LIMBA_F_CBR: {
                uint32_t tk = in->first + 1;
                uint32_t ek = tk + 2 + f->operands[tk + 1];
                next = jump(f, v[o[0]] ? tk : ek, v);
                break;
            }
            case LIMBA_F_SWITCH: {
                limba_id to = o[1];
                limba_id st = f->insts[o[0]].type;
                for (uint32_t c = 0; c < o[2]; c++) {
                    int64_t cv =
                        (int64_t)((uint64_t)o[4 + 3 * c] << 32 | o[3 + 3 * c]);
                    if (norm((uint64_t)cv, st) == v[o[0]]) {
                        to = o[5 + 3 * c];
                        break;
                    }
                }
                next = &f->blocks[to];
                break;
            }
            case LIMBA_F_RET:
                *ret = in->nops ? v[o[0]] : 0;
                goto done;
            case LIMBA_F_NONE:
                e->status = LIMBA_EVAL_UNREACHABLE;
                ok = false;
                break;
            case LIMBA_F_TRAP:
                ok = trap(e, in->imm);
                break;
            case LIMBA_F_CHECK:
                if (!v[o[0]])
                    ok = trap(e, in->imm);
                break;
            case LIMBA_F_PARAM:
                break;
            }
            if (!ok && !e->pos)
                e->pos = limba_inst_pos(f, id);
            v[id] = r;
        }
        if (!ok)
            break;
        bl = next;
    }
done:
    /* the typed slots release their strings, every slot forgets its own */
    for (uint32_t s = 0; ok && e->lim.check_mem && s < f->nslots; s++) {
        uintptr_t p = (uintptr_t)slots[s];
        if (f->slots[s].type != LIMBA_NONE)
            ok = rc_walk(e, p, f->slots[s].type, -1);
        ok = ok && forget(e, p, f->slots[s].size);
    }
    for (uint32_t s = 0; s < f->nslots && slots[s]; s++) {
        free(slots[s]);
        e->used -= zalloc_size(f->slots[s].size, f->slots[s].align);
    }
    free(v);
    free(slots);
    e->depth--;
    return ok;
}

struct run {
    E *e;
    const limba_func *f;
    uint64_t ret;
};

static void *run_thread(void *arg)
{
    struct run *r = arg;
    char base;
    r->e->stack_base = &base;
    r->e->stack_budget = EVAL_STACK - EVAL_STACK_MARGIN;
    call(r->e, r->f, NULL, &r->ret);
    return NULL;
}

/* call f on a stack of EVAL_STACK bytes; without a thread, on this one
   with a budget the default stack of a process holds */
static void run_on_stack(E *e, const limba_func *f, uint64_t *ret)
{
    struct run r = {e, f, 0};
    pthread_attr_t a;
    pthread_t t;
    bool threaded = !pthread_attr_init(&a) &&
                    !pthread_attr_setstacksize(&a, EVAL_STACK) &&
                    !pthread_create(&t, &a, run_thread, &r);
    pthread_attr_destroy(&a);
    if (threaded) {
        pthread_join(t, NULL);
    } else {
        char base;
        e->stack_base = &base;
        e->stack_budget = 4 * EVAL_STACK_MARGIN;
        call(e, f, NULL, &r.ret);
    }
    *ret = r.ret;
}

void limba_eval(const limba_module *m, const char *entry,
                const limba_eval_limits *limits, limba_eval_result *r)
{
    E e = {.m = m, .status = LIMBA_EVAL_OK};
    e.lim.max_steps =
        limits && limits->max_steps ? limits->max_steps : 100000000;
    e.lim.max_depth = limits && limits->max_depth ? limits->max_depth : 10000;
    if (limits) {
        e.lim.argc = limits->argc;
        e.lim.argv = limits->argv;
        e.lim.in = limits->in;
        e.lim.check_mem = limits->check_mem;
    }
    memset(r, 0, sizeof(*r));

    limba_id name = LIMBA_NONE, fid = LIMBA_NONE;
    size_t n = strlen(entry);
    for (uint32_t i = 0; i < limba_str_count(m) && name == LIMBA_NONE; i++) {
        size_t k;
        const char *s = limba_str(m, i, &k);
        if (k == n && !memcmp(s, entry, n))
            name = i;
    }
    if (name != LIMBA_NONE)
        fid = limba_func_find(m, name);
    if (fid == LIMBA_NONE || m->types[m->funcs[fid].type].count != 0) {
        r->status = LIMBA_EVAL_BAD;
        r->out = limba_xcalloc(1, 1);
        return;
    }

    e.budget =
        limits && limits->max_memory ? limits->max_memory : (uint64_t)1 << 30;
    e.sconst = limba_xcalloc((size_t)limba_str_count(m) + 1, sizeof(estr *));
    /* globals, with their initial values; too large: NOMEM, no run */
    e.globals = limba_xcalloc((size_t)m->nglobals + 1, sizeof(void *));
    for (uint32_t i = 0; i < m->nglobals && e.status == LIMBA_EVAL_OK; i++) {
        const limba_global *g = &m->globals[i];
        const limba_type *ty = &m->types[g->type];
        void *p = zalloc_aligned(&e, ty->size, ty->align);
        if (!p) {
            trap(&e, LIMBA_TRAP_NOMEM);
            break;
        }
        e.globals[i] = keep(&e, p);
        if (g->init == LIMBA_INIT_INT)
            store(p, g->type, norm((uint64_t)g->value, g->type));
        else if (g->init == LIMBA_INIT_FLOAT)
            store(p, g->type,
                  g->type == LIMBA_T_F32 ? (uint64_t)(uint32_t)g->value
                                         : (uint64_t)g->value);
        else if (g->init == LIMBA_INIT_STR) {
            size_t k;
            const char *s = limba_str(m, (limba_id)g->value, &k);
            estr *x = str_make(&e, s, k);
            x->immortal = 1;
            store(p, LIMBA_T_STR, sv(x));
            if (e.lim.check_mem)
                amap_put(&e.words, (uintptr_t)p, 0);
        }
    }

    uint64_t ret = 0;
    if (e.status == LIMBA_EVAL_OK)
        run_on_stack(&e, &m->funcs[fid], &ret);
    if (e.status == LIMBA_EVAL_OK && e.lim.check_mem)
        end_check(&e);
    r->status = e.status;
    r->code = e.code;
    r->pos = e.pos;
    r->ret = e.status == LIMBA_EVAL_OK ? ret : 0;
    r->steps = e.steps;
    r->live = e.heap.live;
    limba_w_byte(&e.out, 0);
    r->out = (char *)e.out.buf;
    r->outlen = e.out.len - 1;

    for (size_t i = 0; i < e.narena; i++)
        free(e.arena[i]);
    free(e.arena);
    amap_free(&e.heap);
    for (size_t k = 0; k < e.cstrs.cap; k++) /* C strings never freed */
        if (e.cstrs.key[k] > 1)
            free((void *)e.cstrs.key[k]);
    amap_free(&e.cstrs);
    free(e.sconst);
    amap_free(&e.words);
    free(e.counted);
    free(e.globals);
}

void limba_eval_result_free(limba_eval_result *r)
{
    free(r->out);
    r->out = NULL;
}
