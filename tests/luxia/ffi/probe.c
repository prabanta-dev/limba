/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * probe.c - a C library of known signatures for the calls of C
 * (luxia-0.md § 8.5): probes.luxia declares every function here, calls
 * it and prints what expected/probes.out holds. A back end builds it as
 * the shared library "probe" (on Linux: cc -O2 -shared -fPIC -o
 * libprobe.so probe.c) and runs the program against it; the reference
 * interpreter does not call C.
 *
 * One function for each thing the boundary passes: integers of every
 * width (a result C wraps), long, unsigned char, size_t, float, double,
 * _Bool, structs of 8, 16 (an integer and a double) and 24 bytes by
 * value and as results, addresses (var, out, arrays), an opaque pointer,
 * a string C allocates, and more arguments than registers.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int8_t probe_i8(int8_t a, int8_t b)
{
    return (int8_t)(a - b);
}

uint16_t probe_u16(uint16_t a)
{
    return (uint16_t)(a * 2u);
}

int probe_int(int a, int b)
{
    return a * b - 1;
}

long probe_long(long a)
{
    return a + 1;
}

unsigned char probe_uchar(unsigned char c)
{
    return (unsigned char)(c + 1u);
}

size_t probe_len(const char *s)
{
    return strlen(s);
}

double probe_norm2(double a, double b)
{
    return a * a + b * b;
}

float probe_half(float x)
{
    return x / 2;
}

bool probe_even(int64_t x)
{
    return x % 2 == 0;
}

struct small {
    int32_t a, b;
};

struct small probe_swap(struct small s)
{
    struct small r = {s.b, s.a};
    return r;
}

struct mixed {
    double x;
    int32_t k;
};

struct mixed probe_mixed(struct mixed m, double f)
{
    struct mixed r = {m.x * f, m.k + 1};
    return r;
}

struct big {
    int64_t a, b, c;
};

struct big probe_rotate(struct big b)
{
    struct big r = {b.c, b.a, b.b};
    return r;
}

int64_t probe_sum(struct big b)
{
    return b.a + b.b + b.c;
}

void probe_fill(struct big *b, int32_t *n)
{
    b->a = 1;
    b->b = 2;
    b->c = 3;
    *n = 42;
}

int64_t probe_total(const int32_t *a, size_t n)
{
    int64_t s = 0;
    for (size_t i = 0; i < n; i++)
        s += a[i];
    return s;
}

void probe_scale(double *a, size_t n, double f)
{
    for (size_t i = 0; i < n; i++)
        a[i] *= f;
}

struct handle {
    int64_t tag;
};

void *probe_new(int64_t tag)
{
    struct handle *h = malloc(sizeof(*h));
    if (h)
        h->tag = tag;
    return h;
}

int64_t probe_tag(const void *h)
{
    return h ? ((const struct handle *)h)->tag : -1;
}

void probe_free(void *h)
{
    free(h);
}

/* "hello, " and name, in memory of malloc: the caller frees it */
char *probe_greet(const char *name)
{
    size_t n = strlen(name);
    char *s = malloc(n + 8);
    if (s) {
        memcpy(s, "hello, ", 7);
        memcpy(s + 7, name, n + 1);
    }
    return s;
}

/* eight integers and nine doubles: past the registers of either kind */
double probe_many(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                  int64_t f, int64_t g, int64_t h, double p, double q,
                  double r, double s, double t, double u, double v, double w,
                  double x)
{
    return (double)(a + b + c + d + e + f + g + h) + p + q + r + s + t + u +
           v + w + x;
}
