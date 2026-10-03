/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * gen.c - random Luxia programs and their expected output (see gen.h).
 *
 * The program is a small tree: types, expressions, statements, blocks,
 * routines. It is grown from the variables in scope, so it is valid by
 * construction: both operands of an operator have one type, a constant
 * never meets another constant (the front end would compute it, and
 * complain when it does not fit), a literal always fits where it goes,
 * loops count to a small bound. Functions are pure (no output, no global
 * written, no var parameter), so the order in which an expression calls
 * them does not show; routines call only the routines made before them.
 *
 * The run follows the rules of the specification: checked arithmetic on
 * IntN and UIntN, modular arithmetic on BitsN, div towards zero, mod with
 * the sign of the divisor, rem with the sign of the dividend, and and or
 * on Boolean that stop early, conversions that must hold the value (the
 * BitsN take the low bits), operations on a range done in its base type
 * and the range checked where a value is stored (assignment, declaration,
 * argument, return, the bounds of a for), array indices always checked;
 * operands, arguments and the items of writeln from left to right, each
 * item printed before the next is computed, the target of an assignment
 * before its value, continue in repeat going to the test. The run-time
 * error codes are not in the specification yet: 6 overflow, 11 division
 * by zero, 100 index, 101 range, 103 conversion, 104 shift. A BigInt is
 * computed with bigint.c, the arithmetic of lir_run too: this net watches
 * the front end and the back ends on it, test_front watches bigint.c.
 */
#include "gen.h"

#include "limba/fmt.h"
#include "common/xalloc.h"
#include "front/bigint.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef __int128 v128;
typedef unsigned __int128 u128;

/* the scalar types of the language; ranges and arrays follow them in the
   table of a program */
enum {
    T_I8,
    T_I16,
    T_I32,
    T_I64,
    T_U8,
    T_U16,
    T_U32,
    T_U64,
    T_B8,
    T_B16,
    T_B32,
    T_B64,
    T_BOOL,
    T_F64,
    T_F32,
    T_CHAR,
    T_STR,
    T_BIG,
    NTYPES
};

static const char *const tname[NTYPES] = {
    "Int8",    "Int16",   "Int32",   "Int64",  "UInt8",  "UInt16",
    "UInt32",  "UInt64",  "Bits8",   "Bits16", "Bits32", "Bits64",
    "Boolean", "Float64", "Float32", "Char",   "String", "BigInt",
};

/* S signed, U unsigned, B bits, L Boolean, F real, C Char, T String, N
   BigInt */
static char fam(unsigned t)
{
    return t == T_CHAR                ? 'C'
           : t == T_BIG               ? 'N'
           : t == T_STR               ? 'T'
           : t == T_F64 || t == T_F32 ? 'F'
           : t == T_BOOL              ? 'L'
           : t < T_U8                 ? 'S'
           : t < T_B8                 ? 'U'
                                      : 'B';
}

static bool is_int_base(unsigned t)
{
    return t < T_BOOL;
}

/* a real value travels as a double in the low 64 bits of a v128; a
   Float32 one is a double that a float holds exactly */
static v128 fbits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return (v128)u;
}

static double fval(v128 v)
{
    uint64_t u = (uint64_t)v;
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

/* d rounded to real type t: once, to float for a Float32 (+ - * / and
   sqrt done in double and rounded to float are rounded once) */
static v128 fres(unsigned t, double d)
{
    return fbits(t == T_F32 ? (double)(float)d : d);
}

static unsigned tbits(unsigned t)
{
    return 8u << (t & 3);
}

static v128 tmin(unsigned t)
{
    return fam(t) == 'S' ? -((v128)1 << (tbits(t) - 1)) : 0;
}

static v128 tmax(unsigned t)
{
    if (t == T_BOOL)
        return 1;
    if (fam(t) == 'S')
        return ((v128)1 << (tbits(t) - 1)) - 1;
    return ((v128)1 << tbits(t)) - 1;
}

/* K_RECORD: fields index..index+elem-1 of the table of fields; K_PTR:
   a pointer to record elem */
/* K_ENUM: elem values 0..elem-1; its base is the type itself */
/* K_DYN: an array variable whose bounds are computed, index a scalar
   type like K_OPEN; lo..hi is 0..3, room for its 0 to 4 elements */
/* K_HEAP: a pointer to the open array elem, whose arrays new makes with
   their bounds (§ 3.10, § 9.7) */
enum {
    K_BASE,
    K_RANGE,
    K_ARRAY,
    K_OPEN,
    K_RECORD,
    K_PTR,
    K_ENUM,
    K_DYN,
    K_HEAP
};

typedef struct {
    uint8_t k, base;      /* base: the scalar type of a range or of itself */
    uint32_t index, elem; /* of an open array, the index is a scalar */
    v128 lo, hi;          /* of a range; of the index of an array */
    uint32_t elo, ehi;    /* of a range: its bounds written as constant
                             expressions, 0 for literals */
} xt;

enum {
    O_ADD,
    O_SUB,
    O_MUL,
    O_DIV,
    O_MOD,
    O_REM,
    O_AND,
    O_OR,
    O_XOR,
    O_SHL,
    O_SHR,
    O_EQ,
    O_NE,
    O_LT,
    O_LE,
    O_GT,
    O_GE,
    O_NEG,
    O_ABS,
    O_NOT,
    O_POW,
    O_FDIV,
    O_CAT
};

/* the real functions of the library (§ 9), E_MATH */
enum {
    M_SQRT,
    M_SIN,
    M_COS,
    M_TAN,
    M_ARCTAN,
    M_EXP,
    M_LN,
    M_TRUNC,
    M_ROUND,
    M_FLOOR,
    M_CEIL,
    NMATH
};
static const char *const mtext[NMATH] = {"sqrt",   "sin",   "cos", "tan",
                                         "arctan", "exp",   "ln",  "trunc",
                                         "round",  "floor", "ceil"};

static const char *const otext[] = {
    "+",   "-",   "*",   "div", "mod", "rem", "and", "or",
    "xor", "shl", "shr", "=",   "<>",  "<",   "<=",  ">",
    ">=",  "-",   "abs", "not", "**",  "/",   "&",
};

/* E_IN: a in type fn, or a in b..c when fn is 0; E_LOW, E_HIGH, E_LEN:
   of the array variable var */
enum {
    E_LIT,
    E_VAR,
    E_BIN,
    E_UN,
    E_CONV,
    E_CALL,
    E_STR,
    E_INDEX,
    E_IN,
    E_LOW,
    E_HIGH,
    E_LEN,
    E_FIELD, /* field b of variable var, a record or a pointer to one */
    E_NIL,
    E_SUCC, /* succ, pred and ord of a */
    E_PRED,
    E_ORD,
    E_MATH,   /* function op of the library on the real a */
    E_SLEN,   /* length (op 0), low (1) or high (2) of the string a */
    E_SIDX,   /* byte b of the string variable var */
    E_COPY,   /* copy(a, b, c) */
    E_STRF,   /* str(a) */
    E_CHR,    /* chr(a) */
    E_FMT,    /* a:b, or a:b:c for a real, an item of writeln */
    E_CFIELD, /* field b of the record call a returns */
    E_CINDEX, /* element b of the array call a returns */
    E_VAL,    /* val(a, var): true when the string a is a number */
    E_READ,   /* readline(var) */
    E_ARGC,   /* argcount() */
    E_ARG,    /* arg(a) */
    E_HIDX,   /* element a of the array made by new variable var points to */
    E_HLEN,   /* low (op 0), high (1) or length (2) of var^, made by new */
    E_AGG     /* an aggregate of record or array type t (§ 6.8): the values
                 args, in the order written; by index (op 1) the index
                 literals in list a and the high ends in list b (0 for one
                 index); c the value of else, 0 for none; computed into the
                 hidden variable var */
};

/* a call of a function whose result is a record or an array has in var
   a hidden variable, where the run copies the result */
typedef struct {
    uint8_t k, op;
    bool cst; /* a constant: the front end knows its value, which is lit */
    uint32_t t;
    v128 lit;
    uint32_t var, fn, a, b, c, args, nargs;
    uint32_t line, col; /* where a run-time error here is reported */
} xe;

enum {
    S_VAR,
    S_ASSIGN,
    S_IF,
    S_WHILE,
    S_REPEAT,
    S_FOR,
    S_CASE,
    S_WRITE,
    S_CALL,
    S_EXIT,
    S_CONT,
    S_RETURN,
    S_CONST,
    S_NEW,     /* var := new(its record) */
    S_COPY,    /* var := the record variable of e */
    S_DISPOSE, /* dispose(var) */
    S_HALT,    /* halt(e) */
    /* arrays made by new: var := new(its open array range e..e2); var[idx]
       := e; move(var^, e, idx^, e2, fld) with idx the destination
       variable and fld the count; dispose(var) */
    S_HNEW,
    S_HSET,
    S_MOVE,
    S_HFREE,
    /* the routines of arrays (§ 9.5) on var^, from e, count fld:
       reverse; translate through a table zN[j] = j * fn + args, declared
       and filled before; writeln(occurrences(var^, e, fld, idx^)) */
    S_REV,
    S_TRANS,
    S_OCC,
    /* the standard streams in blocks (§ 9.1, § 9.2), from e, count fld:
       writeln(readbytes(var^, ...)) and writebytes(var^, ...) of an array
       of Bytes made by new; writebytes(var, ...) of a String;
       writeln(readbytes(zN, e, fld)) of an array zN: array[Int32 range
       1..4] of Byte declared before, the span inside */
    S_RDB,
    S_WRB,
    S_WRS,
    S_RDZ
};

typedef struct {
    uint8_t k;
    bool down, has_alt;
    uint32_t var, idx, e, e2; /* idx, e, e2: 0 for none (node 0 unused) */
    uint32_t blk, nblk;       /* the body */
    uint32_t alt, nalt;       /* else */
    uint32_t arm, narm;       /* if and case */
    uint32_t fn, args, nargs;
    uint32_t line, col;   /* the statement */
    uint32_t nline, ncol; /* the name it declares */
    uint32_t iline, icol; /* the [ or the . of its target */
    uint32_t fld;         /* a field of the target, plus 1; 0 for none */
} xs;

typedef struct {
    uint32_t cond;      /* if */
    uint32_t lab, nlab; /* case */
    uint32_t blk, nblk;
} xarm;

typedef struct {
    v128 lo, hi;
} xlab;

/* V_CONST: a constant without a type (t is Int64, for printing only);
   V_TCONST: a constant of type t */
/* V_OUT: an out parameter, copied back at the return (§ 8) */
enum {
    V_GLOBAL,
    V_LOCAL,
    V_IN,
    V_VAR,
    V_LOOP,
    V_COUNT,
    V_CONST,
    V_TCONST,
    V_OUT
};
static const char vprefix[] = {'g', 'v', 'a', 'a', 'i', 'w', 'k', 'k', 'a'};

typedef struct {
    uint32_t t;
    uint8_t kind;
    v128 val; /* of a constant */
} xv;

typedef struct {
    bool func;
    /* sealed: base types only, no global seen, only sealed routines
       called, so that it may go into a unit (§ 11) */
    bool sealed;
    uint32_t rt;
    uint32_t par[4];
    uint8_t np;
    uint32_t blk, nblk;
} xr;

typedef struct {
    uint64_t s;
    xt *ty;
    uint32_t nty, capty;
    xe *e;
    uint32_t ne, cape;
    xs *st;
    uint32_t ns, caps;
    xarm *arm;
    uint32_t narm, caparm;
    xlab *lab;
    uint32_t nlab, caplab;
    uint32_t *ls; /* blocks and argument lists */
    uint32_t nls, capls;
    xv *v;
    uint32_t nv, capv;
    xr *r;
    uint32_t nr, capr;
    uint32_t *scope; /* the visible variables */
    uint32_t nscope, capscope;
    int cur;     /* the routine being made, -1 for the program */
    int loops;   /* loops around the statement being made */
    int budget;  /* statements left */
    int nesting; /* blocks around the statement being made */
    uint32_t main_blk, main_n;
    uint32_t *gc; /* the statements of the global constants */
    uint32_t ngc, capgc;
    uint32_t *fld; /* the types of the fields of the records */
    uint32_t nfld, capfld;
    /* the strings: a String value is an index here; 0 is "", the literals
       come first, the run adds what it makes */
    struct xstr {
        char *b;
        uint32_t n;
    } *str;
    uint32_t nstr, capstr;
    /* the BigInt values, as the strings: a value is an index here, 0 is
       the number 0 */
    limba_big *big;
    uint32_t nbig, capbig;
    /* the command line and the lines of the input, strings; the input
       ends with a newline when nl_end */
    uint32_t arg[3], narg;
    uint32_t line[6], nline;
    bool nl_end;
    /* a program with the unit Lib (§ 11): the routines moved into it */
    bool units;
    uint8_t *moved;
    /* and the web of units around it, NULL for none */
    struct web *web;
    bool lib_named; /* the program names a routine of Lib */
} G;

static uint32_t new_str(G *g, const char *b, size_t n)
{
    LIMBA_GROW(g->str, g->nstr, g->capstr);
    g->str[g->nstr].b = limba_xmalloc(n + 1);
    memcpy(g->str[g->nstr].b, b, n);
    g->str[g->nstr].b[n] = 0;
    g->str[g->nstr].n = (uint32_t)n;
    return g->nstr++;
}

/* a BigInt value: t taken */
static uint32_t new_big(G *g, limba_big *t)
{
    if (limba_big_is_zero(t) && g->nbig) {
        limba_big_free(t);
        return 0;
    }
    LIMBA_GROW(g->big, g->nbig, g->capbig);
    g->big[g->nbig] = *t;
    limba_big_init(t);
    return g->nbig++;
}

static uint32_t big_of_i128(G *g, v128 v)
{
    limba_big t, hi;
    limba_big_init(&t);
    limba_big_init(&hi);
    u128 m = v < 0 ? -(u128)v : (u128)v;
    limba_big_set_u64(&hi, (uint64_t)(m >> 64));
    limba_big_shl_lim(&hi, &hi, 64, 256);
    limba_big_set_u64(&t, (uint64_t)m);
    limba_big_add_lim(&t, &t, &hi, 256);
    if (v < 0)
        limba_big_neg(&t, &t);
    limba_big_free(&hi);
    return new_big(g, &t);
}

/* the UTF-8 form of code point c into b; its length */
static unsigned utf8(uint32_t c, char *b)
{
    if (c < 0x80) {
        b[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        b[0] = (char)(0xc0 | c >> 6);
        b[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        b[0] = (char)(0xe0 | c >> 12);
        b[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    b[0] = (char)(0xf0 | c >> 18);
    b[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    b[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    b[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

/* the characters of the literals: ASCII, the quotes, letters of two,
   three and four bytes, then the control characters, which have names */
static const uint32_t some_chars[] = {
    'a', '~', '0', ' ', '\'', '"', 0xe9, 0x20ac, 0x1d11e, 10, 9, 13, 0};

/* ---- randomness ---- */

static uint64_t rnd(G *g)
{
    uint64_t z = (g->s += 0x9e3779b97f4a7c15ull); /* splitmix64 */
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint32_t below(G *g, uint32_t n)
{
    return n ? (uint32_t)(rnd(g) % n) : 0;
}

static bool chance(G *g, unsigned pct)
{
    return below(g, 100) < pct;
}

static unsigned int_type(G *g)
{
    return below(g, T_BOOL);
}

static unsigned any_type(G *g)
{
    return chance(g, 15)   ? T_BOOL
           : chance(g, 12) ? (chance(g, 50) ? T_F64 : T_F32)
           : chance(g, 6)  ? T_CHAR
                           : int_type(g);
}

static v128 clamp_in(v128 lo, v128 hi, v128 v)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* a value of lo..hi, often one at an edge */
static v128 rand_in(G *g, v128 lo, v128 hi)
{
    switch (below(g, 12)) {
    case 0:
        return lo;
    case 1:
        return hi;
    case 2:
        return clamp_in(lo, hi, lo + 1);
    case 3:
        return clamp_in(lo, hi, hi - 1);
    case 4:
        return clamp_in(lo, hi, lo < 0 ? -1 : 2);
    case 5: {
        u128 range = (u128)(hi - lo) + 1;
        u128 r = ((u128)rnd(g) << 64 | rnd(g)) % range;
        return lo + (v128)r;
    }
    default:
        return clamp_in(lo, hi, (v128)below(g, 41) - 20);
    }
}

/* a real for a literal: small binary fractions, decimals that are not,
   large and tiny ones */
static double rand_real(G *g)
{
    static const double some[] = {0.1,  0.5,     1.0,   2.5,    0.3,  1e16,
                                  1e-5, 1.5e-05, 1e300, 1e-300, 3.75, 100.0};
    double d;
    switch (below(g, 4)) {
    case 0:
        d = some[below(g, sizeof(some) / sizeof(some[0]))];
        break;
    case 1:
        d = (double)((int64_t)below(g, 2001) - 1000) / 8.0;
        break;
    case 2:
        d = (double)(rnd(g) >> 11) * 0x1p-53 * 1e6;
        break;
    default:
        d = (double)below(g, 100);
    }
    /* a constant is an exact rational (§ 4.4): -0.0 written is 0.0 */
    return chance(g, 20) && d != 0 ? -d : d;
}

/* a value of type t for a literal that must fit it */
static v128 init_value(G *g, uint32_t t);

static v128 rand_value(G *g, unsigned t)
{
    if (t == T_F64)
        return fbits(rand_real(g));
    if (t == T_CHAR)
        return some_chars[below(g, sizeof(some_chars) / sizeof(some_chars[0]) -
                                       (chance(g, 70) ? 4 : 0))];
    if (t == T_STR) {
        /* up to four pieces: words, quotes, letters of many bytes */
        static const char *const piece[] = {"a",
                                            "ciao",
                                            " ",
                                            "\"",
                                            "'",
                                            "\xc3\xa8",
                                            "\xe2\x82\xac",
                                            "\xf0\x9d\x84\x9e",
                                            "xyz",
                                            "0"};
        char b[64];
        size_t n = 0;
        for (uint32_t k = 0, m = below(g, 5); k < m; k++) {
            const char *p = piece[below(g, sizeof(piece) / sizeof(piece[0]))];
            memcpy(b + n, p, strlen(p));
            n += strlen(p);
        }
        return new_str(g, b, n);
    }
    if (t == T_F32) {
        /* within the range of a float, and not so tiny that it is 0: a
           literal must fit (§ 4.4) */
        double d = rand_real(g);
        if (fabs(d) > 1e38)
            d = copysign(1e30, d);
        if (d != 0 && fabs(d) < 1e-37)
            d = copysign(1e-30, d);
        return fres(T_F32, d);
    }
    if (t == T_BIG) {
        /* small, at the edges of 64 bits, or large: 2^k + c, 10^k - c */
        static const v128 edges[] = {0,
                                     1,
                                     -1,
                                     2,
                                     7,
                                     1000000007,
                                     (v128)INT64_MAX,
                                     (v128)INT64_MIN,
                                     (v128)UINT64_MAX,
                                     (v128)UINT64_MAX + 1};
        if (chance(g, 50))
            return big_of_i128(g, chance(g, 50) ? edges[below(g, 10)]
                                                : rand_in(g, -20, 20));
        limba_big t, c;
        limba_big_init(&t);
        limba_big_init(&c);
        limba_big_set_u64(&t, chance(g, 50) ? 2 : 10);
        limba_big_pow_lim(&t, &t, 20 + below(g, 180), 4096);
        limba_big_set_i64(&c, (int64_t)below(g, 7) - 3);
        limba_big_add_lim(&t, &t, &c, 4096);
        limba_big_free(&c);
        if (chance(g, 25))
            limba_big_neg(&t, &t);
        return new_big(g, &t);
    }
    return t == T_BOOL ? below(g, 2) : rand_in(g, tmin(t), tmax(t));
}

/* ---- types ---- */

static unsigned base(const G *g, uint32_t t)
{
    return g->ty[t].base;
}

static v128 lo_of(const G *g, uint32_t t)
{
    return g->ty[t].lo;
}

static v128 hi_of(const G *g, uint32_t t)
{
    return g->ty[t].hi;
}

static bool is_scalar(const G *g, uint32_t t)
{
    return g->ty[t].k == K_BASE || g->ty[t].k == K_RANGE ||
           g->ty[t].k == K_ENUM;
}

static bool is_enum(const G *g, uint32_t t)
{
    return t >= NTYPES && g->ty[t].k == K_ENUM;
}

static bool is_record(const G *g, uint32_t t)
{
    return g->ty[t].k == K_RECORD;
}

static bool is_ptr(const G *g, uint32_t t)
{
    return g->ty[t].k == K_PTR;
}

/* the record a variable of type t names: itself, or what it points to */
static uint32_t record_of(const G *g, uint32_t t)
{
    return is_ptr(g, t) ? g->ty[t].elem : t;
}

static bool is_array(const G *g, uint32_t t)
{
    return g->ty[t].k == K_ARRAY || g->ty[t].k == K_OPEN || g->ty[t].k == K_DYN;
}

/* an array type of the program, with variables: not one made for an
   aggregate given to an open parameter (ehi 1, § 6.8) */
static bool program_array(const G *g, uint32_t t)
{
    return g->ty[t].k == K_ARRAY && !g->ty[t].ehi;
}

/* the base type of the index of an array type */
static unsigned index_base(const G *g, uint32_t at)
{
    return g->ty[g->ty[at].index].base;
}

static v128 init_value(G *g, uint32_t t)
{
    unsigned b = g->ty[t].base;
    if (b == T_BOOL || fam(b) == 'F' || fam(b) == 'C' || fam(b) == 'T' ||
        fam(b) == 'N')
        return rand_value(g, b);
    return rand_in(g, g->ty[t].lo, g->ty[t].hi);
}

static uint32_t new_type(G *g, xt x)
{
    LIMBA_GROW(g->ty, g->nty, g->capty);
    g->ty[g->nty] = x;
    return g->nty++;
}

/* a range of an integer type (not Bits): short (the index of an array)
   or any */
static uint32_t new_range(G *g, bool short_range)
{
    unsigned b = below(g, T_B8);
    v128 lo = rand_value(g, b), hi;
    if (short_range) {
        lo = clamp_in(tmin(b), tmax(b) - 5, lo);
        hi = lo + below(g, 6);
    } else {
        hi = rand_value(g, b);
        if (hi < lo) {
            v128 x = lo;
            lo = hi;
            hi = x;
        }
    }
    return new_type(g, (xt){K_RANGE, (uint8_t)b, 0, 0, lo, hi, 0, 0});
}

static bool sealed(const G *g);

/* the type of a variable: a scalar, sometimes a range of the program */
static uint32_t var_type(G *g)
{
    if (sealed(g))
        return any_type(g);
    uint32_t n = 0, chosen = 0;
    for (uint32_t t = NTYPES; t < g->nty; t++)
        if (g->ty[t].k == K_RANGE && below(g, ++n) == 0)
            chosen = t;
    return n && chance(g, 30) ? chosen : any_type(g);
}

/* the type of a variable that is no field or element: a String too (the
   records and arrays of the program choose their own, Strings among
   them) */
static uint32_t var_type_s(G *g)
{
    return chance(g, 10) ? T_STR : chance(g, 8) ? T_BIG : var_type(g);
}

/* the type of a field or an element: a counted one (String, BigInt)
   pct times in 100 */
static uint32_t counted_or_var(G *g, unsigned pct)
{
    return chance(g, pct) ? (chance(g, 60) ? T_STR : T_BIG) : var_type(g);
}

/* ---- the tree ---- */

static uint32_t new_e(G *g, unsigned k, uint32_t t)
{
    LIMBA_GROW(g->e, g->ne, g->cape);
    memset(&g->e[g->ne], 0, sizeof(xe));
    g->e[g->ne].k = (uint8_t)k;
    g->e[g->ne].t = t;
    return g->ne++;
}

static uint32_t new_s(G *g, unsigned k)
{
    LIMBA_GROW(g->st, g->ns, g->caps);
    memset(&g->st[g->ns], 0, sizeof(xs));
    g->st[g->ns].k = (uint8_t)k;
    return g->ns++;
}

static uint32_t new_v(G *g, uint32_t t, unsigned kind)
{
    LIMBA_GROW(g->v, g->nv, g->capv);
    g->v[g->nv] = (xv){t, (uint8_t)kind, 0};
    return g->nv++;
}

static void show(G *g, uint32_t v)
{
    LIMBA_GROW(g->scope, g->nscope, g->capscope);
    g->scope[g->nscope++] = v;
}

/* copy a list built aside into the pool; its start */
static uint32_t keep_list(G *g, const uint32_t *x, uint32_t n)
{
    uint32_t at = g->nls;
    for (uint32_t i = 0; i < n; i++) {
        LIMBA_GROW(g->ls, g->nls, g->capls);
        g->ls[g->nls++] = x[i];
    }
    return at;
}

static uint32_t lit(G *g, uint32_t t, v128 v)
{
    uint32_t i = new_e(g, E_LIT, t);
    g->e[i].lit = v;
    g->e[i].cst = true;
    return i;
}

static bool is_const(const G *g, uint32_t v)
{
    return g->v[v].kind == V_CONST || g->v[v].kind == V_TCONST;
}

static uint32_t var_ref(G *g, uint32_t v)
{
    uint32_t i = new_e(g, E_VAR, g->v[v].t);
    g->e[i].var = v;
    if (is_const(g, v)) {
        g->e[i].cst = true;
        g->e[i].lit = g->v[v].val;
    }
    return i;
}

static uint32_t binop(G *g, unsigned op, uint32_t t, uint32_t a, uint32_t b)
{
    uint32_t i = new_e(g, E_BIN, t);
    g->e[i].op = (uint8_t)op;
    g->e[i].a = a;
    g->e[i].b = b;
    return i;
}

static bool in_function(const G *g)
{
    return g->cur >= 0 && g->r[g->cur].func;
}

static bool writable(const G *g, uint32_t v)
{
    unsigned k = g->v[v].kind;
    if (k == V_IN || k == V_LOOP || k == V_COUNT || is_const(g, v))
        return false;
    return !(k == V_GLOBAL && in_function(g));
}

/* a visible scalar variable whose type has base t (NTYPES: any integer;
   exact: the type is t itself); -1 if none */
static int pick_var(G *g, uint32_t t, bool write, bool exact)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        uint32_t vt = g->v[v].t;
        if (!is_scalar(g, vt) || is_const(g, v))
            continue;
        if (exact         ? vt != t
            : t == NTYPES ? !is_int_base(base(g, vt))
                          : base(g, vt) != t)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a visible array whose elements have base t (NTYPES: any); -1 if none */
static int pick_array(G *g, uint32_t t, bool write)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        uint32_t vt = g->v[v].t;
        if (!is_array(g, vt))
            continue;
        if (t != NTYPES && base(g, g->ty[vt].elem) != t)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a function made before the current routine, whose result has base t;
   -1 if none */
/* the routine being made is sealed: it calls only sealed ones */
static bool sealed(const G *g)
{
    return g->cur >= 0 && g->r[g->cur].sealed;
}

static int pick_func(G *g, unsigned t)
{
    uint32_t top = g->cur >= 0 ? (uint32_t)g->cur : g->nr, n = 0, chosen = 0;
    for (uint32_t i = 0; i < top; i++)
        if (g->r[i].func && is_scalar(g, g->r[i].rt) &&
            (!sealed(g) || g->r[i].sealed) && base(g, g->r[i].rt) == t &&
            below(g, ++n) == 0)
            chosen = i;
    return n ? (int)chosen : -1;
}

/* a function made before the current routine whose result is of type t
   exactly, a record or an array (0: any of them); -1 if none */
static int pick_agg_func(G *g, uint32_t t)
{
    uint32_t top = g->cur >= 0 ? (uint32_t)g->cur : g->nr, n = 0, chosen = 0;
    for (uint32_t i = 0; i < top; i++) {
        uint32_t rt = g->r[i].rt;
        if (!g->r[i].func || is_scalar(g, rt) || (t && rt != t) ||
            (sealed(g) && !g->r[i].sealed))
            continue;
        if (below(g, ++n) == 0)
            chosen = i;
    }
    return n ? (int)chosen : -1;
}

static uint32_t expr(G *g, unsigned t, int d, bool need_var);

/* a value to store where type t is: a literal is made to fit it, any
   other value is checked when stored */
static uint32_t value_for(G *g, uint32_t t, int d)
{
    uint32_t e = expr(g, base(g, t), d, false);
    /* a constant must fit where it goes, or the front end refuses it */
    if (g->e[e].cst && is_int_base(base(g, t)) &&
        (g->e[e].lit < lo_of(g, t) || g->e[e].lit > hi_of(g, t)))
        e = lit(g, t, rand_in(g, lo_of(g, t), hi_of(g, t)));
    return e;
}

static int depth(G *g)
{
    return 1 + (int)below(g, 4);
}

/* an index of array type at: a literal inside it, or any value */
/* low(a), high(a) or length(a) of array variable v: constants for an
   array of the program, values for an open parameter */
static uint32_t bound(G *g, unsigned k, uint32_t v)
{
    uint32_t at = g->v[v].t;
    /* low and high in the base of the index, length an Int64 (§ 3.7) */
    uint32_t i = new_e(g, k, k == E_LEN ? T_I64 : index_base(g, at));
    g->e[i].var = v;
    if (g->ty[at].k == K_ARRAY) {
        const xt *x = &g->ty[at];
        g->e[i].cst = true;
        g->e[i].lit = k == E_LOW    ? x->lo
                      : k == E_HIGH ? x->hi
                                    : x->hi - x->lo + 1;
    }
    return i;
}

/* an index of array variable v: a literal inside a static array, or low
   and high and near them, or any value */
static uint32_t index_for(G *g, uint32_t v, int d)
{
    uint32_t at = g->v[v].t;
    if ((g->ty[at].k == K_OPEN || g->ty[at].k == K_DYN) && chance(g, 70)) {
        unsigned b = index_base(g, at);
        uint32_t e = bound(g, chance(g, 50) ? E_LOW : E_HIGH, v);
        if (chance(g, 50))
            e = binop(g, g->e[e].k == E_LOW ? O_ADD : O_SUB, b, e,
                      lit(g, b, below(g, 3)));
        return e;
    }
    return value_for(g, g->ty[at].index, d);
}

/* a visible array variable whose index has base b (any for NTYPES);
   -1 if none */
static int pick_indexed(G *g, unsigned b)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        if (is_array(g, g->v[v].t) &&
            (b == NTYPES || index_base(g, g->v[v].t) == b) &&
            below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a visible array variable to pass to open parameter type ot: an index
   of the same base, the same elements; -1 if none */
static int pick_argument(G *g, uint32_t ot, bool write)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i], vt = g->v[v].t;
        if (!is_array(g, vt) || index_base(g, vt) != index_base(g, ot) ||
            g->ty[vt].elem != g->ty[ot].elem)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

static uint32_t element(G *g, uint32_t v, int d)
{
    uint32_t at = g->v[v].t;
    uint32_t ix = index_for(g, v, d - 1);
    uint32_t i = new_e(g, E_INDEX, g->ty[at].elem);
    g->e[i].var = v;
    g->e[i].a = ix;
    return i;
}

static uint32_t typed_arg(G *g, uint32_t pt, bool write);
static uint32_t ncells(const G *g, uint32_t t);
static uint32_t agg_literal(G *g, uint32_t t, int d);
static uint32_t agg_self(G *g, uint32_t t, int d, uint32_t self);
static uint32_t field_ref(G *g, uint32_t v, uint32_t f);
static uint32_t field_type(const G *g, uint32_t rt, uint32_t f);
static int pick_typed(G *g, uint32_t t, bool write);

static uint32_t call_expr(G *g, uint32_t fn, int d)
{
    uint32_t a[4];
    uint32_t np = g->r[fn].np;
    for (uint32_t k = 0; k < np; k++) {
        uint32_t pt = g->v[g->r[fn].par[k]].t;
        if (g->ty[pt].k == K_OPEN) {
            /* the array its type came from is a global: always there; or
               an aggregate, read only */
            a[k] = g->v[g->r[fn].par[k]].kind == V_IN && d > 0 && chance(g, 40)
                       ? agg_literal(g, pt, 1)
                       : var_ref(g, (uint32_t)pick_argument(g, pt, false));
        } else if (is_record(g, pt) || is_ptr(g, pt)) {
            /* a global of every such type exists; a record may come from
               a call */
            int h = is_record(g, pt) && d > 0 && chance(g, 30)
                        ? pick_agg_func(g, pt)
                        : -1;
            a[k] = h >= 0 ? call_expr(g, (uint32_t)h, d - 1)
                   : is_record(g, pt) && d > 0 && chance(g, 20) &&
                           g->v[g->r[fn].par[k]].kind == V_IN
                       ? agg_literal(g, pt, d - 1)
                       : typed_arg(g, pt, false);
        } else {
            a[k] = value_for(g, pt, d - 1);
        }
    }
    uint32_t rt = g->r[fn].rt;
    uint32_t hidden =
        is_record(g, rt) || is_array(g, rt) ? new_v(g, rt, V_LOCAL) : 0;
    uint32_t i = new_e(g, E_CALL, rt);
    g->e[i].fn = fn;
    g->e[i].args = keep_list(g, a, np);
    g->e[i].nargs = np;
    g->e[i].var = hidden;
    return i;
}

/* d := f(...) for a function whose result is a record or an array of
   the type of variable d, and when show, often all of d printed at once,
   so that a wrong copy shows; into out, the count */
static uint32_t copy_show(G *g, uint32_t e, uint32_t d, bool show,
                          uint32_t *out);

static uint32_t copy_call(G *g, uint32_t fn, uint32_t d, bool show,
                          uint32_t *out)
{
    return copy_show(g, call_expr(g, fn, depth(g)), d, show, out);
}

/* d := e, e a record or an array of the type of d, and when show often
   all of d printed at once */
static uint32_t copy_show(G *g, uint32_t e, uint32_t d, bool show,
                          uint32_t *out)
{
    uint32_t s = new_s(g, S_COPY);
    g->st[s].var = d;
    g->st[s].e = e;
    out[0] = s;
    if (!show || chance(g, 30))
        return 1;
    uint32_t t = g->v[d].t, items[16], k = 0;
    for (uint32_t j = 0; j < ncells(g, t) && k < 15; j++) {
        if (k)
            items[k++] = new_e(g, E_STR, 0);
        uint32_t it;
        if (is_record(g, t)) {
            it = field_ref(g, d, j);
        } else {
            uint32_t ix = lit(g, g->ty[t].index, g->ty[t].lo + j);
            it = new_e(g, E_INDEX, g->ty[t].elem);
            g->e[it].var = d;
            g->e[it].a = ix;
        }
        if (is_enum(g, g->e[it].t)) {
            uint32_t o = new_e(g, E_ORD, T_U32);
            g->e[o].a = it;
            it = o;
        }
        items[k++] = it;
    }
    uint32_t w = new_s(g, S_WRITE);
    g->st[w].args = keep_list(g, items, k);
    g->st[w].nargs = k;
    out[1] = w;
    return 2;
}

/* a record or an array of type t: a visible variable, or a call */
static uint32_t nil_of(G *g, uint32_t pt);

/* a component of an aggregate of type t: a narrow one gets a value that
   fits (a variable of its type, or a literal), so that no range check
   fails at the component, whose place the run does not know */
static uint32_t agg_component(G *g, uint32_t t, int d)
{
    if (is_ptr(g, t)) {
        int v = chance(g, 50) ? pick_typed(g, t, false) : -1;
        return v >= 0 ? var_ref(g, (uint32_t)v) : nil_of(g, t);
    }
    if (g->ty[t].k == K_RANGE) {
        int v = chance(g, 50) ? pick_typed(g, t, false) : -1;
        if (v >= 0 && is_scalar(g, g->v[v].t))
            return var_ref(g, (uint32_t)v);
        return lit(g, t, init_value(g, t));
    }
    return value_for(g, t, d);
}

/* a part of variable self of type t, read in an aggregate given to self
   (§ 6.8: the aggregate is a value); 0 if none fits */
static uint32_t self_part(G *g, uint32_t self, uint32_t t)
{
    uint32_t st = g->v[self].t, n = 0, chosen = 0;
    if (is_record(g, st)) {
        for (uint32_t f = 0; f < g->ty[st].elem; f++)
            if (field_type(g, st, f) == t && below(g, ++n) == 0)
                chosen = f;
        return n ? field_ref(g, self, chosen) : 0;
    }
    if (g->ty[st].k != K_ARRAY || g->ty[st].elem != t)
        return 0;
    uint32_t len = ncells(g, st);
    uint32_t ix = lit(g, g->ty[st].index, g->ty[st].lo + below(g, len));
    uint32_t i = new_e(g, E_INDEX, t);
    g->e[i].var = self;
    g->e[i].a = ix;
    return i;
}

static uint32_t agg_literal(G *g, uint32_t t, int d)
{
    return agg_self(g, t, d, UINT32_MAX);
}

static uint32_t agg_part_of(G *g, uint32_t t, int d, uint32_t self)
{
    uint32_t e =
        self != UINT32_MAX && chance(g, 40) ? self_part(g, self, t) : 0;
    return e ? e : agg_component(g, t, d);
}

/* an aggregate of record or fixed array type t, whose components read
   self often when it is a variable; for an open-array parameter (open
   its type), positional, of a type of its own from 0 */
static uint32_t agg_self(G *g, uint32_t t, int d, uint32_t self)
{
    uint32_t vals[16], los[16], his[16], n = 0, other = 0;
    bool named = false, open = g->ty[t].k == K_OPEN;
    if (open) {
        /* n elements from 0: the index of an open parameter is an integer
           type, which has 0 (§ 6.8) */
        unsigned ib = g->ty[t].index;
        uint32_t m = 1 + below(g, 4);
        uint32_t ix =
            new_type(g, (xt){K_RANGE, (uint8_t)ib, 0, 0, 0, (v128)m - 1, 0, 0});
        t = new_type(g, (xt){K_ARRAY, g->ty[t].base, ix, g->ty[t].elem, 0,
                             (v128)m - 1, 0, 1});
    }
    if (is_record(g, t)) {
        for (uint32_t f = 0; f < g->ty[t].elem && n < 16; f++)
            vals[n++] = agg_part_of(g, field_type(g, t, f), d - 1, self);
    } else {
        uint32_t len = ncells(g, t), et = g->ty[t].elem, it = g->ty[t].index;
        v128 lo = g->ty[t].lo;
        /* for an open parameter positional only: no bounds for else */
        unsigned form = len > 16 || open ? 0 : below(g, 4);
        if (form == 0 || form == 1) {
            /* positional, all; or some, then else */
            uint32_t m = form == 0 || len < 2 ? len : 1 + below(g, len - 1);
            for (uint32_t k = 0; k < m; k++)
                vals[n++] = agg_part_of(g, et, d - 1, self);
            if (m < len)
                other = agg_part_of(g, et, d - 1, self);
        } else {
            /* by index, in any order, some as ranges; the rest by else
               when form is 3 */
            uint32_t order[16];
            for (uint32_t k = 0; k < len; k++)
                order[k] = k;
            for (uint32_t k = len; k > 1; k--) {
                uint32_t j = below(g, k), x = order[k - 1];
                order[k - 1] = order[j];
                order[j] = x;
            }
            /* runs of consecutive indices, from a random cut */
            uint8_t taken[16] = {0};
            uint32_t skip = form == 3 && len > 1 ? 1 + below(g, len - 1) : 0;
            for (uint32_t k = 0; k < skip; k++)
                taken[order[k]] = 1;
            for (uint32_t k = 0; k < len; k++) {
                uint32_t a = order[k];
                if (taken[a])
                    continue;
                uint32_t b = a;
                while (chance(g, 40) && b + 1 < len && !taken[b + 1])
                    b++;
                for (uint32_t j = a; j <= b; j++)
                    taken[j] = 1;
                los[n] = lit(g, it, lo + a);
                his[n] = b > a ? lit(g, it, lo + b) : 0;
                vals[n++] = agg_part_of(g, et, d - 1, self);
            }
            named = true;
            if (skip)
                other = agg_part_of(g, et, d - 1, self);
        }
    }
    uint32_t i = new_e(g, E_AGG, t);
    g->e[i].args = keep_list(g, vals, n);
    g->e[i].nargs = n;
    g->e[i].op = named;
    if (named) {
        g->e[i].a = keep_list(g, los, n);
        g->e[i].b = keep_list(g, his, n);
    }
    g->e[i].c = other;
    g->e[i].var = new_v(g, t, V_LOCAL);
    return i;
}

static uint32_t agg_expr(G *g, uint32_t t, int d)
{
    if (chance(g, 35))
        return agg_literal(g, t, d);
    int h = d > 0 && chance(g, 40) ? pick_agg_func(g, t) : -1;
    if (h >= 0)
        return call_expr(g, (uint32_t)h, d - 1);
    return var_ref(g, (uint32_t)pick_typed(g, t, false));
}

/* f(...).x or f(...)[i] of base t, from a function whose result is a
   record or an array; 0 if none fits */
static uint32_t agg_part(G *g, unsigned t, int d)
{
    int fn = pick_agg_func(g, 0);
    if (fn < 0)
        return 0;
    uint32_t rt = g->r[fn].rt;
    if (is_record(g, rt)) {
        uint32_t n = 0, f = 0;
        for (uint32_t k = 0; k < g->ty[rt].elem; k++)
            if (base(g, field_type(g, rt, k)) == t && below(g, ++n) == 0)
                f = k;
        if (!n)
            return 0;
        uint32_t c = call_expr(g, (uint32_t)fn, d - 1);
        uint32_t i = new_e(g, E_CFIELD, field_type(g, rt, f));
        g->e[i].a = c;
        g->e[i].b = f;
        return i;
    }
    if (base(g, g->ty[rt].elem) != t)
        return 0;
    uint32_t c = call_expr(g, (uint32_t)fn, d - 1);
    uint32_t ix = value_for(g, g->ty[rt].index, d - 1);
    uint32_t i = new_e(g, E_CINDEX, g->ty[rt].elem);
    g->e[i].a = c;
    g->e[i].b = ix;
    return i;
}

static uint32_t conv(G *g, uint32_t t, uint32_t a)
{
    uint32_t i = new_e(g, E_CONV, t);
    g->e[i].a = a;
    return i;
}

/* an integer type whose values all fit t: a conversion that holds */
static unsigned within(G *g, unsigned t)
{
    unsigned c[NTYPES], n = 0;
    for (unsigned u = 0; u < T_BOOL; u++)
        if (fam(t) == 'B' || (tmin(u) >= tmin(t) && tmax(u) <= tmax(t)))
            c[n++] = u;
    return c[below(g, n)];
}

/* ---- records and pointers ---- */

static uint32_t field_type(const G *g, uint32_t rt, uint32_t f)
{
    return g->fld[g->ty[rt].index + f];
}

/* a field of a record variable, or through a pointer variable, whose type
   has base t (NTYPES: any); its index in *f; -1 if none. write: the
   field is to be assigned (through a pointer only where the heap may be
   written, not in a function) */
static int pick_field(G *g, unsigned t, bool write, uint32_t *f)
{
    uint32_t n = 0;
    int chosen = -1;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i], vt = g->v[v].t;
        if (is_const(g, v) || !(is_record(g, vt) || is_ptr(g, vt)))
            continue;
        if (write && (is_record(g, vt) ? !writable(g, v) : in_function(g)))
            continue;
        uint32_t rt = record_of(g, vt);
        for (uint32_t k = 0; k < g->ty[rt].elem; k++) {
            if (t != NTYPES && base(g, field_type(g, rt, k)) != t)
                continue;
            if (below(g, ++n) == 0) {
                chosen = (int)v;
                *f = k;
            }
        }
    }
    return chosen;
}

/* the first value of a field or an element of type t: a literal, or for
   a String often one made at run time (str of global 0, the integer one,
   after a literal), which memory counts, unlike a literal */
static uint32_t made_str(G *g);

static uint32_t init_expr(G *g, uint32_t t)
{
    if (base(g, t) != T_STR || chance(g, 50))
        return lit(g, t, init_value(g, t));
    return made_str(g);
}

/* a String made at run time: str(g0), sometimes after a literal */
static uint32_t made_str(G *g)
{
    uint32_t a = var_ref(g, 0); /* before: new_e may move g->e */
    uint32_t i = new_e(g, E_STRF, T_STR);
    g->e[i].a = a;
    if (chance(g, 50))
        return i;
    uint32_t l = lit(g, T_STR, rand_value(g, T_STR));
    return binop(g, O_CAT, T_STR, l, i);
}

static uint32_t field_ref(G *g, uint32_t v, uint32_t f)
{
    uint32_t i = new_e(g, E_FIELD, field_type(g, record_of(g, g->v[v].t), f));
    g->e[i].var = v;
    g->e[i].b = f;
    return i;
}

/* a visible variable of type t exactly (a record or a pointer) that is
   not a constant, writable if asked; with a predicate of kind instead
   when t is 0: 1 records, 2 pointers; -1 if none */
static int pick_typed(G *g, uint32_t t, bool write)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i], vt = g->v[v].t;
        if (is_const(g, v))
            continue;
        if (t == 1 ? !is_record(g, vt) : t == 2 ? !is_ptr(g, vt) : vt != t)
            continue;
        if (write && !writable(g, v))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* a range narrower than its base: new gives its fields a value outside
   (luxia_0.md § 4.5), which *bad receives */
static bool narrow_bad(const G *g, uint32_t t, v128 *bad)
{
    if (g->ty[t].k != K_RANGE)
        return false;
    unsigned b = g->ty[t].base;
    if (g->ty[t].lo > tmin(b)) {
        *bad = g->ty[t].lo - 1;
        return true;
    }
    if (g->ty[t].hi < tmax(b)) {
        *bad = g->ty[t].hi + 1;
        return true;
    }
    return false;
}

static uint32_t nil_of(G *g, uint32_t pt)
{
    return new_e(g, E_NIL, pt);
}

/* an argument for a record or pointer parameter of type pt: a variable
   of that type (writable for a var parameter), or nil; 0 if none */
static uint32_t typed_arg(G *g, uint32_t pt, bool write)
{
    if (is_ptr(g, pt) && chance(g, 25))
        return nil_of(g, pt);
    int v = pick_typed(g, pt, write);
    return v < 0 ? 0 : var_ref(g, (uint32_t)v);
}

/* p := new(R) and a value for each field, p := q or nil, r1 := r2: into
   out, the count (0 if nothing fits) */
static uint32_t heap_stmt(G *g, uint32_t *out)
{
    if (chance(g, 30)) {
        int d = pick_typed(g, 1, true);
        if (d < 0)
            return 0;
        int src = pick_typed(g, g->v[d].t, false);
        for (int k = 0; k < 4 && src == d; k++)
            src = pick_typed(g, g->v[d].t, false); /* another, if any */
        uint32_t s = new_s(g, S_COPY);
        g->st[s].var = (uint32_t)d;
        /* or a whole value, which may read d itself (§ 6.8) */
        g->st[s].e = chance(g, 40)
                         ? agg_self(g, g->v[d].t, depth(g), (uint32_t)d)
                         : var_ref(g, (uint32_t)src);
        out[0] = s;
        return 1;
    }
    int p = pick_typed(g, 2, true);
    if (p < 0)
        return 0;
    uint32_t pt = g->v[p].t, rt = g->ty[pt].elem;
    if (chance(g, 60)) {
        uint32_t n = 0;
        uint32_t s = new_s(g, S_NEW);
        g->st[s].var = (uint32_t)p;
        out[n++] = s;
        for (uint32_t k = 0; k < g->ty[rt].elem; k++) {
            /* literals: a field of a new record has no value yet, and an
               expression could read one; some stay without a value */
            if (chance(g, 45))
                continue;
            uint32_t ft = field_type(g, rt, k);
            /* a String made at run time: dispose must release it */
            uint32_t e = base(g, ft) == T_STR ? made_str(g) : init_expr(g, ft);
            uint32_t a = new_s(g, S_ASSIGN);
            g->st[a].var = (uint32_t)p;
            g->st[a].fld = k + 1;
            g->st[a].e = e;
            out[n++] = a;
        }
        if (chance(g, 50)) {
            /* read a field at once: without a value, it is caught */
            uint32_t f = below(g, g->ty[rt].elem);
            uint32_t item = field_ref(g, (uint32_t)p, f);
            uint32_t w = new_s(g, S_WRITE);
            g->st[w].args = keep_list(g, &item, 1);
            g->st[w].nargs = 1;
            out[n++] = w;
        }
        if (chance(g, 40)) {
            /* given back at once, when nothing else can point to it:
               dispose(p); p := nil */
            uint32_t ds = new_s(g, S_DISPOSE);
            g->st[ds].var = (uint32_t)p;
            out[n++] = ds;
            uint32_t z = nil_of(g, pt);
            uint32_t a = new_s(g, S_ASSIGN);
            g->st[a].var = (uint32_t)p;
            g->st[a].e = z;
            out[n++] = a;
        }
        return n;
    }
    int q = pick_typed(g, pt, false);
    uint32_t e =
        q >= 0 && chance(g, 70) ? var_ref(g, (uint32_t)q) : nil_of(g, pt);
    uint32_t s = new_s(g, S_ASSIGN);
    g->st[s].var = (uint32_t)p;
    g->st[s].e = e;
    out[0] = s;
    return 1;
}

/* ---- constants computed exactly ---- */

#define CONST_LIMIT ((v128)1 << 100)

static bool small(v128 v)
{
    return v <= CONST_LIMIT && v >= -CONST_LIMIT;
}

/* a * b within the limit, into *out */
static bool mul_small(v128 a, v128 b, v128 *out)
{
    v128 ma = a < 0 ? -a : a, mb = b < 0 ? -b : b;
    if (ma && mb > CONST_LIMIT / ma)
        return false;
    *out = a * b;
    return true;
}

static uint32_t cst_node(G *g, uint32_t i, v128 v)
{
    g->e[i].cst = true;
    g->e[i].lit = v;
    return i;
}

/* an exact constant expression without a type (§ 4.4): literals, the
   constants without a type in scope, + - * div mod rem **, abs and the
   minus; the front end computes it with no limit, here every value stays
   within 2^100, and its value is in lit */
static uint32_t cexpr(G *g, int d)
{
    if (d <= 0 || chance(g, 30)) {
        uint32_t n = 0, c = 0;
        for (uint32_t i = 0; i < g->nscope; i++) {
            uint32_t v = g->scope[i];
            if (g->v[v].kind == V_CONST && small(g->v[v].val) &&
                below(g, ++n) == 0)
                c = v;
        }
        if (n && chance(g, 50))
            return var_ref(g, c);
        v128 v = chance(g, 20) ? (v128)(rnd(g) >> 24) : (v128)below(g, 41) - 20;
        return lit(g, T_I64, chance(g, 10) ? -v : v);
    }
    unsigned op = below(g, 9);
    uint32_t a = cexpr(g, d - 1);
    v128 x = g->e[a].lit, v;
    if (op >= 7) {
        uint32_t i = new_e(g, E_UN, T_I64);
        g->e[i].op = op == 7 ? O_NEG : O_ABS;
        g->e[i].a = a;
        return cst_node(g, i, op == 7 || x < 0 ? -x : x);
    }
    if (op == 6) {
        unsigned n = below(g, 6);
        v = 1;
        for (unsigned k = 0; k < n; k++)
            if (!mul_small(v, x, &v))
                return a;
        uint32_t e = lit(g, T_I64, n);
        return cst_node(g, binop(g, O_POW, T_I64, a, e), v);
    }
    uint32_t b = cexpr(g, d - 1);
    v128 y = g->e[b].lit;
    switch (op) {
    case 0:
        v = x + y;
        break;
    case 1:
        v = x - y;
        break;
    case 2:
        if (!mul_small(x, y, &v))
            return a;
        break;
    default:
        if (y == 0)
            return a; /* the front end refuses a constant division by 0 */
        v = op == 3 ? x / y : x % y;
        if (op == 4 && v != 0 && (v < 0) != (y < 0))
            v += y; /* mod has the sign of the divisor */
    }
    if (!small(v))
        return a;
    static const unsigned ops[] = {O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_REM};
    return cst_node(g, binop(g, ops[op], T_I64, a, b), v);
}

/* a constant expression that fits type t: (e) mod 97 when e does not */
static uint32_t cexpr_in(G *g, uint32_t t, int d)
{
    uint32_t e = cexpr(g, d);
    v128 v = g->e[e].lit;
    if (v >= lo_of(g, t) && v <= hi_of(g, t))
        return e;
    v128 m = v % 97;
    if (m < 0)
        m += 97;
    if (m >= lo_of(g, t) && m <= hi_of(g, t))
        return cst_node(g, binop(g, O_MOD, T_I64, e, lit(g, T_I64, 97)), m);
    return lit(g, t, rand_in(g, lo_of(g, t), hi_of(g, t)));
}

/* const k = e; or const k: T = e; its statement */
static uint32_t constant(G *g)
{
    uint32_t s = new_s(g, S_CONST);
    uint32_t t = T_I64, e;
    unsigned kind = V_CONST;
    if (chance(g, 40)) {
        t = var_type(g);
        if (!is_int_base(base(g, t)))
            t = int_type(g);
        kind = V_TCONST;
        e = cexpr_in(g, t, depth(g));
    } else {
        e = cexpr(g, depth(g));
    }
    uint32_t v = new_v(g, t, kind);
    g->v[v].val = g->e[e].lit;
    g->st[s].var = v;
    g->st[s].e = e;
    show(g, v);
    return s;
}

/* a constant of base t: one declared, or an expression of constants */
static uint32_t const_leaf(G *g, unsigned t)
{
    uint32_t n = 0, c = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i];
        const xv *x = &g->v[v];
        bool ok =
            x->kind == V_TCONST
                ? base(g, x->t) == t
                : x->kind == V_CONST && x->val >= tmin(t) && x->val <= tmax(t);
        if (ok && below(g, ++n) == 0)
            c = v;
    }
    if (n && chance(g, 50))
        return var_ref(g, c);
    return cexpr_in(g, t, 1 + (int)below(g, 3));
}

/* ---- enumerations ---- */

/* an enumeration of the program; 0 if none */
static uint32_t pick_enum(G *g)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t t = NTYPES; t < g->nty && !sealed(g); t++)
        if (g->ty[t].k == K_ENUM && below(g, ++n) == 0)
            chosen = t;
    return chosen;
}

/* a value of enumeration et: a literal, a variable, succ or pred */
static uint32_t enum_expr(G *g, uint32_t et, int d, bool need_var)
{
    int v = pick_var(g, et, false, true);
    if (!need_var && (v < 0 || chance(g, 30)))
        return lit(g, et, below(g, (uint32_t)g->ty[et].elem));
    if (v < 0) /* every enumeration has global variables: not here */
        return lit(g, et, 0);
    if (d > 0 && chance(g, 30)) {
        uint32_t a = enum_expr(g, et, d - 1, true);
        uint32_t i = new_e(g, chance(g, 50) ? E_SUCC : E_PRED, et);
        g->e[i].a = a;
        return i;
    }
    return var_ref(g, (uint32_t)v);
}

/* a variable, an element, a literal, or a variable of another type made
   into t */
static uint32_t str_expr(G *g, int d, bool need_var);
static uint32_t char_expr(G *g, int d, bool need_var);

static uint32_t val_expr(G *g, int d);
static uint32_t read_expr(G *g);
static uint32_t arg_expr(G *g);

static uint32_t leaf(G *g, unsigned t, bool need_var, int d)
{
    if (!need_var && is_int_base(t) && chance(g, 12))
        return const_leaf(g, t);
    if (t == T_BOOL && d > 0 && chance(g, 6)) {
        uint32_t r = chance(g, 50) ? val_expr(g, d) : read_expr(g);
        if (r)
            return r;
    }
    if (t == T_I32 && chance(g, 3))
        return new_e(g, E_ARGC, T_I32);
    uint32_t et = t == T_U32 && chance(g, 15) ? pick_enum(g) : 0;
    if (et) {
        uint32_t a = enum_expr(g, et, d - 1, true);
        uint32_t i = new_e(g, E_ORD, T_U32);
        g->e[i].a = a;
        return i;
    }
    if (chance(g, 15)) {
        uint32_t f;
        int v = pick_field(g, t, false, &f);
        if (v >= 0)
            return field_ref(g, (uint32_t)v, f);
    }
    if (d > 0 && chance(g, 25)) {
        uint32_t p = agg_part(g, t, d);
        if (p)
            return p;
    }
    if (t == T_I64 && d > 0 && chance(g, 6)) {
        uint32_t a = str_expr(g, d - 1, true);
        uint32_t i = new_e(g, E_SLEN, T_I64);
        g->e[i].op = (uint8_t)(chance(g, 60) ? 0 : 1 + below(g, 2));
        g->e[i].a = a;
        return i;
    }
    int sv = t == T_B8 && chance(g, 15) ? pick_var(g, T_STR, false, false) : -1;
    if (sv >= 0) {
        /* s[i]: a byte, from 1 to length(s), checked */
        uint32_t ix;
        if (chance(g, 50)) {
            ix = lit(g, T_I64, 1 + below(g, 4));
        } else if (chance(g, 50)) {
            uint32_t a = var_ref(g, (uint32_t)sv);
            ix = new_e(g, E_SLEN, T_I64);
            g->e[ix].a = a;
        } else {
            ix = value_for(g, T_I64, d - 1);
        }
        uint32_t i = new_e(g, E_SIDX, T_B8);
        g->e[i].var = (uint32_t)sv;
        g->e[i].b = ix;
        return i;
    }
    if (t == T_U32 && d > 0 && chance(g, 8)) {
        uint32_t a = char_expr(g, d - 1, true);
        uint32_t i = new_e(g, E_ORD, T_U32);
        g->e[i].a = a;
        return i;
    }
    int v = pick_var(g, t, false, false);
    int a = d > 0 && chance(g, 25) ? pick_array(g, t, false) : -1;
    if (a >= 0)
        return element(g, (uint32_t)a, d);
    int x = t != T_BOOL && chance(g, 10) ? pick_indexed(g, t) : -1;
    if (x >= 0) {
        uint32_t b = bound(g, E_LOW + below(g, 2), (uint32_t)x);
        if (!need_var || !g->e[b].cst)
            return b;
    }
    /* length(a) is an Int64 whatever the index of a */
    x = t == T_I64 && chance(g, 10) ? pick_indexed(g, NTYPES) : -1;
    if (x >= 0) {
        uint32_t b = bound(g, E_LEN, (uint32_t)x);
        if (!need_var || !g->e[b].cst)
            return b;
    }
    if (v >= 0 && (need_var || chance(g, 65)))
        return var_ref(g, (uint32_t)v);
    if (!need_var)
        return lit(g, t, rand_value(g, t));
    int w = pick_var(g, NTYPES, false, false);
    if (w < 0) {
        /* none of an integer type: a Boolean compared with a literal */
        w = pick_var(g, T_BOOL, false, false);
        return binop(g, O_EQ, T_BOOL, var_ref(g, (uint32_t)w),
                     lit(g, T_BOOL, below(g, 2)));
    }
    if (t == T_BOOL) {
        unsigned u = base(g, g->v[w].t);
        return binop(g, O_EQ + below(g, 6), T_BOOL, var_ref(g, (uint32_t)w),
                     lit(g, u, rand_value(g, u)));
    }
    if (t != T_BIG && chance(g, 70)) {
        int x = pick_var(g, within(g, t), false, false);
        if (x >= 0)
            w = x;
    }
    return conv(g, t, var_ref(g, (uint32_t)w));
}

/* x in R, or x in lo..hi */
static uint32_t membership(G *g, int d)
{
    uint32_t n = 0, range = 0;
    for (uint32_t t = NTYPES; t < g->nty && !sealed(g); t++)
        if (g->ty[t].k == K_RANGE && below(g, ++n) == 0)
            range = t;
    unsigned b = n && chance(g, 60) ? base(g, range) : int_type(g);
    /* the parts first: making them may move the nodes */
    uint32_t a = expr(g, b, d - 1, true), fn = 0, lo = 0, hi = 0;
    if (n && base(g, range) == b && chance(g, 60)) {
        fn = range;
    } else {
        v128 l = rand_value(g, b), h = rand_value(g, b);
        if (h < l && chance(g, 80)) {
            v128 x = l;
            l = h;
            h = x;
        }
        lo = chance(g, 70) ? lit(g, b, l) : expr(g, b, d - 1, false);
        hi = chance(g, 70) ? lit(g, b, h) : expr(g, b, d - 1, false);
    }
    uint32_t i = new_e(g, E_IN, T_BOOL);
    g->e[i].a = a;
    g->e[i].fn = fn;
    g->e[i].b = lo;
    g->e[i].c = hi;
    return i;
}

/* a String: a variable, a literal, a & b of strings and characters,
   copy, str of a number, a Boolean or a character, an argument, a
   call */
static uint32_t str_expr(G *g, int d, bool need_var)
{
    if (d <= 0 || chance(g, 30)) {
        if (chance(g, g->narg ? 8 : 1))
            return arg_expr(g);
        int v = pick_var(g, T_STR, false, false);
        if (v >= 0 && (need_var || chance(g, 70)))
            return var_ref(g, (uint32_t)v);
        if (!need_var)
            return lit(g, T_STR, rand_value(g, T_STR));
        int w = pick_var(g, NTYPES, false, false); /* global 0 is one */
        uint32_t a = var_ref(g, (uint32_t)w);
        uint32_t i = new_e(g, E_STRF, T_STR);
        g->e[i].a = a;
        return i;
    }
    unsigned c = below(g, 10);
    int fn = c == 9 ? pick_func(g, T_STR) : -1;
    if (fn >= 0)
        return call_expr(g, (uint32_t)fn, d);
    if (c < 5) {
        /* strings and characters in any mix; not two constants */
        uint32_t a = chance(g, 25) ? char_expr(g, d - 1, need_var)
                                   : str_expr(g, d - 1, need_var);
        bool k = g->e[a].cst || g->e[a].k == E_LIT;
        uint32_t b =
            chance(g, 25) ? char_expr(g, d - 1, k) : str_expr(g, d - 1, k);
        return binop(g, O_CAT, T_STR, a, b);
    }
    if (c < 7) {
        /* copy(s, from, count): near the edges, or any Int64 */
        uint32_t a = str_expr(g, d - 1, true);
        uint32_t b = chance(g, 80) ? lit(g, T_I64, (v128)below(g, 8) - 1)
                                   : value_for(g, T_I64, d - 1);
        uint32_t n = chance(g, 80) ? lit(g, T_I64, (v128)below(g, 7) - 1)
                                   : value_for(g, T_I64, d - 1);
        uint32_t i = new_e(g, E_COPY, T_STR);
        g->e[i].a = a;
        g->e[i].b = b;
        g->e[i].c = n;
        return i;
    }
    unsigned u = chance(g, 15) ? T_BIG : any_type(g);
    uint32_t a = is_enum(g, u) ? 0 : expr(g, u, d - 1, true);
    if (!a)
        a = var_ref(g, (uint32_t)pick_var(g, NTYPES, false, false));
    uint32_t i = new_e(g, E_STRF, T_STR);
    g->e[i].a = a;
    return i;
}

/* a Char: a variable, a literal, chr of an integer (checked), often of
   ord of a character plus a little, a call */
static uint32_t char_expr(G *g, int d, bool need_var)
{
    if (d <= 0 || chance(g, 40)) {
        int v = pick_var(g, T_CHAR, false, false);
        if (v >= 0 && (need_var || chance(g, 65)))
            return var_ref(g, (uint32_t)v);
        if (!need_var)
            return lit(g, T_CHAR, rand_value(g, T_CHAR));
    }
    int fn = chance(g, 10) ? pick_func(g, T_CHAR) : -1;
    if (fn >= 0)
        return call_expr(g, (uint32_t)fn, d);
    uint32_t a;
    if (chance(g, 50)) {
        uint32_t c = char_expr(g, d - 1, true);
        uint32_t o = new_e(g, E_ORD, T_U32);
        g->e[o].a = c;
        uint32_t k = lit(g, T_U32, below(g, 4));
        a = binop(g, O_ADD, T_U32, o, k);
    } else {
        a = expr(g, int_type(g), d - 1, true);
    }
    uint32_t i = new_e(g, E_CHR, T_CHAR);
    g->e[i].a = a;
    return i;
}

/* halt(0) or halt(k), k in 2..255 but 141; or a status at an edge (-1,
   0, 1, 2, 141, 255, 256) through a variable, which is no constant: 1,
   141 and those out of 0..255 are a range error at the name. Into out,
   the count */
static uint32_t halt_stmt(G *g, uint32_t *out)
{
    uint32_t n = 0, e;
    if (chance(g, 50)) {
        uint32_t k = chance(g, 30) ? 0 : 2 + below(g, 253);
        e = lit(g, T_I32, k < 141 ? k : k + 1);
    } else {
        static const int edge[] = {-1, 0, 1, 2, 141, 255, 256};
        uint32_t v = new_v(g, T_I32, V_LOCAL);
        uint32_t d = new_s(g, S_VAR);
        uint32_t k = lit(g, T_I32, edge[below(g, 7)]);
        g->st[d].var = v;
        g->st[d].e = k;
        show(g, v);
        out[n++] = d;
        e = var_ref(g, v);
    }
    uint32_t s = new_s(g, S_HALT);
    g->st[s].e = e;
    out[n++] = s;
    return n;
}

/* texts for val: numbers of both kinds, at the edges of the types, in
   every base, with a sign and spaces around, inf and nan, and texts that
   are almost numbers; 1.0000000596046447753906250001 is a Float32 that
   two roundings (to Float64, then to Float32) get wrong */
static const char *const val_text[] = {"0",
                                       "-0",
                                       "+7",
                                       "42",
                                       "-1",
                                       "127",
                                       "-129",
                                       "255",
                                       "256",
                                       "300",
                                       "65535",
                                       "-32769",
                                       "2147483648",
                                       "4294967296",
                                       "16777217",
                                       "9223372036854775807",
                                       "-9223372036854775808",
                                       "9223372036854775808",
                                       "18446744073709551615",
                                       "18446744073709551616",
                                       " 12",
                                       "12 ",
                                       "  -3  ",
                                       "1_000",
                                       "0xff",
                                       "0xFF",
                                       "-0x80",
                                       "0x7fff_ffff",
                                       "0xffffffffffffffff",
                                       "0o17",
                                       "0b1010",
                                       "0b1_0000_0001",
                                       "1.5",
                                       "-2.25",
                                       "1e3",
                                       "1.5e-9",
                                       "2e+10",
                                       "0.1",
                                       "3.4028235e38",
                                       "3.5e38",
                                       "1e-50",
                                       "1.0000000596046447753906250001",
                                       "1e400",
                                       "1e-400",
                                       "inf",
                                       "-inf",
                                       "+inf",
                                       "nan",
                                       "",
                                       " ",
                                       "-",
                                       "+",
                                       "x",
                                       "12x",
                                       "1 2",
                                       "--1",
                                       "1e",
                                       "1.",
                                       ".5",
                                       "1E3",
                                       "0XFF",
                                       "0x",
                                       "0o8",
                                       "0b2",
                                       "1__0",
                                       "_1",
                                       "1_",
                                       "Inf",
                                       "-nan",
                                       "0x_1"};

static uint32_t number_text(G *g)
{
    const char *t = val_text[below(g, sizeof(val_text) / sizeof(val_text[0]))];
    return new_str(g, t, strlen(t));
}

/* a text for val into a variable of type t: half the time one of the
   table; else, for an integer, an edge of t (low - 1, low, high, high +
   1) in decimal or hexadecimal, maybe with spaces around and a _ in the
   digits or out of them; for a real, a text at the edges of the reals */
static uint32_t val_text_for(G *g, uint32_t t)
{
    if (chance(g, fam(base(g, t)) == 'F' ? 50 : 30))
        return number_text(g);
    if (fam(base(g, t)) == 'F') {
        static const char *const r[] = {"inf",
                                        "-inf",
                                        "+inf",
                                        "nan",
                                        "-nan",
                                        "+nan",
                                        "1e400",
                                        "-1e400",
                                        "1e-400",
                                        "3.4028235e38",
                                        "3.5e38",
                                        "1e-50",
                                        "1.0000000596046447753906250001",
                                        "0x1_0000_0000_0000_0001",
                                        "-0.0"};
        const char *x = r[below(g, sizeof(r) / sizeof(r[0]))];
        return new_str(g, x, strlen(x));
    }
    /* the edges of the type; below its minimum half the time, the case a
       lower bound left unchecked would pass */
    v128 edge[4] = {lo_of(g, t) - 1, lo_of(g, t), hi_of(g, t), hi_of(g, t) + 1};
    v128 v = chance(g, 50) ? edge[0] : edge[1 + below(g, 3)];
    u128 m = v < 0 ? -(u128)v : (u128)v;
    bool hex = chance(g, 30);
    char b[64], d[48];
    size_t n = 0;
    do {
        d[n++] = "0123456789abcdef"[m % (hex ? 16 : 10)];
        m /= hex ? 16 : 10;
    } while (m);
    size_t k = 0;
    if (chance(g, 15))
        b[k++] = ' ';
    if (v < 0 || chance(g, 20))
        b[k++] = v < 0 ? '-' : '+';
    if (hex) {
        b[k++] = '0';
        b[k++] = 'x';
    }
    /* now and then a _ among the digits, or before or after them */
    size_t under = chance(g, 20) ? below(g, (uint32_t)n + 1) : SIZE_MAX;
    for (size_t q = n; q-- > 0;) {
        if (q + 1 == n - under)
            b[k++] = '_';
        b[k++] = d[q];
    }
    if (under == n)
        b[k++] = '_';
    if (chance(g, 15))
        b[k++] = ' ';
    return new_str(g, b, k);
}

/* arg(k): k a literal from 1 to argcount(), or argcount() itself; now
   and then one past, argcount() + 1 or a literal (a range error) */
static uint32_t arg_expr(G *g)
{
    uint32_t k;
    if (g->narg && chance(g, 70)) {
        k = lit(g, T_I32, 1 + below(g, g->narg));
    } else if (g->narg && chance(g, 60)) {
        k = new_e(g, E_ARGC, T_I32);
    } else if (chance(g, 50)) {
        k = lit(g, T_I32, g->narg + 1);
    } else {
        uint32_t c = new_e(g, E_ARGC, T_I32), one = lit(g, T_I32, 1);
        k = binop(g, O_ADD, T_I32, c, one);
    }
    uint32_t i = new_e(g, E_ARG, T_STR);
    g->e[i].a = k;
    return i;
}

/* val(s, x) into a writable variable x of a numeric type, s a text of
   the table, str of an integer or an argument; 0 if there is no x */
static uint32_t val_expr(G *g, int d)
{
    /* a real one often, as they are fewer */
    bool real = chance(g, 40);
    uint32_t n = 0, v = 0;
    /* half the integer ones into a signed type narrower than 64 bits: the
       texts below its minimum show whether that bound is checked */
    bool narrow = !real && chance(g, 50);
    for (int pass = 0; pass < 3 && !n; pass++) {
        if (pass == 1 && narrow)
            narrow = false;
        else if (pass)
            real = !real;
        for (uint32_t i = 0; i < g->nscope; i++) {
            uint32_t w = g->scope[i], t = g->v[w].t;
            if (!is_scalar(g, t) || is_enum(g, t) || !writable(g, w) ||
                !strchr(real ? "F" : "SUB", fam(base(g, t))))
                continue;
            if (narrow && (fam(base(g, t)) != 'S' || tbits(base(g, t)) >= 64))
                continue;
            if (below(g, ++n) == 0)
                v = w;
        }
    }
    if (!n)
        return 0;
    uint32_t a;
    if (g->narg && chance(g, 10)) {
        a = arg_expr(g);
    } else if (d > 0 && chance(g, 20)) {
        uint32_t b = expr(g, int_type(g), d - 1, true);
        a = new_e(g, E_STRF, T_STR);
        g->e[a].a = b;
    } else {
        a = lit(g, T_STR, val_text_for(g, g->v[v].t));
    }
    uint32_t i = new_e(g, E_VAL, T_BOOL);
    g->e[i].var = v;
    g->e[i].a = a;
    return i;
}

/* readline(s) into a writable String variable; 0 if none, or in a
   function (it reads the input) */
static uint32_t read_expr(G *g)
{
    int v = in_function(g) ? -1 : pick_var(g, T_STR, true, true);
    if (v < 0)
        return 0;
    uint32_t i = new_e(g, E_READ, T_BOOL);
    g->e[i].var = (uint32_t)v;
    return i;
}

/* argcount() + k, no constant: a width or decimals out of their range
   that the front end cannot see */
static uint32_t past(G *g, int k)
{
    uint32_t c = new_e(g, E_ARGC, T_I32), n = lit(g, T_I32, k < 0 ? -k : k);
    return binop(g, k < 0 ? O_SUB : O_ADD, T_I32, c, n);
}

/* item a of a writeln, sometimes a:width or, for a real, a:width:decimals:
   widths 0 to 12 and decimals 0 to 10, seldom a negative width or
   decimals out of 0..100 (a range error at them) */
static uint32_t format(G *g, uint32_t a)
{
    if (g->e[a].k == E_STR || !chance(g, 25))
        return a;
    bool real = fam(base(g, g->e[a].t)) == 'F';
    uint32_t w = chance(g, 1) ? past(g, -4 - (int)below(g, 3))
                              : lit(g, T_I32, below(g, 13));
    uint32_t d = 0;
    if (real && chance(g, 60))
        d = chance(g, 1) ? past(g, chance(g, 50) ? -4 : 101)
                         : lit(g, T_I32, below(g, 11));
    uint32_t i = new_e(g, E_FMT, g->e[a].t);
    g->e[i].a = a;
    g->e[i].b = w;
    g->e[i].c = d;
    return i;
}

/* sqrt(a) or another function of the library on the real a of type t */
static uint32_t math_call(G *g, unsigned t, uint32_t a, unsigned m)
{
    if (m >= M_TRUNC && chance(g, 60)) {
        /* most reals here are whole: a fraction, often a half, where the
           four roundings differ */
        static const double frac[] = {0.5, -0.5, 0.25, 0.75, 1.5};
        uint32_t h = lit(g, t, fbits(frac[below(g, 5)]));
        a = binop(g, O_ADD, t, a, h);
    }
    uint32_t i = new_e(g, E_MATH, t);
    g->e[i].op = (uint8_t)m;
    g->e[i].a = a;
    return i;
}

/* a BigInt: + - * div mod rem, a power, the minus and abs, BigInt of an
   integer or a real, a call, a leaf */
static uint32_t big_expr(G *g, int d, bool need_var)
{
    if (d <= 0 || chance(g, 25))
        return leaf(g, T_BIG, need_var, d);
    unsigned choice = below(g, 10);
    if (choice == 9) {
        int fn = pick_func(g, T_BIG);
        if (fn >= 0)
            return call_expr(g, (uint32_t)fn, d);
        choice = 0;
    }
    if (choice < 5) {
        unsigned op = below(g, 6);
        bool divide = op >= O_DIV && op <= O_REM;
        uint32_t a = expr(g, T_BIG, d - 1, divide);
        uint32_t b;
        if (divide && chance(g, 70)) {
            /* a literal divisor, now and then 0 */
            v128 k = chance(g, 5) ? 0 : (v128)below(g, 1000) + 1;
            b = lit(g, T_BIG, big_of_i128(g, chance(g, 30) ? -k : k));
        } else {
            b = expr(g, T_BIG, d - 1, g->e[a].cst);
        }
        return binop(g, O_ADD + op, T_BIG, a, b);
    }
    if (choice == 5) {
        /* the exponent small, or any integer, maybe negative */
        uint32_t a = expr(g, T_BIG, d - 1, true);
        uint32_t n = chance(g, 70) ? lit(g, T_I32, below(g, 40))
                                   : expr(g, int_type(g), d - 1, true);
        return binop(g, O_POW, T_BIG, a, n);
    }
    if (choice == 6) {
        uint32_t a = expr(g, T_BIG, d - 1, true);
        uint32_t i = new_e(g, E_UN, T_BIG);
        g->e[i].op = chance(g, 50) ? O_NEG : O_ABS;
        g->e[i].a = a;
        return i;
    }
    unsigned from = choice == 7 ? int_type(g) : chance(g, 50) ? T_F64 : T_F32;
    return conv(g, T_BIG, expr(g, from, d - 1, true));
}

static uint32_t expr(G *g, unsigned t, int d, bool need_var)
{
    if (is_enum(g, t))
        return enum_expr(g, t, d, need_var);
    if (t == T_STR)
        return str_expr(g, d, need_var);
    if (t == T_CHAR)
        return char_expr(g, d, need_var);
    if (t == T_BIG)
        return big_expr(g, d, need_var);
    if (d <= 0 || chance(g, 20))
        return leaf(g, t, need_var, d);
    unsigned f = fam(t);
    int bv = f != 'L' && chance(g, 5) ? pick_var(g, T_BIG, false, false) : -1;
    if (bv >= 0) /* a BigInt made into t: it must fit (§ 6.6) */
        return conv(g, t, var_ref(g, (uint32_t)bv));
    unsigned choice = below(g, 10);
    if (choice == 9) {
        int fn = pick_func(g, t);
        if (fn >= 0)
            return call_expr(g, (uint32_t)fn, d);
        choice = 0;
    }
    if (f == 'F') {
        /* + - * /, the minus and abs, a conversion from an integer */
        if (choice < 6) {
            static const unsigned fops[] = {O_ADD, O_SUB, O_MUL, O_FDIV};
            uint32_t a = expr(g, t, d - 1, false);
            uint32_t b = expr(g, t, d - 1, g->e[a].cst);
            return binop(g, fops[below(g, 4)], t, a, b);
        }
        if (choice < 8) {
            uint32_t a = expr(g, t, d - 1, true);
            uint32_t i = new_e(g, E_UN, t);
            g->e[i].op = chance(g, 50) ? O_NEG : O_ABS;
            g->e[i].a = a;
            return i;
        }
        if (choice == 8) {
            /* sqrt(a) and the others: IEEE 754, no error (§ 9) */
            uint32_t a = expr(g, t, d - 1, true);
            return math_call(g, t, a, below(g, NMATH));
        }
        return conv(g, t, expr(g, int_type(g), d - 1, true));
    }
    if (f == 'L') {
        int p = chance(g, 10) ? pick_typed(g, 2, false) : -1;
        if (p >= 0) {
            uint32_t pt = g->v[p].t;
            int q = pick_typed(g, pt, false);
            uint32_t other = q >= 0 && chance(g, 50) ? var_ref(g, (uint32_t)q)
                                                     : nil_of(g, pt);
            return binop(g, chance(g, 50) ? O_EQ : O_NE, T_BOOL,
                         var_ref(g, (uint32_t)p), other);
        }
        uint32_t et = chance(g, 15) ? pick_enum(g) : 0;
        if (et && choice < 4) {
            uint32_t a = enum_expr(g, et, d - 1, false);
            uint32_t b = enum_expr(g, et, d - 1, g->e[a].cst);
            return binop(g, O_EQ + below(g, 6), T_BOOL, a, b);
        }
        if (choice < 4) {
            unsigned u = chance(g, 15)  ? T_STR
                         : chance(g, 8) ? T_BIG
                                        : any_type(g);
            uint32_t a = expr(g, u, d - 1, false);
            uint32_t b = expr(g, u, d - 1, g->e[a].cst);
            return binop(g, O_EQ + below(g, 6), T_BOOL, a, b);
        }
        if (choice < 5)
            return membership(g, d);
        if (choice < 8) {
            uint32_t a = expr(g, T_BOOL, d - 1, false);
            uint32_t b = expr(g, T_BOOL, d - 1, g->e[a].cst);
            return binop(g, O_AND + below(g, 3), T_BOOL, a, b);
        }
        uint32_t i = new_e(g, E_UN, T_BOOL);
        g->e[i].op = O_NOT;
        uint32_t a = expr(g, T_BOOL, d - 1, true);
        g->e[i].a = a;
        return i;
    }
    if (choice < 6 && chance(g, 10)) {
        /* a power: the exponent a literal, or any integer, maybe
           negative (a range error) */
        uint32_t a = expr(g, t, d - 1, true);
        uint32_t n = chance(g, 60) ? lit(g, T_I32, below(g, 8))
                                   : expr(g, int_type(g), d - 1, true);
        return binop(g, O_POW, t, a, n);
    }
    if (choice < 6) {
        unsigned op = f == 'B' ? below(g, 11) : below(g, 6);
        if (op == O_SHL || op == O_SHR) {
            uint32_t a = expr(g, t, d - 1, true);
            uint32_t n = chance(g, 50) ? lit(g, T_I32, below(g, tbits(t)))
                                       : expr(g, int_type(g), d - 1, true);
            return binop(g, op, t, a, n);
        }
        bool divide = op >= O_DIV && op <= O_REM;
        uint32_t a = expr(g, t, d - 1, divide);
        /* most divisors are literals, so that fewer programs stop early */
        uint32_t b = divide && chance(g, 70) ? lit(g, t, rand_value(g, t))
                                             : expr(g, t, d - 1, g->e[a].cst);
        if (divide && g->e[b].k == E_LIT && g->e[b].lit == 0)
            g->e[b].lit = 1; /* the front end may reject a literal 0 */
        return binop(g, op, t, a, b);
    }
    if (choice < 8) {
        unsigned op = f == 'S'   ? (chance(g, 50) ? O_NEG : O_ABS)
                      : f == 'U' ? O_ABS
                                 : (chance(g, 50) ? O_NEG : O_NOT);
        uint32_t a = expr(g, t, d - 1, true);
        uint32_t i = new_e(g, E_UN, t);
        g->e[i].op = (uint8_t)op;
        g->e[i].a = a;
        return i;
    }
    /* a conversion, sometimes to a range of the same base */
    uint32_t to = t;
    for (uint32_t r = NTYPES; r < g->nty && !sealed(g); r++)
        if (g->ty[r].k == K_RANGE && base(g, r) == t && chance(g, 30))
            to = r;
    return conv(g, to,
                expr(g,
                     chance(g, 10)   ? (chance(g, 50) ? T_F64 : T_F32)
                     : chance(g, 60) ? within(g, t)
                                     : int_type(g),
                     d - 1, true));
}

/* a range whose bounds are exact constant expressions (§ 4.4) of the
   constants without a type in scope, computed by the front end; none if
   they do not fit a base */
static void const_range(G *g)
{
    for (int k = 0; k < 4; k++) {
        unsigned b = below(g, T_B8);
        uint32_t lo = cexpr(g, 2);
        uint32_t hi = cexpr(g, 2);
        v128 l = g->e[lo].lit, h = g->e[hi].lit;
        if (l > h) {
            uint32_t x = lo;
            lo = hi;
            hi = x;
            v128 y = l;
            l = h;
            h = y;
        }
        if (l < tmin(b) || h > tmax(b))
            continue;
        uint32_t t = new_type(g, (xt){K_RANGE, (uint8_t)b, 0, 0, l, h, 0, 0});
        g->ty[t].elo = lo;
        g->ty[t].ehi = hi;
        return;
    }
}

/* ---- statements ---- */

static uint32_t block(G *g, int n, uint32_t *count);

/* a counter that bounds a while or a repeat: var w := 0 */
static uint32_t counter(G *g, uint32_t *decl)
{
    uint32_t w = new_v(g, T_I32, V_COUNT);
    *decl = new_s(g, S_VAR);
    g->st[*decl].var = w;
    g->st[*decl].e = lit(g, T_I32, 0);
    show(g, w);
    return w;
}

static uint32_t bump(G *g, uint32_t w)
{
    uint32_t s = new_s(g, S_ASSIGN);
    uint32_t e = binop(g, O_ADD, T_I32, var_ref(g, w), lit(g, T_I32, 1));
    g->st[s].var = w;
    g->st[s].e = e;
    return s;
}

/* the body of a loop, whose first statement is first (0: none) */
static uint32_t declare_dyn(G *g);
static uint32_t declare_agg(G *g);
static uint32_t fill_strings(G *g, uint32_t s, uint32_t *out);
static uint32_t read_write(G *g);
static void append(G *g, uint32_t *b, uint32_t *n, uint32_t s, bool front);

static void loop_body(G *g, uint32_t s, uint32_t first)
{
    uint32_t mark = g->nscope;
    g->loops++;
    /* sometimes an array with computed bounds, alive at every exit and
       continue of the body */
    uint32_t dyn = chance(g, 30) ? declare_dyn(g) : 0;
    /* sometimes a record or an array without a value, its Strings made
       at once: each round declares it again from zero (§ 3.11) */
    uint32_t agg[5], nagg = 0;
    if (chance(g, 25) && (agg[0] = declare_agg(g)) != 0)
        nagg = 1 + (g->st[agg[0]].e ? 0 : fill_strings(g, agg[0], agg + 1));
    uint32_t n;
    uint32_t b = block(g, 1 + (int)below(g, 4), &n);
    g->loops--;
    g->nscope = mark;
    while (nagg)
        append(g, &b, &n, agg[--nagg], true);
    if (dyn)
        append(g, &b, &n, dyn, true);
    if (first) /* the counter goes first, so continue cannot skip it */
        append(g, &b, &n, first, true);
    g->st[s].blk = b;
    g->st[s].nblk = n;
}

/* var s: String := a string made at run time; var t: String := s; a loop
   of 1 to 4 rounds that changes s, or both s and t, or shares s with t
   again; then s and t written: one string in two variables, and Strings
   carried round a loop and read after it (in SSA, the arguments of the
   jumps of the loop, which an engine that counts must keep; when both
   change, the jump into the loop passes one string twice); into out, 5
   statements */
static uint32_t str_loop(G *g, uint32_t *out)
{
    uint32_t sv = new_v(g, T_STR, V_LOCAL);
    uint32_t ds = new_s(g, S_VAR);
    uint32_t e = made_str(g);
    g->st[ds].var = sv;
    g->st[ds].e = e;
    show(g, sv);
    uint32_t tv = new_v(g, T_STR, V_LOCAL);
    uint32_t dt = new_s(g, S_VAR);
    e = var_ref(g, sv);
    g->st[dt].var = tv;
    g->st[dt].e = e;
    show(g, tv);
    uint32_t decl, w = counter(g, &decl);
    uint32_t loop = new_s(g, S_WHILE);
    e = binop(g, O_LT, T_BOOL, var_ref(g, w), lit(g, T_I32, 1 + below(g, 4)));
    g->st[loop].e = e;
    uint32_t a = new_s(g, S_ASSIGN), a2 = 0;
    unsigned how = below(g, 4);
    if (how == 2) { /* t := s: shared again */
        e = var_ref(g, sv);
        g->st[a].var = tv;
    } else {
        uint32_t l = var_ref(g, how == 1 ? tv : sv);
        uint32_t r = made_str(g);
        e = binop(g, O_CAT, T_STR, l, r);
        g->st[a].var = sv;
    }
    g->st[a].e = e;
    if (how == 3) { /* and t := t & a string made at run time */
        a2 = new_s(g, S_ASSIGN);
        uint32_t l = var_ref(g, tv);
        uint32_t r = made_str(g);
        e = binop(g, O_CAT, T_STR, l, r);
        g->st[a2].var = tv;
        g->st[a2].e = e;
    }
    uint32_t b = 0, n = 0, bs = bump(g, w);
    append(g, &b, &n, bs, false);
    append(g, &b, &n, a, false);
    if (a2)
        append(g, &b, &n, a2, false);
    g->st[loop].blk = b;
    g->st[loop].nblk = n;
    uint32_t items[3];
    items[0] = var_ref(g, sv);
    items[1] = new_e(g, E_STR, 0);
    items[2] = var_ref(g, tv);
    uint32_t wr = new_s(g, S_WRITE);
    g->st[wr].args = keep_list(g, items, 3);
    g->st[wr].nargs = 3;
    out[0] = ds;
    out[1] = dt;
    out[2] = decl;
    out[3] = loop;
    out[4] = wr;
    return 5;
}

static bool overlaps(const xlab *l, uint32_t n, v128 lo, v128 hi)
{
    for (uint32_t i = 0; i < n; i++)
        if (!(hi < l[i].lo || lo > l[i].hi))
            return true;
    return false;
}

static uint32_t declare(G *g, uint32_t t)
{
    uint32_t s = new_s(g, S_VAR);
    uint32_t e = value_for(g, t, depth(g));
    uint32_t v = new_v(g, t, V_LOCAL);
    g->st[s].var = v;
    g->st[s].e = e;
    show(g, v);
    return s;
}

/* var d: T, a record or an array of the program, without a value (each
   time it runs, in a loop too: § 3.11) or a copy of a visible variable
   of T; 0 if the program has neither */
static uint32_t declare_agg(G *g)
{
    uint32_t n = 0, t = 0;
    for (uint32_t u = NTYPES; u < g->nty && !sealed(g); u++)
        if ((g->ty[u].k == K_RECORD || program_array(g, u)) &&
            below(g, ++n) == 0)
            t = u;
    if (!n)
        return 0;
    int from = chance(g, 50) ? pick_typed(g, t, false) : -1;
    uint32_t s = new_s(g, S_VAR);
    g->st[s].e = from >= 0 ? var_ref(g, (uint32_t)from) : 0;
    uint32_t v = new_v(g, t, V_LOCAL);
    g->st[s].var = v;
    show(g, v);
    return s;
}

/* after the declaration s of a record or an array without a value, its
   Strings given values made at run time, which memory counts: a loop
   declares it again, a computed array is freed; into out, at most 4 */
static uint32_t fill_strings(G *g, uint32_t s, uint32_t *out)
{
    uint32_t v = g->st[s].var, t = g->v[v].t, n = 0;
    if (is_record(g, t)) {
        for (uint32_t f = 0; f < g->ty[t].elem && n < 4; f++) {
            if (base(g, field_type(g, t, f)) != T_STR)
                continue;
            uint32_t a = new_s(g, S_ASSIGN);
            g->st[a].var = v;
            g->st[a].fld = f + 1;
            uint32_t e = made_str(g);
            g->st[a].e = e;
            out[n++] = a;
        }
        return n;
    }
    if (base(g, g->ty[t].elem) != T_STR)
        return 0;
    /* the first element: the variable of the bounds of a computed array
       (an empty one traps on it), the low bound of the index otherwise */
    uint32_t idx = g->ty[t].k == K_DYN
                       ? var_ref(g, g->e[g->st[s].e].var)
                       : lit(g, base(g, g->ty[t].index), g->ty[t].lo);
    uint32_t a = new_s(g, S_ASSIGN);
    g->st[a].var = v;
    g->st[a].idx = idx;
    uint32_t e = made_str(g);
    g->st[a].e = e;
    out[n++] = a;
    return n;
}

/* var v: array[B range x..x + k] of T, with x a variable and k in -1..3:
   bounds computed at run time (§ 4.5), 0 to 4 elements, on the heap; 0
   if no variable fits */
static uint32_t declare_dyn(G *g)
{
    int w = -1;
    for (int k = 0; k < 4 && w < 0; k++) {
        w = pick_var(g, NTYPES, false, false);
        if (w >= 0 && fam(base(g, g->v[w].t)) == 'B')
            w = -1; /* a signed or unsigned integer */
    }
    if (w < 0)
        return 0;
    unsigned b = base(g, g->v[w].t);
    int k = (int)below(g, 5) - 1;
    uint32_t lo = var_ref(g, (uint32_t)w);
    uint32_t x = var_ref(g, (uint32_t)w);
    uint32_t n = lit(g, b, k < 0 ? 1 : k);
    uint32_t hi = binop(g, k < 0 ? O_SUB : O_ADD, b, x, n);
    uint32_t el = counted_or_var(g, 20);
    uint32_t t =
        new_type(g, (xt){K_DYN, (uint8_t)base(g, el), b, el, 0, 3, 0, 0});
    uint32_t v = new_v(g, t, V_LOCAL);
    uint32_t s = new_s(g, S_VAR);
    g->st[s].var = v;
    g->st[s].e = lo;
    g->st[s].e2 = hi;
    show(g, v);
    return s;
}

/* writeln(x[i]) and often y[i], for i a loop variable of index base b
   and y another array with that index base, which may not hold i */
static uint32_t write_elements(G *g, uint32_t x, uint32_t i, unsigned b)
{
    uint32_t items[3], k = 0;
    int y = chance(g, 50) ? pick_indexed(g, b) : -1;
    for (int n = 0; n < (y >= 0 ? 2 : 1); n++) {
        uint32_t a = n ? (uint32_t)y : x;
        if (k)
            items[k++] = new_e(g, E_STR, 0);
        uint32_t ix = var_ref(g, i);
        uint32_t el = new_e(g, E_INDEX, g->ty[g->v[a].t].elem);
        g->e[el].var = a;
        g->e[el].a = ix;
        if (is_enum(g, g->e[el].t)) {
            uint32_t o = new_e(g, E_ORD, T_U32);
            g->e[o].a = el;
            el = o;
        }
        items[k++] = el;
    }
    uint32_t w = new_s(g, S_WRITE);
    g->st[w].args = keep_list(g, items, k);
    g->st[w].nargs = k;
    return w;
}

/* the body of for i over the bounds of array x begins printing x[i],
   and sometimes a loop for j := i + c to high(x) (or low(x) to i - c)
   that prints x[j]: the checks the front end may leave out, next to
   those it must keep */
static void over_array(G *g, uint32_t s, uint32_t x, uint32_t i, unsigned b)
{
    uint32_t pre[2], np = 0;
    pre[np++] = write_elements(g, x, i, b);
    if (chance(g, 40)) {
        /* i + c .. high(x) and low(x) .. i - c stay inside x; i - c ..
           high(x) and low(x) .. i + c, with c > 0, may not */
        uint32_t f = new_s(g, S_FOR);
        bool up = chance(g, 50), out = chance(g, 30);
        v128 cv = out ? 1 + below(g, 2) : below(g, 3);
        unsigned op = up != out ? O_ADD : O_SUB;
        if (out && fam(b) == 'S' && chance(g, 50)) {
            /* the same step written the other way: i + (-c), i - (-c) */
            op = op == O_ADD ? O_SUB : O_ADD;
            cv = -cv;
        }
        uint32_t c = lit(g, b, cv);
        uint32_t iv = var_ref(g, i);
        uint32_t near = binop(g, op, b, iv, c);
        uint32_t edge = bound(g, up ? E_HIGH : E_LOW, x);
        uint32_t j = new_v(g, b, V_LOOP);
        g->st[f].var = j;
        g->st[f].e = up ? near : edge;
        g->st[f].e2 = up ? edge : near;
        uint32_t w = write_elements(g, x, j, b);
        g->st[f].blk = keep_list(g, &w, 1);
        g->st[f].nblk = 1;
        pre[np++] = f;
    }
    uint32_t bb = g->st[s].blk, nb = g->st[s].nblk;
    while (np)
        append(g, &bb, &nb, pre[--np], true);
    g->st[s].blk = bb;
    g->st[s].nblk = nb;
}

/* the type of the values a selector can take: its range, if a variable
   or an element of a range type gives it */
static uint32_t selector_type(const G *g, uint32_t e)
{
    unsigned k = g->e[e].k;
    return k == E_VAR || k == E_INDEX || k == E_CALL || k == E_CONV ||
                   k == E_FIELD || k == E_CFIELD || k == E_CINDEX
               ? g->e[e].t
               : base(g, g->e[e].t);
}

/* one statement, or two when a loop needs its counter declared first;
   they go to out, and the count is returned */
/* a visible variable of a pointer type to an array made by new, of type t
   exactly (0: any); -1 if none */
static int pick_heap(G *g, uint32_t t)
{
    uint32_t n = 0, chosen = 0;
    for (uint32_t i = 0; i < g->nscope; i++) {
        uint32_t v = g->scope[i], vt = g->v[v].t;
        if (vt < NTYPES || g->ty[vt].k != K_HEAP || (t && vt != t))
            continue;
        if (below(g, ++n) == 0)
            chosen = v;
    }
    return n ? (int)chosen : -1;
}

/* an index near the bounds new gives (-2..4): inside or just outside */
static v128 near_index(G *g, unsigned ix)
{
    return fam(ix) == 'S' ? (v128)below(g, 7) - 3 : (v128)below(g, 6);
}

/* the value for an element: a String made at run time, counted */
static uint32_t element_value(G *g, uint32_t el)
{
    return base(g, el) == T_STR ? made_str(g) : value_for(g, el, depth(g));
}

/* an index of an array lo..hi (known when lo <= hi): inside it most of
   the time, near its bounds otherwise */
static v128 some_index(G *g, unsigned ix, v128 lo, v128 hi)
{
    if (lo <= hi && chance(g, 85))
        return lo + (v128)below(g, (uint32_t)(hi - lo + 1));
    return near_index(g, ix);
}

/* writeln(h[i], " ", low/high/length(h^)) in some order; lo..hi the
   bounds of h if known (else lo > hi) */
static uint32_t heap_write(G *g, uint32_t h, bool bounds, v128 lo, v128 hi)
{
    uint32_t ot = g->ty[g->v[h].t].elem, el = g->ty[ot].elem;
    unsigned ix = g->ty[ot].index;
    uint32_t idx = lit(g, ix, some_index(g, ix, lo, hi));
    uint32_t it = new_e(g, E_HIDX, el);
    g->e[it].var = h;
    g->e[it].a = idx;
    uint32_t items[3] = {it, 0, 0}, n = 1;
    if (bounds) {
        uint32_t sep = new_e(g, E_STR, 0);
        uint8_t op = (uint8_t)below(g, 3);
        uint32_t len = new_e(g, E_HLEN, op == 2 ? T_I64 : ix);
        g->e[len].var = h;
        g->e[len].op = op;
        bool first = chance(g, 50);
        items[0] = first ? len : it;
        items[1] = sep;
        items[2] = first ? it : len;
        n = 3;
    }
    uint32_t w = new_s(g, S_WRITE);
    g->st[w].args = keep_list(g, items, n);
    g->st[w].nargs = n;
    return w;
}

/* move(h^, from, d^, to, count), count -1..4, an Int64; lo..hi as
   heap_write */
static uint32_t heap_move(G *g, uint32_t h, uint32_t d, v128 lo, v128 hi)
{
    unsigned ix = g->ty[g->ty[g->v[h].t].elem].index;
    v128 k = (v128)below(g, 6) - 1;
    if (lo <= hi && k > hi - lo + 1 && chance(g, 80))
        k = hi - lo + 1;
    uint32_t from = lit(g, ix, some_index(g, ix, lo, hi));
    uint32_t to = lit(g, ix, some_index(g, ix, lo, hi));
    uint32_t count = lit(g, T_I64, k);
    uint32_t m = new_s(g, S_MOVE);
    g->st[m].var = h;
    g->st[m].idx = d;
    g->st[m].e = from;
    g->st[m].e2 = to;
    g->st[m].fld = count;
    return m;
}

/* a span of h for reverse, translate, occurrences, readbytes and
   writebytes: from and count, count -1..4 and an Int64 (lo..hi as
   heap_write), into the statement's e and fld */
static uint32_t heap_span(G *g, unsigned k, uint32_t h, v128 lo, v128 hi)
{
    unsigned ix = g->ty[g->ty[g->v[h].t].elem].index;
    v128 n = (v128)below(g, 6) - 1;
    v128 f = some_index(g, ix, lo, hi);
    if (lo <= hi && chance(g, 70)) {
        /* inside: the routine is reached, its work seen */
        v128 len = hi - lo + 1 < 4 ? hi - lo + 1 : 4;
        n = (v128)below(g, (uint32_t)len + 1);
        f = lo + (v128)below(g, (uint32_t)(hi - lo + 1 - n) + 1);
    } else if (lo <= hi && n > hi - lo + 1 && chance(g, 80)) {
        n = hi - lo + 1;
    }
    uint32_t from = lit(g, ix, f);
    uint32_t count = lit(g, T_I64, n);
    uint32_t m = new_s(g, k);
    g->st[m].var = h;
    g->st[m].e = from;
    g->st[m].fld = count;
    return m;
}

/* translate(h^, ...) through a table of j * K + C, h of Bytes */
static uint32_t heap_trans(G *g, uint32_t h, v128 lo, v128 hi)
{
    uint32_t m = heap_span(g, S_TRANS, h, lo, hi);
    g->st[m].fn = chance(g, 30) ? 1 : below(g, 256);
    g->st[m].args = below(g, 256);
    return m;
}

/* writeln(occurrences(h^, from, count, d^)), h and d of Bytes */
static uint32_t heap_occ(G *g, uint32_t h, uint32_t d, v128 lo, v128 hi)
{
    uint32_t m = heap_span(g, S_OCC, h, lo, hi);
    g->st[m].idx = d;
    return m;
}

/* writebytes(s, from, count) of a String variable s in scope: mostly
   from 1..2 and count 0..2, inside most Strings, so that the bytes are
   seen; else from 0..5 and count -1..4 around its length; 0 if there is
   none */
static uint32_t str_bytes(G *g)
{
    int sv = pick_var(g, T_STR, false, false);
    if (sv < 0)
        return 0;
    uint32_t m = new_s(g, S_WRS);
    g->st[m].var = (uint32_t)sv;
    bool inside = chance(g, 70);
    g->st[m].e = lit(g, T_I64, inside ? 1 + below(g, 2) : below(g, 6));
    g->st[m].fld = lit(g, T_I64, inside ? below(g, 3) : (v128)below(g, 6) - 1);
    return m;
}

/* writeln(readbytes(zN, from, count)) of a local array of 4 Bytes, the
   span inside: the input read in blocks often, whatever the program
   holds */
static uint32_t local_read(G *g)
{
    unsigned f = 1 + below(g, 2);
    uint32_t m = new_s(g, S_RDZ);
    g->st[m].e = lit(g, T_I32, f);
    g->st[m].fld = lit(g, T_I64, below(g, 6 - f));
    return m;
}

/* dispose(h), sometimes followed at once by a second dispose (invalid)
   or a read (dangling); into out, the count */
static uint32_t heap_free(G *g, uint32_t h, uint32_t *out)
{
    uint32_t f = new_s(g, S_HFREE);
    g->st[f].var = h;
    out[0] = f;
    if (chance(g, 50))
        return 1;
    if (chance(g, 40)) {
        uint32_t f2 = new_s(g, S_HFREE);
        g->st[f2].var = h;
        out[1] = f2;
        return 2;
    }
    out[1] = heap_write(g, h, false, 1, 0);
    return 2;
}

/* an array made by new (§ 3.10, § 9.7): declared with small bounds,
   filled, written, often moved within and disposed of; or on one in
   scope, an element set or written, a move between two of one type, a
   dispose; reversed, and for Bytes translated, counted, read and
   written; into out, at most 24 */
static uint32_t heap_array(G *g, uint32_t *out)
{
    uint32_t nh = 0, ht = 0;
    for (uint32_t u = NTYPES; u < g->nty && !sealed(g); u++)
        if (g->ty[u].k == K_HEAP && below(g, ++nh) == 0)
            ht = u;
    if (!nh)
        return 0;
    int h = pick_heap(g, 0);
    unsigned c = below(g, 10);
    if (h < 0 || c < 2) {
        uint32_t ot = g->ty[ht].elem, el = g->ty[ot].elem;
        unsigned ix = g->ty[ot].index;
        v128 lo = fam(ix) == 'S' ? (v128)below(g, 5) - 2 : (v128)below(g, 3);
        v128 hi = lo + (v128)below(g, 6) - 1;
        if (hi < tmin(ix))
            hi = lo;
        uint32_t el_lo = lit(g, ix, lo), el_hi = lit(g, ix, hi);
        uint32_t v = new_v(g, ht, V_LOCAL);
        uint32_t s = new_s(g, S_HNEW);
        g->st[s].var = v;
        g->st[s].e = el_lo;
        g->st[s].e2 = el_hi;
        show(g, v);
        uint32_t n = 0;
        out[n++] = s;
        for (v128 i = lo; i <= hi && n < 6; i++) {
            if (chance(g, 20))
                continue; /* without a value: a read of it is caught */
            /* Bytes of a few values: occurrences finds some */
            uint32_t idx = lit(g, ix, i), e = el == T_B8 && chance(g, 70)
                                                  ? lit(g, T_B8, below(g, 2))
                                                  : element_value(g, el);
            uint32_t a = new_s(g, S_HSET);
            g->st[a].var = v;
            g->st[a].idx = idx;
            g->st[a].e = e;
            out[n++] = a;
        }
        out[n++] = heap_write(g, v, true, lo, hi);
        if (chance(g, 40)) {
            out[n++] = heap_move(g, v, v, lo, hi);
            out[n++] = heap_write(g, v, false, lo, hi);
        }
        if (chance(g, 30)) {
            out[n++] = heap_span(g, S_REV, v, lo, hi);
            out[n++] = heap_write(g, v, false, lo, hi);
        }
        if (el == T_B8 && chance(g, 50)) {
            out[n++] = heap_trans(g, v, lo, hi);
            out[n++] = heap_write(g, v, false, lo, hi);
        }
        if (el == T_B8 && chance(g, 50)) {
            /* the pattern: the array itself, or a new one of 0 to 3
               Bytes, so that occurrences overlap or the pattern is
               empty */
            uint32_t pv = v;
            if (chance(g, 70)) {
                v128 plo = lo, phi = lo + (v128)below(g, 4) - 1;
                if (phi < tmin(ix))
                    phi = plo;
                pv = new_v(g, ht, V_LOCAL);
                uint32_t ps = new_s(g, S_HNEW);
                g->st[ps].var = pv;
                g->st[ps].e = lit(g, ix, plo);
                g->st[ps].e2 = lit(g, ix, phi);
                show(g, pv);
                out[n++] = ps;
                for (v128 i = plo; i <= phi; i++) {
                    uint32_t a = new_s(g, S_HSET);
                    g->st[a].var = pv;
                    g->st[a].idx = lit(g, ix, i);
                    g->st[a].e = lit(g, T_B8, below(g, 2));
                    out[n++] = a;
                }
            }
            out[n++] = heap_occ(g, v, pv, lo, hi);
        }
        if (el == T_B8 && chance(g, 50)) {
            out[n++] = heap_span(g, S_RDB, v, lo, hi);
            out[n++] = heap_write(g, v, false, lo, hi);
        }
        if (el == T_B8 && chance(g, 50))
            out[n++] = heap_span(g, S_WRB, v, lo, hi);
        if (chance(g, 30))
            n += heap_free(g, v, out + n);
        return n;
    }
    ht = g->v[h].t;
    uint32_t ot = g->ty[ht].elem, el = g->ty[ot].elem;
    unsigned ix = g->ty[ot].index;
    if (c < 4) {
        uint32_t idx = lit(g, ix, near_index(g, ix)), e = element_value(g, el);
        uint32_t a = new_s(g, S_HSET);
        g->st[a].var = (uint32_t)h;
        g->st[a].idx = idx;
        g->st[a].e = e;
        out[0] = a;
        return 1;
    }
    if (c < 6) {
        out[0] = heap_write(g, (uint32_t)h, true, 1, 0);
        return 1;
    }
    if (c < 8) {
        out[0] = heap_move(g, (uint32_t)h, (uint32_t)pick_heap(g, ht), 1, 0);
        return 1;
    }
    if (c < 9 && chance(g, 50)) {
        unsigned w = el == T_B8 ? below(g, 4) : 4;
        if (w == 0)
            out[0] = heap_trans(g, (uint32_t)h, 1, 0);
        else if (w == 1)
            out[0] = heap_occ(g, (uint32_t)h, (uint32_t)pick_heap(g, ht), 1, 0);
        else if (w < 4)
            out[0] = heap_span(g, w == 2 ? S_RDB : S_WRB, (uint32_t)h, 1, 0);
        else
            out[0] = heap_span(g, S_REV, (uint32_t)h, 1, 0);
        return 1;
    }
    return heap_free(g, (uint32_t)h, out);
}

static uint32_t stmt(G *g, uint32_t *out)
{
    g->budget--;
    unsigned c = below(g, 100);
    bool pure = in_function(g);
    bool deep = g->nesting < 4 && g->budget > 4;
    if (!pure && chance(g, 10))
        c = 84; /* a call of a procedure, more often */
    if (chance(g, 4)) {
        out[0] = constant(g);
        return 1;
    }
    uint32_t et = chance(g, 5) ? pick_enum(g) : 0;
    int ev_ = et ? pick_var(g, et, true, true) : -1;
    if (ev_ >= 0) {
        uint32_t e = enum_expr(g, et, depth(g), false);
        uint32_t s = new_s(g, S_ASSIGN);
        g->st[s].var = (uint32_t)ev_;
        g->st[s].e = e;
        out[0] = s;
        return 1;
    }
    if (!pure && chance(g, 6)) {
        uint32_t k = heap_stmt(g, out);
        if (k)
            return k;
    }
    if (chance(g, 12)) {
        int fn = pick_agg_func(g, 0);
        int d = fn >= 0 ? pick_typed(g, g->r[fn].rt, true) : -1;
        if (d >= 0)
            return copy_call(g, (uint32_t)fn, (uint32_t)d, !pure, out);
    }
    if (chance(g, 8)) {
        /* a record or an array given whole, which may read itself
           (§ 6.8) */
        uint32_t n = 0, d = 0;
        for (uint32_t i = 0; i < g->nscope; i++) {
            uint32_t v = g->scope[i], vt = g->v[v].t;
            if ((is_record(g, vt) || g->ty[vt].k == K_ARRAY) &&
                writable(g, v) && below(g, ++n) == 0)
                d = v;
        }
        if (n)
            return copy_show(g, agg_self(g, g->v[d].t, depth(g), d), d, !pure,
                             out);
    }
    if (!pure && chance(g, 1)) {
        /* halt(0), halt(k) for k in 2..255 but 141, or a computed status
           that may be outside */
        return halt_stmt(g, out);
    }
    if (!pure && chance(g, 4))
        return str_loop(g, out);
    if (!pure && chance(g, 8)) {
        uint32_t k = heap_array(g, out);
        if (k)
            return k;
    }
    if (!pure && chance(g, 8)) {
        uint32_t s = read_write(g);
        if (s) {
            out[0] = s;
            return 1;
        }
    }
    if (!pure && chance(g, 4)) {
        out[0] = local_read(g);
        return 1;
    }
    if (!pure && chance(g, 10)) {
        uint32_t s = str_bytes(g);
        if (s) {
            out[0] = s;
            return 1;
        }
    }
    int sv = chance(g, 4) ? pick_var(g, T_STR, false, false) : -1;
    if (sv >= 0 && deep) {
        /* for var i: Int64 := 1 to length(s) (or down) printing s[i]; the
           body may assign s, and then each s[i] is checked again */
        uint32_t s = new_s(g, S_FOR);
        bool down = chance(g, 40);
        uint32_t one = lit(g, T_I64, 1 + below(g, 2));
        if (chance(g, 25)) { /* low(s) */
            uint32_t r = var_ref(g, (uint32_t)sv);
            one = new_e(g, E_SLEN, T_I64);
            g->e[one].op = 1;
            g->e[one].a = r;
        }
        uint32_t sr = var_ref(g, (uint32_t)sv);
        uint32_t len = new_e(g, E_SLEN, T_I64);
        g->e[len].op = chance(g, 30) ? 2 : 0; /* high(s) or length(s) */
        g->e[len].a = sr;
        uint32_t i = new_v(g, T_I64, V_LOOP);
        g->st[s].var = i;
        g->st[s].down = down;
        g->st[s].e = down ? len : one;
        g->st[s].e2 = down ? one : len;
        uint32_t mark = g->nscope;
        show(g, i);
        loop_body(g, s, 0);
        g->nscope = mark;
        if (writable(g, (uint32_t)sv) && chance(g, 40)) {
            /* s := copy(s, 1, k): shorter, and s[i] is checked again */
            uint32_t a = var_ref(g, (uint32_t)sv);
            uint32_t from = lit(g, T_I64, 1);
            uint32_t k = lit(g, T_I64, below(g, 4));
            uint32_t cp = new_e(g, E_COPY, T_STR);
            g->e[cp].a = a;
            g->e[cp].b = from;
            g->e[cp].c = k;
            uint32_t as = new_s(g, S_ASSIGN);
            g->st[as].var = (uint32_t)sv;
            g->st[as].e = cp;
            uint32_t bb = g->st[s].blk, nb = g->st[s].nblk;
            append(g, &bb, &nb, as, true);
            g->st[s].blk = bb;
            g->st[s].nblk = nb;
        }
        uint32_t ix = var_ref(g, i);
        uint32_t el = new_e(g, E_SIDX, T_B8);
        g->e[el].var = (uint32_t)sv;
        g->e[el].b = ix;
        uint32_t w = new_s(g, S_WRITE);
        g->st[w].args = keep_list(g, &el, 1);
        g->st[w].nargs = 1;
        uint32_t bb = g->st[s].blk, nb = g->st[s].nblk;
        append(g, &bb, &nb, w, chance(g, 50));
        g->st[s].blk = bb;
        g->st[s].nblk = nb;
        out[0] = s;
        return 1;
    }
    if (c < 14 && chance(g, 40)) {
        uint32_t s = declare_dyn(g);
        if (s) {
            out[0] = s;
            return 1 + fill_strings(g, s, out + 1);
        }
    }
    if (c < 14 && chance(g, 25)) {
        uint32_t s = declare_agg(g);
        if (s) {
            out[0] = s;
            return 1 + (g->st[s].e ? 0 : fill_strings(g, s, out + 1));
        }
    }
    if (c < 14) {
        out[0] = declare(g, var_type_s(g));
        return 1;
    }
    if (c < 36 && chance(g, 25)) {
        uint32_t f;
        int v = pick_field(g, NTYPES, true, &f);
        if (v >= 0) {
            uint32_t ft = field_type(g, record_of(g, g->v[v].t), f);
            uint32_t e = value_for(g, ft, depth(g));
            uint32_t s = new_s(g, S_ASSIGN);
            g->st[s].var = (uint32_t)v;
            g->st[s].fld = f + 1;
            g->st[s].e = e;
            out[0] = s;
            return 1;
        }
    }
    if (c < 36) {
        int a = chance(g, 30) ? pick_array(g, NTYPES, true) : -1;
        if (a >= 0) {
            uint32_t at = g->v[a].t;
            uint32_t s = new_s(g, S_ASSIGN);
            uint32_t ix = index_for(g, (uint32_t)a, depth(g));
            uint32_t e = value_for(g, g->ty[at].elem, depth(g));
            g->st[s].var = (uint32_t)a;
            g->st[s].idx = ix;
            g->st[s].e = e;
            out[0] = s;
            return 1;
        }
        int v = pick_var(g,
                         chance(g, 10)  ? T_STR
                         : chance(g, 8) ? T_BIG
                                        : any_type(g),
                         true, false);
        if (v < 0)
            v = pick_var(g, NTYPES, true, false);
        if (v >= 0) {
            uint32_t s = new_s(g, S_ASSIGN);
            uint32_t e = value_for(g, g->v[v].t, depth(g));
            g->st[s].var = (uint32_t)v;
            g->st[s].e = e;
            out[0] = s;
            return 1;
        }
    }
    if (c < 48 && deep) {
        uint32_t s = new_s(g, S_IF);
        uint32_t na = 1 + below(g, 3);
        xarm arms[3];
        for (uint32_t i = 0; i < na; i++) {
            arms[i].cond = expr(g, T_BOOL, depth(g), true);
            uint32_t mark = g->nscope;
            arms[i].blk = block(g, 1 + (int)below(g, 3), &arms[i].nblk);
            g->nscope = mark;
        }
        g->st[s].arm = g->narm;
        g->st[s].narm = na;
        for (uint32_t i = 0; i < na; i++) {
            LIMBA_GROW(g->arm, g->narm, g->caparm);
            g->arm[g->narm++] = arms[i];
        }
        if (chance(g, 50)) {
            uint32_t mark = g->nscope;
            uint32_t n;
            uint32_t b = block(g, 1 + (int)below(g, 3), &n);
            g->nscope = mark;
            g->st[s].alt = b;
            g->st[s].nalt = n;
            g->st[s].has_alt = true;
        }
        out[0] = s;
        return 1;
    }
    if (c < 56 && deep) {
        uint32_t decl, w = counter(g, &decl);
        uint32_t s = new_s(g, S_WHILE);
        uint32_t cond = binop(g, O_LT, T_BOOL, var_ref(g, w),
                              lit(g, T_I32, 1 + below(g, 4)));
        if (chance(g, 50))
            cond =
                binop(g, O_AND, T_BOOL, cond, expr(g, T_BOOL, depth(g), true));
        g->st[s].e = cond;
        loop_body(g, s, bump(g, w));
        out[0] = decl;
        out[1] = s;
        return 2;
    }
    if (c < 62 && deep) {
        uint32_t decl, w = counter(g, &decl);
        uint32_t s = new_s(g, S_REPEAT);
        loop_body(g, s, bump(g, w));
        uint32_t cond = binop(g, O_GE, T_BOOL, var_ref(g, w),
                              lit(g, T_I32, 1 + below(g, 4)));
        if (chance(g, 50))
            cond =
                binop(g, O_OR, T_BOOL, cond, expr(g, T_BOOL, depth(g), true));
        g->st[s].e = cond;
        out[0] = decl;
        out[1] = s;
        return 2;
    }
    if (c < 72 && deep) {
        /* an integer type, or a range, for the loop variable */
        uint32_t t = var_type(g);
        if (!is_int_base(base(g, t)))
            t = int_type(g);
        unsigned b = base(g, t);
        uint32_t s = new_s(g, S_FOR);
        bool down = chance(g, 30);
        uint32_t lo, hi;
        int x = pick_var(g, b, false, false);
        int arr = chance(g, 35) ? pick_indexed(g, b) : -1;
        uint32_t et = chance(g, 15) ? pick_enum(g) : 0;
        if (et) {
            /* over the values of an enumeration */
            uint32_t n = g->ty[et].elem;
            v128 a = below(g, n), z = below(g, n);
            if (down ? a < z : a > z) {
                v128 w = a;
                a = z;
                z = w;
            }
            t = et;
            lo = lit(g, et, a);
            hi = lit(g, et, z);
        } else if (arr >= 0) {
            /* over an array: t is the base of its index */
            t = b;
            lo = bound(g, down ? E_HIGH : E_LOW, (uint32_t)arr);
            hi = bound(g, down ? E_LOW : E_HIGH, (uint32_t)arr);
        } else if (x >= 0 && chance(g, 50)) {
            /* from a variable to a few steps away: the bound may overflow
               or leave the range */
            lo = var_ref(g, (uint32_t)x);
            hi = binop(g, down ? O_SUB : O_ADD, b, var_ref(g, (uint32_t)x),
                       lit(g, b, below(g, 4)));
        } else {
            v128 a = rand_in(g, lo_of(g, t), hi_of(g, t));
            v128 z = clamp_in(lo_of(g, t), hi_of(g, t),
                              down ? a - (v128)below(g, 5) + 1
                                   : a + (v128)below(g, 5) - 1);
            lo = lit(g, b, a);
            hi = lit(g, b, z);
        }
        uint32_t v = new_v(g, t, V_LOOP);
        g->st[s].var = v;
        g->st[s].e = lo;
        g->st[s].e2 = hi;
        g->st[s].down = down;
        uint32_t mark = g->nscope;
        show(g, v);
        loop_body(g, s, 0);
        if (!et && arr >= 0 && chance(g, 60))
            over_array(g, s, (uint32_t)arr, v, b);
        g->nscope = mark;
        out[0] = s;
        return 1;
    }
    if (c < 78 && deep) {
        unsigned b = int_type(g);
        uint32_t s = new_s(g, S_CASE);
        uint32_t et = chance(g, 30) ? pick_enum(g) : 0;
        uint32_t sel =
            et ? enum_expr(g, et, depth(g), true) : expr(g, b, depth(g), true);
        uint32_t st = et ? et : selector_type(g, sel);
        g->st[s].e = sel;
        uint32_t na = 1 + below(g, 3);
        bool all = et && chance(g, 50); /* every value, and no else */
        if (all)
            na = g->ty[et].elem < 3 ? g->ty[et].elem : 3;
        xarm arms[3];
        xlab labs[12];
        uint32_t nl = 0;
        for (uint32_t i = 0; i < na; i++) {
            arms[i].lab = nl;
            arms[i].nlab = 0;
            if (all) {
                /* the values split among the arms, in order */
                uint32_t n = g->ty[et].elem;
                v128 lo = (v128)(i * n / na), hi = (v128)((i + 1) * n / na) - 1;
                labs[nl++] = (xlab){lo, hi};
                arms[i].nlab = 1;
            }
            for (uint32_t k = 0, want = all ? 0 : 1 + below(g, 2); k < want;
                 k++) {
                v128 lo = rand_in(g, lo_of(g, st), hi_of(g, st));
                v128 hi = chance(g, 30) ? clamp_in(lo_of(g, st), hi_of(g, st),
                                                   lo + below(g, 6))
                                        : lo;
                if (overlaps(labs, nl, lo, hi))
                    continue;
                labs[nl++] = (xlab){lo, hi};
                arms[i].nlab++;
            }
            if (!arms[i].nlab) {
                na = i;
                break;
            }
            uint32_t mark = g->nscope;
            arms[i].blk = block(g, 1 + (int)below(g, 3), &arms[i].nblk);
            g->nscope = mark;
        }
        uint32_t first = g->nlab;
        for (uint32_t i = 0; i < nl; i++) {
            LIMBA_GROW(g->lab, g->nlab, g->caplab);
            g->lab[g->nlab++] = labs[i];
        }
        g->st[s].arm = g->narm;
        g->st[s].narm = na;
        for (uint32_t i = 0; i < na; i++) {
            arms[i].lab += first;
            LIMBA_GROW(g->arm, g->narm, g->caparm);
            g->arm[g->narm++] = arms[i];
        }
        uint32_t mark = g->nscope;
        uint32_t n;
        uint32_t bl = block(g, (int)below(g, 3), &n);
        g->nscope = mark;
        g->st[s].alt = bl;
        g->st[s].nalt = n;
        g->st[s].has_alt = !all;
        out[0] = s;
        return 1;
    }
    if (c < 84 && g->loops) {
        uint32_t s = new_s(g, chance(g, 50) ? S_EXIT : S_CONT);
        g->st[s].e = expr(g, T_BOOL, depth(g), true);
        out[0] = s;
        return 1;
    }
    if (c < 90 && !pure) {
        /* a procedure made before */
        uint32_t top = g->cur >= 0 ? (uint32_t)g->cur : g->nr, n = 0;
        uint32_t fn = 0;
        for (uint32_t i = 0; i < top; i++)
            if (!g->r[i].func && (!sealed(g) || g->r[i].sealed) &&
                below(g, ++n) == 0)
                fn = i;
        if (n) {
            uint32_t a[4];
            bool ok = true;
            for (uint32_t k = 0; k < g->r[fn].np && ok; k++) {
                uint32_t p = g->r[fn].par[k];
                uint32_t pt = g->v[p].t;
                if (g->ty[pt].k == K_OPEN) {
                    int v = pick_argument(g, pt, g->v[p].kind == V_VAR);
                    if (g->v[p].kind == V_IN && chance(g, 40))
                        a[k] = agg_literal(g, pt, 1);
                    else if (v < 0)
                        ok = false;
                    else
                        a[k] = var_ref(g, (uint32_t)v);
                } else if (is_record(g, pt) && g->v[p].kind == V_IN &&
                           chance(g, 15)) {
                    a[k] = agg_literal(g, pt, depth(g));
                } else if (is_record(g, pt) || is_ptr(g, pt)) {
                    a[k] = typed_arg(g, pt, g->v[p].kind == V_VAR);
                    ok = a[k] != 0;
                } else if (g->v[p].kind == V_OUT) {
                    /* a variable of its very type, not given to another
                       out parameter of the call (the copies back would
                       race) */
                    int v = pick_var(g, pt, true, true);
                    for (uint32_t j = 0; j < k && v >= 0; j++)
                        if (g->v[g->r[fn].par[j]].kind == V_OUT &&
                            g->e[a[j]].var == (uint32_t)v)
                            v = -1;
                    if (v < 0)
                        ok = false;
                    else
                        a[k] = var_ref(g, (uint32_t)v);
                } else if (g->v[p].kind == V_VAR) {
                    /* a var parameter takes a variable of its very type */
                    int v = pick_var(g, g->v[p].t, true, true);
                    if (v < 0)
                        ok = false;
                    else
                        a[k] = var_ref(g, (uint32_t)v);
                } else {
                    a[k] = value_for(g, g->v[p].t, depth(g));
                }
            }
            if (ok) {
                uint32_t s = new_s(g, S_CALL);
                g->st[s].fn = fn;
                g->st[s].args = keep_list(g, a, g->r[fn].np);
                g->st[s].nargs = g->r[fn].np;
                out[0] = s;
                /* what came back through an out parameter, printed */
                for (uint32_t k = 0; k < g->r[fn].np; k++)
                    if (g->v[g->r[fn].par[k]].kind == V_OUT &&
                        !in_function(g) && chance(g, 60)) {
                        uint32_t item = var_ref(g, g->e[a[k]].var);
                        if (is_enum(g, g->e[item].t)) { /* ord of it */
                            uint32_t o = new_e(g, E_ORD, T_U32);
                            g->e[o].a = item;
                            item = o;
                        }
                        uint32_t w = new_s(g, S_WRITE);
                        g->st[w].args = keep_list(g, &item, 1);
                        g->st[w].nargs = 1;
                        out[1] = w;
                        return 2;
                    }
                return 1;
            }
        }
    }
    if (pure) {
        /* a function has nothing to print: one more variable */
        out[0] = declare(g, var_type_s(g));
        return 1;
    }
    uint32_t items[5];
    uint32_t n = 1 + below(g, 3), k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i)
            items[k++] = new_e(g, E_STR, 0);
        unsigned u = chance(g, 12)   ? T_STR
                     : chance(g, 20) ? T_BIG
                                     : any_type(g);
        uint32_t a = expr(g, u, depth(g), true);
        items[k++] = format(g, a);
    }
    uint32_t s = new_s(g, S_WRITE);
    g->st[s].args = keep_list(g, items, k);
    g->st[s].nargs = k;
    out[0] = s;
    return 1;
}

/* writeln(val(s, x), " ", x) or writeln(readline(s), " ", s): what was
   read, shown at once; 0 if there is no variable for it */
static uint32_t read_write(G *g)
{
    uint32_t r = chance(g, 70) ? val_expr(g, depth(g)) : read_expr(g);
    if (!r)
        return 0;
    uint32_t v = g->e[r].var;
    uint32_t items[3] = {r, new_e(g, E_STR, 0), 0};
    items[2] = var_ref(g, v);
    uint32_t s = new_s(g, S_WRITE);
    g->st[s].args = keep_list(g, items, 3);
    g->st[s].nargs = 3;
    return s;
}

/* a jump that ends a block: return, or exit or continue in a loop */
static uint32_t jump(G *g)
{
    if (g->cur >= 0 && (!g->loops || chance(g, 50))) {
        uint32_t s = new_s(g, S_RETURN);
        uint32_t rt = g->r[g->cur].rt;
        if (g->r[g->cur].func) {
            uint32_t e = is_scalar(g, rt) ? value_for(g, rt, depth(g))
                                          : agg_expr(g, rt, depth(g));
            g->st[s].e = e;
        }
        return s;
    }
    return new_s(g, chance(g, 50) ? S_EXIT : S_CONT);
}

static uint32_t block(G *g, int n, uint32_t *count)
{
    uint32_t *x = NULL, nx = 0, cap = 0;
    g->nesting++;
    for (int i = 0; i < n && g->budget > 0; i++) {
        uint32_t some[32];
        uint32_t k = stmt(g, some);
        for (uint32_t j = 0; j < k; j++) {
            LIMBA_GROW(x, nx, cap);
            x[nx++] = some[j];
        }
    }
    if (g->nesting > 1 && (g->cur >= 0 || g->loops) && chance(g, 12)) {
        LIMBA_GROW(x, nx, cap);
        x[nx++] = jump(g);
    }
    g->nesting--;
    uint32_t at = keep_list(g, x, nx);
    free(x);
    *count = nx;
    return at;
}

/* append statement s to the block (b, n) */
static void append(G *g, uint32_t *b, uint32_t *n, uint32_t s, bool front)
{
    uint32_t *x = limba_xmalloc((*n + 1) * sizeof(uint32_t));
    if (*n) /* an empty list may have no storage yet */
        memcpy(x + front, g->ls + *b, *n * sizeof(uint32_t));
    x[front ? 0 : *n] = s;
    *b = keep_list(g, x, *n + 1);
    (*n)++;
    free(x);
}

static void routine(G *g)
{
    LIMBA_GROW(g->r, g->nr, g->capr);
    uint32_t id = g->nr++;
    memset(&g->r[id], 0, sizeof(xr));
    /* sometimes sealed: an Int64 and a Boolean first, base types, the
       globals out of sight (the unit of lx_gen -d, § 11) */
    bool seal = chance(g, 30);
    g->r[id].sealed = seal;
    g->r[id].func = chance(g, 50);
    g->r[id].rt = seal ? (chance(g, 15) ? T_STR : any_type(g)) : var_type_s(g);
    if (!seal && g->r[id].func && chance(g, 45)) {
        /* a record or an array of the program */
        uint32_t n = 0;
        for (uint32_t u = NTYPES; u < g->nty; u++)
            if ((is_record(g, u) || program_array(g, u)) && below(g, ++n) == 0)
                g->r[id].rt = u;
    }
    g->r[id].np = (uint8_t)(seal ? 2 + below(g, 3) : below(g, 4));
    uint32_t mark = g->nscope;
    /* sealed: the scope empty while it is made, given back after */
    uint32_t *outer = NULL;
    if (seal) {
        outer = limba_xmalloc((g->nscope + 1) * sizeof(*outer));
        memcpy(outer, g->scope, g->nscope * sizeof(*outer));
        g->nscope = 0;
    }
    for (uint32_t k = 0; k < g->r[id].np; k++) {
        if (seal) {
            uint32_t t = k == 0 ? T_I64 : k == 1 ? T_BOOL : any_type(g);
            bool byref = k >= 2 && !g->r[id].func && chance(g, 40);
            bool out = k >= 2 && !byref && !g->r[id].func && chance(g, 30);
            uint32_t p = new_v(g, t, out ? V_OUT : byref ? V_VAR : V_IN);
            g->r[id].par[k] = p;
            if (!out)
                show(g, p);
            continue;
        }
        uint32_t t = var_type_s(g);
        /* sometimes an open array, made from an array of the program */
        uint32_t n = 0, from = 0;
        for (uint32_t u = NTYPES; u < g->nty; u++)
            if (program_array(g, u) && !is_enum(g, g->ty[u].index) &&
                below(g, ++n) == 0)
                from = u;
        if (n && chance(g, 30))
            t = new_type(g, (xt){K_OPEN, g->ty[from].base, index_base(g, from),
                                 g->ty[from].elem, 0, 0, 0, 0});
        /* or a record (var in a procedure, in in a function: a function
           writes nothing) or a pointer (in) */
        uint32_t nr = 0, rec = 0;
        for (uint32_t u = NTYPES; u < g->nty; u++)
            if ((is_record(g, u) || is_ptr(g, u)) && below(g, ++nr) == 0)
                rec = u;
        if (nr && chance(g, 25))
            t = rec;
        bool byref = !g->r[id].func && chance(g, 40);
        if (is_record(g, t))
            byref = !g->r[id].func;
        if (is_ptr(g, t))
            byref = false;
        bool out = !g->r[id].func && chance(g, 30);
        if (out) {
            /* the type of a global scalar: a call finds an argument */
            uint32_t n = 0;
            for (uint32_t v = 0; v < g->nv; v++)
                if (g->v[v].kind == V_GLOBAL && is_scalar(g, g->v[v].t) &&
                    below(g, ++n) == 0)
                    t = g->v[v].t;
        }
        uint32_t p = new_v(g, t, out ? V_OUT : byref ? V_VAR : V_IN);
        g->r[id].par[k] = p;
        if (!out)
            show(g, p);
    }
    g->cur = (int)id;
    /* every out parameter is given a value first, from what is visible
       before it: reading it earlier is an error (L0053) */
    uint32_t init[4], ninit = 0;
    for (uint32_t k = 0; k < g->r[id].np; k++) {
        uint32_t p = g->r[id].par[k];
        if (g->v[p].kind != V_OUT)
            continue;
        uint32_t e = value_for(g, g->v[p].t, depth(g));
        uint32_t s = new_s(g, S_ASSIGN);
        g->st[s].var = p;
        g->st[s].e = e;
        init[ninit++] = s;
    }
    /* sometimes an array with computed bounds, alive at every return */
    uint32_t dyn = chance(g, 40) ? declare_dyn(g) : 0;
    for (uint32_t k = 0; k < g->r[id].np; k++)
        if (g->v[g->r[id].par[k]].kind == V_OUT)
            show(g, g->r[id].par[k]);
    uint32_t n;
    uint32_t b = block(g, 2 + (int)below(g, 5), &n);
    if (dyn)
        append(g, &b, &n, dyn, true);
    while (ninit)
        append(g, &b, &n, init[--ninit], true);
    /* an open parameter, so that its elements reach the output */
    int open = -1;
    for (uint32_t k = 0; k < g->r[id].np; k++)
        if (g->ty[g->v[g->r[id].par[k]].t].k == K_OPEN)
            open = (int)g->r[id].par[k];
    if (open >= 0 && !g->r[id].func && chance(g, 70)) {
        /* for var i := low(a) to high(a) do writeln(a[i]); end; */
        uint32_t at = g->v[open].t;
        uint32_t s = new_s(g, S_FOR);
        uint32_t i = new_v(g, index_base(g, at), V_LOOP);
        g->st[s].var = i;
        g->st[s].e = bound(g, E_LOW, (uint32_t)open);
        g->st[s].e2 = bound(g, E_HIGH, (uint32_t)open);
        uint32_t ix = var_ref(g, i); /* before: it may move the nodes */
        uint32_t el = new_e(g, E_INDEX, g->ty[at].elem);
        g->e[el].var = (uint32_t)open;
        g->e[el].a = ix;
        /* the index too: the bounds of the argument show (§ 6.8) */
        uint32_t items[3] = {var_ref(g, i), new_e(g, E_STR, 0), el};
        uint32_t w = new_s(g, S_WRITE);
        g->st[w].args = keep_list(g, items, 3);
        g->st[w].nargs = 3;
        g->st[s].blk = keep_list(g, &w, 1);
        g->st[s].nblk = 1;
        append(g, &b, &n, s, chance(g, 50));
    }
    if (g->r[id].func) {
        uint32_t s = new_s(g, S_RETURN);
        uint32_t rt = g->r[id].rt;
        if (!is_scalar(g, rt)) {
            /* a local made from a record or an array, changed, returned:
               var v := src; v.f := e; return v */
            uint32_t src = agg_expr(g, rt, depth(g));
            uint32_t v = new_v(g, rt, V_LOCAL);
            uint32_t decl = new_s(g, S_VAR);
            g->st[decl].var = v;
            g->st[decl].e = src;
            append(g, &b, &n, decl, false);
            for (uint32_t k = 0, nk = below(g, 3); k < nk; k++) {
                uint32_t a = new_s(g, S_ASSIGN);
                g->st[a].var = v;
                uint32_t e;
                if (is_record(g, rt)) {
                    uint32_t f = below(g, g->ty[rt].elem);
                    g->st[a].fld = f + 1;
                    e = value_for(g, field_type(g, rt, f), depth(g));
                } else {
                    uint32_t ix = lit(g, g->ty[rt].index,
                                      rand_in(g, g->ty[rt].lo, g->ty[rt].hi));
                    g->st[a].idx = ix;
                    e = value_for(g, g->ty[rt].elem, depth(g));
                }
                g->st[a].e = e;
                append(g, &b, &n, a, false);
            }
            g->st[s].e = var_ref(g, v);
        } else if (open >= 0 &&
                   base(g, g->ty[g->v[open].t].elem) == base(g, rt) &&
                   chance(g, 60)) {
            /* an element of the open parameter */
            uint32_t el = new_e(g, E_INDEX, g->ty[g->v[open].t].elem);
            uint32_t ix =
                bound(g, chance(g, 50) ? E_LOW : E_HIGH, (uint32_t)open);
            g->e[el].var = (uint32_t)open;
            g->e[el].a = ix;
            g->st[s].e = el;
        } else {
            g->st[s].e = value_for(g, rt, depth(g));
        }
        append(g, &b, &n, s, false);
    }
    g->r[id].blk = b;
    g->r[id].nblk = n;
    g->cur = -1;
    g->nscope = mark;
    if (outer) {
        memcpy(g->scope, outer, mark * sizeof(*outer));
        free(outer);
    }
}

/* ---- the source ---- */

typedef struct {
    char *b;
    size_t n, cap;
    uint32_t line, col;
    /* added to the lines recorded: UNIT_LINES in the unit, so that a
       place says its file */
    uint32_t lbase;
} text;

/* the lines of the unit Lib, recorded from here (a program has fewer) */
#define UNIT_LINES 1000000u

/* n bytes of s; a column is a code point, as the front end counts */
static void putn(text *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        LIMBA_GROW(o->b, o->n, o->cap);
        o->b[o->n++] = s[i];
        if (s[i] == '\n') {
            o->line++;
            o->col = 1;
        } else if (((unsigned char)s[i] & 0xc0) != 0x80) {
            o->col++;
        }
    }
}

static void put(text *o, const char *s)
{
    putn(o, s, strlen(s));
}

static void putf(text *o, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put(o, buf);
}

static void put_u128(text *o, u128 v)
{
    char buf[48];
    int i = (int)sizeof(buf) - 1;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    put(o, buf + i);
}

static void put_name(text *o, const G *g, uint32_t v)
{
    putf(o, "%c%u", vprefix[g->v[v].kind], v);
}

static void put_rname(text *o, const G *g, uint32_t r)
{
    /* a routine of Lib named by the program: directly, or qualified
       (§ 11.3), as the place falls */
    if (g->moved && g->moved[r] && !o->lbase && ((o->n * 2654435761u) >> 7) & 1)
        put(o, "Lib.");
    if (g->moved && g->moved[r] && !o->lbase)
        ((G *)g)->lib_named = true;
    putf(o, "%c%u", g->r[r].func ? 'f' : 'p', r);
}

/* the names of the types are upper case letters that no variable takes
   (Luxia does not tell the case apart): Y for the arrays */
static void put_type(text *o, const G *g, uint32_t t)
{
    if (t < NTYPES)
        put(o, tname[t]);
    else
        putf(o, "%c%u", "RRYOTPEDH"[g->ty[t].k], t);
}

static void put_value(text *o, unsigned t, v128 v)
{
    if (fam(t) == 'F') {
        /* the shortest form reads back to the same double */
        char buf[LIMBA_FMT_F64_MAX];
        double d = fval(v);
        limba_fmt_f64(buf, fabs(d));
        put(o, signbit(d) ? "(-" : "");
        put(o, buf);
        put(o, signbit(d) ? ")" : "");
    } else if (t == T_BOOL) {
        put(o, v ? "true" : "false");
    } else if (v < 0) {
        put(o, "(-");
        put_u128(o, (u128)(-(v + 1)) + 1);
        put(o, ")");
    } else if (fam(t) == 'B' && v > 9) {
        putf(o, "0x%llx", (unsigned long long)v);
    } else {
        put_u128(o, (u128)v);
    }
}

/* the decimal digits of BigInt value v, with a - if negative */
static void put_big(text *o, const G *g, v128 v)
{
    const limba_big *b = &g->big[(uint32_t)v];
    size_t size = limba_big_bits(b) / 3 + 5;
    char *buf = limba_xmalloc(size);
    put(o, limba_big_str(b, buf, size));
    free(buf);
}

static void here(xe *x, const text *o)
{
    x->line = o->line + o->lbase;
    x->col = o->col;
}

/* a literal of type t: the name of a value of an enumeration */
static void put_lit(text *o, const G *g, uint32_t t, v128 v)
{
    if (is_enum(g, t)) {
        putf(o, "q%ux%u", t, (unsigned)v);
    } else if (t == T_CHAR) {
        /* no control character in a literal: their names (§ 3) */
        static const char *const ctl[] = {
            [0] = "NUL", [9] = "TAB", [10] = "LF", [13] = "CR"};
        char b[4];
        if (v < 14 && ctl[v]) {
            put(o, ctl[v]);
        } else if (v == '\'') {
            put(o, "'''");
        } else {
            put(o, "'");
            putn(o, b, utf8((uint32_t)v, b));
            put(o, "'");
        }
    } else if (t == T_BIG) {
        bool neg = g->big[(uint32_t)v].neg;
        put(o, neg ? "(" : "");
        put_big(o, g, v);
        put(o, neg ? ")" : "");
    } else if (t == T_STR) {
        /* a quote inside is written twice (§ 3) */
        const struct xstr *x = &g->str[(uint32_t)v];
        put(o, "\"");
        for (uint32_t k = 0; k < x->n; k++)
            putn(o, x->b[k] == '"' ? "\"\"" : x->b + k, x->b[k] == '"' ? 2 : 1);
        put(o, "\"");
    } else {
        put_value(o, base(g, t), v);
    }
}

static void pexpr(G *g, text *o, uint32_t i)
{
    xe *x = &g->e[i];
    switch (x->k) {
    case E_LIT:
        put_lit(o, g, x->t, x->lit);
        break;
    case E_VAR:
        put_name(o, g, x->var);
        break;
    case E_STR:
        put(o, "\" \"");
        break;
    case E_BIN:
        put(o, "(");
        pexpr(g, o, x->a);
        put(o, " ");
        here(&g->e[i], o);
        put(o, otext[g->e[i].op]);
        put(o, " ");
        pexpr(g, o, g->e[i].b);
        put(o, ")");
        break;
    case E_UN:
        put(o, "(");
        here(x, o);
        put(o, x->op == O_NEG ? "-(" : x->op == O_ABS ? "abs (" : "not (");
        pexpr(g, o, g->e[i].a);
        put(o, "))");
        break;
    case E_CONV:
        here(x, o);
        put_type(o, g, x->t);
        put(o, "(");
        pexpr(g, o, g->e[i].a);
        put(o, ")");
        break;
    case E_CALL:
        here(x, o);
        put_rname(o, g, x->fn);
        put(o, "(");
        for (uint32_t k = 0; k < g->e[i].nargs; k++) {
            if (k)
                put(o, ", ");
            pexpr(g, o, g->ls[g->e[i].args + k]);
        }
        put(o, ")");
        break;
    case E_INDEX:
    case E_HIDX:
        put_name(o, g, x->var);
        here(x, o);
        put(o, "[");
        pexpr(g, o, g->e[i].a);
        put(o, "]");
        break;
    case E_HLEN:
        here(x, o); /* nil and dangling at the name of the function */
        put(o, x->op == 0 ? "low(" : x->op == 1 ? "high(" : "length(");
        put_name(o, g, x->var);
        put(o, "^)");
        break;
    case E_FIELD:
        put_name(o, g, x->var);
        here(x, o); /* a nil pointer is reported at the . */
        putf(o, ".f%u", x->b);
        break;
    case E_MATH:
        putf(o, "%s(", mtext[x->op]);
        pexpr(g, o, g->e[i].a);
        put(o, ")");
        break;
    case E_SLEN:
        put(o, x->op == 1 ? "low(" : x->op == 2 ? "high(" : "length(");
        pexpr(g, o, x->a);
        put(o, ")");
        break;
    case E_VAL:
        put(o, "val(");
        pexpr(g, o, x->a);
        put(o, ", ");
        put_name(o, g, g->e[i].var);
        put(o, ")");
        break;
    case E_READ:
        put(o, "readline(");
        put_name(o, g, x->var);
        put(o, ")");
        break;
    case E_ARGC:
        put(o, "argcount()");
        break;
    case E_ARG:
        here(x, o); /* the index is checked at its name */
        put(o, "arg(");
        pexpr(g, o, x->a);
        put(o, ")");
        break;
    case E_SIDX:
        put_name(o, g, x->var);
        here(x, o); /* the index is checked at the [ */
        put(o, "[");
        pexpr(g, o, g->e[i].b);
        put(o, "]");
        break;
    case E_COPY:
        here(x, o); /* checked at its name */
        put(o, "copy(");
        pexpr(g, o, x->a);
        put(o, ", ");
        pexpr(g, o, g->e[i].b);
        put(o, ", ");
        pexpr(g, o, g->e[i].c);
        put(o, ")");
        break;
    case E_STRF:
        put(o, "str(");
        pexpr(g, o, x->a);
        put(o, ")");
        break;
    case E_FMT:
        pexpr(g, o, x->a);
        put(o, ":");
        pexpr(g, o, g->e[i].b);
        if (g->e[i].c) {
            put(o, ":");
            pexpr(g, o, g->e[i].c);
        }
        break;
    case E_CHR:
        here(x, o); /* checked at its name */
        put(o, "chr(");
        pexpr(g, o, g->e[i].a);
        put(o, ")");
        break;
    case E_CFIELD:
        pexpr(g, o, x->a);
        here(&g->e[i], o); /* a field no one assigned is caught at the . */
        putf(o, ".f%u", g->e[i].b);
        break;
    case E_CINDEX:
        pexpr(g, o, x->a);
        here(&g->e[i], o); /* the index is checked at the [ */
        put(o, "[");
        pexpr(g, o, g->e[i].b);
        put(o, "]");
        break;
    case E_NIL:
        put(o, "nil");
        break;
    case E_SUCC:
    case E_PRED:
    case E_ORD:
        here(x, o); /* succ and pred are checked at their name */
        put(o, x->k == E_SUCC ? "succ(" : x->k == E_PRED ? "pred(" : "ord(");
        pexpr(g, o, g->e[i].a);
        put(o, ")");
        break;
    case E_LOW:
    case E_HIGH:
    case E_LEN:
        put(o, x->k == E_LOW ? "low(" : x->k == E_HIGH ? "high(" : "length(");
        put_name(o, g, x->var);
        put(o, ")");
        break;
    case E_AGG: {
        bool rec = is_record(g, x->t);
        put(o, "{");
        for (uint32_t k = 0; k < x->nargs; k++) {
            if (k)
                put(o, rec ? "; " : ", ");
            if (rec) {
                putf(o, "f%u: ", k);
            } else if (x->op) {
                pexpr(g, o, g->ls[g->e[i].a + k]);
                if (g->ls[g->e[i].b + k]) {
                    put(o, "..");
                    pexpr(g, o, g->ls[g->e[i].b + k]);
                }
                put(o, ": ");
            }
            pexpr(g, o, g->ls[g->e[i].args + k]);
        }
        if (g->e[i].c) {
            put(o, g->e[i].nargs ? ", else " : "else ");
            pexpr(g, o, g->e[i].c);
        }
        put(o, "}");
        break;
    }
    case E_IN:
        put(o, "(");
        pexpr(g, o, x->a);
        put(o, " in ");
        if (g->e[i].fn) {
            put_type(o, g, g->e[i].fn);
        } else {
            pexpr(g, o, g->e[i].b);
            put(o, "..");
            pexpr(g, o, g->e[i].c);
        }
        put(o, ")");
        break;
    }
}

static void pblock(G *g, text *o, uint32_t b, uint32_t n, int ind);

/* k = e; or k: T = e; */
static void pconst(G *g, text *o, uint32_t si)
{
    uint32_t v = g->st[si].var;
    put_name(o, g, v);
    if (g->v[v].kind == V_TCONST) {
        put(o, ": ");
        put_type(o, g, g->v[v].t);
    }
    put(o, " = ");
    pexpr(g, o, g->st[si].e);
    put(o, ";\n");
}

static void indent(text *o, int ind)
{
    for (int i = 0; i < ind; i++)
        put(o, "  ");
}

static void pargs(G *g, text *o, uint32_t args, uint32_t n)
{
    put(o, "(");
    for (uint32_t k = 0; k < n; k++) {
        if (k)
            put(o, ", ");
        pexpr(g, o, g->ls[args + k]);
    }
    put(o, ")");
}

static void pstmt(G *g, text *o, uint32_t si, int ind)
{
    xs s = g->st[si];
    indent(o, ind);
    g->st[si].line = o->line + o->lbase;
    g->st[si].col = o->col;
    switch (s.k) {
    case S_VAR:
        put(o, "var ");
        g->st[si].nline = o->line + o->lbase;
        g->st[si].ncol = o->col;
        put_name(o, g, s.var);
        put(o, ": ");
        if (g->ty[g->v[s.var].t].k == K_DYN) {
            const xt *d = &g->ty[g->v[s.var].t];
            putf(o, "array[%s range ", tname[d->index]);
            pexpr(g, o, s.e);
            put(o, "..");
            pexpr(g, o, g->st[si].e2);
            put(o, "] of ");
            put_type(o, g, d->elem);
            put(o, ";\n");
            break;
        }
        put_type(o, g, g->v[s.var].t);
        if (s.e) {
            put(o, " := ");
            pexpr(g, o, s.e);
        }
        put(o, ";\n");
        break;
    case S_ASSIGN:
        put_name(o, g, s.var);
        if (s.fld) {
            g->st[si].iline = o->line + o->lbase; /* nil is reported at the . */
            g->st[si].icol = o->col;
            putf(o, ".f%u", s.fld - 1);
        }
        if (s.idx) {
            g->st[si].iline =
                o->line + o->lbase; /* the index is checked at the [ */
            g->st[si].icol = o->col;
            put(o, "[");
            pexpr(g, o, s.idx);
            put(o, "]");
        }
        put(o, " := ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_IF:
        for (uint32_t k = 0; k < s.narm; k++) {
            if (k)
                indent(o, ind);
            put(o, k ? "elsif " : "if ");
            pexpr(g, o, g->arm[s.arm + k].cond);
            put(o, " then\n");
            pblock(g, o, g->arm[s.arm + k].blk, g->arm[s.arm + k].nblk,
                   ind + 1);
        }
        if (s.has_alt) {
            indent(o, ind);
            put(o, "else\n");
            pblock(g, o, s.alt, s.nalt, ind + 1);
        }
        indent(o, ind);
        put(o, "end;\n");
        break;
    case S_WHILE:
        put(o, "while ");
        pexpr(g, o, s.e);
        put(o, " do\n");
        pblock(g, o, s.blk, s.nblk, ind + 1);
        indent(o, ind);
        put(o, "end;\n");
        break;
    case S_REPEAT:
        put(o, "repeat\n");
        pblock(g, o, s.blk, s.nblk, ind + 1);
        indent(o, ind);
        put(o, "until ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_FOR:
        put(o, "for var ");
        put_name(o, g, s.var);
        put(o, ": ");
        put_type(o, g, g->v[s.var].t);
        put(o, " := ");
        pexpr(g, o, s.e);
        put(o, s.down ? " downto " : " to ");
        pexpr(g, o, s.e2);
        put(o, " do\n");
        pblock(g, o, s.blk, s.nblk, ind + 1);
        indent(o, ind);
        put(o, "end;\n");
        break;
    case S_CASE: {
        uint32_t t = g->e[s.e].t;
        put(o, "case ");
        pexpr(g, o, s.e);
        put(o, " of\n");
        for (uint32_t k = 0; k < s.narm; k++) {
            const xarm *a = &g->arm[s.arm + k];
            indent(o, ind + 1);
            put(o, "when ");
            for (uint32_t j = 0; j < a->nlab; j++) {
                const xlab *l = &g->lab[a->lab + j];
                if (j)
                    put(o, ", ");
                put_lit(o, g, t, l->lo);
                if (l->hi != l->lo) {
                    put(o, "..");
                    put_lit(o, g, t, l->hi);
                }
            }
            put(o, ":\n");
            pblock(g, o, a->blk, a->nblk, ind + 2);
        }
        if (s.has_alt) {
            indent(o, ind + 1);
            put(o, "else\n");
            pblock(g, o, s.alt, s.nalt, ind + 2);
        }
        indent(o, ind);
        put(o, "end;\n");
        break;
    }
    case S_WRITE:
        put(o, "writeln");
        pargs(g, o, s.args, s.nargs);
        put(o, ";\n");
        break;
    case S_CALL:
        put_rname(o, g, s.fn);
        pargs(g, o, s.args, s.nargs);
        put(o, ";\n");
        break;
    case S_EXIT:
    case S_CONT:
        put(o, s.k == S_EXIT ? "exit" : "continue");
        if (s.e) {
            put(o, " when ");
            pexpr(g, o, s.e);
        }
        put(o, ";\n");
        break;
    case S_RETURN:
        put(o, "return");
        if (s.e) {
            put(o, " ");
            pexpr(g, o, s.e);
        }
        put(o, ";\n");
        break;
    case S_CONST:
        put(o, "const ");
        pconst(g, o, si);
        break;
    case S_NEW:
        put_name(o, g, s.var);
        put(o, " := new(");
        put_type(o, g, g->ty[g->v[s.var].t].elem);
        put(o, ");\n");
        break;
    case S_COPY:
        put_name(o, g, s.var);
        put(o, " := ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_DISPOSE:
    case S_HFREE:
        put(o, "dispose(");
        put_name(o, g, s.var);
        put(o, ");\n");
        break;
    case S_HNEW:
        put(o, "var ");
        g->st[si].nline = o->line + o->lbase;
        g->st[si].ncol = o->col;
        put_name(o, g, s.var);
        put(o, " := new(");
        put_type(o, g, g->ty[g->v[s.var].t].elem);
        put(o, " range ");
        pexpr(g, o, s.e);
        put(o, "..");
        pexpr(g, o, s.e2);
        put(o, ");\n");
        break;
    case S_HSET:
        put_name(o, g, s.var);
        g->st[si].iline =
            o->line + o->lbase; /* nil, dangling and index at the [ */
        g->st[si].icol = o->col;
        put(o, "[");
        pexpr(g, o, s.idx);
        put(o, "] := ");
        pexpr(g, o, s.e);
        put(o, ";\n");
        break;
    case S_MOVE:
        put(o, "move(");
        put_name(o, g, s.var);
        put(o, "^, ");
        pexpr(g, o, s.e);
        put(o, ", ");
        put_name(o, g, s.idx);
        put(o, "^, ");
        pexpr(g, o, s.e2);
        put(o, ", ");
        pexpr(g, o, s.fld);
        put(o, ");\n");
        break;
    case S_REV:
    case S_TRANS:
    case S_OCC:
        if (s.k == S_TRANS) {
            /* the table first, in its own lines */
            putf(o, "var z%u: array[Byte] of Byte;\n", si);
            indent(o, ind);
            putf(o, "for var j%u := low(z%u) to high(z%u) do\n", si, si, si);
            indent(o, ind + 1);
            putf(o, "z%u[j%u] := j%u * %u + %u;\n", si, si, si, s.fn, s.args);
            indent(o, ind);
            put(o, "end;\n");
            indent(o, ind);
        }
        if (s.k == S_OCC)
            put(o, "writeln(");
        /* nil, dangling, range and index at the name */
        g->st[si].line = o->line + o->lbase;
        g->st[si].col = o->col;
        put(o, s.k == S_REV     ? "reverse("
               : s.k == S_TRANS ? "translate("
                                : "occurrences(");
        put_name(o, g, s.var);
        put(o, "^, ");
        pexpr(g, o, s.e);
        put(o, ", ");
        pexpr(g, o, s.fld);
        if (s.k == S_TRANS) {
            putf(o, ", z%u", si);
        } else if (s.k == S_OCC) {
            put(o, ", ");
            put_name(o, g, s.idx);
            put(o, "^)");
        }
        put(o, ");\n");
        break;
    case S_RDZ:
        putf(o, "var z%u: array[Int32 range 1..4] of Byte;\n", si);
        indent(o, ind);
        put(o, "writeln(");
        g->st[si].line = o->line + o->lbase;
        g->st[si].col = o->col;
        putf(o, "readbytes(z%u, ", si);
        pexpr(g, o, s.e);
        put(o, ", ");
        pexpr(g, o, s.fld);
        put(o, "));\n");
        break;
    case S_RDB:
    case S_WRB:
    case S_WRS:
        if (s.k == S_RDB)
            put(o, "writeln(");
        /* nil, dangling, range and index at the name */
        g->st[si].line = o->line + o->lbase;
        g->st[si].col = o->col;
        put(o, s.k == S_RDB ? "readbytes(" : "writebytes(");
        put_name(o, g, s.var);
        put(o, s.k == S_WRS ? ", " : "^, ");
        pexpr(g, o, s.e);
        put(o, ", ");
        pexpr(g, o, s.fld);
        put(o, s.k == S_RDB ? "));\n" : ");\n");
        break;
    case S_HALT:
        put(o, "halt(");
        pexpr(g, o, s.e);
        put(o, ");\n");
        break;
    }
}

static void pblock(G *g, text *o, uint32_t b, uint32_t n, int ind)
{
    for (uint32_t i = 0; i < n; i++)
        pstmt(g, o, g->ls[b + i], ind);
}

/* routine r, or only its heading */
static void routine_text(G *g, text *o, uint32_t r, bool heading)
{
    const xr *x = &g->r[r];
    put(o, x->func ? "\nfunction " : "\nprocedure ");
    putf(o, "%c%u", x->func ? 'f' : 'p', r);
    put(o, "(");
    for (uint32_t k = 0; k < x->np; k++) {
        uint32_t p = x->par[k];
        if (k)
            put(o, "; ");
        if (g->v[p].kind == V_VAR)
            put(o, "var ");
        if (g->v[p].kind == V_OUT)
            put(o, "out ");
        put_name(o, g, p);
        put(o, ": ");
        put_type(o, g, g->v[p].t);
    }
    put(o, ")");
    if (x->func) {
        put(o, ": ");
        put_type(o, g, x->rt);
    }
    if (heading) {
        put(o, ";\n");
        return;
    }
    put(o, ";\nbegin\n");
    pblock(g, o, x->blk, x->nblk, 1);
    putf(o, "end %c%u;\n", x->func ? 'f' : 'p', r);
}

/* ---- the web of units (§ 11) ----

   Beside Lib, units of their own that the program uses: from 2 to 4 of
   the program (Wa, Wb, ...), whose initialisations read and write each
   other's variables, and sometimes a small library with collisions
   between the two spaces. Each unit W has
     ucW, ready (a constant never written), uxW, written by its
     initialisation, uyW, a constant that at most one other
     initialisation changes through USetW; UGetW reads uxW.
   The initialisations follow a valid order (pos); a read of uxW, or of
   uyW after its writer, is a constraint (§ 11.5) the order respects, and
   the values do not depend on which valid order the compiler takes. The
   uses that no constraint asks, in implementations, go both ways
   (cycles), and the names are written directly or qualified. The
   program prints every value first. */

#define WEB_MAX 4

struct web {
    unsigned n;
    unsigned pos[WEB_MAX]; /* the place of each unit in a valid order */
    unsigned order[WEB_MAX];
    int64_t c[WEB_MAX], x[WEB_MAX], y[WEB_MAX], y0[WEB_MAX];
    int writer[WEB_MAX];  /* the unit whose initialisation sets uy, -1 */
    int64_t set[WEB_MAX]; /* what it adds */
    uint8_t use[WEB_MAX][WEB_MAX]; /* 1 in the implementation, 2 in the
                                      interface */
    /* the terms of each initialisation: kind (0 uc, 1 ux, 2 UGet, 3 uy),
       unit, written qualified */
    uint8_t tk[WEB_MAX][6], tu[WEB_MAX][6], tq[WEB_MAX][6];
    unsigned nt[WEB_MAX];
    int64_t k[WEB_MAX];
    int dup[2]; /* two units that give udup, -1 */
    int hid;    /* the unit whose uc the program hides, -1 */
    bool lib;   /* Coll in both spaces, Lb and Lbase */
    bool named; /* Coll gives a variable Lb */
    text out;   /* the line the program prints first */
};

static void web_make(G *g, struct web *w)
{
    memset(w, 0, sizeof(*w));
    w->n = 2 + below(g, WEB_MAX - 1);
    for (unsigned i = 0; i < w->n; i++)
        w->order[i] = i;
    for (unsigned i = w->n - 1; i > 0; i--) {
        unsigned j = below(g, i + 1), t = w->order[i];
        w->order[i] = w->order[j];
        w->order[j] = t;
    }
    for (unsigned i = 0; i < w->n; i++) {
        w->pos[w->order[i]] = i;
        w->c[i] = below(g, 1000);
        w->y0[i] = w->y[i] = below(g, 1000);
        w->k[i] = below(g, 1000);
        w->writer[i] = -1;
    }
    /* the writers of the uy: any other unit */
    for (unsigned j = 0; j < w->n; j++)
        if (chance(g, 50)) {
            unsigned i = below(g, w->n - 1);
            w->writer[j] = (int)(i >= j ? i + 1 : i);
            w->set[j] = 1 + below(g, 999);
            w->use[w->writer[j]][j] = 1;
        }
    /* the terms: what comes before in the order, the ready ones of any */
    for (unsigned i = 0; i < w->n; i++) {
        unsigned nt = below(g, 5);
        for (unsigned t = 0; t < nt; t++) {
            unsigned j = below(g, w->n);
            unsigned kind = below(g, 4);
            if (j == i)
                kind = 0; /* its own uc */
            else if ((kind == 1 || kind == 2) && w->pos[j] > w->pos[i])
                kind = 0; /* its ux is not written yet: the ready one */
            else if (kind == 3 && w->writer[j] >= 0 && w->writer[j] != (int)i &&
                     w->pos[w->writer[j]] > w->pos[i])
                kind = 0; /* uy not yet set by its writer */
            w->tk[i][w->nt[i]] = (uint8_t)kind;
            w->tu[i][w->nt[i]] = (uint8_t)j;
            w->tq[i][w->nt[i]] = (uint8_t)(j == i ? 0 : chance(g, 50));
            w->nt[i]++;
            if (j != i)
                w->use[i][j] = 1;
        }
    }
    /* the uses to the interface where the order allows it */
    for (unsigned i = 0; i < w->n; i++)
        for (unsigned j = 0; j < w->n; j++)
            if (w->use[i][j] && w->pos[j] < w->pos[i] && chance(g, 40))
                w->use[i][j] = 2;
    w->dup[0] = w->dup[1] = -1;
    if (chance(g, 50)) {
        w->dup[0] = (int)below(g, w->n);
        w->dup[1] = (int)((w->dup[0] + 1 + below(g, w->n - 1)) % w->n);
    }
    w->hid = chance(g, 50) ? (int)below(g, w->n) : -1;
    w->lib = chance(g, 50);
    w->named = w->lib && chance(g, 50);
    /* the run, in the order */
    for (unsigned o = 0; o < w->n; o++) {
        unsigned i = w->order[o];
        for (unsigned j = 0; j < w->n; j++)
            if (w->writer[j] == (int)i)
                w->y[j] = (w->y[j] + w->set[j]) % 1000;
        int64_t v = w->k[i];
        for (unsigned t = 0; t < w->nt[i]; t++) {
            unsigned j = w->tu[i][t];
            v += w->tk[i][t] == 0   ? w->c[j]
                 : w->tk[i][t] == 1 ? w->x[j]
                 : w->tk[i][t] == 2 ? w->x[j] * 2 + w->c[j]
                                    : w->y[j];
        }
        w->x[i] = v % 1000;
    }
}

static void web_uses(text *o, const struct web *w, unsigned i, uint8_t part)
{
    bool any = false;
    for (unsigned j = 0; j < w->n; j++)
        if (w->use[i][j] == part) {
            putf(o, "%sW%c", any ? ", " : "uses ", 'a' + j);
            any = true;
        }
    if (any)
        put(o, ";\n");
}

/* unit Wi */
static void web_unit(const struct web *w, unsigned i, text *o)
{
    char u = (char)('a' + i);
    putf(o, "unit W%c;\n\ninterface\n", u);
    web_uses(o, w, i, 2);
    putf(o, "var\n  uc%c: Int64 := %d;\n  ux%c: Int64;\n  uy%c: Int64 := %d;\n",
         u, (int)w->c[i], u, u, (int)w->y0[i]);
    if (w->dup[0] == (int)i || w->dup[1] == (int)i) {
        putf(o, "  udup: Int64 := %d;\n", 10 + (int)i);
        /* it hides the other's, if it uses that unit: on purpose */
        int other = w->dup[w->dup[0] == (int)i];
        if (w->use[i][other])
            putf(o, "pragma hides(W%c.udup);\n", 'a' + other);
    }
    putf(o, "function UGet%c(): Int64;\nprocedure USet%c(n: Int64);\n", u, u);
    put(o, "\nimplementation\n");
    web_uses(o, w, i, 1);
    /* putf holds 128 bytes: a piece at a time */
    putf(o, "\nfunction UGet%c(): Int64;\nbegin\n", u);
    putf(o, "  return ux%c * 2 + uc%c;\nend UGet%c;\n", u, u, u);
    putf(o, "\nprocedure USet%c(n: Int64);\nbegin\n", u);
    putf(o, "  uy%c := (uy%c + n) mod 1000;\nend USet%c;\n\nbegin\n", u, u, u);
    for (unsigned j = 0; j < w->n; j++)
        if (w->writer[j] == (int)i)
            putf(o, "  W%c.USet%c(%d);\n", 'a' + j, 'a' + j, (int)w->set[j]);
    putf(o, "  ux%c := (%d", u, (int)w->k[i]);
    static const char *const what[] = {"uc", "ux", "UGet", "uy"};
    for (unsigned t = 0; t < w->nt[i]; t++) {
        unsigned j = w->tu[i][t];
        put(o, " + ");
        if (w->tq[i][t])
            putf(o, "W%c.", 'a' + j);
        putf(o, "%s%c%s", what[w->tk[i][t]], 'a' + j,
             w->tk[i][t] == 2 ? "()" : "");
    }
    putf(o, ") mod 1000;\nend W%c.\n", u);
}

/* the program's part: its uses after Lib */
static void web_program_uses(text *o, const struct web *w)
{
    for (unsigned i = 0; i < w->n; i++)
        putf(o, ", W%c", 'a' + i);
    if (w->lib)
        put(o, ", Coll, Lb");
}

/* the program's declarations: a uc hidden on purpose */
static void web_program_decls(text *o, const struct web *w)
{
    if (w->hid >= 0)
        putf(o, "\nvar\n  uc%c: Int64 := %d;\npragma hides(W%c.uc%c);\n",
             'a' + w->hid, 2000 + w->hid, 'a' + w->hid, 'a' + w->hid);
}

/* the first statement of the program: every value, and the line it
   prints */
static void web_program_print(text *o, struct web *w)
{
    text *out = &w->out;
    put(o, "  writeln(");
    bool first = true;
#define WEB_ITEM(...)                                                          \
    do {                                                                       \
        if (!first) {                                                          \
            put(o, ", \" \", ");                                               \
            put(out, " ");                                                     \
        }                                                                      \
        first = false;                                                         \
        putf(o, __VA_ARGS__);                                                  \
    } while (0)
    for (unsigned i = 0; i < w->n; i++) {
        char u = (char)('a' + i);
        /* direct or qualified, as the values fall */
        bool q = (w->c[i] + i) & 1;
        char qu[4] = "", nq[4] = "";
        snprintf(q ? qu : nq, 4, "W%c.", u);
        WEB_ITEM("%sux%c", qu, u);
        putf(out, "%d", (int)w->x[i]);
        WEB_ITEM("%sUGet%c()", nq, u);
        putf(out, "%d", (int)(w->x[i] * 2 + w->c[i]));
        WEB_ITEM("%suy%c", qu, u);
        putf(out, "%d", (int)w->y[i]);
        WEB_ITEM("W%c.uc%c", u, u);
        putf(out, "%d", (int)w->c[i]);
        /* written alone: the program's own if it hides it */
        WEB_ITEM("uc%c", u);
        putf(out, "%d", w->hid == (int)i ? 2000 + (int)i : (int)w->c[i]);
    }
    for (unsigned k = 0; k < 2; k++)
        if (w->dup[k] >= 0) {
            WEB_ITEM("W%c.udup", 'a' + w->dup[k]);
            putf(out, "%d", 10 + w->dup[k]);
        }
    if (w->lib) {
        WEB_ITEM("%s", w->c[0] & 2 ? "cv" : "Coll.cv");
        put(out, "100");
        WEB_ITEM("Lb.LGet()");
        put(out, "320");
        if (w->named) {
            WEB_ITEM("Lb");
            put(out, "42");
            WEB_ITEM("Coll.Lb");
            put(out, "42");
        }
    }
#undef WEB_ITEM
    put(o, ");\n");
    put(out, "\n");
}

/* the files of the web: name, text, in the library */
static unsigned web_files(const struct web *w, limba_lxgen_file *f)
{
    unsigned n = 0;
    for (unsigned i = 0; i < w->n; i++, n++) {
        text t = {NULL, 0, 0, 1, 1, 0};
        web_unit(w, i, &t);
        LIMBA_GROW(t.b, t.n, t.cap);
        t.b[t.n] = 0;
        f[n].text = t.b;
        snprintf(f[n].name, sizeof(f[n].name), "w%c", 'a' + i);
        f[n].library = false;
    }
    if (!w->lib)
        return n;
    static const struct {
        const char *name, *text;
        bool library;
    } lib[] = {
        {"coll",
         "unit Coll;\n\ninterface\nvar\n  cv: Int64 := 100;\n%s\n"
         "implementation\nend Coll.\n",
         false},
        {"coll",
         "unit Coll;\n\ninterface\nvar\n  cv: Int64 := 300;\n\n"
         "implementation\nend Coll.\n",
         true},
        {"lb",
         "unit Lb;\n\ninterface\nfunction LGet(): Int64;\n\nimplementation\n"
         "uses Coll, Lbase;\n\nfunction LGet(): Int64;\nbegin\n  return "
         "Coll.cv + Lbase.bk;\nend LGet;\nend Lb.\n",
         true},
        {"lbase",
         "unit Lbase;\n\ninterface\nconst\n  bk = 20;\n\nimplementation\n"
         "end Lbase.\n",
         true},
    };
    for (unsigned k = 0; k < 4; k++, n++) {
        char buf[512];
        snprintf(buf, sizeof(buf), lib[k].text,
                 w->named ? "  Lb: Int64 := 42;\n" : "");
        f[n].text = limba_xmalloc(strlen(buf) + 1);
        memcpy(f[n].text, buf, strlen(buf) + 1);
        snprintf(f[n].name, sizeof(f[n].name), "%s", lib[k].name);
        f[n].library = lib[k].library;
    }
    return n;
}

static void program(G *g, text *o, uint32_t nglob)
{
    g->lib_named = false;
    put(o, g->units ? "program Random;\nuses Lib" : "program Random;\n");
    if (g->units) {
        if (g->web)
            web_program_uses(o, g->web);
        put(o, ";\n");
    }
    if (g->ngc) {
        put(o, "\nconst\n");
        for (uint32_t k = 0; k < g->ngc; k++) {
            put(o, "  ");
            pconst(g, o, g->gc[k]);
        }
    }
    bool types = false;
    for (uint32_t t = NTYPES; t < g->nty; t++)
        types |= g->ty[t].k != K_DYN;
    if (types) {
        put(o, "\ntype\n");
        for (uint32_t t = NTYPES; t < g->nty; t++) {
            const xt *x = &g->ty[t];
            if (x->k == K_DYN)
                continue; /* written where its variable is declared */
            put(o, "  ");
            put_type(o, g, t);
            put(o, " = ");
            if (x->k == K_RANGE && x->elo) {
                put(o, tname[x->base]);
                put(o, " range ");
                pexpr(g, o, x->elo);
                put(o, "..");
                pexpr(g, o, g->ty[t].ehi);
            } else if (x->k == K_RANGE) {
                put(o, tname[x->base]);
                put(o, " range ");
                put_value(o, x->base, x->lo);
                put(o, "..");
                put_value(o, x->base, x->hi);
            } else if (x->k == K_RECORD) {
                put(o, "record");
                for (uint32_t f = 0; f < x->elem; f++) {
                    putf(o, " f%u: ", f);
                    put_type(o, g, g->fld[x->index + f]);
                    put(o, ";");
                }
                put(o, " end");
            } else if (x->k == K_PTR || x->k == K_HEAP) {
                put(o, "^");
                put_type(o, g, x->elem);
            } else if (x->k == K_ENUM) {
                put(o, "(");
                for (uint32_t k = 0; k < x->elem; k++)
                    putf(o, "%sq%ux%u", k ? ", " : "", t, k);
                put(o, ")");
            } else {
                put(o, "array[");
                put_type(o, g, x->index);
                put(o, x->k == K_OPEN ? " range <>] of " : "] of ");
                put_type(o, g, x->elem);
            }
            put(o, ";\n");
        }
    }
    if (nglob) {
        put(o, "\nvar\n");
        for (uint32_t v = 0; v < nglob; v++) {
            put(o, "  ");
            put_name(o, g, v);
            put(o, ": ");
            put_type(o, g, g->v[v].t);
            if (is_scalar(g, g->v[v].t) || is_ptr(g, g->v[v].t)) {
                put(o, " := ");
                pexpr(g, o, g->st[v].e);
            }
            put(o, ";\n");
        }
    }
    if (g->web)
        web_program_decls(o, g->web);
    for (uint32_t r = 0; r < g->nr; r++)
        if (!g->moved || !g->moved[r])
            routine_text(g, o, r, false);
    put(o, "\nbegin\n");
    if (g->web)
        web_program_print(o, g->web);
    pblock(g, o, g->main_blk, g->main_n, 1);
    put(o, "end Random.\n");
    LIMBA_GROW(o->b, o->n, o->cap);
    o->b[o->n] = 0;
}

/* may routine r go into the unit Lib: its text names no global, no
   type, value or constant of the program, no routine left in it (the
   unit cannot see the program) */
static bool movable(const G *g, uint32_t r, const char *b, size_t n)
{
    for (size_t i = 0; i < n;) {
        char c = b[i];
        if (c == '"') {
            /* a String: "" inside it */
            size_t j = i + 1;
            while (j < n && !(b[j] == '"' && (j + 1 >= n || b[j + 1] != '"')))
                j += b[j] == '"' ? 2 : 1;
            i = j + 1;
            continue;
        }
        if (c == '\'') {
            /* a Char: the apostrophe written three times, or one
               character (UTF-8) between two */
            if (i + 2 < n && b[i + 1] == '\'' && b[i + 2] == '\'') {
                i += 3;
                continue;
            }
            size_t j = i + 1;
            while (j < n && b[j] != '\'')
                j++;
            i = j + 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            i++;
            if (c >= '0' && c <= '9')
                while (i < n && ((b[i] >= '0' && b[i] <= '9') ||
                                 (b[i] >= 'a' && b[i] <= 'z') ||
                                 (b[i] >= 'A' && b[i] <= 'Z') || b[i] == '_' ||
                                 b[i] == '.'))
                    i++;
            continue;
        }
        size_t w = i;
        while (w < n &&
               ((b[w] >= 'a' && b[w] <= 'z') || (b[w] >= 'A' && b[w] <= 'Z')))
            w++;
        size_t d = w;
        while (d < n && b[d] >= '0' && b[d] <= '9')
            d++;
        if (d > w) {
            /* a name of the generator: a letter or a type, then a number */
            unsigned num = (unsigned)strtoul(b + w, NULL, 10);
            bool type = false;
            for (unsigned t = 0; t < NTYPES; t++)
                if (strlen(tname[t]) == d - i &&
                    !memcmp(tname[t], b + i, d - i))
                    type = true;
            if (!type) {
                if (w - i != 1 || (d < n && b[d] == 'x'))
                    return false; /* a type, a value of an enumeration */
                switch (i && b[i - 1] == '.' ? '.' : b[i]) {
                case '.':
                    break; /* a field */
                case 'f':
                case 'p':
                    /* itself, or a routine already in Lib */
                    if (num != r && (num >= g->nr || !g->moved[num]))
                        return false;
                    break;
                case 'g':
                    return false;
                case 'k':
                case 'v':
                case 'a':
                case 'i':
                case 'w':
                    if (num >= g->nv || g->v[num].kind == V_GLOBAL)
                        return false;
                    /* a constant of the program: one of its statements */
                    for (uint32_t k = 0; k < g->ngc; k++)
                        if (g->st[g->gc[k]].var == num)
                            return false;
                    break;
                case 'z':
                case 'j':
                    break; /* the tables of translate and readbytes */
                default:
                    return false;
                }
            }
        }
        i = d;
    }
    return true;
}

/* the unit Lib: the headings of the routines moved, their bodies, and an
   initialisation that prints a line (§ 11.2, § 11.5) */
static void unit_text(G *g, text *u)
{
    for (uint32_t r = 0; r < g->nr; r++) {
        text t = {NULL, 0, 0, 1, 1, 0};
        routine_text(g, &t, r, false);
        g->moved[r] = movable(g, r, t.b, t.n);
        free(t.b);
    }
    put(u, "unit Lib;\n\ninterface\n");
    for (uint32_t r = 0; r < g->nr; r++)
        if (g->moved[r])
            routine_text(g, u, r, true);
    put(u, "\nimplementation\n");
    for (uint32_t r = 0; r < g->nr; r++)
        if (g->moved[r])
            routine_text(g, u, r, false);
    put(u, "\nbegin\n  writeln(\"lib\");\nend Lib.\n");
    LIMBA_GROW(u->b, u->n, u->cap);
    u->b[u->n] = 0;
}

/* ---- the run ---- */

enum { X_NEXT, X_EXIT, X_CONT, X_RET };

#define STEP_LIMIT 200000

typedef struct {
    G *g;
    v128 *cell;
    uint32_t *ref;   /* the first cell of a variable, a var parameter's too */
    v128 *alo, *ahi; /* the bounds of an array variable, or of an open
                        parameter's argument */
    v128 *heap;      /* the records made by new: a pointer is the index of
                        the first field plus 1, nil is 0 */
    uint32_t nheap, capheap;
    uint32_t live; /* the records made by new and not disposed of */
    /* the arrays made by new: a pointer is the index plus 1; the cells of
       each from first */
    struct xh {
        v128 lo, hi;
        uint32_t first;
        bool dead;
    } *harr;
    uint32_t nharr, capharr;
    v128 *hcell;
    uint32_t nhcell, caphcell;
    text out;
    bool trap, toolong;
    bool halt; /* the trap is a halt, code its status */
    int code;
    uint32_t line, col;
    uint64_t steps;
    v128 ret;
    uint32_t rt; /* the type of the result of the running function */
    /* the input, which readline and readbytes read from inpos; in_end:
       its end met, final (§ 9.2) */
    const char *in;
    size_t inlen, inpos;
    bool in_end;
} X;

static v128 wrap(unsigned t, v128 v)
{
    u128 mask = ((u128)1 << tbits(t)) - 1;
    return (v128)((u128)v & mask);
}

static bool fits(unsigned t, v128 v)
{
    return v >= tmin(t) && v <= tmax(t);
}

static void stop(X *x, int code, uint32_t line, uint32_t col)
{
    if (!x->trap) {
        x->trap = true;
        x->code = code;
        x->line = line;
        x->col = col;
    }
}

static v128 fail(X *x, uint32_t at, int code)
{
    stop(x, code, x->g->e[at].line, x->g->e[at].col);
    return 0;
}

/* the array made by new that variable v points to; nil (102) and a freed
   one (105) stop the run at line:col */
static struct xh *heap_at(X *x, uint32_t v, uint32_t line, uint32_t col)
{
    v128 pv = x->cell[x->ref[v]];
    if (pv == 0) {
        stop(x, 102, line, col);
        return NULL;
    }
    struct xh *a = &x->harr[pv - 1];
    if (a->dead) {
        stop(x, 105, line, col);
        return NULL;
    }
    return a;
}

/* v stored where type t is: a range is checked, reported at line:col */
static bool store_ok(X *x, uint32_t t, v128 v, uint32_t line, uint32_t col)
{
    const G *g = x->g;
    if (g->ty[t].k == K_RANGE && (v < lo_of(g, t) || v > hi_of(g, t))) {
        stop(x, 101, line, col);
        return false;
    }
    return true;
}

static int run_block(X *x, uint32_t b, uint32_t n);

static v128 ev(X *x, uint32_t i);

/* the arguments from left to right, each checked at the call at */
static v128 call(X *x, uint32_t fn, uint32_t args, uint32_t nargs,
                 uint32_t line, uint32_t col)
{
    G *g = x->g;
    v128 val[4];
    uint32_t target[4];
    for (uint32_t k = 0; k < nargs; k++) {
        uint32_t a = g->ls[args + k], p = g->r[fn].par[k];
        if (g->ty[g->v[p].t].k == K_OPEN) {
            if (g->e[a].k == E_AGG)
                ev(x, a);            /* into its hidden variable, in order */
            target[k] = g->e[a].var; /* by reference: bound below */
        } else if (is_record(g, g->v[p].t)) {
            /* a variable, or the cells of a call: bound below */
            target[k] = (uint32_t)ev(x, a);
        } else if (g->v[p].kind == V_OUT) {
            target[k] = x->ref[g->e[a].var]; /* copied back at the end */
        } else if (g->v[p].kind == V_VAR) {
            target[k] = x->ref[g->e[a].var];
        } else {
            val[k] = ev(x, a);
            if (!x->trap)
                store_ok(x, g->v[p].t, val[k], line, col);
        }
        if (x->trap)
            return 0;
    }
    for (uint32_t k = 0; k < nargs; k++) {
        uint32_t p = g->r[fn].par[k];
        if (g->ty[g->v[p].t].k == K_OPEN) {
            uint32_t a = target[k];
            x->ref[p] = x->ref[a];
            x->alo[p] = x->alo[a];
            x->ahi[p] = x->ahi[a];
        } else if (is_record(g, g->v[p].t)) {
            /* in a function a record is only read: by copy or by
               reference cannot be told apart (§ 8) */
            x->ref[p] = target[k];
        } else if (g->v[p].kind == V_OUT) {
            /* its own cell: nothing to do before */
        } else if (g->v[p].kind == V_VAR) {
            x->ref[p] = target[k];
        } else {
            x->cell[x->ref[p]] = val[k];
        }
    }
    uint32_t outer = x->rt;
    x->ret = 0;
    x->rt = g->r[fn].rt;
    run_block(x, g->r[fn].blk, g->r[fn].nblk);
    x->rt = outer;
    if (!x->trap) /* the out parameters go back, in order */
        for (uint32_t k = 0; k < nargs; k++) {
            uint32_t p = g->r[fn].par[k];
            if (g->v[p].kind == V_OUT)
                x->cell[target[k]] = x->cell[x->ref[p]];
        }
    return x->ret;
}

/* the value v of a field or an element of type t, read at node i: one
   that no one assigned is caught (§ 4.5) */
static v128 valid(X *x, uint32_t i, uint32_t t, v128 v)
{
    v128 bad;
    if (narrow_bad(x->g, t, &bad) && (v < lo_of(x->g, t) || v > hi_of(x->g, t)))
        return fail(x, i, 101);
    return v;
}

/* the cells of a record or an array of type t without a value (§ 4.5):
   what new gives */
static void unassigned(const G *g, v128 *cell, uint32_t t)
{
    uint32_t n = ncells(g, t);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t ct = is_record(g, t) ? field_type(g, t, k) : g->ty[t].elem;
        v128 bad = 0;
        narrow_bad(g, ct, &bad);
        cell[k] = bad;
    }
}

/* the cells a value of type t takes */
static uint32_t ncells(const G *g, uint32_t t)
{
    return is_array(g, t)    ? (uint32_t)(g->ty[t].hi - g->ty[t].lo) + 1
           : is_record(g, t) ? g->ty[t].elem
                             : 1;
}

/* the cell of element index of array variable v; false if outside */
static bool element_cell(X *x, uint32_t v, v128 index, uint32_t *cell)
{
    if (index < x->alo[v] || index > x->ahi[v])
        return false;
    *cell = x->ref[v] + (uint32_t)(index - x->alo[v]);
    return true;
}

/* l ** n in type t: exact, then it must fit, except in a Bits type */
static v128 power(X *x, uint32_t i, unsigned t, v128 l, v128 n)
{
    if (n < 0)
        return fail(x, i, 101); /* the exponent is a Natural (§ 4.3) */
    if (fam(t) == 'B') {
        u128 mask = ((u128)1 << tbits(t)) - 1, acc = 1, b = (u128)l & mask;
        for (u128 e = (u128)n; e; e >>= 1) {
            if (e & 1)
                acc = (acc * b) & mask;
            b = (b * b) & mask;
        }
        return (v128)acc;
    }
    if (l == 0 || l == 1)
        return n == 0 ? 1 : l;
    if (l == -1)
        return n % 2 ? -1 : 1;
    if (n > 127)
        return fail(x, i, 6);
    v128 acc = 1;
    u128 ml = (u128)(l < 0 ? -l : l);
    for (v128 k = 0; k < n; k++) {
        u128 ma = (u128)(acc < 0 ? -acc : acc);
        if (ma > ((u128)1 << 65) / ml) /* past every type: no overflow here */
            return fail(x, i, 6);
        acc *= l;
        if (!fits(t, acc))
            return fail(x, i, 6);
    }
    return acc;
}

static void print_value(const G *g, text *o, unsigned t, v128 v);

/* a string made by the run; past 64 KiB the attempt is given up */
static v128 run_str(X *x, const char *b, size_t n)
{
    if (n > 65536) {
        x->toolong = x->trap = true;
        return 0;
    }
    return (v128)new_str(x->g, b, n);
}

/* a String or a Char of type t as a string */
static v128 as_str(X *x, unsigned t, v128 v)
{
    if (t == T_STR)
        return v;
    char b[4];
    return run_str(x, b, utf8((uint32_t)v, b));
}

/* the bits past which a BigInt of the run is too large: the program is
   dropped, as one that runs too long */
#define BIG_LIMIT 65536

/* BigInt v as a v128, when it lies within 2^126 */
static bool big_i128(const G *g, v128 v, v128 *out)
{
    const limba_big *b = &g->big[(uint32_t)v];
    if (limba_big_bits(b) > 126)
        return false;
    u128 m = 0;
    for (uint32_t k = b->n; k-- > 0;)
        m = m << 32 | b->w[k];
    *out = b->neg ? -(v128)m : (v128)m;
    return true;
}

/* a new BigInt from t, or too long past the limit */
static v128 big_result(X *x, bool ok, limba_big *t)
{
    if (!ok) {
        limba_big_free(t);
        x->toolong = x->trap = true;
        return 0;
    }
    return new_big(x->g, t);
}

/* a BigInt operation of node i: the operands BigInt values, but the
   exponent of ** an integer */
static v128 big_binary(X *x, uint32_t i, unsigned op, v128 l, v128 r)
{
    G *g = x->g;
    limba_big t;
    limba_big_init(&t);
    const limba_big *a = &g->big[(uint32_t)l];
    if (op == O_POW) {
        if (r < 0)
            return fail(x, i, 101); /* a Natural (§ 4.3) */
        return big_result(x, limba_big_pow_lim(&t, a, (uint64_t)r, BIG_LIMIT),
                          &t);
    }
    const limba_big *b = &g->big[(uint32_t)r];
    int c = limba_big_cmp(a, b);
    switch (op) {
    case O_EQ:
        return c == 0;
    case O_NE:
        return c != 0;
    case O_LT:
        return c < 0;
    case O_LE:
        return c <= 0;
    case O_GT:
        return c > 0;
    case O_GE:
        return c >= 0;
    case O_ADD:
        return big_result(x, limba_big_add_lim(&t, a, b, BIG_LIMIT), &t);
    case O_SUB:
        return big_result(x, limba_big_sub_lim(&t, a, b, BIG_LIMIT), &t);
    case O_MUL:
        return big_result(x, limba_big_mul_lim(&t, a, b, BIG_LIMIT), &t);
    default: { /* div, mod, rem */
        if (limba_big_is_zero(b))
            return fail(x, i, 11);
        limba_big m;
        limba_big_init(&m);
        limba_big_divmod(&t, &m, a, b);
        if (op == O_DIV) {
            limba_big_free(&m);
            return new_big(g, &t);
        }
        limba_big_free(&t);
        if (op == O_MOD && !limba_big_is_zero(&m) && m.neg != b->neg)
            limba_big_add_lim(&m, &m, b, BIG_LIMIT + 64);
        return new_big(g, &m);
    }
    }
}

static v128 binary(X *x, uint32_t i)
{
    const xe *e = &x->g->e[i];
    unsigned op = e->op, t = base(x->g, e->t);
    if (t == T_BOOL && (op == O_AND || op == O_OR)) {
        v128 l = ev(x, e->a);
        if (x->trap)
            return 0;
        if ((op == O_AND) != (l != 0))
            return l;
        return ev(x, e->b);
    }
    v128 l = ev(x, e->a);
    v128 r = ev(x, e->b);
    if (x->trap)
        return 0;
    if (op == O_CAT) {
        l = as_str(x, base(x->g, x->g->e[e->a].t), l);
        r = as_str(x, base(x->g, x->g->e[e->b].t), r);
        if (x->trap)
            return 0;
        const struct xstr *a = &x->g->str[(uint32_t)l],
                          *b = &x->g->str[(uint32_t)r];
        char *c = limba_xmalloc((size_t)a->n + b->n + 1);
        memcpy(c, a->b, a->n);
        memcpy(c + a->n, b->b, b->n);
        v128 v = run_str(x, c, (size_t)a->n + b->n);
        free(c);
        return v;
    }
    if (base(x->g, x->g->e[e->a].t) == T_BIG)
        return big_binary(x, i, op, l, r);
    if (base(x->g, x->g->e[e->a].t) == T_STR) {
        /* byte by byte, then the shorter first (§ 6) */
        const struct xstr *a = &x->g->str[(uint32_t)l],
                          *b = &x->g->str[(uint32_t)r];
        int c = memcmp(a->b, b->b, a->n < b->n ? a->n : b->n);
        if (c == 0)
            c = a->n < b->n ? -1 : a->n > b->n;
        l = c;
        r = 0;
    }
    if (fam(base(x->g, x->g->e[e->a].t)) == 'F') {
        /* IEEE 754: a comparison with NaN is false, except <> */
        double a = fval(l), b = fval(r);
        switch (op) {
        case O_EQ:
            return a == b;
        case O_NE:
            return a != b;
        case O_LT:
            return a < b;
        case O_LE:
            return a <= b;
        case O_GT:
            return a > b;
        case O_GE:
            return a >= b;
        case O_ADD:
            return fres(t, a + b);
        case O_SUB:
            return fres(t, a - b);
        case O_MUL:
            return fres(t, a * b);
        default:
            return fres(t, a / b);
        }
    }
    bool mod = fam(t) == 'B';
    v128 v;
    switch (op) {
    case O_EQ:
        return l == r;
    case O_NE:
        return l != r;
    case O_LT:
        return l < r;
    case O_LE:
        return l <= r;
    case O_GT:
        return l > r;
    case O_GE:
        return l >= r;
    case O_ADD:
        v = l + r;
        break;
    case O_SUB:
        v = l - r;
        break;
    case O_MUL:
        if (fam(t) == 'S')
            v = l * r;
        else if (mod)
            return wrap(t, (v128)((u128)l * (u128)r & (((u128)1 << 64) - 1)));
        else if ((u128)l * (u128)r > (u128)tmax(t))
            return fail(x, i, 6);
        else
            v = (v128)((u128)l * (u128)r);
        break;
    case O_DIV:
    case O_MOD:
    case O_REM:
        if (r == 0)
            return fail(x, i, 11);
        if (op == O_DIV) {
            v = l / r;
            break;
        }
        v = l % r;
        if (op == O_MOD && v != 0 && (v < 0) != (r < 0))
            v += r;
        return v;
    case O_AND:
        return l & r;
    case O_OR:
        return l | r;
    case O_XOR:
        return l ^ r;
    case O_POW:
        return power(x, i, t, l, r);
    case O_SHL:
    case O_SHR:
        if (r < 0 || r >= (v128)tbits(t))
            return fail(x, i, 104);
        if (op == O_SHR)
            return l >> (int)r;
        return wrap(t, (v128)(((u128)l << (int)r) & (((u128)1 << 64) - 1)));
    default:
        return 0;
    }
    if (mod)
        return wrap(t, v);
    if (!fits(t, v))
        return fail(x, i, 6);
    return v;
}

/* v of base from converted to type to at node i: a real rounded, an
   integer that must fit, the low bits for a Bits type */
/* the BigInt of a finite integral double */
static uint32_t big_of_double(G *g, double d)
{
    if (fabs(d) < 0x1p100)
        return big_of_i128(g, (v128)d);
    int ex;
    double frac = frexp(fabs(d), &ex);
    limba_big t;
    limba_big_init(&t);
    limba_big_set_u64(&t, (uint64_t)ldexp(frac, 53));
    limba_big_shl_lim(&t, &t, (uint32_t)(ex - 53), 2048);
    if (d < 0)
        limba_big_neg(&t, &t);
    return new_big(g, &t);
}

/* T(v) with a BigInt on either side (§ 6.6): exact from an integer, half
   away from zero from a real (NaN and the infinities 103), rounded once
   to a real, the low bits to a Bits type, into any other integer type
   only if it fits */
static v128 big_convert(X *x, uint32_t i, unsigned from, uint32_t to, v128 v)
{
    G *g = x->g;
    unsigned t = base(g, to);
    if (from == T_BIG && t == T_BIG)
        return v;
    if (t == T_BIG) {
        if (fam(from) != 'F')
            return big_of_i128(g, v);
        double d = round(fval(v));
        if (!isfinite(d))
            return fail(x, i, 103);
        return big_of_double(g, d);
    }
    const limba_big *b = &g->big[(uint32_t)v];
    if (fam(t) == 'F') {
        limba_rat q;
        limba_rat_init(&q);
        limba_rat_set_big(&q, b);
        double d;
        float f;
        if (t == T_F32) {
            if (!limba_rat_to_f32(&q, &f))
                f = b->neg ? -INFINITY : INFINITY;
            d = f;
        } else if (!limba_rat_to_f64(&q, &d)) {
            d = b->neg ? -INFINITY : INFINITY;
        }
        limba_rat_free(&q);
        return fbits(d);
    }
    if (fam(t) == 'B') { /* the low 64 bits, in two's complement */
        uint64_t low = b->n == 0   ? 0
                       : b->n == 1 ? b->w[0]
                                   : (uint64_t)b->w[1] << 32 | b->w[0];
        return wrap(t, (v128)(b->neg ? 0 - low : low));
    }
    v128 w;
    if (!big_i128(g, v, &w) || w < lo_of(g, to) || w > hi_of(g, to))
        return fail(x, i, 103);
    return w;
}

static v128 convert(X *x, uint32_t i, unsigned from, uint32_t to, v128 v)
{
    const G *g = x->g;
    unsigned t = base(g, to);
    if (from == T_BIG || t == T_BIG)
        return big_convert(x, i, from, to, v);
    if (fam(t) == 'F') {
        if (fam(from) == 'F')
            return fres(t, fval(v));
        if (t == T_F32) /* rounded once, to float */
            return fbits(fam(from) == 'S' ? (double)(float)(int64_t)v
                                          : (double)(float)(uint64_t)v);
        return fbits(fam(from) == 'S' ? (double)(int64_t)v
                                      : (double)(uint64_t)v);
    }
    if (fam(from) == 'F') {
        /* halves away from zero (Ada), then it must fit, Bits too */
        double d = round(fval(v));
        if (!(fabs(d) < 0x1p100))
            return fail(x, i, 103);
        v = (v128)d;
    } else if (fam(t) == 'B') {
        return wrap(t, v);
    }
    return v >= lo_of(g, to) && v <= hi_of(g, to) ? v : fail(x, i, 103);
}

static unsigned digit_value(char c)
{
    return c >= '0' && c <= '9'   ? (unsigned)(c - '0')
           : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
           : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                  : 99;
}

/* a group of digits of base b at p[*i], a _ only between two of them,
   copied to t at *k without the _; false if there is none */
static bool digit_group(const char *p, size_t n, size_t *i, unsigned b, char *t,
                        size_t *k)
{
    size_t from = *i;
    while (*i < n) {
        if (digit_value(p[*i]) < b)
            t[(*k)++] = p[(*i)++];
        else if (p[*i] == '_' && *i > from && *i + 1 < n &&
                 digit_value(p[*i + 1]) < b)
            (*i)++;
        else
            break;
    }
    return *i > from;
}

/* the text s as val reads it into a variable of type t (§ 9): spaces and
   tabs around, a sign, then a literal of Luxia (0x 0o 0b, a real with
   digits on both sides of the point and a lowercase e); an integer must
   lie in t, a real is rounded once to t and must not overflow it; inf
   and nan (with no sign) too. The texts made here are short */
static bool read_number(const G *g, const struct xstr *s, uint32_t t, v128 *v)
{
    const char *p = s->b, *e = s->b + s->n;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    char sign = p < e && (*p == '+' || *p == '-') ? *p++ : 0;
    size_t n = (size_t)(e - p), i = 0, k = 0;
    unsigned bt = base(g, t);
    bool real = fam(bt) == 'F';
    if (real && n == 3 &&
        (!memcmp(p, "inf", 3) || (!memcmp(p, "nan", 3) && !sign))) {
        double d = p[0] == 'i' ? INFINITY : NAN;
        *v = fbits(sign == '-' ? -d : d);
        return true;
    }
    char txt[160];
    if (n >= sizeof(txt) - 1)
        return false; /* none so long here */
    unsigned b = 10;
    if (n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'o' || p[1] == 'b')) {
        b = p[1] == 'x' ? 16 : p[1] == 'o' ? 8 : 2;
        i = 2;
    }
    if (!digit_group(p, n, &i, b, txt, &k))
        return false;
    u128 mag = 0;
    for (size_t q = 0; q < k && mag >> 120 == 0; q++)
        mag = mag * b + digit_value(txt[q]);
    bool whole = true;
    if (b == 10 && i < n && p[i] == '.') {
        txt[k++] = '.';
        i++;
        if (!digit_group(p, n, &i, 10, txt, &k))
            return false;
        whole = false;
    }
    if (b == 10 && i < n && p[i] == 'e') {
        txt[k++] = 'e';
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-'))
            txt[k++] = p[i++];
        if (!digit_group(p, n, &i, 10, txt, &k))
            return false;
        whole = false;
    }
    txt[k] = 0;
    if (i != n || (!real && !whole))
        return false;
    if (!real) {
        v128 x = sign == '-' ? -(v128)mag : (v128)mag;
        if (mag >> 120 || x < lo_of(g, t) || x > hi_of(g, t))
            return false;
        *v = x;
        return true;
    }
    /* a decimal text by the library, in the base of the text a whole
       number converted once */
    double d = b == 10 ? (bt == T_F32 ? strtof(txt, NULL) : strtod(txt, NULL))
               : bt == T_F32 ? (double)(float)mag
                             : (double)mag;
    if (isinf(d))
        return false;
    *v = fbits(sign == '-' ? -d : d);
    return true;
}

static v128 ev(X *x, uint32_t i)
{
    const G *g = x->g;
    const xe *e = &g->e[i];
    if (x->trap)
        return 0;
    if (++x->steps > STEP_LIMIT) {
        x->toolong = x->trap = true;
        return 0;
    }
    if (e->cst)
        return e->lit; /* computed by the front end, exactly */
    unsigned t = base(g, e->t);
    switch (e->k) {
    case E_LIT:
        return e->lit;
    case E_VAR:
        /* a record or an array: its first cell */
        if (is_record(g, e->t) || is_array(g, e->t))
            return (v128)x->ref[e->var];
        return x->cell[x->ref[e->var]];
    case E_BIN:
        return binary(x, i);
    case E_UN: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        if (fam(t) == 'F') /* the sign bit, as IEEE 754 negate and abs */
            return e->op == O_NEG ? fbits(-fval(v)) : fbits(fabs(fval(v)));
        if (t == T_BIG) {
            limba_big r;
            limba_big_init(&r);
            if (e->op == O_NEG)
                limba_big_neg(&r, &x->g->big[(uint32_t)v]);
            else
                limba_big_abs(&r, &x->g->big[(uint32_t)v]);
            return new_big(x->g, &r);
        }
        if (e->op == O_NOT)
            return t == T_BOOL ? !v : wrap(t, ~v);
        if (e->op == O_NEG && fam(t) == 'B')
            return wrap(t, -v);
        v = e->op == O_NEG ? -v : v < 0 ? -v : v;
        return fits(t, v) ? v : fail(x, i, 6);
    }
    case E_CONV: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        return convert(x, i, base(g, g->e[e->a].t), e->t, v);
    }
    case E_VAL: {
        /* the variable changes only when the text is a number */
        v128 s = ev(x, e->a);
        if (x->trap)
            return 0;
        uint32_t vt = g->v[e->var].t;
        v128 v;
        if (!read_number(g, &g->str[(uint32_t)s], vt, &v))
            return 0;
        x->cell[x->ref[e->var]] = v;
        return 1;
    }
    case E_READ: {
        /* the next line, without its LF or CR LF; at the end "" and
           false, and the end is final */
        if (x->in_end || x->inpos == x->inlen) {
            x->in_end = true;
            x->cell[x->ref[e->var]] = 0;
            return 0;
        }
        const char *b = x->in + x->inpos;
        const char *nl = memchr(b, '\n', x->inlen - x->inpos);
        size_t n = nl ? (size_t)(nl - b) : x->inlen - x->inpos;
        x->inpos += n + (nl != NULL);
        if (n && b[n - 1] == '\r')
            n--;
        x->cell[x->ref[e->var]] = run_str(x, b, n);
        return 1;
    }
    case E_ARGC:
        return g->narg;
    case E_ARG: {
        v128 k = ev(x, e->a);
        if (x->trap)
            return 0;
        /* from 1 to argcount(), checked at its name */
        return k >= 1 && k <= g->narg ? g->arg[k - 1] : fail(x, i, 101);
    }
    case E_CALL: {
        v128 r = call(x, e->fn, e->args, e->nargs, e->line, e->col);
        if (x->trap || !(is_record(g, e->t) || is_array(g, e->t)))
            return r;
        /* a record or an array: the cells returned, copied at once to the
           hidden variable of the call, as into a slot of the caller */
        memmove(&x->cell[x->ref[e->var]], &x->cell[(uint32_t)r],
                ncells(g, e->t) * sizeof(v128));
        return (v128)x->ref[e->var];
    }
    case E_MATH: {
        /* in double, rounded to the type: what Float32 does too */
        double a = fval(ev(x, e->a)), r;
        if (x->trap)
            return 0;
        switch (e->op) {
        case M_SQRT:
            r = sqrt(a);
            break;
        case M_SIN:
            r = sin(a);
            break;
        case M_COS:
            r = cos(a);
            break;
        case M_TAN:
            r = tan(a);
            break;
        case M_ARCTAN:
            r = atan(a);
            break;
        case M_EXP:
            r = exp(a);
            break;
        case M_LN:
            r = log(a);
            break;
        case M_TRUNC:
            r = trunc(a);
            break;
        case M_ROUND:
            r = nearbyint(a); /* half to even */
            break;
        case M_FLOOR:
            r = floor(a);
            break;
        default:
            r = ceil(a);
        }
        return fres(t, r);
    }
    case E_AGG: {
        /* the components from left to right, else last and once, into the
           hidden variable: a value, then copied where it goes (§ 6.8) */
        uint32_t at = x->ref[e->var], n = ncells(g, e->t);
        bool rec = is_record(g, e->t);
        uint8_t given[64] = {0};
        uint32_t k = 0;
        for (uint32_t j = 0; j < e->nargs; j++) {
            v128 v = ev(x, g->ls[e->args + j]);
            if (x->trap)
                return 0;
            if (rec || !e->op) {
                x->cell[at + k] = v;
                given[k++] = 1;
                continue;
            }
            v128 lo = g->e[g->ls[e->a + j]].lit, hi = lo;
            if (g->ls[e->b + j])
                hi = g->e[g->ls[e->b + j]].lit;
            for (v128 q = lo; q <= hi; q++) {
                x->cell[at + (uint32_t)(q - g->ty[e->t].lo)] = v;
                given[q - g->ty[e->t].lo] = 1;
            }
        }
        if (e->c) {
            v128 v = ev(x, e->c);
            if (x->trap)
                return 0;
            for (uint32_t q = 0; q < n && q < 64; q++)
                if (!given[q])
                    x->cell[at + q] = v;
        }
        return (v128)at;
    }
    case E_CFIELD: {
        v128 c = ev(x, e->a);
        return x->trap ? 0 : valid(x, i, e->t, x->cell[(uint32_t)c + e->b]);
    }
    case E_SLEN: {
        /* low evaluates the string too */
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        return e->op == 1 ? 1 : (v128)g->str[(uint32_t)v].n;
    }
    case E_SIDX: {
        v128 v = x->cell[x->ref[e->var]];
        v128 k = ev(x, e->b);
        if (x->trap)
            return 0;
        const struct xstr *a = &g->str[(uint32_t)v];
        if (k < 1 || k > (v128)a->n)
            return fail(x, i, 100);
        return (unsigned char)a->b[k - 1];
    }
    case E_COPY: {
        /* from left to right; from 1 on and a count from 0, past the
           end cut short (§ 4.5) */
        v128 v = ev(x, e->a);
        v128 from = ev(x, e->b);
        v128 n = ev(x, e->c);
        if (x->trap)
            return 0;
        if (from < 1 || n < 0)
            return fail(x, i, 101);
        uint32_t sn = g->str[(uint32_t)v].n;
        v128 at = from - 1 > sn ? sn : from - 1;
        if (n > sn - at)
            n = sn - at;
        return run_str(x, g->str[(uint32_t)v].b + at, (size_t)n);
    }
    case E_STRF: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        text o = {NULL, 0, 0, 1, 1, 0};
        print_value(g, &o, base(g, g->e[e->a].t), v);
        v128 r = run_str(x, o.b ? o.b : "", o.n);
        free(o.b);
        return r;
    }
    case E_CHR: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        if (v < 0 || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
            return fail(x, i, 103);
        return v;
    }
    case E_CINDEX: {
        v128 c = ev(x, e->a);
        if (x->trap)
            return 0;
        v128 k = ev(x, e->b);
        if (x->trap)
            return 0;
        const xt *at = &g->ty[g->e[e->a].t];
        if (k < at->lo || k > at->hi)
            return fail(x, i, 100);
        return valid(x, i, e->t, x->cell[(uint32_t)c + (uint32_t)(k - at->lo)]);
    }
    case E_INDEX: {
        v128 k = ev(x, e->a);
        uint32_t c;
        if (x->trap)
            return 0;
        if (!element_cell(x, e->var, k, &c))
            return fail(x, i, 100);
        return valid(x, i, e->t, x->cell[c]);
    }
    case E_HIDX: {
        /* nil, dangling, then the index: the bounds of the block */
        struct xh *a = heap_at(x, e->var, e->line, e->col);
        if (!a)
            return 0;
        uint32_t h = (uint32_t)(a - x->harr);
        v128 k = ev(x, e->a);
        if (x->trap)
            return 0;
        a = &x->harr[h];
        if (k < a->lo || k > a->hi)
            return fail(x, i, 100);
        return valid(x, i, e->t, x->hcell[a->first + (uint32_t)(k - a->lo)]);
    }
    case E_HLEN: {
        struct xh *a = heap_at(x, e->var, e->line, e->col);
        if (!a)
            return 0;
        return e->op == 0      ? a->lo
               : e->op == 1    ? a->hi
               : a->hi < a->lo ? 0
                               : a->hi - a->lo + 1;
    }
    case E_FIELD: {
        uint32_t vt = g->v[e->var].t;
        if (is_record(g, vt))
            return valid(x, i, e->t, x->cell[x->ref[e->var] + e->b]);
        v128 pv = x->cell[x->ref[e->var]];
        if (pv == 0)
            return fail(x, i, 102);
        v128 v = x->heap[pv - 1 + e->b], bad;
        /* a value new gave and nobody assigned */
        uint32_t ft = field_type(g, record_of(g, vt), e->b);
        if (narrow_bad(g, ft, &bad) && (v < lo_of(g, ft) || v > hi_of(g, ft)))
            return fail(x, i, 101);
        return v;
    }
    case E_NIL:
        return 0;
    case E_SUCC:
    case E_PRED: {
        v128 v = ev(x, e->a);
        if (x->trap)
            return 0;
        if (e->k == E_SUCC ? v >= g->ty[e->t].hi : v <= g->ty[e->t].lo)
            return fail(x, i, 101);
        return e->k == E_SUCC ? v + 1 : v - 1;
    }
    case E_ORD:
        return ev(x, e->a);
    case E_LOW:
        return x->alo[e->var];
    case E_HIGH:
        return x->ahi[e->var];
    case E_LEN:
        return x->ahi[e->var] < x->alo[e->var]
                   ? 0
                   : x->ahi[e->var] - x->alo[e->var] + 1;
    case E_IN: {
        v128 v = ev(x, e->a), lo, hi;
        if (e->fn) {
            lo = lo_of(g, e->fn);
            hi = hi_of(g, e->fn);
        } else {
            lo = ev(x, e->b);
            hi = ev(x, e->c);
        }
        return lo <= v && v <= hi;
    }
    }
    return 0;
}

static void print_value(const G *g, text *o, unsigned t, v128 v)
{
    if (t == T_BIG) {
        put_big(o, g, v);
    } else if (t == T_STR) {
        putn(o, g->str[(uint32_t)v].b, g->str[(uint32_t)v].n);
    } else if (t == T_CHAR) {
        char b[4];
        putn(o, b, utf8((uint32_t)v, b));
    } else if (fam(t) == 'F') {
        /* a Float32 in its own shortest form */
        char buf[LIMBA_FMT_F64_MAX];
        if (t == T_F32)
            limba_fmt_f32(buf, (float)fval(v));
        else
            limba_fmt_f64(buf, fval(v));
        put(o, buf);
    } else if (t == T_BOOL) {
        put(o, v ? "true" : "false");
    } else if (v < 0) {
        put(o, "-");
        put_u128(o, (u128)(-(v + 1)) + 1);
    } else {
        put_u128(o, (u128)v);
    }
}

static int run_stmt(X *x, uint32_t si)
{
    G *g = x->g;
    const xs *s = &g->st[si];
    if (++x->steps > STEP_LIMIT) {
        x->toolong = x->trap = true;
        return X_RET;
    }
    switch (s->k) {
    case S_VAR: {
        uint32_t vt = g->v[s->var].t;
        if (!s->e) { /* a record or an array without a value */
            unassigned(g, &x->cell[x->ref[s->var]], vt);
            return X_NEXT;
        }
        v128 v = ev(x, s->e);
        if (g->ty[vt].k == K_DYN) {
            /* the bounds, low first; the elements are 0 */
            v128 hi = x->trap ? 0 : ev(x, s->e2);
            if (x->trap)
                return X_RET;
            x->alo[s->var] = v;
            x->ahi[s->var] = hi;
            unassigned(g, &x->cell[x->ref[s->var]], vt);
            return X_NEXT;
        }
        if (!x->trap && (is_record(g, vt) || is_array(g, vt))) {
            memmove(&x->cell[x->ref[s->var]], &x->cell[(uint32_t)v],
                    ncells(g, vt) * sizeof(v128));
            return X_NEXT;
        }
        if (x->trap || !store_ok(x, g->v[s->var].t, v, s->nline, s->ncol))
            return X_RET;
        x->cell[x->ref[s->var]] = v;
        return X_NEXT;
    }
    case S_ASSIGN: {
        uint32_t c = x->ref[s->var];
        uint32_t t = g->v[s->var].t;
        if (s->fld) {
            /* the target first: through a pointer, checked */
            uint32_t f = s->fld - 1;
            uint32_t ft = field_type(g, record_of(g, t), f);
            v128 pv = 0;
            if (is_ptr(g, t)) {
                pv = x->cell[c];
                if (pv == 0) {
                    stop(x, 102, s->iline, s->icol);
                    return X_RET;
                }
            }
            v128 v = ev(x, s->e);
            if (x->trap || !store_ok(x, ft, v, s->line, s->col))
                return X_RET;
            if (pv)
                x->heap[pv - 1 + f] = v; /* ev made no record: no move */
            else
                x->cell[c + f] = v;
            return X_NEXT;
        }
        if (s->idx) {
            /* the target first: its index, checked */
            v128 k = ev(x, s->idx);
            if (x->trap)
                return X_RET;
            if (!element_cell(x, s->var, k, &c)) {
                stop(x, 100, s->iline, s->icol);
                return X_RET;
            }
            t = g->ty[t].elem;
        }
        v128 v = ev(x, s->e);
        if (x->trap || !store_ok(x, t, v, s->line, s->col))
            return X_RET;
        x->cell[c] = v;
        return X_NEXT;
    }
    case S_IF:
        for (uint32_t k = 0; k < s->narm; k++) {
            const xarm *a = &g->arm[s->arm + k];
            v128 c = ev(x, a->cond);
            if (x->trap)
                return X_RET;
            if (c)
                return run_block(x, a->blk, a->nblk);
        }
        return s->has_alt ? run_block(x, s->alt, s->nalt) : X_NEXT;
    case S_WHILE:
        for (;;) {
            v128 c = ev(x, s->e);
            if (x->trap)
                return X_RET;
            if (!c)
                return X_NEXT;
            int r = run_block(x, s->blk, s->nblk);
            if (r == X_RET)
                return X_RET;
            if (r == X_EXIT)
                return X_NEXT;
        }
    case S_REPEAT:
        for (;;) {
            int r = run_block(x, s->blk, s->nblk);
            if (r == X_RET)
                return X_RET;
            if (r == X_EXIT)
                return X_NEXT;
            v128 c = ev(x, s->e);
            if (x->trap)
                return X_RET;
            if (c)
                return X_NEXT;
        }
    case S_FOR: {
        uint32_t t = g->v[s->var].t;
        v128 lo = ev(x, s->e);
        if (x->trap || !store_ok(x, t, lo, s->line, s->col))
            return X_RET;
        v128 hi = ev(x, s->e2);
        if (x->trap || !store_ok(x, t, hi, s->line, s->col))
            return X_RET;
        if (s->down ? lo < hi : lo > hi)
            return X_NEXT;
        for (v128 v = lo;; v += s->down ? -1 : 1) {
            x->cell[x->ref[s->var]] = v;
            int r = run_block(x, s->blk, s->nblk);
            if (r == X_RET)
                return X_RET;
            if (r == X_EXIT || v == hi)
                return X_NEXT;
        }
    }
    case S_CASE: {
        v128 v = ev(x, s->e);
        if (x->trap)
            return X_RET;
        for (uint32_t k = 0; k < s->narm; k++) {
            const xarm *a = &g->arm[s->arm + k];
            for (uint32_t j = 0; j < a->nlab; j++)
                if (v >= g->lab[a->lab + j].lo && v <= g->lab[a->lab + j].hi)
                    return run_block(x, a->blk, a->nblk);
        }
        return run_block(x, s->alt, s->nalt);
    }
    case S_WRITE:
        for (uint32_t k = 0; k < s->nargs; k++) {
            uint32_t a = g->ls[s->args + k];
            if (g->e[a].k == E_STR) {
                put(&x->out, " ");
                continue;
            }
            if (g->e[a].k == E_FMT) {
                /* the value, then the width and the decimals; padded on
                   the left to the width, in characters */
                const xe *fe = &g->e[a];
                v128 v = ev(x, fe->a);
                v128 w = x->trap ? 0 : ev(x, fe->b);
                if (!x->trap && w < 0)
                    fail(x, fe->b, 101); /* at the width */
                v128 d = x->trap || !fe->c ? 0 : ev(x, fe->c);
                if (!x->trap && (d < 0 || d > 100))
                    fail(x, fe->c, 101); /* at the decimals */
                if (x->trap)
                    return X_RET;
                text t = {NULL, 0, 0, 1, 1, 0};
                unsigned b = base(g, g->e[fe->a].t);
                if (fe->c && isnan(fval(v))) {
                    put(&t, "nan");
                } else if (fe->c) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "%.*f", (int)d, fval(v));
                    put(&t, buf);
                } else {
                    print_value(g, &t, b, v);
                }
                v128 chars = 0;
                for (size_t k2 = 0; k2 < t.n; k2++)
                    chars += ((unsigned char)t.b[k2] & 0xc0) != 0x80;
                for (; w > chars; w--)
                    put(&x->out, " ");
                putn(&x->out, t.b ? t.b : "", t.n);
                free(t.b);
                continue;
            }
            v128 v = ev(x, a);
            if (x->trap)
                return X_RET;
            print_value(g, &x->out, base(g, g->e[a].t), v);
        }
        put(&x->out, "\n");
        return X_NEXT;
    case S_CALL:
        call(x, s->fn, s->args, s->nargs, s->line, s->col);
        return x->trap ? X_RET : X_NEXT;
    case S_EXIT:
    case S_CONT:
        if (s->e) {
            v128 c = ev(x, s->e);
            if (x->trap)
                return X_RET;
            if (!c)
                return X_NEXT;
        }
        return s->k == S_EXIT ? X_EXIT : X_CONT;
    case S_RETURN:
        if (s->e) {
            x->ret = ev(x, s->e);
            if (!x->trap && is_scalar(x->g, x->rt))
                store_ok(x, x->rt, x->ret, s->line, s->col);
        }
        return X_RET;
    case S_CONST:
        return X_NEXT; /* computed by the front end */
    case S_NEW: {
        uint32_t rt = g->ty[g->v[s->var].t].elem, n = g->ty[rt].elem;
        uint32_t at = x->nheap;
        for (uint32_t k = 0; k < n; k++) {
            v128 bad = 0;
            narrow_bad(g, field_type(g, rt, k), &bad);
            LIMBA_GROW(x->heap, x->nheap, x->capheap);
            x->heap[x->nheap++] = bad;
        }
        x->cell[x->ref[s->var]] = (v128)at + 1;
        x->live++;
        return X_NEXT;
    }
    case S_DISPOSE:
        x->live--; /* the variable points to a record new made, alone */
        return X_NEXT;
    case S_HNEW: {
        /* the elements without values (§ 3.11), then the pointer */
        v128 lo = ev(x, s->e), hi = ev(x, s->e2);
        uint32_t el = g->ty[g->ty[g->v[s->var].t].elem].elem;
        uint32_t first = x->nhcell;
        for (v128 k = lo; k <= hi; k++) {
            v128 bad = 0;
            narrow_bad(g, el, &bad);
            LIMBA_GROW(x->hcell, x->nhcell, x->caphcell);
            x->hcell[x->nhcell++] = bad;
        }
        LIMBA_GROW(x->harr, x->nharr, x->capharr);
        x->harr[x->nharr] = (struct xh){lo, hi, first, false};
        x->cell[x->ref[s->var]] = (v128)++x->nharr;
        x->live++;
        return X_NEXT;
    }
    case S_HSET: {
        /* the target first: nil, dangling, index at the [; the value,
           its range at the assignment */
        struct xh *a = heap_at(x, s->var, s->iline, s->icol);
        if (!a)
            return X_RET;
        uint32_t h = (uint32_t)(a - x->harr);
        uint32_t el = g->ty[g->ty[g->v[s->var].t].elem].elem;
        v128 k = ev(x, s->idx);
        if (x->trap)
            return X_RET;
        a = &x->harr[h];
        if (k < a->lo || k > a->hi) {
            stop(x, 100, s->iline, s->icol);
            return X_RET;
        }
        uint32_t c = a->first + (uint32_t)(k - a->lo);
        v128 v = ev(x, s->e);
        if (x->trap || !store_ok(x, el, v, s->line, s->col))
            return X_RET;
        x->hcell[c] = v;
        return X_NEXT;
    }
    case S_MOVE: {
        /* from the left: the source (nil, dangling), from, the target,
           to, count; count < 0 a range error, the two ranges inside
           their arrays unless count is 0, then a copy that may overlap */
        struct xh *a = heap_at(x, s->var, s->line, s->col);
        if (!a)
            return X_RET;
        uint32_t sh = (uint32_t)(a - x->harr);
        v128 from = ev(x, s->e);
        if (x->trap)
            return X_RET;
        struct xh *b = heap_at(x, s->idx, s->line, s->col);
        if (!b)
            return X_RET;
        uint32_t dh = (uint32_t)(b - x->harr);
        v128 to = ev(x, s->e2);
        v128 count = x->trap ? 0 : ev(x, s->fld);
        if (x->trap)
            return X_RET;
        a = &x->harr[sh];
        b = &x->harr[dh];
        if (count < 0) { /* an Int64 */
            stop(x, 101, s->line, s->col);
            return X_RET;
        }
        if (count > 0 &&
            !(from >= a->lo && from <= a->hi && count - 1 <= a->hi - from &&
              to >= b->lo && to <= b->hi && count - 1 <= b->hi - to)) {
            stop(x, 100, s->line, s->col);
            return X_RET;
        }
        if (count > 0)
            memmove(&x->hcell[b->first + (uint32_t)(to - b->lo)],
                    &x->hcell[a->first + (uint32_t)(from - a->lo)],
                    (size_t)count * sizeof(v128));
        return X_NEXT;
    }
    case S_REV:
    case S_TRANS:
    case S_OCC: {
        /* from the left: the array (nil, dangling), from, count, the
           pattern of occurrences (nil, dangling); count < 0 a range
           error, the span inside the array unless count is 0, an empty
           pattern a range error */
        struct xh *a = heap_at(x, s->var, s->line, s->col);
        if (!a)
            return X_RET;
        uint32_t ah = (uint32_t)(a - x->harr);
        v128 from = ev(x, s->e);
        v128 count = x->trap ? 0 : ev(x, s->fld);
        if (x->trap)
            return X_RET;
        uint32_t ph = 0;
        if (s->k == S_OCC) {
            struct xh *p = heap_at(x, s->idx, s->line, s->col);
            if (!p)
                return X_RET;
            ph = (uint32_t)(p - x->harr);
        }
        a = &x->harr[ah];
        if (count < 0) { /* an Int64 */
            stop(x, 101, s->line, s->col);
            return X_RET;
        }
        if (count > 0 &&
            !(from >= a->lo && from <= a->hi && count - 1 <= a->hi - from)) {
            stop(x, 100, s->line, s->col);
            return X_RET;
        }
        /* no address of a cell when the span is empty: hcell may be
           NULL */
        v128 *c = count > 0 ? &x->hcell[a->first + (from - a->lo)] : NULL;
        if (s->k == S_REV) {
            for (v128 i = 0, j = count - 1; i < j; i++, j--) {
                v128 t = c[i];
                c[i] = c[j];
                c[j] = t;
            }
        } else if (s->k == S_TRANS) {
            for (v128 i = 0; i < count; i++)
                c[i] = (c[i] * (v128)s->fn + (v128)s->args) & 255;
        } else {
            const struct xh *p = &x->harr[ph];
            v128 m = p->hi - p->lo + 1;
            if (m < 1) {
                stop(x, 101, s->line, s->col);
                return X_RET;
            }
            const v128 *q = &x->hcell[p->first];
            v128 found = 0;
            for (v128 i = 0; c && i + m <= count;) {
                v128 k = 0;
                while (k < m && c[i + k] == q[k])
                    k++;
                if (k == m) {
                    found++;
                    i += m;
                } else {
                    i++;
                }
            }
            print_value(g, &x->out, T_I64, found);
            put(&x->out, "\n");
        }
        return X_NEXT;
    }
    case S_RDB:
    case S_WRB: {
        /* from the left: the array (nil, dangling), from, count; count <
           0 a range error, the span inside the array unless count is 0;
           then up to count bytes of the input, fewer only at its end,
           which is final; or the bytes written as they are */
        struct xh *a = heap_at(x, s->var, s->line, s->col);
        if (!a)
            return X_RET;
        uint32_t ah = (uint32_t)(a - x->harr);
        v128 from = ev(x, s->e);
        v128 count = x->trap ? 0 : ev(x, s->fld);
        if (x->trap)
            return X_RET;
        a = &x->harr[ah];
        if (count < 0) { /* an Int64 */
            stop(x, 101, s->line, s->col);
            return X_RET;
        }
        if (count > 0 &&
            !(from >= a->lo && from <= a->hi && count - 1 <= a->hi - from)) {
            stop(x, 100, s->line, s->col);
            return X_RET;
        }
        v128 *c = count > 0 ? &x->hcell[a->first + (from - a->lo)] : NULL;
        if (s->k == S_WRB) {
            for (v128 i = 0; i < count; i++) {
                char b = (char)(uint8_t)c[i];
                putn(&x->out, &b, 1);
            }
            return X_NEXT;
        }
        v128 got = 0;
        if (count > 0 && !x->in_end) {
            size_t left = x->inlen - x->inpos;
            got = (v128)left < count ? (v128)left : count;
            for (v128 i = 0; i < got; i++)
                c[i] = (uint8_t)x->in[x->inpos++];
            x->in_end = got < count;
        }
        print_value(g, &x->out, T_I64, got);
        put(&x->out, "\n");
        return X_NEXT;
    }
    case S_RDZ: {
        /* the span inside zN: up to count bytes of the input, fewer
           only at its end, which is final; how many, written */
        v128 count = ev(x, s->fld);
        if (x->trap)
            return X_RET;
        v128 got = 0;
        if (count > 0 && !x->in_end) {
            size_t left = x->inlen - x->inpos;
            got = (v128)left < count ? (v128)left : count;
            x->inpos += (size_t)got;
            x->in_end = got < count;
        }
        print_value(g, &x->out, T_I64, got);
        put(&x->out, "\n");
        return X_NEXT;
    }
    case S_WRS: {
        /* writebytes(s, from, count): from and count Int64, the String
           from 1 */
        v128 from = ev(x, s->e);
        v128 count = x->trap ? 0 : ev(x, s->fld);
        if (x->trap)
            return X_RET;
        /* after: a string made by the run may move the table */
        const struct xstr *t = &g->str[x->cell[x->ref[s->var]]];
        v128 len = (v128)t->n;
        if (count < 0) {
            stop(x, 101, s->line, s->col);
            return X_RET;
        }
        if (count > 0 &&
            !(from >= 1 && from <= len && count - 1 <= len - from)) {
            stop(x, 100, s->line, s->col);
            return X_RET;
        }
        if (count > 0)
            putn(&x->out, t->b + (from - 1), (size_t)count);
        return X_NEXT;
    }
    case S_HFREE: {
        v128 pv = x->cell[x->ref[s->var]];
        if (!pv)
            return X_NEXT;
        struct xh *a = &x->harr[pv - 1];
        if (a->dead) {
            stop(x, 106, s->line, s->col);
            return X_RET;
        }
        a->dead = true;
        x->live--;
        return X_NEXT;
    }
    case S_HALT: {
        /* 0 or 2..255 but 141, checked at the name: 1 is for the
           errors, 141 for a closed output (§ 9.8) */
        v128 v = ev(x, s->e);
        if (x->trap)
            return X_RET;
        if (v == 1 || v == 141 || v < 0 || v > 255) {
            stop(x, 101, s->line, s->col);
            return X_RET;
        }
        stop(x, (int)v, s->line, s->col);
        x->halt = true;
        return X_RET;
    }
    case S_COPY: {
        v128 c = ev(x, s->e);
        if (x->trap)
            return X_RET;
        memmove(&x->cell[x->ref[s->var]], &x->cell[(uint32_t)c],
                ncells(g, g->v[s->var].t) * sizeof(v128));
        return X_NEXT;
    }
    }
    return X_NEXT;
}

static int run_block(X *x, uint32_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int r = run_stmt(x, x->g->ls[b + i]);
        if (r != X_NEXT)
            return r;
    }
    return X_NEXT;
}

/* ---- one attempt ---- */

static void g_free(G *g)
{
    free(g->ty);
    free(g->e);
    free(g->st);
    free(g->arm);
    free(g->lab);
    free(g->ls);
    free(g->v);
    free(g->r);
    free(g->scope);
    free(g->gc);
    free(g->fld);
    for (uint32_t k = 0; k < g->nstr; k++)
        free(g->str[k].b);
    free(g->str);
    for (uint32_t k = 0; k < g->nbig; k++)
        limba_big_free(&g->big[k]);
    free(g->big);
}

static bool attempt(uint64_t seed, limba_lxgen *p, bool units)
{
    G g;
    memset(&g, 0, sizeof(g));
    g.s = seed;
    g.cur = -1;
    new_e(&g, E_LIT, 0); /* node 0 stands for none */
    new_str(&g, "", 0);  /* string 0 is empty */
    limba_big zero;
    limba_big_init(&zero);
    new_big(&g, &zero); /* BigInt 0 is zero */
    /* the command line and the input: pieces like those of the literals,
       and texts for val */
    g.narg = below(&g, 4);
    for (uint32_t k = 0; k < g.narg; k++)
        g.arg[k] =
            chance(&g, 50) ? (uint32_t)rand_value(&g, T_STR) : number_text(&g);
    g.nline = below(&g, 7);
    for (uint32_t k = 0; k < g.nline; k++)
        g.line[k] =
            chance(&g, 50) ? (uint32_t)rand_value(&g, T_STR) : number_text(&g);
    /* an empty last line with no newline is no line */
    g.nl_end = chance(&g, 80) || (g.nline && !g.str[g.line[g.nline - 1]].n);
    for (unsigned t = 0; t < NTYPES; t++)
        new_type(&g, (xt){K_BASE, (uint8_t)t, 0, 0, t == T_BOOL ? 0 : tmin(t),
                          tmax(t), 0, 0});

    /* the ranges, then the arrays, each indexed by a short range of its
       own */
    for (uint32_t k = 0, n = below(&g, 4); k < n; k++)
        new_range(&g, false);
    uint32_t narrays = 0, arrays[16];
    /* enumerations of 2 to 5 values, two global variables of each */
    for (uint32_t k = 0, n = below(&g, 3); k < n; k++) {
        uint32_t et = g.nty;
        uint32_t nv = 2 + below(&g, 4);
        new_type(&g, (xt){K_ENUM, (uint8_t)et, 0, nv, 0, (v128)nv - 1, 0, 0});
        arrays[narrays++] = et;
        arrays[narrays++] = et;
    }
    for (uint32_t k = 0, n = below(&g, 3); k < n; k++) {
        uint32_t en = pick_enum(&g);
        /* an index: a short range, or an enumeration */
        uint32_t ix = en && chance(&g, 30) ? en : new_range(&g, true);
        /* Strings too: counted in memory, copied with the array */
        uint32_t el = counted_or_var(&g, 20);
        arrays[narrays++] =
            new_type(&g, (xt){K_ARRAY, (uint8_t)base(&g, el), ix, el,
                              lo_of(&g, ix), hi_of(&g, ix), 0, 0});
    }
    /* records of 1 to 4 scalar fields, some with a pointer type; two
       global variables of each type, so that copies show */
    for (uint32_t k = 0, n = below(&g, 3); k < n; k++) {
        uint32_t first = g.nfld, nf = 1 + below(&g, 4);
        bool strings = false;
        for (uint32_t f = 0; f < nf; f++) {
            uint32_t ft = counted_or_var(&g, 25);
            strings |= ft == T_STR;
            LIMBA_GROW(g.fld, g.nfld, g.capfld);
            g.fld[g.nfld++] = ft;
        }
        uint32_t rt =
            new_type(&g, (xt){K_RECORD, T_BOOL, first, nf, 0, 0, 0, 0});
        arrays[narrays++] = rt;
        arrays[narrays++] = rt;
        /* a pointer type often; always for Strings, which new and
           dispose must count */
        if (strings || chance(&g, 60)) {
            uint32_t pt = new_type(&g, (xt){K_PTR, T_BOOL, 0, rt, 0, 0, 0, 0});
            arrays[narrays++] = pt;
            arrays[narrays++] = pt;
        }
    }

    /* arrays made by new: an open array with an integer index, Strings
       among its elements, and a pointer to it; their variables are local
       (§ 3.10, § 9.7) */
    for (uint32_t k = 0, n = below(&g, 3); k < n; k++) {
        unsigned ix = int_type(&g);
        /* Bytes often, for translate and occurrences (§ 9.5) */
        uint32_t el = chance(&g, 35) ? T_B8 : counted_or_var(&g, 20);
        uint32_t ot = new_type(
            &g, (xt){K_OPEN, (uint8_t)base(&g, el), ix, el, 0, 0, 0, 0});
        new_type(&g, (xt){K_HEAP, T_BOOL, 0, ot, 0, 0, 0, 0});
    }

    /* the globals: one of an integer type and one Boolean at least, then
       the arrays; their initialisers are the statements of the same
       numbers (none for an array) */
    uint32_t nglob = 2 + below(&g, 5);
    for (uint32_t k = 0; k < nglob + narrays; k++) {
        uint32_t t = k == 0       ? int_type(&g)
                     : k == 1     ? T_BOOL
                     : k >= nglob ? arrays[k - nglob]
                     : k == 2     ? (chance(&g, 50) ? T_F64 : T_F32)
                                  : var_type_s(&g);
        uint32_t v = new_v(&g, t, V_GLOBAL);
        uint32_t s = new_s(&g, S_VAR);
        g.st[s].var = v;
        if (!is_array(&g, t))
            g.st[s].e = is_ptr(&g, t)      ? nil_of(&g, t)
                        : is_record(&g, t) ? 0
                                           : lit(&g, t, init_value(&g, t));
        show(&g, v);
    }
    nglob += narrays;
    /* the global constants, after the variables: those take the first
       numbers */
    for (uint32_t k = 0, n = below(&g, 5); k < n; k++) {
        uint32_t c = constant(&g);
        LIMBA_GROW(g.gc, g.ngc, g.capgc);
        g.gc[g.ngc++] = c;
    }
    /* ranges whose bounds are constant expressions of those constants,
       for the variables of the routines and of main */
    for (uint32_t k = 0, n = below(&g, 3); k < n; k++)
        const_range(&g);
    g.budget = 30 + (int)below(&g, 60);
    for (uint32_t k = 0, nr = below(&g, 5); k < nr && g.budget > 10; k++)
        routine(&g);
    /* main calls most functions whose result is a record or an array
       first, into a global, and prints it: later a trap may come first */
    uint32_t first[16], nfirst = 0;
    for (uint32_t r = 0; r < g.nr && nfirst < 14; r++) {
        uint32_t rt = g.r[r].rt;
        if (!g.r[r].func || is_scalar(&g, rt) || chance(&g, 25))
            continue;
        uint32_t d = 0;
        while (g.v[d].t != rt) /* a global of every such type exists */
            d++;
        nfirst += copy_call(&g, r, d, true, first + nfirst);
    }
    /* and prints the functions of the library on some real globals */
    for (uint32_t v = 0; v < nglob && nfirst < 14; v++) {
        unsigned t = base(&g, g.v[v].t);
        if (!is_scalar(&g, g.v[v].t) || fam(t) != 'F')
            continue;
        uint32_t items[2 * NMATH], k = 0;
        for (unsigned m = 0; m < NMATH; m++) {
            if (k)
                items[k++] = new_e(&g, E_STR, 0);
            uint32_t a = var_ref(&g, v);
            uint32_t mc = math_call(&g, t, a, m);
            items[k++] = format(&g, mc);
        }
        uint32_t w = new_s(&g, S_WRITE);
        g.st[w].args = keep_list(&g, items, k);
        g.st[w].nargs = k;
        first[nfirst++] = w;
    }
    /* and prints its String globals, their length and bytes: the initial
       value is known here, so each index is inside */
    for (uint32_t v = 0; v < nglob && nfirst < 15; v++) {
        if (g.v[v].t != T_STR)
            continue;
        uint32_t n = g.str[(uint32_t)g.e[g.st[v].e].lit].n;
        uint32_t items[12], k = 0;
        items[k++] = var_ref(&g, v);
        items[k++] = new_e(&g, E_STR, 0);
        uint32_t a = var_ref(&g, v);
        items[k] = new_e(&g, E_SLEN, T_I64);
        g.e[items[k++]].a = a;
        for (uint32_t j = 1; j <= n && k < 11; j++) {
            items[k++] = new_e(&g, E_STR, 0);
            uint32_t ix = lit(&g, T_I64, j);
            items[k] = new_e(&g, E_SIDX, T_B8);
            g.e[items[k]].var = v;
            g.e[items[k++]].b = ix;
        }
        uint32_t w = new_s(&g, S_WRITE);
        g.st[w].args = keep_list(&g, items, k);
        g.st[w].nargs = k;
        first[nfirst++] = w;
    }
    /* and runs over the bounds of some global arrays */
    for (uint32_t v = 0; v < nglob && nfirst < 15; v++) {
        uint32_t at = g.v[v].t;
        if (g.ty[at].k != K_ARRAY || is_enum(&g, g.ty[at].index) ||
            chance(&g, 40))
            continue;
        unsigned b = index_base(&g, at);
        uint32_t s = new_s(&g, S_FOR);
        uint32_t i = new_v(&g, b, V_LOOP);
        g.st[s].var = i;
        g.st[s].e = bound(&g, E_LOW, v);
        g.st[s].e2 = bound(&g, E_HIGH, v);
        over_array(&g, s, v, i, b);
        first[nfirst++] = s;
    }
    g.main_blk = block(&g, 4 + (int)below(&g, 12), &g.main_n);
    while (nfirst)
        append(&g, &g.main_blk, &g.main_n, first[--nfirst], true);

    /* the program begins giving a value to every field of the records, and
       to half of the pointers a new record */
    {
        uint32_t *init = NULL, ni = 0, cap = 0;
        for (uint32_t v = 0; v < nglob; v++) {
            uint32_t t = g.v[v].t;
            if (!is_record(&g, t) && !(is_ptr(&g, t) && chance(&g, 50)))
                continue;
            if (is_ptr(&g, t)) {
                uint32_t s = new_s(&g, S_NEW);
                g.st[s].var = v;
                LIMBA_GROW(init, ni, cap);
                init[ni++] = s;
            }
            uint32_t rt = record_of(&g, t);
            for (uint32_t f = 0; f < g.ty[rt].elem; f++) {
                if (is_record(&g, t) && chance(&g, 10))
                    continue; /* without a value: a read is caught */
                uint32_t ft = field_type(&g, rt, f);
                uint32_t e = init_expr(&g, ft);
                uint32_t s = new_s(&g, S_ASSIGN);
                g.st[s].var = v;
                g.st[s].fld = f + 1;
                g.st[s].e = e;
                LIMBA_GROW(init, ni, cap);
                init[ni++] = s;
            }
        }
        while (ni)
            append(&g, &g.main_blk, &g.main_n, init[--ni], true);
        free(init);
    }

    /* the program begins filling the arrays and ends printing every
       scalar global */
    for (uint32_t v = 0; v < nglob; v++) {
        uint32_t at = g.v[v].t;
        if (!is_array(&g, at))
            continue;
        for (v128 k = g.ty[at].hi; k >= g.ty[at].lo; k--) {
            if (chance(&g, 8))
                continue; /* without a value: a read is caught */
            uint32_t s = new_s(&g, S_ASSIGN);
            uint32_t el = g.ty[at].elem;
            g.st[s].var = v;
            g.st[s].idx = lit(&g, base(&g, g.ty[at].index), k);
            g.st[s].e = init_expr(&g, el);
            append(&g, &g.main_blk, &g.main_n, s, true);
        }
    }
    {
        uint32_t items[96], k = 0;
        for (uint32_t v = 0; v < nglob; v++) {
            uint32_t t = g.v[v].t;
            if (is_array(&g, t))
                continue;
            if (is_record(&g, t)) {
                for (uint32_t f = 0; f < g.ty[t].elem; f++) {
                    if (k)
                        items[k++] = new_e(&g, E_STR, 0);
                    items[k++] = field_ref(&g, v, f);
                }
                continue;
            }
            if (k)
                items[k++] = new_e(&g, E_STR, 0);
            if (is_enum(&g, t)) {
                uint32_t a = var_ref(&g, v);
                items[k] = new_e(&g, E_ORD, T_U32);
                g.e[items[k++]].a = a;
                continue;
            }
            items[k++] = is_ptr(&g, t) ? binop(&g, O_EQ, T_BOOL, var_ref(&g, v),
                                               nil_of(&g, t))
                                       : var_ref(&g, v);
        }
        uint32_t s = new_s(&g, S_WRITE);
        g.st[s].args = keep_list(&g, items, k);
        g.st[s].nargs = k;
        append(&g, &g.main_blk, &g.main_n, s, false);
    }
    if (chance(&g, 20)) {
        /* the program ends by halt, after printing its globals */
        uint32_t hs[2], nh = halt_stmt(&g, hs);
        for (uint32_t k = 0; k < nh; k++)
            append(&g, &g.main_blk, &g.main_n, hs[k], false);
    }

    text src = {NULL, 0, 0, 1, 1, 0};
    text unit = {NULL, 0, 0, 1, 1, UNIT_LINES};
    struct web web;
    if (units) {
        g.units = true;
        g.moved = limba_xcalloc(g.nr ? g.nr : 1, 1);
        unit_text(&g, &unit);
        /* drawn after the program, which stays the one of one file */
        if (chance(&g, 75)) {
            web_make(&g, &web);
            g.web = &web;
        }
    }
    program(&g, &src, nglob);

    X x;
    memset(&x, 0, sizeof(x));
    x.g = &g;
    uint32_t ncell = 0;
    x.ref = limba_xmalloc((g.nv ? g.nv : 1) * sizeof(uint32_t));
    for (uint32_t v = 0; v < g.nv; v++) {
        x.ref[v] = ncell;
        uint32_t t = g.v[v].t;
        ncell += is_array(&g, t)    ? (uint32_t)(g.ty[t].hi - g.ty[t].lo) + 1
                 : is_record(&g, t) ? g.ty[t].elem
                                    : 1;
    }
    x.cell = limba_xcalloc(ncell ? ncell : 1, sizeof(v128));
    for (uint32_t v = 0; v < g.nv; v++)
        if (is_record(&g, g.v[v].t) || g.ty[g.v[v].t].k == K_ARRAY)
            unassigned(&g, &x.cell[x.ref[v]], g.v[v].t);
    x.alo = limba_xcalloc(g.nv ? g.nv : 1, sizeof(v128));
    x.ahi = limba_xcalloc(g.nv ? g.nv : 1, sizeof(v128));
    for (uint32_t v = 0; v < g.nv; v++)
        if (g.ty[g.v[v].t].k == K_ARRAY) {
            x.alo[v] = g.ty[g.v[v].t].lo;
            x.ahi[v] = g.ty[g.v[v].t].hi;
        }
    x.out.line = x.out.col = 1;
    if (units)
        put(&x.out, "lib\n"); /* the initialisation of Lib, first */
    if (g.web)
        putn(&x.out, web.out.b, web.out.n); /* the program's first line */
    for (uint32_t v = 0; v < nglob; v++)
        if (!is_array(&g, g.v[v].t) && !is_record(&g, g.v[v].t))
            x.cell[x.ref[v]] = g.e[g.st[v].e].lit;
    text in = {NULL, 0, 0, 1, 1, 0};
    for (uint32_t k = 0; k < g.nline; k++) {
        putn(&in, g.str[g.line[k]].b, g.str[g.line[k]].n);
        if (k + 1 < g.nline || g.nl_end)
            put(&in, "\n");
    }
    x.in = in.b;
    x.inlen = in.n;
    run_block(&x, g.main_blk, g.main_n);
    bool ok = !x.toolong;
    if (ok) {
        p->src = src.b;
        src.b = NULL;
        LIMBA_GROW(x.out.b, x.out.n, x.out.cap);
        x.out.b[x.out.n] = 0;
        p->out = x.out.b;
        p->outlen = x.out.n;
        x.out.b = NULL;
        p->in = in.b;
        p->inlen = in.n;
        in.b = NULL;
        p->argc = (int)g.narg;
        p->argv = limba_xcalloc(g.narg + 1, sizeof(char *));
        for (uint32_t k = 0; k < g.narg; k++) {
            const struct xstr *a = &g.str[g.arg[k]];
            p->argv[k] = limba_xmalloc(a->n + 1);
            if (a->n)
                memcpy(p->argv[k], a->b, a->n);
            p->argv[k][a->n] = 0;
        }
        /* a place in Lib says its file */
        const char *file = x.line >= UNIT_LINES ? "lib.luxia:" : "";
        uint32_t line = x.line >= UNIT_LINES ? x.line - UNIT_LINES : x.line;
        if (x.halt)
            snprintf(p->end, sizeof(p->end), "halt %d at %s%u:%u", x.code, file,
                     line, x.col);
        else if (x.trap)
            snprintf(p->end, sizeof(p->end), "trap %d at %s%u:%u", x.code, file,
                     line, x.col);
        else if (x.live)
            snprintf(p->end, sizeof(p->end), "ok, %u live", x.live);
        else
            snprintf(p->end, sizeof(p->end), "ok");
        if (units) {
            p->file[0].text = unit.b;
            unit.b = NULL;
            snprintf(p->file[0].name, sizeof(p->file[0].name), "lib");
            p->nfile = 1;
            if (g.web)
                p->nfile += web_files(&web, p->file + 1);
            /* the note on Coll comes as the units are read, the warning
               on a Lib never named after the analysis */
            snprintf(p->notes, sizeof(p->notes), "%s%s",
                     g.web && web.lib ? "L0080" : "",
                     g.lib_named        ? ""
                     : g.web && web.lib ? " L0077"
                                        : "L0077");
        }
    }
    free(src.b);
    free(unit.b);
    if (g.web)
        free(web.out.b);
    free(g.moved);
    free(in.b);
    free(x.out.b);
    free(x.cell);
    free(x.ref);
    free(x.alo);
    free(x.ahi);
    free(x.heap);
    free(x.harr);
    free(x.hcell);
    g_free(&g);
    return ok;
}

bool limba_lxgen_make(uint64_t seed, limba_lxgen *p)
{
    memset(p, 0, sizeof(*p));
    for (uint64_t k = 0; k < 16; k++)
        if (attempt(seed * 16 + k, p, false))
            return true;
    return false;
}

bool limba_lxgen_make_units(uint64_t seed, limba_lxgen *p)
{
    memset(p, 0, sizeof(*p));
    for (uint64_t k = 0; k < 16; k++)
        if (attempt(seed * 16 + k, p, true))
            return true;
    return false;
}

void limba_lxgen_free(limba_lxgen *p)
{
    free(p->src);
    for (unsigned k = 0; k < p->nfile; k++)
        free(p->file[k].text);
    free(p->out);
    free(p->in);
    for (int k = 0; k < p->argc; k++)
        free(p->argv[k]);
    free(p->argv);
    memset(p, 0, sizeof(*p));
}
