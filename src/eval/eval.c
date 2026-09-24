/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * eval.c - the reference interpreter of the IR (see eval.h).
 *
 * Every value is 64 bits: integers in their canonical form (sign-extended
 * from their width, 0 or 1 for i1), floats as the bits of a double (an f32
 * is a double holding a float value, computed in float), pointers as the
 * address, strings as a pointer to an immutable string of this file.
 * Memory is real: slots and globals are allocated, load and store touch
 * them. The arithmetic here is written again on purpose rather than shared
 * with the constant folder: an oracle that reuses the code it judges would
 * agree with its mistakes.
 */
#include "eval.h"

#include "common/fmt_f64.h"
#include "common/leb128.h"
#include "common/xalloc.h"
#include "ir/internal.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a string of the program: length, bytes, a NUL for str_ptr */
typedef struct {
    size_t len;
    char data[];
} estr;

typedef struct {
    const limba_module *m;
    limba_eval_limits lim;
    uint64_t steps;
    uint32_t depth;
    limba_wbuf out;
    void **arena; /* everything allocated, freed at the end */
    size_t narena, caparena;
    void **globals;
    int status;
    int64_t code;
    uint32_t pos; /* where the run stopped, innermost call first */
} E;

static void *keep(E *e, void *p)
{
    LIMBA_GROW(e->arena, e->narena, e->caparena);
    e->arena[e->narena++] = p;
    return p;
}

/* a string of n bytes, its contents to be written by the caller */
static estr *str_alloc(E *e, size_t n)
{
    estr *x = keep(e, limba_xmalloc(sizeof(estr) + n + 1));
    x->len = n;
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

static uint64_t fbits(double d, limba_id t)
{
    if (t == LIMBA_T_F32)
        d = (float)d;
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return b;
}

static bool trap(E *e, int64_t code)
{
    e->status = LIMBA_EVAL_TRAP;
    e->code = code;
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

/* a whole string as a number: optional blanks around, nothing else */
static bool parse_number(const estr *s, bool real, uint64_t *out)
{
    char buf[128];
    size_t n = s->len < sizeof(buf) - 1 ? s->len : sizeof(buf) - 1;
    memcpy(buf, s->data, n);
    buf[n] = 0;
    char *end;
    errno = 0;
    if (real) {
        double d = strtod(buf, &end);
        *out = fbits(d, LIMBA_T_F64);
    } else {
        long long v = strtoll(buf, &end, 10);
        *out = (uint64_t)v;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    return errno == 0 && end != buf && *end == 0 && n == s->len;
}

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
        int d = (int32_t)a[1];
        d = d < 0 ? 0 : d > 100 ? 100 : d;
        n = snprintf(buf, sizeof(buf), "%.*f", d, dv(a[0]));
        *r = sv(str_make(e, buf,
                         n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1));
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
        store((void *)(uintptr_t)a[0], LIMBA_T_STR, s);
        *r = ok;
        return true;
    }
    case LIMBA_RT_STR_TO_I64:
    case LIMBA_RT_STR_TO_F64: {
        uint64_t v;
        bool ok = parse_number(str_of(a[0]), rt == LIMBA_RT_STR_TO_F64, &v);
        if (ok)
            store((void *)(uintptr_t)a[1],
                  rt == LIMBA_RT_STR_TO_F64 ? LIMBA_T_F64 : LIMBA_T_I64, v);
        *r = ok;
        return true;
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
        int64_t base = (int64_t)a[0], ex = (int64_t)a[1], acc = 1;
        if (ex < 0)
            return trap(e, LIMBA_TRAP_OVERFLOW);
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
    }
    e->status = LIMBA_EVAL_UNSUPPORTED;
    return false;
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
    case LIMBA_RT_INPUT_LINE:
        *r = sv(str_make(e, "", 0)); /* no input in a test */
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
        if ((int64_t)a[0] < 0 || a[0] > (1u << 30))
            return trap(e, LIMBA_TRAP_NOMEM);
        *r = (uint64_t)(uintptr_t)keep(e, limba_xcalloc(a[0] ? a[0] : 1, 1));
        return true;
    }
    case LIMBA_RT_MEM_FREE:
        return true; /* the arena frees everything at the end */
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
    case LIMBA_T_F32: {
        float x;
        memcpy(&x, p, sizeof(x));
        return fbits(x, LIMBA_T_F32);
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
        float x = (float)dv(v);
        memcpy(p, &x, 4);
        return;
    }
    default:
        memcpy(p, &v, 8);
    }
}

static void *zalloc_aligned(E *e, size_t size, size_t align)
{
    void *p;
    if (align < sizeof(void *))
        align = sizeof(void *);
    if (!size)
        size = 1;
    size = (size + align - 1) / align * align;
    if (posix_memalign(&p, align, size))
        p = NULL;
    if (!p) {
        fputs("limba: out of memory\n", stderr);
        exit(70);
    }
    memset(p, 0, size);
    return keep(e, p);
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
        float x = (float)dv(a), y = (float)dv(b), z = (float)dv(c), r = 0;
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
            r = nearbyintf(x); /* the default mode: to nearest, ties even */
            break;
        case LIMBA_OP_FROUNDA:
            r = round_awayf(x);
            break;
        case LIMBA_OP_FMA:
            r = fmaf(x, y, z);
            break;
        }
        return fbits(r, t);
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
        double x = dv(a), y = dv(b);
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
    double d = dv(a), tr;
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
        if (from == LIMBA_T_I32) {
            uint32_t u = (uint32_t)a;
            float f;
            memcpy(&f, &u, 4);
            *r = fbits(f, LIMBA_T_F32);
        } else if (from == LIMBA_T_F32) {
            float f = (float)d;
            uint32_t u;
            memcpy(&u, &f, 4);
            *r = norm(u, LIMBA_T_I32);
        } else {
            *r = a; /* i64 and f64 share their bits */
        }
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
        ok = runtime(e, (uint32_t)in->imm, args, r);
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
    if (e->depth >= e->lim.max_depth) {
        e->status = LIMBA_EVAL_LIMIT;
        return false;
    }
    e->depth++;
    uint64_t *v = limba_xcalloc((size_t)f->ninsts + 1, sizeof(*v));
    void **slots = limba_xcalloc((size_t)f->nslots + 1, sizeof(*slots));
    for (uint32_t s = 0; s < f->nslots; s++)
        slots[s] = zalloc_aligned(e, f->slots[s].size, f->slots[s].align);
    const limba_block *bl = &f->blocks[0];
    for (uint32_t i = 0; i < bl->nparams; i++)
        v[bl->insts[i]] = args[i];
    bool ok = true;

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
                r = fbits(dv((uint64_t)in->imm), t);
                break;
            case LIMBA_F_SCONST: {
                size_t n;
                const char *s = limba_str(m, (limba_id)in->imm, &n);
                r = sv(str_make(e, s, n));
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
                r = load((void *)(uintptr_t)v[o[0]], t);
                break;
            case LIMBA_F_STORE:
                store((void *)(uintptr_t)v[o[1]], f->insts[o[0]].type, v[o[0]]);
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
                void *dst = (void *)(uintptr_t)v[o[0]];
                size_t len = (size_t)uv(v[o[2]], f->insts[o[2]].type);
                if (in->op == LIMBA_OP_MEMCPY)
                    memmove(dst, (void *)(uintptr_t)v[o[1]], len);
                else
                    memset(dst, (int)(uint8_t)v[o[1]], len);
                break;
            }
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
    free(v);
    free(slots); /* the slot memory itself is in the arena */
    e->depth--;
    return ok;
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

    /* globals, with their initial values */
    e.globals = limba_xcalloc((size_t)m->nglobals + 1, sizeof(void *));
    for (uint32_t i = 0; i < m->nglobals; i++) {
        const limba_global *g = &m->globals[i];
        const limba_type *ty = &m->types[g->type];
        void *p = zalloc_aligned(&e, ty->size, ty->align);
        e.globals[i] = p;
        if (g->init == LIMBA_INIT_INT)
            store(p, g->type, norm((uint64_t)g->value, g->type));
        else if (g->init == LIMBA_INIT_FLOAT)
            store(p, g->type, fbits(dv((uint64_t)g->value), g->type));
        else if (g->init == LIMBA_INIT_STR) {
            size_t k;
            const char *s = limba_str(m, (limba_id)g->value, &k);
            store(p, LIMBA_T_STR, sv(str_make(&e, s, k)));
        }
    }

    uint64_t ret = 0;
    call(&e, &m->funcs[fid], NULL, &ret);
    r->status = e.status;
    r->code = e.code;
    r->pos = e.pos;
    r->ret = e.status == LIMBA_EVAL_OK ? ret : 0;
    r->steps = e.steps;
    limba_w_byte(&e.out, 0);
    r->out = (char *)e.out.buf;
    r->outlen = e.out.len - 1;

    for (size_t i = 0; i < e.narena; i++)
        free(e.arena[i]);
    free(e.arena);
    free(e.globals);
}

void limba_eval_result_free(limba_eval_result *r)
{
    free(r->out);
    r->out = NULL;
}
