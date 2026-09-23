/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * fuzz_lir.c - fuzzing target of the two readers of external input: the
 * binary form (.lir) and the text form (.lit). Whatever the bytes, reading
 * must not crash; what reads is verified; what verifies is printed and
 * written again.
 *
 * With clang: clang -fsanitize=fuzzer,address -DLIMBA_FUZZER ... builds a
 * libFuzzer program. Without, it is a driver that runs the files named on
 * its command line through the same function, to replay a corpus or a
 * crash.
 */
#include "limba/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static FILE *null;
    if (!null)
        null = fopen("/dev/null", "w");
    limba_module *m = limba_read(data, size, NULL);
    if (!m)
        m = limba_parse((const char *)data, size, NULL);
    if (m && limba_verify(m, NULL) == 0) {
        uint8_t *buf;
        size_t len;
        if (null)
            limba_print(m, null);
        limba_write(m, &buf, &len);
        free(buf);
    }
    limba_module_free(m);
    return 0;
}

#ifndef LIMBA_FUZZER
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)n + 1);
        if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
            perror(argv[i]);
            return 1;
        }
        fclose(f);
        LLVMFuzzerTestOneInput(buf, (size_t)n);
        free(buf);
    }
    return 0;
}
#endif
