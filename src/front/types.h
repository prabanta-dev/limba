/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * types.h - the types of a source language, shared by the front ends. A
 * type is an index; the language decides what its types mean, this table
 * holds them, lays records out as C does, and prints them for messages.
 *
 * Identity: every type has a root. A subtype with a range keeps the root
 * of its base (it is compatible with it); a distinct type (new T) is its
 * own root. Records, enumerations and arrays are distinct by declaration;
 * pointers, open arrays and routine signatures are equal by structure
 * (limba_types_same).
 */
#ifndef LIMBA_FRONT_TYPES_H
#define LIMBA_FRONT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t limba_ltype;

enum {
    LIMBA_LTK_ERROR, /* type 0: a mistake already reported */
    LIMBA_LTK_VOID,
    LIMBA_LTK_INT,
    LIMBA_LTK_FLOAT,
    LIMBA_LTK_BOOL,
    LIMBA_LTK_CHAR,
    LIMBA_LTK_ENUM,
    LIMBA_LTK_STRING,
    LIMBA_LTK_ARRAY,
    LIMBA_LTK_OPEN, /* an array of any length, as a parameter */
    LIMBA_LTK_RECORD,
    LIMBA_LTK_POINTER,
    LIMBA_LTK_ROUTINE,
    LIMBA_LTK_UINT,  /* an integer constant not typed yet */
    LIMBA_LTK_UREAL, /* a real constant not typed yet */
    LIMBA_LTK_NIL,
};

#define LIMBA_TF_SIGNED 1u
#define LIMBA_TF_MODULAR 2u     /* wraps around */
#define LIMBA_TF_RANGE 4u       /* lo and hi narrow the base */
#define LIMBA_TF_DYNAMIC 8u     /* ARRAY: bounds computed at run time */
#define LIMBA_TF_INCOMPLETE 16u /* RECORD: fields still being added */

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t bits;         /* INT, FLOAT: the width */
    limba_ltype root;      /* equal roots: compatible */
    limba_ltype base;      /* what a range narrows; itself otherwise */
    limba_ltype elem;      /* ARRAY, OPEN: elements; POINTER: target; ROUTINE:
                             result (VOID for a procedure) */
    limba_ltype index;     /* ARRAY: the index type */
    __int128 lo, hi;       /* discrete types: the bounds */
    uint32_t name;         /* a name id for messages, UINT32_MAX if none */
    uint32_t first, count; /* RECORD: fields; ROUTINE: parameters; ENUM:
                              count only */
    uint64_t size;         /* bytes; 0 when not known before run time */
    uint32_t align;
} limba_typeinfo;

typedef struct {
    uint32_t name;
    limba_ltype type;
    uint64_t offset;
} limba_field;

typedef struct {
    limba_ltype type;
    uint8_t mode; /* the language's: in, var, out */
} limba_param;

typedef struct {
    limba_typeinfo *t;
    uint32_t n, cap;
    limba_field *field;
    uint32_t nfield, capfield;
    limba_param *param;
    uint32_t nparam, capparam;
    limba_ltype void_, uint, ureal, nil, bool_, char_, string;
} limba_types;

void limba_types_init(limba_types *ts);
void limba_types_free(limba_types *ts);

static inline const limba_typeinfo *limba_ty(const limba_types *ts,
                                             limba_ltype t)
{
    return &ts->t[t];
}

/* a new integer or float type of its own (the language names it) */
limba_ltype limba_types_int(limba_types *ts, unsigned bits, unsigned flags);
limba_ltype limba_types_float(limba_types *ts, unsigned bits);
limba_ltype limba_types_enum(limba_types *ts, uint32_t count);
/* base narrowed to lo..hi: compatible with base */
limba_ltype limba_types_range(limba_types *ts, limba_ltype base, __int128 lo,
                              __int128 hi);
/* a copy of base with a root of its own */
limba_ltype limba_types_distinct(limba_types *ts, limba_ltype base);
/* static bounds from the index type, or DYNAMIC; false if the size
   overflows (the type is made anyway, with size 0) */
limba_ltype limba_types_array(limba_types *ts, limba_ltype index,
                              limba_ltype elem, bool dynamic, bool *ok);
limba_ltype limba_types_open(limba_types *ts, limba_ltype elem);
/* target may be completed later with limba_types_set_target */
limba_ltype limba_types_pointer(limba_types *ts, limba_ltype target);
void limba_types_set_target(limba_types *ts, limba_ltype p, limba_ltype target);
limba_ltype limba_types_routine(limba_types *ts, const limba_param *params,
                                uint32_t n, limba_ltype result);
/* a record: begin, add the fields in order, end lays it out */
limba_ltype limba_types_record_begin(limba_types *ts);
void limba_types_record_field(limba_types *ts, limba_ltype r, uint32_t name,
                              limba_ltype type);
/* false if the size overflows */
bool limba_types_record_end(limba_types *ts, limba_ltype r);

void limba_types_set_name(limba_types *ts, limba_ltype t, uint32_t name);

bool limba_types_same(const limba_types *ts, limba_ltype a, limba_ltype b);
bool limba_types_is_discrete(const limba_types *ts, limba_ltype t);

/* the type as a message says it; names come from the callback */
typedef const char *(*limba_name_fn)(const void *ctx, uint32_t id, size_t *len);
const char *limba_types_show(const limba_types *ts, limba_ltype t,
                             limba_name_fn name, const void *ctx, char *buf,
                             size_t size);

#endif
