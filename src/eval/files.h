/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * files.h - the files of a run of the reference interpreter (file_* of
 * runtime.def, doc/ir.md § 15.11): handles numbered from 1 and never
 * given again, at most FILES_MAX open, a buffer of FILES_BUF bytes each,
 * written with write(2) when full, at flush and at close. The memory of
 * the buffers is the run time's, not the program's: outside its budget.
 */
#ifndef LIMBA_EVAL_FILES_H
#define LIMBA_EVAL_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FILES_MAX 256
#define FILES_BUF 65536

typedef struct {
    int64_t handle; /* 0: the place is free */
    int fd;
    int mode; /* 0 read, 1 write, 2 append */
    bool end; /* reading: the end met, final */
    uint8_t *buf;
    size_t len, at; /* reading: buf[at .. len) not read yet; writing: len */
} efile;

typedef struct {
    efile f[FILES_MAX];
    int64_t next; /* the handle of the next file opened, from 1 */
} efiles;

/* what a call ends with: the trap and the words it takes */
enum {
    FILES_OK,
    FILES_HANDLE,   /* never opened, or closed: INVALID_HANDLE */
    FILES_IO,       /* an error of reading or writing: IO */
    FILES_NOTFOUND, /* IO, "not found: <name>" */
    FILES_DENIED,   /* IO, "not allowed: <name>" */
    FILES_CANNOT,   /* IO, "cannot open: " or "cannot delete: <name>" */
    FILES_TOOMANY,  /* IO, "too many open files" */
};

void files_init(efiles *t);
/* name ends with a NUL and holds no other; mode 0, 1 or 2 */
int files_open(efiles *t, const char *name, int mode, int64_t *handle);
int files_close(efiles *t, int64_t handle);
int files_flush(efiles *t, int64_t handle);
int files_write(efiles *t, int64_t handle, const void *p, size_t n);
/* up to n bytes, fewer only at the end: *got */
int files_read(efiles *t, int64_t handle, void *p, size_t n, size_t *got);
/* a line without its "\n" and a "\r" before it, malloc'd into *line;
 *got false at the end, final */
int files_read_line(efiles *t, int64_t handle, char **line, size_t *len,
                    bool *got);
bool files_exists(const char *name);
int files_delete(const char *name);
/* the end of the run: every file written and closed; false if one of
   them failed */
bool files_close_all(efiles *t);

#endif
