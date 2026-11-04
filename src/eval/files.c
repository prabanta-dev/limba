/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * files.c - the files of the reference interpreter (see files.h): open(2),
 * read(2), write(2), close(2), with buffers of their own, so that an error
 * of writing is found by the same call as in any other engine, which the
 * buffer of a FILE of the C library would move.
 */
#include "files.h"

#include "common/xalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void files_init(efiles *t)
{
    memset(t, 0, sizeof(*t));
    t->next = 1;
}

static efile *find(efiles *t, int64_t handle)
{
    if (handle <= 0)
        return NULL;
    for (int i = 0; i < FILES_MAX; i++)
        if (t->f[i].handle == handle)
            return &t->f[i];
    return NULL;
}

/* why a name could not be opened or removed */
static int why(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
        return FILES_NOTFOUND;
    case EACCES:
    case EPERM:
    case EROFS:
        return FILES_DENIED;
    default:
        return FILES_CANNOT;
    }
}

int files_open(efiles *t, const char *name, int mode, int64_t *handle)
{
    efile *f = NULL;
    for (int i = 0; i < FILES_MAX && !f; i++)
        if (!t->f[i].handle)
            f = &t->f[i];
    if (!f)
        return FILES_TOOMANY;
    static const int how[3] = {O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC,
                               O_WRONLY | O_CREAT | O_APPEND};
    int fd;
    do
        fd = open(name, how[mode] | O_CLOEXEC, 0666);
    while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return why(errno);
    struct stat st;
    if (fstat(fd, &st) || S_ISDIR(st.st_mode)) {
        close(fd);
        return FILES_CANNOT;
    }
    *f = (efile){.handle = t->next++, .fd = fd, .mode = mode};
    f->buf = limba_xmalloc(FILES_BUF);
    *handle = f->handle;
    return FILES_OK;
}

/* the n bytes from p written out, all of them */
static bool put(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* the buffer of a file written out */
static int drain(efile *f)
{
    if (!f->mode || !f->len)
        return FILES_OK;
    bool ok = put(f->fd, f->buf, f->len);
    f->len = 0;
    return ok ? FILES_OK : FILES_IO;
}

static int shut(efile *f)
{
    int r = drain(f);
    if (close(f->fd) && r == FILES_OK && errno != EINTR)
        r = FILES_IO;
    free(f->buf);
    *f = (efile){0};
    return r;
}

int files_close(efiles *t, int64_t handle)
{
    efile *f = find(t, handle);
    return f ? shut(f) : FILES_HANDLE;
}

int files_flush(efiles *t, int64_t handle)
{
    efile *f = find(t, handle);
    return f ? drain(f) : FILES_HANDLE;
}

int files_write(efiles *t, int64_t handle, const void *p, size_t n)
{
    efile *f = find(t, handle);
    if (!f)
        return FILES_HANDLE;
    if (!f->mode)
        return FILES_IO;
    const uint8_t *s = p;
    while (n) {
        size_t k = FILES_BUF - f->len < n ? FILES_BUF - f->len : n;
        memcpy(f->buf + f->len, s, k);
        f->len += k;
        s += k;
        n -= k;
        if (f->len == FILES_BUF && drain(f) != FILES_OK)
            return FILES_IO;
    }
    return FILES_OK;
}

/* more bytes into the buffer of a file read: false at an error, *f->end
   at its end */
static bool fill(efile *f)
{
    if (f->end)
        return true;
    if (f->at == f->len)
        f->at = f->len = 0;
    ssize_t r;
    do
        r = read(f->fd, f->buf + f->len, FILES_BUF - f->len);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return false;
    if (r == 0)
        f->end = true;
    f->len += (size_t)r;
    return true;
}

int files_read(efiles *t, int64_t handle, void *p, size_t n, size_t *got)
{
    efile *f = find(t, handle);
    *got = 0;
    if (!f)
        return FILES_HANDLE;
    if (f->mode)
        return FILES_IO;
    uint8_t *d = p;
    while (*got < n) {
        if (f->at == f->len) {
            if (f->end)
                break;
            if (!fill(f))
                return FILES_IO;
            continue;
        }
        size_t k = f->len - f->at < n - *got ? f->len - f->at : n - *got;
        memcpy(d + *got, f->buf + f->at, k);
        f->at += k;
        *got += k;
    }
    return FILES_OK;
}

int files_read_line(efiles *t, int64_t handle, char **line, size_t *len,
                    bool *got)
{
    efile *f = find(t, handle);
    *line = NULL;
    *len = 0;
    *got = false;
    if (!f)
        return FILES_HANDLE;
    if (f->mode)
        return FILES_IO;
    size_t n = 0, cap = 0;
    char *s = NULL;
    bool any = false, nl = false;
    while (!nl) {
        if (f->at == f->len) {
            if (f->end)
                break;
            if (f->len == FILES_BUF) /* all read: room for more */
                f->at = f->len = 0;
            if (!fill(f)) {
                free(s);
                return FILES_IO;
            }
            continue;
        }
        uint8_t c = f->buf[f->at++];
        any = true;
        if (c == '\n') {
            nl = true;
            break;
        }
        if (n + 1 >= cap) {
            cap = cap ? 2 * cap : 64;
            s = limba_xrealloc(s, cap, 1);
        }
        s[n++] = (char)c;
    }
    if (!any) {
        free(s);
        return FILES_OK; /* the end */
    }
    if (n && s[n - 1] == '\r')
        n--;
    *line = s;
    *len = n;
    *got = true;
    return FILES_OK;
}

bool files_exists(const char *name)
{
    struct stat st;
    return stat(name, &st) == 0;
}

int files_delete(const char *name)
{
    return unlink(name) ? why(errno) : FILES_OK;
}

bool files_close_all(efiles *t)
{
    bool ok = true;
    for (int i = 0; i < FILES_MAX; i++)
        if (t->f[i].handle && shut(&t->f[i]) != FILES_OK)
            ok = false;
    return ok;
}
