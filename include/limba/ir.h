/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ir.h - the Limba intermediate representation, shared with Meri.
 *
 * A module holds interned strings, types, globals, external (C) functions
 * and functions. A function is a list of blocks; a block is a list of
 * instructions, its parameters first (LIMBA_OP_PARAM, in place of phi
 * nodes) and one terminator last. Everything is referred to by index
 * (limba_id), never by pointer, so the binary form is the arrays written
 * out. An instruction produces at most one value, named by its index in
 * the function.
 *
 * Design and reasons: job/docs/progetto_ir.md.
 */
#ifndef LIMBA_IR_H
#define LIMBA_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* bumped whenever the binary form or the tables change incompatibly */
#define LIMBA_IR_VERSION 2

typedef uint32_t limba_id;
#define LIMBA_NONE UINT32_MAX

/* ---- operations ---- */

/* shapes of the operands */
enum limba_format {
    LIMBA_F_ICONST,   /* imm = value */
    LIMBA_F_FCONST,   /* imm = bits of the double (f32 too, widened) */
    LIMBA_F_SCONST,   /* imm = string id */
    LIMBA_F_TYPED,    /* nothing but the result type (null, undef) */
    LIMBA_F_UN,       /* one value */
    LIMBA_F_BIN,      /* two values */
    LIMBA_F_TERN,     /* three values */
    LIMBA_F_CMP,      /* two values, the condition in cc */
    LIMBA_F_CONV,     /* one value, the result type is the target */
    LIMBA_F_LOAD,     /* address */
    LIMBA_F_STORE,    /* value, address */
    LIMBA_F_SLOT,     /* imm = slot index */
    LIMBA_F_GADDR,    /* imm = global id */
    LIMBA_F_FADDR,    /* imm = function id */
    LIMBA_F_ADDR,     /* base, index; imm = scale, imm2 = displacement */
    LIMBA_F_MEM3,     /* destination, source or byte, length */
    LIMBA_F_CALL,     /* imm = function id; arguments */
    LIMBA_F_CALL_IND, /* imm = function type; callee value, arguments */
    LIMBA_F_CALL_EXT, /* imm = extern id; arguments */
    LIMBA_F_CALL_RT,  /* imm = runtime id; arguments */
    LIMBA_F_BR,       /* target block, n, n arguments */
    LIMBA_F_CBR,      /* condition, then (block, n, args), else (same) */
    LIMBA_F_SWITCH,   /* value, default block, n, n x (lo, hi, block) */
    LIMBA_F_RET,      /* zero or one value */
    LIMBA_F_NONE,     /* nothing */
    LIMBA_F_TRAP,     /* imm = error code */
    LIMBA_F_CHECK,    /* condition; imm = error code */
    LIMBA_F_PARAM,    /* block parameter: nothing */
};

/* what an optimisation may assume */
enum {
    LIMBA_OPF_PURE = 1u << 0,        /* no effect: may move, merge, die */
    LIMBA_OPF_COMMUTATIVE = 1u << 1, /* the two operands may swap */
    LIMBA_OPF_MAY_TRAP = 1u << 2,    /* may raise a run-time error */
    LIMBA_OPF_READS_MEM = 1u << 3,
    LIMBA_OPF_WRITES_MEM = 1u << 4,
    LIMBA_OPF_TERMINATOR = 1u << 5, /* ends a block */
    LIMBA_OPF_NO_RESULT = 1u << 6,  /* produces no value */
    LIMBA_OPF_CALL = 1u << 7,       /* a call: see the callee */
};

enum limba_op {
#define LIMBA_OP(name, text, format, flags) LIMBA_OP_##name,
#include "limba/ir_ops.def"
#undef LIMBA_OP
    LIMBA_OP_COUNT
};

typedef struct {
    const char *text;
    uint8_t format; /* enum limba_format */
    uint16_t flags; /* LIMBA_OPF_* */
} limba_op_info;

extern const limba_op_info limba_ops[LIMBA_OP_COUNT];

/* conditions of icmp and fcmp (cc) */
enum limba_cc {
    LIMBA_CC_EQ, /* icmp */
    LIMBA_CC_NE,
    LIMBA_CC_SLT,
    LIMBA_CC_SLE,
    LIMBA_CC_SGT,
    LIMBA_CC_SGE,
    LIMBA_CC_ULT,
    LIMBA_CC_ULE,
    LIMBA_CC_UGT,
    LIMBA_CC_UGE,
    LIMBA_CC_OEQ, /* fcmp, ordered: false when a NaN is involved */
    LIMBA_CC_ONE,
    LIMBA_CC_OLT,
    LIMBA_CC_OLE,
    LIMBA_CC_OGT,
    LIMBA_CC_OGE,
    LIMBA_CC_ORD,
    LIMBA_CC_UNO, /* unordered: true when a NaN is involved */
    LIMBA_CC_UEQ,
    LIMBA_CC_UNE,
    LIMBA_CC_FULT,
    LIMBA_CC_FULE,
    LIMBA_CC_FUGT,
    LIMBA_CC_FUGE,
    LIMBA_CC_COUNT
};

extern const char *const limba_cc_text[LIMBA_CC_COUNT];
bool limba_cc_is_float(unsigned cc);

/* ---- the run-time library ---- */

enum {
    LIMBA_RTA_PURE = 1u << 0,       /* depends on its arguments only */
    LIMBA_RTA_IO = 1u << 1,         /* talks to the outside */
    LIMBA_RTA_ALLOC = 1u << 2,      /* returns a new object */
    LIMBA_RTA_WRITES_MEM = 1u << 3, /* writes memory of the program */
    LIMBA_RTA_MAY_TRAP = 1u << 4,
    LIMBA_RTA_NORETURN = 1u << 5,
};

enum limba_rt {
#define LIMBA_RT(name, text, sig, attrs) LIMBA_RT_##name,
#include "limba/runtime.def"
#undef LIMBA_RT
    LIMBA_RT_COUNT
};

typedef struct {
    const char *name; /* as in the text form */
    const char *sig;  /* "(i64) -> void" */
    uint32_t attrs;   /* LIMBA_RTA_* */
} limba_rt_info;

extern const limba_rt_info limba_rts[LIMBA_RT_COUNT];
/* FNV-1a over names, signatures and attributes: written in every .lir */
uint64_t limba_rt_fingerprint(void);
/* the id of a name of the text form, LIMBA_NONE if unknown */
limba_id limba_rt_find(const char *name, size_t len);

/* the checks that stop a program at run time (traps.def) */
enum limba_trap {
#define LIMBA_TRAP(name, code, text) LIMBA_TRAP_##name = code,
#include "limba/traps.def"
#undef LIMBA_TRAP
};

/* what failed, for the message; NULL for a code no check has */
const char *limba_trap_text(int64_t code);

/* ---- types ---- */

enum limba_type_kind {
    LIMBA_TK_VOID,
    LIMBA_TK_I1,
    LIMBA_TK_I8,
    LIMBA_TK_I16,
    LIMBA_TK_I32,
    LIMBA_TK_I64,
    LIMBA_TK_F32,
    LIMBA_TK_F64,
    LIMBA_TK_PTR, /* a machine address, opaque */
    LIMBA_TK_STR, /* a string of the run time, a handle */
    LIMBA_TK_REF, /* another object of the run time, a handle */
    LIMBA_TK_STRUCT,
    LIMBA_TK_ARRAY,
    LIMBA_TK_FUNC,
};

/* the scalar types have fixed ids, equal to their kind */
enum {
    LIMBA_T_VOID = LIMBA_TK_VOID,
    LIMBA_T_I1 = LIMBA_TK_I1,
    LIMBA_T_I8 = LIMBA_TK_I8,
    LIMBA_T_I16 = LIMBA_TK_I16,
    LIMBA_T_I32 = LIMBA_TK_I32,
    LIMBA_T_I64 = LIMBA_TK_I64,
    LIMBA_T_F32 = LIMBA_TK_F32,
    LIMBA_T_F64 = LIMBA_TK_F64,
    LIMBA_T_PTR = LIMBA_TK_PTR,
    LIMBA_T_STR = LIMBA_TK_STR,
    LIMBA_T_REF = LIMBA_TK_REF,
    LIMBA_T_FIRST_USER
};

typedef struct {
    uint8_t kind;     /* enum limba_type_kind */
    uint8_t variadic; /* function types */
    uint32_t size;    /* bytes; 0 for void and function types */
    uint32_t align;
    limba_id name;  /* structs: the name, a string id */
    limba_id elem;  /* arrays: element type; functions: result type */
    uint32_t count; /* arrays: elements; structs: fields; func: params */
    uint32_t first; /* structs and functions: index in m->members */
} limba_type;

/* a struct field (type, offset) or a function parameter (type, 0) */
typedef struct {
    limba_id type;
    uint32_t offset;
} limba_member;

/* ---- module contents ---- */

enum { LIMBA_SYM_EXPORT = 1u << 0, LIMBA_SYM_CONST = 1u << 1 };

enum limba_init {
    LIMBA_INIT_ZERO,
    LIMBA_INIT_INT,
    LIMBA_INIT_FLOAT,
    LIMBA_INIT_STR
};

typedef struct {
    limba_id name;
    limba_id type;
    uint32_t flags; /* LIMBA_SYM_* */
    uint8_t init;   /* enum limba_init */
    int64_t value;  /* integer, bits of a double, or string id */
} limba_global;

typedef struct {
    limba_id name;    /* the name in the IR */
    limba_id type;    /* a function type */
    limba_id symbol;  /* the C symbol */
    limba_id library; /* LIMBA_NONE: resolved from the process */
} limba_extern;

typedef struct {
    uint32_t size;
    uint32_t align;
} limba_slot;

typedef struct {
    uint16_t op; /* enum limba_op */
    uint8_t cc;  /* enum limba_cc, for icmp and fcmp */
    uint8_t pad;
    limba_id type;  /* the result type, LIMBA_T_VOID when there is none */
    limba_id block; /* the block it belongs to */
    uint32_t nops;  /* operands, in f->operands[first .. first + nops) */
    uint32_t first;
    int64_t imm;
    int64_t imm2;
} limba_inst;

typedef struct {
    uint32_t *insts; /* instruction ids in order: parameters first */
    uint32_t ninsts;
    uint32_t cap;
    uint32_t nparams;
} limba_block;

/* a place in the source a front end compiled: a file (a string id), a
   line and a column, from 1; an instruction refers to one by index, from
   1, and 0 means none */
typedef struct {
    limba_id file;
    uint32_t line, col;
} limba_pos;

typedef struct {
    limba_id name;
    limba_id type;  /* a function type */
    uint32_t flags; /* LIMBA_SYM_* */
    /* the position of every instruction, NULL if none has one; new
       instructions get pos_cur */
    uint32_t *locs;
    uint32_t caplocs;
    uint32_t pos_cur;
    limba_inst *insts;
    uint32_t ninsts, capinsts;
    uint32_t *operands;
    uint32_t noperands, capoperands;
    limba_block *blocks; /* block 0 is the entry */
    uint32_t nblocks, capblocks;
    limba_slot *slots;
    uint32_t nslots, capslots;
} limba_func;

enum limba_memory { LIMBA_MEM_STRICT, LIMBA_MEM_FB };

typedef struct limba_strtab limba_strtab;
typedef struct limba_hash limba_hash;

typedef struct limba_module {
    limba_id name;   /* string id, LIMBA_NONE if unnamed */
    uint32_t memory; /* enum limba_memory; STRICT by default */
    limba_strtab *strings;
    limba_type *types;
    uint32_t ntypes, captypes;
    limba_member *members;
    uint32_t nmembers, capmembers;
    limba_hash *typeidx; /* interning of types: arrays, functions, structs */
    limba_hash *symidx;  /* functions, globals and externs by name */
    limba_global *globals;
    uint32_t nglobals, capglobals;
    limba_extern *externs;
    uint32_t nexterns, capexterns;
    limba_func *funcs;
    uint32_t nfuncs, capfuncs;
    limba_pos *pos; /* pos[k - 1] is position k */
    uint32_t npos, cappos;
} limba_module;

/* ---- building ---- */

limba_module *limba_module_new(void);
void limba_module_free(limba_module *m);

limba_id limba_str_intern(limba_module *m, const char *s, size_t len);
/* the bytes of a string id; *len may be NULL */
const char *limba_str(const limba_module *m, limba_id id, size_t *len);
uint32_t limba_str_count(const limba_module *m);

limba_id limba_type_array(limba_module *m, limba_id elem, uint32_t count);
limba_id limba_type_func(limba_module *m, limba_id ret, const limba_id *params,
                         uint32_t nparams, bool variadic);
/* a named struct with an explicit layout; LIMBA_NONE if the name is taken */
limba_id limba_type_struct(limba_module *m, limba_id name,
                           const limba_member *fields, uint32_t nfields,
                           uint32_t size, uint32_t align);
/* the struct named so, LIMBA_NONE if none */
limba_id limba_type_find_struct(const limba_module *m, limba_id name);
bool limba_type_is_int(limba_id t); /* i1 .. i64 */
bool limba_type_is_float(limba_id t);
/* bits of an integer type, 0 for anything else */
unsigned limba_type_bits(limba_id t);

limba_id limba_global_add(limba_module *m, limba_id name, limba_id type,
                          uint32_t flags);
limba_id limba_extern_add(limba_module *m, limba_id name, limba_id type,
                          limba_id symbol, limba_id library);
limba_id limba_func_add(limba_module *m, limba_id name, limba_id type,
                        uint32_t flags);
/* the function, global or extern named so, LIMBA_NONE if none */
limba_id limba_func_find(const limba_module *m, limba_id name);
limba_id limba_global_find(const limba_module *m, limba_id name);
limba_id limba_extern_find(const limba_module *m, limba_id name);

/* free the body of a function (blocks, instructions, slots, positions):
   it keeps its name and type, once written somewhere else */
void limba_func_clear(limba_func *f);

limba_id limba_block_add(limba_func *f);
limba_id limba_slot_add(limba_func *f, uint32_t size, uint32_t align);
/* append an instruction to block b; its value id is the return */
limba_id limba_inst_add(limba_func *f, limba_id b, unsigned op, limba_id type,
                        unsigned cc, int64_t imm, int64_t imm2,
                        const uint32_t *ops, uint32_t nops);
/* a parameter of block b, of type t */
limba_id limba_param_add(limba_func *f, limba_id b, limba_id t);

/* a position, by index from 1; the last one again if it is the same */
uint32_t limba_pos_add(limba_module *m, limba_id file, uint32_t line,
                       uint32_t col);
/* the position of an instruction, 0 if none */
static inline uint32_t limba_inst_pos(const limba_func *f, limba_id i)
{
    return f->locs && i < f->caplocs ? f->locs[i] : 0;
}

/* ---- control flow ---- */

/* the successors of block b, read from its terminator; returns how many
   were written, at most max (a switch may have many) */
uint32_t limba_succs(const limba_func *f, limba_id b, limba_id *out,
                     uint32_t max);

/* ---- checking, text and binary forms ---- */

/* error text of the last failure of verify, parse or read */
typedef struct {
    char msg[256];
    unsigned line; /* text form: line of the error, 0 if none */
} limba_diag;

/* 0 if the module is well formed; otherwise -1 and the first defect */
int limba_verify(const limba_module *m, limba_diag *d);
/* the same in parts: the types, globals, externs, the declarations of the
   functions and the positions; then the function bodies one by one, with
   a verifier that keeps its memory from one to the next */
int limba_verify_decls(const limba_module *m, limba_diag *d);
typedef struct limba_verifier limba_verifier;
limba_verifier *limba_verifier_new(const limba_module *m);
int limba_verifier_func(limba_verifier *v, limba_id fid, limba_diag *d);
void limba_verifier_free(limba_verifier *v);

/* the text form (.lit) */
void limba_print(const limba_module *m, FILE *out);
/* NULL on error, with d filled */
limba_module *limba_parse(const char *text, size_t len, limba_diag *d);

/* the binary form (.lir), in memory: *buf is malloc'd, the caller frees */
int limba_write(const limba_module *m, uint8_t **buf, size_t *len);
/* the same a function at a time, for a front end that frees each body
   once written: limba_writer_func encodes one (it may then be cleared),
   limba_writer_end encodes the rest of m, the functions not given too,
   frees the writer and gives the bytes of limba_write(m) */
typedef struct limba_writer limba_writer;
limba_writer *limba_writer_new(void);
void limba_writer_func(limba_writer *w, const limba_module *m, limba_id fid);
int limba_writer_end(limba_writer *w, const limba_module *m, uint8_t **buf,
                     size_t *len);
/* a writer given up before its end */
void limba_writer_free(limba_writer *w);
limba_module *limba_read(const uint8_t *buf, size_t len, limba_diag *d);

#endif
