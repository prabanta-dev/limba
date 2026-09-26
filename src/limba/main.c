/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * main.c - the limba program. It reads and writes the IR: the text form
 * (.lit) and the binary form (.lir), verifying it on the way; and it reads
 * Luxia sources (.luxia), for now as far as their tokens.
 */
#include "limba/ir.h"
#include "limba/opt.h"
#include "luxia.h"

#include <errno.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fputs("Usage: limba [options] <input.lit | input.lir | input.luxia>\n"
          "\n"
          "Reads the IR in its text (.lit) or binary (.lir) form, verifies\n"
          "it and writes it in the other form. Reads a Luxia source\n"
          "(.luxia), reports its errors and writes its IR.\n"
          "\n"
          "Options\n"
          "  --emit=lir|lit  the form to write; by default the other one\n"
          "  --emit=tokens   the tokens of a Luxia source, one per line\n"
          "  --emit=ast      the syntax tree of a Luxia source\n"
          "  -o FILE         where to write; by default a .lir goes next to\n"
          "                  the input, a .lit to the standard output\n"
          "  --check         verify only, write nothing\n"
          "  -O0, -O1        optimise: no (default) or yes\n"
          "  --stats         what each pass changed, on standard error\n"
          "  --verify        verify the IR a Luxia source gives, and after\n"
          "                  every pass (always on in the builds that are\n"
          "                  not release; --check verifies anyway)\n"
          "  --verify-each   the same\n"
          "  --skip=a,b      passes not to run (also LIMBA_OPTSKIP)\n"
          "  --suppress=a,b  checks off in the whole Luxia source\n"
          "                  (index_check, range_check, overflow_check,\n"
          "                  division_check, conversion_check,\n"
          "                  shift_check, nil_check, all_checks); where\n"
          "                  one would fail, the behaviour is undefined\n"
          "  -h, --help      this text\n",
          out);
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && strcmp(s + n - k, suffix) == 0;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? 2 * cap : 65536;
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    bool err = ferror(f);
    fclose(f);
    if (err) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
#ifdef __GLIBC__
    /* a compiler lives briefly and grows: the heap grows by 64 MiB at a
       time and keeps what is freed, so its pages are touched once (with
       glibc's defaults a large source faults 14 000 pages, now 600) */
    mallopt(M_TOP_PAD, 64 << 20);
    mallopt(M_MMAP_THRESHOLD, 1 << 30);
    mallopt(M_TRIM_THRESHOLD, 1 << 30);
#endif
    const char *in = NULL, *outpath = NULL, *emit = NULL, *suppress = NULL;
    bool check = false;
    int level = 0;
    limba_opt_options opt = {false, NULL, NULL, NULL};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--check")) {
            check = true;
        } else if (!strcmp(a, "-O0") || !strcmp(a, "-O1")) {
            level = a[2] - '0';
        } else if (!strcmp(a, "--stats")) {
            opt.stats = stderr;
        } else if (!strcmp(a, "--verify-each") || !strcmp(a, "--verify")) {
            opt.verify_each = true;
        } else if (!strncmp(a, "--skip=", 7)) {
            opt.skip = a + 7;
        } else if (!strncmp(a, "--suppress=", 11)) {
            suppress = a + 11;
        } else if (!strncmp(a, "--emit=", 7)) {
            emit = a + 7;
            if (strcmp(emit, "lir") && strcmp(emit, "lit") &&
                strcmp(emit, "tokens") && strcmp(emit, "ast")) {
                fprintf(stderr,
                        "limba: --emit takes lir, lit, tokens or ast\n");
                return 2;
            }
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            outpath = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "limba: unknown option %s (--help)\n", a);
            return 2;
        } else if (!in) {
            in = a;
        } else {
            fprintf(stderr, "limba: one input only\n");
            return 2;
        }
    }
    if (!in) {
        usage(stderr);
        return 2;
    }
    if (ends_with(in, ".luxia"))
        return limba_luxia_main(in, emit, outpath, check, level, &opt,
                                suppress);
    bool binary_in = ends_with(in, ".lir");
    if (!binary_in && !ends_with(in, ".lit")) {
        fprintf(stderr, "limba: %s: a .lit, .lir or .luxia file is expected\n",
                in);
        return 2;
    }
    if (emit && (!strcmp(emit, "tokens") || !strcmp(emit, "ast"))) {
        fprintf(stderr, "limba: --emit=%s is for a .luxia source\n", emit);
        return 2;
    }
    if (!emit)
        emit = binary_in ? "lit" : "lir";

    size_t len;
    char *data = slurp(in, &len);
    if (!data) {
        fprintf(stderr, "limba: %s: %s\n", in, strerror(errno));
        return 1;
    }
    limba_diag d = {{0}, 0};
    limba_module *m = binary_in ? limba_read((const uint8_t *)data, len, &d)
                                : limba_parse(data, len, &d);
    free(data);
    if (!m || limba_verify(m, &d) != 0) {
        fprintf(stderr, "limba: %s: %s\n", in, d.msg);
        limba_module_free(m);
        return 1;
    }
    if (level > 0 && limba_optimize(m, &opt, &d) != 0) {
        fprintf(stderr, "limba: %s: %s\n", in, d.msg);
        limba_module_free(m);
        return 1;
    }
    if (check) {
        limba_module_free(m);
        return 0;
    }

    int status = 0;
    if (!strcmp(emit, "lit")) {
        FILE *out = outpath ? fopen(outpath, "w") : stdout;
        if (!out) {
            fprintf(stderr, "limba: %s: %s\n", outpath, strerror(errno));
            status = 1;
        } else {
            limba_print(m, out);
            if (out != stdout && fclose(out) != 0)
                status = 1;
        }
    } else {
        char *derived = NULL;
        if (!outpath) {
            size_t n = strlen(in);
            derived = malloc(n + 1);
            if (!derived)
                return 1;
            memcpy(derived, in, n + 1);
            memcpy(derived + n - 4, ".lir", 4);
            outpath = derived;
        }
        uint8_t *buf;
        size_t blen;
        limba_write(m, &buf, &blen);
        FILE *out = fopen(outpath, "wb");
        if (!out || fwrite(buf, 1, blen, out) != blen) {
            fprintf(stderr, "limba: %s: %s\n", outpath, strerror(errno));
            status = 1;
        }
        if (out && fclose(out) != 0)
            status = 1;
        free(buf);
        free(derived);
    }
    limba_module_free(m);
    return status;
}
